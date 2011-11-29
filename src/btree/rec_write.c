/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

struct __rec_kv;		typedef struct __rec_kv WT_KV;
struct __rec_boundary;		typedef struct __rec_boundary WT_BOUNDARY;

/*
 * Reconciliation is the process of taking an in-memory page, walking each entry
 * in the page, building a backing disk image in a temporary buffer representing
 * that information, and writing that buffer to disk.  What could be simpler?
 *
 * WT_RECONCILE --
 *	Information tracking a single page reconciliation.
 */
typedef struct {
	WT_PAGE *page;			/* Page being reconciled */

	WT_BUF	 dsk;			/* Temporary disk-image buffer */

	int	 salvage;		/* Called from salvage */

	/*
	 * Reconciliation gets tricky if we have to split a page, that is, if
	 * the disk image we create exceeds the maximum size of disk images for
	 * this page type.  First, the split sizes: reconciliation splits to a
	 * smaller-than-maximum page size when a split is required so we don't
	 * repeatedly split a packed page.
	 */
	uint32_t btree_split_pct;	/* Split page percent */
	uint32_t page_size;		/* Maximum page size */
	uint32_t split_size;		/* Split page size */

	/*
	 * The problem with splits is we've done a lot of work by the time we
	 * realize we're going to have to split, we don't want to start over.
	 *
	 * To keep from having to start over when we hit the maximum page size,
	 * we track the page information when we approach a split boundary.
	 * If we eventually have to split, we walk this structure and pretend
	 * we were splitting all along.  After that, we continue to append to
	 * this structure, and eventually walk it to create a new internal page
	 * that references all of our split pages.
	 */
	struct __rec_boundary {
		/*
		 * The start field records location in the initial split buffer,
		 * that is, the first byte of the split chunk recorded before we
		 * decide to split a page; the offset between the first byte of
		 * chunk[0] and the first byte of chunk[1] is chunk[0]'s length.
		 *
		 * Once we split a page, we stop filling in the start field, as
		 * we're writing the split chunks as we find them.
		 */
		uint8_t *start;		/* Split's first byte */

		/*
		 * The recno and entries fields are the starting record number
		 * of the split chunk (for column-store splits), and the number
		 * of entries in the split chunk.  These fields are used both
		 * to write the split chunk, and to create a new internal page
		 * to reference the split pages.
		 */
		uint64_t recno;		/* Split's starting record */
		uint32_t entries;	/* Split's entries */

		WT_OFF off;		/* Split's written addr/size pair */

		/*
		 * The key for a row-store page; no column-store key is needed
		 * because the page's recno, stored in the recno field, is the
		 * column-store key.
		 */
		WT_BUF key;		/* Promoted row-store key */
	} *bnd;				/* Saved boundaries */
	uint32_t bnd_next;		/* Next boundary slot */
	uint32_t bnd_entries;		/* Total boundary slots */
	size_t   bnd_allocated;		/* Bytes allocated */

	/*
	 * We track the total number of page entries copied into split chunks
	 * so we can easily figure out how many entries in the current split
	 * chunk.
	 */
	uint32_t total_entries;		/* Total entries in splits */

	/*
	 * And there's state information as to where in this process we are:
	 * (1) tracking split boundaries because we can still fit more split
	 * chunks into the maximum page size, (2) tracking the maximum page
	 * size boundary because we can't fit any more split chunks into the
	 * maximum page size, (3) not performing boundary checks because it's
	 * either not useful with the current page size configuration, or
	 * because we've already been forced to split.
	 */
	enum {	SPLIT_BOUNDARY=0,	/* Next: a split page boundary */
		SPLIT_MAX=1,		/* Next: the maximum page boundary */
		SPLIT_TRACKING_OFF=2 }	/* No boundary checks */
	bnd_state;

	/*
	 * We track current information about the current record number, the
	 * number of entries copied into the temporary buffer, where we are
	 * in the temporary buffer, and how much memory remains.  Those items
	 * are packaged here rather than passing pointers to stack locations
	 * around the code.
	 */
	uint64_t recno;			/* Current record number */
	uint32_t entries;		/* Current number of entries */
	uint8_t *first_free;		/* Current first free byte */
	uint32_t space_avail;		/* Remaining space in this chunk */

	/*
	 * We don't need to keep the 0th key around on internal pages, the
	 * search code ignores them as nothing can sort less by definition.
	 * There's some trickiness here, see the code for comments on how
	 * these fields work.
	 */
	int	    cell_zero;		/* Row-store internal page 0th key */
	WT_ROW_REF *merge_ref;		/* Row-store merge correction key */

	/*
	 * WT_KV--
	 *	An on-page key/value item we're building.
	 */
	struct __rec_kv {
		WT_BUF	 buf;		/* Data */
		WT_CELL	 cell;		/* Cell and cell's length */
		uint32_t cell_len;
		uint32_t len;		/* Total length of cell + data */

		WT_OFF	 off;		/* Associated off-page value */
	} k, v;				/* Key/Value being built */

	WT_BUF *cur, _cur;		/* Key/Value being built */
	WT_BUF *last, _last;		/* Last key/value built */

	int	key_pfx_compress;	/* If can prefix-compress next key */
	int     key_pfx_compress_conf;	/* If prefix compression configured */
	int	key_sfx_compress;	/* If can suffix-compress next key */
	int     key_sfx_compress_conf;	/* If suffix compression configured */
} WT_RECONCILE;

static int  __rec_cell_build_key(
		WT_SESSION_IMPL *, const void *, uint32_t, int, int *);
static int  __rec_cell_build_ovfl(
		WT_SESSION_IMPL *, const void *, WT_KV *, uint8_t, uint64_t);
static int  __rec_cell_build_val(
		WT_SESSION_IMPL *, const void *, uint32_t, uint64_t);
static int  __rec_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_fix_bulk(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_fix_slvg(
		WT_SESSION_IMPL *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_merge(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_var(WT_SESSION_IMPL *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_col_var_bulk(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_var_helper(WT_SESSION_IMPL *,
		WT_SALVAGE_COOKIE *, WT_BUF *, int, int, uint64_t);
static int  __rec_row_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_row_leaf_insert(WT_SESSION_IMPL *, WT_INSERT *);
static int  __rec_row_merge(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_split(WT_SESSION_IMPL *session);
static int  __rec_split_col(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **);
static int  __rec_split_finish(WT_SESSION_IMPL *);
static int  __rec_split_fixup(WT_SESSION_IMPL *);
static int  __rec_split_init(WT_SESSION_IMPL *, WT_PAGE *, uint64_t, uint32_t);
static int  __rec_split_row(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **);
static int  __rec_split_row_promote(WT_SESSION_IMPL *, uint8_t);
static int  __rec_split_write(WT_SESSION_IMPL *, WT_BOUNDARY *, WT_BUF *);
static int  __rec_track_cell(WT_SESSION_IMPL *, WT_PAGE *, WT_CELL_UNPACK *);
static int  __rec_track_ovfl_active(WT_SESSION_IMPL *,
		WT_PAGE *, const void *, uint32_t *, uint32_t *);
static int  __rec_track_restart_col(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_track_restart_ovfl(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_track_restart_row(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_track_verbose(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE_TRACK *);
static int  __rec_write_init(WT_SESSION_IMPL *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_write_wrapup(WT_SESSION_IMPL *, WT_PAGE *);

/*
 * __wt_rec_modify_init --
 *	A page is about to be modified, get ready for reconciliation.
 */
int
__wt_rec_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_RET(__wt_calloc_def(session, 1, &page->modify));

	if (WT_PADDR(page) != WT_ADDR_INVALID)
		WT_RET(__wt_rec_track(session, page, WT_PT_BLOCK,
		    NULL, WT_PADDR(page), WT_PSIZE(page)));
	return (0);
}

/*
 * __wt_rec_write --
 *	Reconcile an in-memory page into its on-disk format, and write it.
 */
int
__wt_rec_write(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_VERBOSE(session, reconcile,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	WT_BSTAT_INCR(session, rec_written);

	/* We're only interested in dirty pages. */
	WT_ASSERT(session, __wt_page_is_modified(page));

	/* Update the disk generation before we read anything from the page. */
	WT_ORDERED_READ(page->modify->disk_gen, page->modify->write_gen);

	/* Initialize the reconciliation structure for each new run. */
	WT_RET(__rec_write_init(session, page, salvage));

	/* Reset overflow tracking information for this page. */
	__rec_track_restart_ovfl(session, page);

	/* Reconcile the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_fix_bulk(session, page));
		else if (salvage != NULL)
			WT_RET(__rec_col_fix_slvg(session, page, salvage));
		else
			WT_RET(__rec_col_fix(session, page));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__rec_col_int(session, page));
		break;
	case WT_PAGE_COL_VAR:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_var_bulk(session, page));
		else
			WT_RET(__rec_col_var(session, page, salvage));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__rec_row_int(session, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__rec_row_leaf(session, page, salvage));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* Wrap it up. */
	WT_RET(__rec_write_wrapup(session, page));

	/*
	 * If this page has a parent, mark the parent dirty.
	 *
	 * There's no chance we need to flush this write -- the eviction thread
	 * is the only thread that eventually cares if the page is dirty or not,
	 * and it's our update making the parent dirty.   (Other threads do have
	 * to flush their set-page-modified update, of course).
	 *
	 * We don't care if we race with updates: the page will still be marked
	 * dirty and that's all we care about.
	 */
	if (!WT_PAGE_IS_ROOT(page))
		WT_RET(__wt_page_set_modified(session, page->parent));

	return (0);
}

/*
 * __rec_write_init --
 *	Initialize the reconciliation structure.
 */
static int
__rec_write_init(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_CONFIG_ITEM cval;
	WT_RECONCILE *r;

	/* Allocate a reconciliation structure if we don't already have one. */
	if ((r = session->btree->reconcile) == NULL) {
		WT_RET(__wt_calloc_def(session, 1, &r));
		session->btree->reconcile = r;

		/* Connect prefix compression pointers/buffers. */
		r->cur = &r->_cur;
		r->last = &r->_last;

		/* Configuration. */
		WT_RET(__wt_config_getones(session,
		    session->btree->config, "split_pct", &cval));
		r->btree_split_pct = (uint32_t)cval.val;

		WT_RET(__wt_config_getones(session,
		    session->btree->config,
		    "internal_key_truncate", &cval));
		r->key_sfx_compress_conf = (cval.val != 0);

		WT_RET(__wt_config_getones(session,
		    session->btree->config,
		    "prefix_compression", &cval));
		r->key_pfx_compress_conf = (cval.val != 0);
	}

	r->page = page;
	r->salvage = salvage == NULL ? 0 : 1;

	return (0);
}

/*
 * __rec_destroy --
 *	Clean up the reconciliation structure.
 */
void
__wt_rec_destroy(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_RECONCILE *r;
	uint32_t i;

	if ((r = session->btree->reconcile) == NULL)
		return;

	__wt_buf_free(session, &r->dsk);

	if (r->bnd != NULL) {
		for (bnd = r->bnd, i = 0; i < r->bnd_entries; ++bnd, ++i)
			__wt_buf_free(session, &bnd->key);
		__wt_free(session, r->bnd);
	}

	__wt_buf_free(session, &r->k.buf);
	__wt_buf_free(session, &r->v.buf);
	__wt_buf_free(session, &r->_cur);
	__wt_buf_free(session, &r->_last);

	__wt_free(session, session->btree->reconcile);
}

/*
 * __rec_incr --
 *	Update the memory tracking structure for a set of new entries.
 */
static inline void
__rec_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t v, uint32_t size)
{
	/*
	 * The buffer code is fragile and prone to off-by-one errors -- check
	 * for overflow in diagnostic mode.
	 */
	WT_ASSERT(session, r->space_avail >= size);
	WT_ASSERT(session,
	    WT_BLOCK_FITS(r->first_free, size, r->dsk.mem, r->page_size));

	r->entries += v;
	r->space_avail -= size;
	r->first_free += size;
}

/*
 * __rec_copy_incr --
 *	Copy a key/value cell and buffer pair into the new image.
 */
static inline void
__rec_copy_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_KV *kv)
{
	uint32_t len;
	uint8_t *p, *t;

	/*
	 * If there's only one chunk of data to copy (because the cell and data
	 * are being copied from the original disk page), the cell length won't
	 * be set, the WT_BUF data/length will reference the data to be copied.
	 *
	 * WT_CELLs are typically small, 1 or 2 bytes -- don't call memcpy, do
	 * the copy in-line.
	 */
	for (p = (uint8_t *)r->first_free,
	    t = (uint8_t *)&kv->cell, len = kv->cell_len; len > 0; --len)
		*p++ = *t++;

	/* The data can be quite large -- call memcpy. */
	if (kv->buf.size != 0)
		memcpy(p, kv->buf.data, kv->buf.size);

	WT_ASSERT(session, kv->len == kv->cell_len + kv->buf.size);
	__rec_incr(session, r, 1, kv->len);
}

/*
 * __rec_key_state_update --
 *	Update prefix and suffix compression based on the last key.
 */
static inline void
__rec_key_state_update(WT_RECONCILE *r, int ovfl_key)
{
	WT_BUF *a;

	/*
	 * If writing an overflow key onto the page, don't update the "last key"
	 * value, and leave the state of prefix compression alone.  (If we are
	 * currently doing prefix compression, we have a key state which will
	 * continue to work, we're just skipping the key just created because
	 * it's an overflow key and doesn't participate in prefix compression.
	 * If we are not currently doing prefix compression, we can't start, an
	 * overflow key doesn't give us any state.)
	 *
	 * Additionally, if we wrote an overflow key onto the page, turn off the
	 * suffix compression of row-store internal node keys.  (When we split,
	 * "last key" is the largest key on the previous page, and "cur key" is
	 * the first key on the next page, which is being promoted.  In some
	 * cases we can discard bytes from the "cur key" that are not needed to
	 * distinguish between the "last key" and "cur key", compressing the
	 * size of keys on internal nodes.  If we just built an overflow key,
	 * we're not going to update the "last key", making suffix compression
	 * impossible for the next key.   Alternatively, we could remember where
	 * the last key was on the page, detect it's an overflow key, read it
	 * from disk and do suffix compression, but that's too much work for an
	 * unlikely event.)
	 *
	 * If we're not writing an overflow key on the page, update the last-key
	 * value and turn on both prefix and suffix compression.
	 */
	if (ovfl_key)
		r->key_sfx_compress = 0;
	else {
		a = r->cur;
		r->cur = r->last;
		r->last = a;

		r->key_pfx_compress = r->key_pfx_compress_conf;
		r->key_sfx_compress = r->key_sfx_compress_conf;
	}
}

/*
 * __rec_split_bnd_grow --
 *	Grow the boundary array as necessary.
 */
static inline int
__rec_split_bnd_grow(WT_SESSION_IMPL *session)
{
	WT_RECONCILE *r;

	r = session->btree->reconcile;

	/*
	 * Make sure there's enough room in which to save another boundary.
	 *
	 * The calculation is actually +1, because we save the start point one
	 * past the current entry -- make it +20 so we don't grow slot-by-slot.
	 */
	if (r->bnd_next + 1 >= r->bnd_entries) {
		WT_RET(__wt_realloc(session, &r->bnd_allocated,
		    (r->bnd_entries + 20) * sizeof(*r->bnd), &r->bnd));
		r->bnd_entries += 20;
	}
	return (0);
}

/*
 * __rec_split_init --
 *	Initialization for the reconciliation split functions.
 */
static int
__rec_split_init(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t recno, uint32_t max)
{
	WT_BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;

	r = session->btree->reconcile;
	btree = session->btree;
						/* New page, compression off. */
	r->key_pfx_compress = r->key_sfx_compress = 0;

	/* Ensure the scratch buffer is large enough. */
	WT_RET(__wt_buf_initsize(session, &r->dsk, (size_t)max));

	/*
	 * Some fields of the disk image are fixed based on the original page,
	 * set them.
	 */
	dsk = r->dsk.mem;
	WT_CLEAR(*dsk);
	dsk->type = page->type;

	/*
	 * If we have to split, we want to choose a smaller page size for the
	 * split pages, because otherwise we could end up splitting one large
	 * packed page over and over.   We don't want to pick the minimum size
	 * either, because that penalizes an application that did a bulk load
	 * and subsequently inserted a few items into packed pages.  Currently,
	 * I'm using 75%, but I have no empirical evidence that's a good value.
	 * We should leave this as a tuning variable, but probably undocumented.
	 *
	 * The maximum page size may be a multiple of the split page size (for
	 * example, there's a maximum page size of 128KB, but because the table
	 * is active and we don't want to split a lot, the split size is 20KB).
	 * The maximum page size may NOT be an exact multiple of the split page
	 * size.
	 *
	 * It's lots of work to build these pages and don't want to start over
	 * when we reach the maximum page size (it's painful to restart after
	 * creating overflow items and compacted data, for example, as those
	 * items have already been written to disk).  So, the loop calls the
	 * helper functions when approaching a split boundary, and we save the
	 * information at that point.  That allows us to go back and split the
	 * page at the boundary points if we eventually overflow the maximum
	 * page size.
	 *
	 * Finally, fixed-size column-store pages can split under (very) rare
	 * circumstances, but they're usually allocated at a fixed page size,
	 * never anything smaller.
	 */
	r->page_size = max;
	r->split_size = page->type == WT_PAGE_COL_FIX ?
	    max :
	    WT_SPLIT_PAGE_SIZE(max, btree->allocsize, r->btree_split_pct);

	/*
	 * If the maximum page size is the same as the split page size, there
	 * is no need to maintain split boundaries within a larger page.
	 */
	r->bnd_state =
	    max == r->split_size ? SPLIT_TRACKING_OFF : SPLIT_BOUNDARY;

	/*
	 * Initialize the array of boundary items and set the initial record
	 * number and buffer address.
	 */
	r->bnd_next = 0;
	WT_RET(__rec_split_bnd_grow(session));
	r->bnd[0].recno = recno;
	r->bnd[0].start = WT_PAGE_DISK_BYTE(dsk);

	/* Initialize the total entries. */
	r->total_entries = 0;

	/*
	 * Set the caller's information and configure so the loop calls us
	 * when approaching the split boundary.
	 */
	r->recno = recno;
	r->entries = 0;
	r->first_free = WT_PAGE_DISK_BYTE(dsk);
	r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
	return (0);
}

/*
 * __rec_split --
 *	Handle the page reconciliation bookkeeping.  (Did you know "bookkeeper"
 * has 3 doubled letters in a row?  Sweet-tooth does, too.)
 */
static int
__rec_split(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t current_len;

	/*
	 * Handle page-buffer size tracking; we have to do this work in every
	 * reconciliation loop, and I don't want to repeat the code that many
	 * times.
	 */
	r = session->btree->reconcile;
	dsk = r->dsk.mem;

	/*
	 * There are 3 cases we have to handle.
	 *
	 * #1
	 * Not done, and about to cross a split boundary, in which case we save
	 * away the current boundary information and return.
	 *
	 * #2
	 * Not done, and about to cross the max boundary, in which case we have
	 * to physically split the page -- use the saved split information to
	 * write all the split pages.
	 *
	 * #3
	 * Not done, and about to cross the split boundary, but we've already
	 * done the split thing when we approached the max boundary, in which
	 * case we write the page and keep going.
	 *
	 * Cases #1 and #2 are the hard ones: we're called when we're about to
	 * cross each split boundary, and we save information away so we can
	 * split if we have to.  We're also called when we're about to cross
	 * the maximum page boundary: in that case, we do the actual split,
	 * clean things up, then keep going.
	 */
	switch (r->bnd_state) {
	case SPLIT_BOUNDARY:				/* Case #1 */
		/*
		 * Save the information about where we are when the split would
		 * have happened.
		 */
		WT_RET(__rec_split_bnd_grow(session));
		bnd = &r->bnd[r->bnd_next++];

		/* Set the number of entries for the just finished chunk. */
		bnd->entries = r->entries - r->total_entries;
		r->total_entries = r->entries;

		/*
		 * Set the starting record number, buffer address and promotion
		 * key for the next chunk, clear the entries (not required, but
		 * cleaner).
		 */
		++bnd;
		bnd->recno = r->recno;
		bnd->start = r->first_free;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(session, dsk->type));
		bnd->entries = 0;

		/*
		 * Set the space available to another split-size chunk, if we
		 * have one.  If we don't have room for another split chunk,
		 * add whatever space remains in the maximum page size, and
		 * hope it's enough.
		 */
		current_len = WT_PTRDIFF32(r->first_free, dsk);
		if (current_len + r->split_size <= r->page_size)
			r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
		else {
			r->bnd_state = SPLIT_MAX;
			r->space_avail =
			    (r->page_size - WT_PAGE_DISK_SIZE) - current_len;
		}
		break;
	case SPLIT_MAX:					/* Case #2 */
		/*
		 * It didn't all fit into a single page.
		 *
		 * Cycle through the saved split-point information, writing the
		 * split chunks we have tracked.
		 */
		WT_RET(__rec_split_fixup(session));

		/* We're done saving split chunks. */
		r->bnd_state = SPLIT_TRACKING_OFF;
		break;
	case SPLIT_TRACKING_OFF:			/* Case #3 */
		WT_RET(__rec_split_bnd_grow(session));
		bnd = &r->bnd[r->bnd_next++];

		/*
		 * It didn't all fit, but either we've already noticed it and
		 * are now processing the rest of the page at the split-size
		 * boundaries, or the split size was the same as the page size,
		 * so we never bothered with saving split-point information.
		 *
		 * Write the current disk image.
		 */
		dsk->recno = bnd->recno;
		dsk->u.entries = r->entries;
		r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
		WT_RET(__rec_split_write(session, bnd, &r->dsk));

		/*
		 * Set the starting record number and promotion key for the next
		 * chunk, clear the entries (not required, but cleaner).
		 */
		++bnd;
		bnd->recno = r->recno;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(session, dsk->type));
		bnd->entries = 0;

		/*
		 * Set the caller's entry count and buffer information for the
		 * next chunk.  We only get here if we're not splitting or have
		 * already split, so it's split-size chunks from here on out.
		 */
		r->entries = 0;
		r->first_free = WT_PAGE_DISK_BYTE(dsk);
		r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
		break;
	}
	return (0);
}

/*
 * __rec_split_finish --
 *	Finish processing a split page.
 */
static int
__rec_split_finish(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;

	r = session->btree->reconcile;

	/*
	 * We're done reconciling a page.
	 *
	 * First, we only arrive here with no entries to write if the page was
	 * entirely empty (if the page wasn't empty, the only reason to split,
	 * resetting entries to 0, is because there's another entry to write,
	 * which then sets entries to 1).  If the page was empty, we eventually
	 * delete it.
	 */
	if (r->entries == 0) {
		WT_ASSERT_RET(session, r->bnd_next == 0);
		return (0);
	}

	/*
	 * Second, check our split status:
	 *
	 * If we have already split, put the remaining data in the next boundary
	 * slot.
	 *
	 * If we have not yet split, the reconciled page fit into a maximum page
	 * size, all of our boundary checking was wasted.   Change the first
	 * boundary slot to represent the full page (the first boundary slot is
	 * largely correct, just update the number of entries).
	 */
	if (r->bnd_state == SPLIT_TRACKING_OFF) {
		WT_RET(__rec_split_bnd_grow(session));
		bnd = &r->bnd[r->bnd_next++];
	} else {
		r->bnd_next = 1;
		bnd = &r->bnd[0];
		bnd->entries = r->entries;
	}

	/* Write the remaining information. */
	dsk = r->dsk.mem;
	dsk->recno = bnd->recno;
	dsk->u.entries = r->entries;
	r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
	return (__rec_split_write(session, bnd, &r->dsk));
}

/*
 * __rec_split_fixup --
 *	Fix up after crossing the maximum page boundary.
 */
static int
__rec_split_fixup(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t i, len;
	uint8_t *dsk_start;
	int ret;

	/*
	 * When we overflow physical limits of the page, we walk the list of
	 * split chunks we've created and write those pages out, then update
	 * the caller's information.
	 */
	r = session->btree->reconcile;
	tmp = NULL;
	ret = 0;

	/*
	 * The data isn't laid out on a page boundary or nul padded; copy it to
	 * a clean, aligned, padded buffer before writing it.
	 *
	 * Allocate a scratch buffer to hold the new disk image.  Copy the
	 * WT_PAGE_DISK header onto the scratch buffer, most of the header
	 * information remains unchanged between the pages.
	 */
	WT_RET(__wt_scr_alloc(session, r->split_size, &tmp));
	dsk = tmp->mem;
	dsk_start = WT_PAGE_DISK_BYTE(dsk);
	memcpy(dsk, r->dsk.mem, WT_PAGE_DISK_SIZE);

	/*
	 * For each split chunk we've created, update the disk image and copy
	 * it into place.
	 */
	for (i = 0, bnd = r->bnd; i < r->bnd_next; ++i, ++bnd) {
		/* Copy the page contents to the temporary buffer. */
		len = WT_PTRDIFF32((bnd + 1)->start, bnd->start);
		memcpy(dsk_start, bnd->start, len);

		/* Write the page. */
		dsk->recno = bnd->recno;
		dsk->u.entries = bnd->entries;
		tmp->size = WT_PAGE_DISK_SIZE + len;
		WT_ERR(__rec_split_write(session, bnd, tmp));
	}

	/*
	 * There is probably a remnant in the working buffer that didn't get
	 * written; copy it down to the beginning of the working buffer, and
	 * update the starting record number.
	 *
	 * Confirm the remnant is no larger than the available split buffer.
	 *
	 * Fix up our caller's information.
	 */
	len = WT_PTRDIFF32(r->first_free, bnd->start);
	WT_ASSERT_RET(session, len < r->split_size - WT_PAGE_DISK_SIZE);

	dsk = r->dsk.mem;
	dsk_start = WT_PAGE_DISK_BYTE(dsk);
	(void)memmove(dsk_start, bnd->start, len);

	r->entries -= r->total_entries;
	r->first_free = dsk_start + len;
	r->space_avail = (r->split_size - WT_PAGE_DISK_SIZE) - len;

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __rec_split_write --
 *	Write a disk block out for the split helper functions.
 */
static int
__rec_split_write(WT_SESSION_IMPL *session, WT_BOUNDARY *bnd, WT_BUF *buf)
{
	WT_CELL *cell;
	WT_PAGE_DISK *dsk;

	/*
	 * We always write an additional byte on row-store leaf pages after the
	 * key value pairs.  The reason is that zero-length value items are not
	 * written on the page and they're detected by finding two adjacent key
	 * cells.  If the last value item on a page is zero length, we need a
	 * key cell after it on the page to detect it.  The row-store leaf page
	 * reconciliation code made sure we had a spare byte in the buffer, now
	 * write a trailing zero-length key cell.  This isn't a valid key cell,
	 * but since it's not referenced by the entries on the page, no code but
	 * the code reading after the key cell, to find the key value, will ever
	 * see it.
	 */
#define	WT_TRAILING_KEY_CELL	(sizeof(uint8_t))
	dsk = buf->mem;
	if (dsk->type == WT_PAGE_ROW_LEAF) {
		WT_ASSERT_RET(session, buf->size < buf->memsize);

		cell = (WT_CELL *)&(((uint8_t *)buf->data)[buf->size]);
		(void)__wt_cell_pack_type(cell, WT_CELL_KEY, (uint64_t)0);
		++buf->size;
	}

	/* Write the chunk. */
	return (__wt_block_write(session, buf, &bnd->off.addr, &bnd->off.size));
}

/*
 * __rec_split_row_promote --
 *	Key promotion for a row-store.
 */
static int
__rec_split_row_promote(WT_SESSION_IMPL *session, uint8_t type)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_RECONCILE *r;
	uint32_t cnt, len, size;
	const uint8_t *pa, *pb;

	r = session->btree->reconcile;
	unpack = &_unpack;

	/*
	 * For a column-store, the promoted key is the recno and we already have
	 * a copy.  For a row-store, it's the first key on the page, a variable-
	 * length byte string, get a copy.
	 *
	 * This function is called from __rec_split at each split boundary, but
	 * that means we're not called before the first boundary.  It's painful,
	 * but we need to detect that case and copy the key from the page we're
	 * building.  We could simplify this by grabbing a copy of the first key
	 * we put on a page, perhaps in the function building keys for a page,
	 * but that's going to be uglier than this.
	 */
	if (r->bnd_next == 1) {
		/*
		 * The cell had better have a zero-length prefix: it's the first
		 * key on the page.  (If it doesn't have a zero-length prefix,
		 * __wt_cell_update_copy() won't be sufficient any way, we'd
		 * only copy the non-prefix-compressed portion of the key.)
		 */
		cell = WT_PAGE_DISK_BYTE(r->dsk.mem);
		__wt_cell_unpack(cell, unpack);
		WT_ASSERT_RET(session,
		    unpack->prefix == 0 || unpack->type == WT_CELL_KEY_OVFL);
		WT_RET(
		    __wt_cell_unpack_copy(session, unpack, &r->bnd[0].key));
	}

	/*
	 * For the current slot, take the last key we built, after doing suffix
	 * compression.
	 *
	 * Suffix compression is a hack to shorten keys on internal pages.  We
	 * only need enough bytes in the promoted key to ensure searches go to
	 * the correct page: the promoted key has to be larger than the last key
	 * on the leaf page preceding it, but we don't need any more bytes than
	 * that.   In other words, we can discard any suffix bytes not required
	 * to distinguish between the key being promoted and the last key on the
	 * leaf page preceding it.  This can only be done for the first level of
	 * internal pages, you cannot repeat suffix truncation as you split up
	 * the tree, it loses too much information.
	 *
	 * The r->last key sorts before the r->cur key, so we'll either find a
	 * larger byte value in r->cur, or r->cur will be the longer key. One
	 * caveat: if the largest key on the previous page was an overflow key,
	 * we don't have a key against which to compare, and we can't do suffix
	 * compression.
	 */
	if (type == WT_PAGE_ROW_LEAF && r->key_sfx_compress) {
		pa = r->last->data;
		pb = r->cur->data;
		len = WT_MIN(r->last->size, r->cur->size);
		size = len + 1;
		for (cnt = 1; len > 0; ++cnt, --len, ++pa, ++pb)
			if (*pa != *pb) {
				size = cnt;
				break;
			}
	} else
		size = r->cur->size;
	return (__wt_buf_set(
	    session, &r->bnd[r->bnd_next].key, r->cur->data, size));
}

/*
 * __rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__rec_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_RET(__rec_split_init(session,
	    page, page->u.col_int.recno, session->btree->maxintlpage));

	/*
	 * Walking the row-store internal pages is complicated by the fact that
	 * we're taking keys from the underlying disk image for the top-level
	 * page and we're taking keys from in-memory structures for merge pages.
	 * Column-store is simpler because the only information we copy is the
	 * WT_OFF_RECORD structure, and it comes from in-memory structures in
	 * both the top-level and merge cases.  In short, both the top-level
	 * and merge page walks look the same, and we just call the merge page
	 * function on the top-level page.
	 */
	WT_RET(__rec_col_merge(session, page));

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_merge --
 *	Recursively walk a column-store internal tree of merge pages.
 */
static int
__rec_col_merge(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD off;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	uint32_t i;

	WT_BSTAT_INCR(session, rec_page_merge);

	r = session->btree->reconcile;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(page, cref, i) {
		/* Update the starting record number in case we split. */
		r->recno = cref->recno;

		/*
		 * The page may be deleted or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 *
		 * !!!
		 * Column-store formats don't support deleted pages; they can
		 * shrink, but deleting a page would remove part of the record
		 * count name space.
		 */
		if (WT_COL_REF_STATE(cref) == WT_REF_DISK) {
			off.addr = WT_COL_REF_ADDR(cref);
			off.size = WT_COL_REF_SIZE(cref);
		} else {
			rp = WT_COL_REF_PAGE(cref);
			switch (F_ISSET(rp, WT_PAGE_REC_MASK)) {
			case WT_PAGE_REC_EMPTY:
				WT_ASSERT(session,
				    !F_ISSET(rp, WT_PAGE_REC_EMPTY));
				continue;
			case WT_PAGE_REC_REPLACE:
				off.addr = rp->modify->u.write_off.addr;
				off.size = rp->modify->u.write_off.size;
				break;
			case WT_PAGE_REC_SPLIT:
				WT_RET(__rec_col_merge(session,
				    rp->modify == NULL ?
				    rp : rp->modify->u.write_split));
				continue;
			default:
				off.addr = WT_COL_REF_ADDR(cref);
				off.size = WT_COL_REF_SIZE(cref);
				break;
			}
		}

		/* Boundary: split or write the page. */
		while (sizeof(WT_OFF_RECORD) > r->space_avail)
			WT_RET(__rec_split(session));

		/*
		 * Copy a new WT_OFF_RECORD structure onto the page; any
		 * off-page reference must be a valid disk address.
		 */
		WT_ASSERT_RET(
		    session, WT_COL_REF_ADDR(cref) != WT_ADDR_INVALID);
		WT_RECNO(&off) = cref->recno;
		memcpy(r->first_free, &off, sizeof(WT_OFF_RECORD));
		__rec_incr(session, r, 1, sizeof(WT_OFF_RECORD));
	}

	return (0);
}

/*
 * __rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page.
 */
static int
__rec_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_RECONCILE *r;
	uint64_t recno;
	uint32_t entry, nrecs;

	r = session->btree->reconcile;
	btree = session->btree;

	/* Update any changes to the original on-page data items. */
	WT_SKIP_FOREACH(ins, WT_COL_UPDATE_SINGLE(page))
		__bit_setv_recno(
		    page, WT_INSERT_RECNO(ins), btree->bitcnt,
		    ((uint8_t *)WT_UPDATE_DATA(ins->upd))[0]);

	/* Allocate the memory. */
	WT_RET(__rec_split_init(session,
	    page, page->u.bulk.recno, btree->maxleafpage));

	/* Copy the updated, disk-image bytes into place. */
	memcpy(r->first_free, page->u.col_leaf.bitf,
	    __bitstr_size(page->entries * btree->bitcnt));

	/* Calculate the number of entries per page remainder. */
	entry = page->entries;
	nrecs = (r->space_avail / btree->bitcnt) - page->entries;
	r->recno += entry;

	/* Walk any append list. */
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page))
		for (;;) {
			/*
			 * The application may have inserted records which left
			 * gaps in the name space.
			 */
			for (recno = WT_INSERT_RECNO(ins);
			    nrecs > 0 && r->recno < recno;
			    --nrecs, ++entry, ++r->recno)
				__bit_setv(
				    r->first_free, entry, btree->bitcnt, 0);

			if (nrecs > 0) {
				__bit_setv(r->first_free, entry, btree->bitcnt,
				    ((uint8_t *)WT_UPDATE_DATA(ins->upd))[0]);
				--nrecs;
				++entry;
				++r->recno;
				break;
			}

			/*
			 * If everything didn't fit, update the counters and
			 * split.
			 *
			 * Boundary: split or write the page.
			 */
			__rec_incr(session,
			    r, entry, __bitstr_size(entry * btree->bitcnt));
			WT_RET(__rec_split(session));

			/* Calculate the number of entries per page. */
			entry = 0;
			nrecs = r->space_avail / btree->bitcnt;
		}

	/* Update the counters. */
	__rec_incr(session, r, entry, __bitstr_size(entry * btree->bitcnt));

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page created during salvage.
 */
static int
__rec_col_fix_slvg(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_BTREE *btree;
	WT_RECONCILE *r;
	uint64_t page_start, page_take;
	uint32_t entry, nrecs;

	r = session->btree->reconcile;
	btree = session->btree;

	/*
	 * !!!
	 * It's vanishingly unlikely and probably impossible for fixed-length
	 * column-store files to have overlapping key ranges.  It's possible
	 * for an entire key range to go missing (if a page is corrupted and
	 * lost), but because pages can't split, it shouldn't be possible to
	 * find pages where the key ranges overlap.  That said, we check for
	 * it during salvage and clean up after it here because it doesn't
	 * cost much and future column-store formats or operations might allow
	 * for fixed-length format ranges to overlap during salvage, and I
	 * don't want to have to retrofit the code later.
	 */
	WT_RET(__rec_split_init(session,
	    page, page->u.col_leaf.recno, btree->maxleafpage));

	/* We may not be taking all of the entries on the original page. */
	page_take = salvage->take == 0 ? page->entries : salvage->take;
	page_start = salvage->skip == 0 ? 0 : salvage->skip;
	for (;;) {
		/* Calculate the number of entries per page. */
		entry = 0;
		nrecs = r->space_avail / btree->bitcnt;

		for (; nrecs > 0 && salvage->missing > 0;
		    --nrecs, --salvage->missing, ++entry)
			__bit_setv(r->first_free, entry, btree->bitcnt, 0);

		for (; nrecs > 0 && page_take > 0;
		    --nrecs, --page_take, ++page_start, ++entry)
			__bit_setv(r->first_free, entry, btree->bitcnt,
			    __bit_getv(page->u.col_leaf.bitf,
				(uint32_t)page_start, btree->bitcnt));

		r->recno += entry;
		__rec_incr(
		    session, r, entry, __bitstr_size(entry * btree->bitcnt));

		/*
		 * If everything didn't fit, then we have to force a split and
		 * keep going.
		 *
		 * Boundary: split or write the page.
		 */
		if (salvage->missing == 0 && page_take == 0)
			break;
		WT_RET(__rec_split(session));
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_fix_bulk --
 *	Reconcile a bulk-loaded, fixed-width column-store leaf page.
 */
static int
__rec_col_fix_bulk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_RECONCILE *r;
	uint32_t len;

	r = session->btree->reconcile;
	btree = session->btree;

	/* Allocate the memory -- we know the entire page will fit. */
	WT_RET(__rec_split_init(session,
	    page, page->u.bulk.recno, btree->maxleafpage));

	/* Copy the bytes into place. */
	len = __bitstr_size(page->entries * btree->bitcnt);
	memcpy(r->first_free, page->u.bulk.bitf, len);
	__rec_incr(session, r, page->entries, len);
	r->recno += page->entries;

	/* Write the page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_var_helper --
 *	Create a column-store variable length record cell and write it onto a
 * page.
 */
static int
__rec_col_var_helper(
    WT_SESSION_IMPL *session, WT_SALVAGE_COOKIE *salvage,
    WT_BUF *value, int deleted, int raw, uint64_t rle)
{
	WT_RECONCILE *r;
	WT_KV *val;

	r = session->btree->reconcile;
	val = &r->v;

	/*
	 * Occasionally, salvage needs to discard records from the beginning or
	 * end of the page, and because the items may be part of a RLE cell, do
	 * the adjustments here.   It's not a mistake we don't bother telling
	 * our caller we've handled all the records from the page we care about,
	 * and can quit processing the page: salvage is a rare operation and I
	 * don't want to complicate our caller's loop.
	 */
	if (salvage != NULL) {
		if (salvage->done)
			return (0);
		if (salvage->skip != 0) {
			if (rle <= salvage->skip) {
				salvage->skip -= rle;
				return (0);
			}
			salvage->skip = 0;
			rle -= salvage->skip;
		}
		if (salvage->take != 0) {
			if (rle <= salvage->take)
				salvage->take -= rle;
			else {
				rle = salvage->take;
				salvage->take = 0;
			}
			if (salvage->take == 0)
				salvage->done = 1;
		}
	}

	if (deleted) {
		val->cell_len =
		    __wt_cell_pack_type(&val->cell, WT_CELL_DEL, rle);
		val->buf.size = 0;
		val->len = val->cell_len;
	} else if (raw) {
		val->buf.data = value->data;
		val->buf.size = value->size;
		val->cell_len = 0;
		val->len = val->buf.size;
	} else
		WT_RET(__rec_cell_build_val(
		    session, value->data, value->size, rle));

	/* Boundary: split or write the page. */
	while (val->len > r->space_avail)
		WT_RET(__rec_split(session));

	/* Copy the value onto the page. */
	__rec_copy_incr(session, r, val);

	/* Update the starting record number in case we split. */
	r->recno += rle;

	return (0);
}

/*
 * __rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__rec_col_var(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_BUF *last, orig;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint64_t n, nrepeat, repeat_count, rle, slvg_missing, src_recno;
	uint32_t i, size;
	int can_compare, deleted, last_deleted, orig_deleted;
	const void *data;

	r = session->btree->reconcile;
	last = r->last;
	unpack = &_unpack;

	WT_CLEAR(orig);
	data = NULL;
	size = 0;

	WT_RET(__rec_split_init(session,
	    page, page->u.col_leaf.recno, session->btree->maxleafpage));

	/*
	 * The salvage code may be calling us to reconcile a page where there
	 * were missing records in the column-store name space.  In this case
	 * we write a single RLE element onto a new page, so we know it fits,
	 * then update the starting record number.
	 *
	 * Note that we DO NOT pass the salvage cookie to our helper function
	 * in this case, we're handling one of the salvage cookie fields on
	 * our own, and don't need assistance from the helper function.
	 */
	slvg_missing = salvage == NULL ? 0 : salvage->missing;
	if (slvg_missing)
		WT_RET(__rec_col_var_helper(
		    session, NULL, NULL, 1, 0, slvg_missing));

	/*
	 * We track two data items through this loop: the previous (last) item
	 * and the current item: if the last item is the same as the current
	 * item, we increment the RLE count for the last item; if the last item
	 * is different from the current item, we write the last item onto the
	 * page, and replace it with the current item.  The r->recno counter
	 * tracks records written to the page, and is incremented by the helper
	 * function immediately after writing records to the page.  The record
	 * number of our source record, that is, the current item, is maintained
	 * in src_recno.
	 */
	src_recno = r->recno;

	/* For each entry in the in-memory page... */
	rle = 0;
	can_compare = deleted = last_deleted = 0;
	WT_COL_FOREACH(page, cip, i) {
		/*
		 * Review the original cell, and get its repeat count and
		 * insert list.
		 */
		cell = WT_COL_PTR(page, cip);
		ins = WT_SKIP_FIRST(WT_COL_UPDATE(page, cip));
		if (cell == NULL) {
			nrepeat = 1;
			orig_deleted = 1;
		} else {
			__wt_cell_unpack(cell, unpack);

			/*
			 * The data may be Huffman encoded, which means we have
			 * to decode it in order to compare it with the last
			 * item we saw, which may have been an update string.
			 * This code guarantees we find every single pair of
			 * objects we can RLE encode, including the application
			 * inserting an update to an existing record where it
			 * happened (?) to match a Huffman-encoded value in the
			 * previous or next record.   However, we try to avoid
			 * copying in overflow records: if there's a WT_INSERT
			 * entry inserting a new record into a reference counted
			 * overflow record, then we have to write copies of the
			 * overflow record, and we do the comparisons.  But, we
			 * don't copy in the overflow record just to see if it
			 * matches records on either side.
			 */
			if (unpack->ovfl && ins == NULL) {
				/*
				 * Write out any record we're tracking and turn
				 * off comparisons for the next item.
				 */
				if (can_compare) {
					WT_RET(__rec_col_var_helper(
					    session, salvage,
					    last, last_deleted, 0, rle));
					can_compare = 0;
				}

				/* Write out the overflow cell as a raw cell. */
				last->data = cell;
				last->size = unpack->len;
				WT_RET(__rec_col_var_helper(
				    session, salvage, last, 0, 1, unpack->rle));
				src_recno += unpack->rle;
				continue;
			}

			nrepeat = unpack->rle;
			orig_deleted = unpack->type == WT_CELL_DEL ? 1 : 0;

			/* Get a copy of the cell. */
			if (!orig_deleted)
				WT_RET(__wt_cell_unpack_copy(
				    session, unpack, &orig));

			/*
			 * If we're re-writing a cell's reference of an overflow
			 * value, free the underlying file space.
			 *
			 * !!!
			 * We could optimize here by using the original overflow
			 * information for some set of the column values.  (For
			 * example, if column cells #10-17 reference overflow X,
			 * and cell #12 is updated with a new record: we could
			 * use the original overflow X for either cells #10-11
			 * or cells #13-17.)  We don't do that, instead we write
			 * new overflow records for both groups.  I'm skipping
			 * that work because I don't want the complexity, and
			 * overflow records should be rare.
			 */
			WT_RET(__rec_track_cell(session, page, unpack));
		}

		/*
		 * Generate on-page entries: loop repeat records, looking for
		 * WT_INSERT entries matching the record number.  The WT_INSERT
		 * lists are in sorted order, so only need check the next one.
		 */
		for (n = 0;
		    n < nrepeat; n += repeat_count, src_recno += repeat_count) {
			if (ins != NULL && WT_INSERT_RECNO(ins) == src_recno) {
				upd = ins->upd;
				ins = WT_SKIP_NEXT(ins);

				deleted = WT_UPDATE_DELETED_ISSET(upd);
				if (!deleted) {
					data = WT_UPDATE_DATA(upd);
					size = upd->size;
				}

				repeat_count = 1;
			} else {
				upd = NULL;
				deleted = orig_deleted;
				if (!deleted) {
					data = orig.data;
					size = orig.size;
				}

				/*
				 * The repeat count is the number of records up
				 * to the next WT_INSERT record, or up to the
				 * end of the entry if we have no more WT_INSERT
				 * records.
				 */
				if (ins == NULL)
					repeat_count = nrepeat - n;
				else
					repeat_count =
					    WT_INSERT_RECNO(ins) - src_recno;
			}

			/*
			 * Handle RLE accounting and comparisons.
			 *
			 * If we don't have a record against which to compare,
			 * save this record for the purpose and continue.
			 *
			 * If we have a record against which to compare, and
			 * the records compare equal, increment the rle counter
			 * and continue.  If the records don't compare equal,
			 * output the last record and swap the last and current
			 * buffers: do NOT update the starting record number,
			 * we've been doing that all along.
			 */
			if (can_compare) {
				if ((deleted && last_deleted) ||
				    (!last_deleted && !deleted &&
				    last->size == size &&
				    memcmp(last->data, data, size) == 0)) {
					rle += repeat_count;
					continue;
				}

				WT_RET(__rec_col_var_helper(session,
				    salvage, last, last_deleted, 0, rle));
			}

			/* Swap the current/last state. */
			if (!deleted)
				WT_RET(__wt_buf_set(session, last, data, size));
			last_deleted = deleted;

			/* Reset RLE counter and turn on comparisons. */
			rle = repeat_count;
			can_compare = 1;
		}
	}

	/* Walk any append list. */
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page))
		for (n = WT_INSERT_RECNO(ins); src_recno <= n; ++src_recno) {
			/*
			 * The application may have inserted records which left
			 * gaps in the name space.
			 */
			if (src_recno < n)
				deleted = 1;
			else {
				upd = ins->upd;
				deleted = WT_UPDATE_DELETED_ISSET(upd);
				if (!deleted) {
					data = WT_UPDATE_DATA(upd);
					size = upd->size;
				}
			}

			/*
			 * Handle RLE accounting and comparisons -- see comment
			 * above, this code fragment does the same thing.
			 */
			if (can_compare) {
				if ((deleted && last_deleted) ||
				    (!last_deleted && !deleted &&
				    last->size == size &&
				    memcmp(last->data, data, size) == 0)) {
					++rle;
					continue;
				}

				WT_RET(__rec_col_var_helper(session,
				    salvage, last, last_deleted, 0, rle));
			}

			/* Swap the current/last state. */
			if (!deleted)
				WT_RET(__wt_buf_set(session, last, data, size));
			last_deleted = deleted;

			/* Reset RLE counter and turn on comparisons. */
			rle = 1;
			can_compare = 1;
		}

	/* If we were tracking a record, write it. */
	if (can_compare)
		WT_RET(__rec_col_var_helper(
		    session, salvage, last, last_deleted, 0, rle));

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_var_bulk --
 *	Reconcile a bulk-loaded, variable-width column-store leaf page.
 */
static int
__rec_col_var_bulk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_KV *val;
	WT_RECONCILE *r;
	WT_UPDATE *upd, *next;
	uint64_t rle;

	r = session->btree->reconcile;
	val = &r->v;

	WT_RET(__rec_split_init(session,
	    page, page->u.bulk.recno, session->btree->maxleafpage));

	/* For each entry in the update list... */
	rle = 1;
	for (upd = page->u.bulk.upd; upd != NULL; upd = next) {
		next = upd->next;
		if (next != NULL && upd->size == next->size &&
		    memcmp(WT_UPDATE_DATA(upd),
		    WT_UPDATE_DATA(next), upd->size) == 0) {
			++rle;
			continue;
		}

		WT_RET(__rec_cell_build_val(
		    session, WT_UPDATE_DATA(upd), upd->size, rle));

		/* Boundary: split or write the page. */
		while (val->len > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the value onto the page. */
		__rec_copy_incr(session, r, val);

		/*
		 * Update the starting record number in case we split.
		 * Reset the RLE counter.
		 */
		r->recno += rle;
		rle = 1;
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_row_int --
 *	Reconcile a row-store internal page.
 */
static int
__rec_row_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;
	int ovfl_key;

	r = session->btree->reconcile;
	unpack = &_unpack;
	key = &r->k;
	val = &r->v;

	WT_RET(__rec_split_init(session,
	    page, 0ULL, session->btree->maxintlpage));

	/*
	 * Ideally, we'd never store the 0th key on row-store internal pages
	 * because it's never used during tree search and there's no reason
	 * to waste the space.  The problem is how we do splits: when we split,
	 * we've potentially picked out several "split points" in the buffer
	 * which is overflowing the maximum page size, and when the overflow
	 * happens, we go back and physically split the buffer, at those split
	 * points, into new pages.  It would be both difficult and expensive
	 * to re-process the 0th key at each split point to be an empty key,
	 * so we don't do that.  However, we are reconciling an internal page
	 * for whatever reason, and the 0th key is known to be useless.  We
	 * truncate the key to a single byte, instead of removing it entirely,
	 * it simplifies various things in other parts of the code (we don't
	 * have to special case transforming the page from its disk image to
	 * its in-memory version, for example).
	 */
	r->cell_zero = 1;

	/*
	 * The value cells all look the same -- we can set it up once and then
	 * just reset the addr/size pairs we're writing after the cell.
	 */
	val->cell_len =
	    __wt_cell_pack_type(&val->cell, WT_CELL_OFF, (uint64_t)0);
	val->buf.data = &val->off;
	val->buf.size = sizeof(WT_OFF);
	val->len = WT_STORE_SIZE(val->cell_len + sizeof(WT_OFF));

	/* For each entry in the in-memory page... */
	WT_ROW_REF_FOREACH(page, rref, i) {
		/*
		 * Keys are always instantiated for row-store internal pages,
		 * set the WT_IKEY reference.  We may have key overflow items
		 * on the page, in which case the disk image was retained and
		 * the key's WT_CELL reference was set.
		 */
		ikey = rref->key;
		if (ikey->cell_offset == 0)
			cell = NULL;
		else {
			cell = WT_REF_OFFSET(page, ikey->cell_offset);
			__wt_cell_unpack(cell, unpack);
		}

		/*
		 * The page may be deleted or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 *
		 * There's one special case we have to handle here: the internal
		 * page being merged has a potentially incorrect first key and
		 * we need to replace it with the one we have.  The problem is
		 * caused by the fact that the page search algorithm coerces the
		 * 0th key on any internal page to be smaller than any search
		 * key.  We do that because we don't want to have to update the
		 * internal pages every time a new "smallest" key is inserted
		 * into the tree.  But, if a new "smallest" key is inserted into
		 * our split-created subtree, and we don't update the internal
		 * page, when we merge that internal page into its parent page,
		 * the key may be incorrect.  Imagine the following tree:
		 *
		 *	2	5	40	internal page
		 *		|
		 * 	    10  | 20		split-created internal page
		 *	    |
		 *	    6			inserted smallest key
		 *
		 * after a simple merge, we'd have corruption:
		 *
		 *	2    10    20	40	merged internal page
		 *	     |
		 *	     6			key sorts before parent's key
		 *
		 * To fix this problem, we take the original page's key as our
		 * first key, because we know that key sorts before any possible
		 * key inserted into the subtree, and discard whatever 0th key
		 * is on the split-created internal page.
		 */
		if (WT_ROW_REF_STATE(rref) == WT_REF_DISK) {
			val->off.addr = WT_ROW_REF_ADDR(rref);
			val->off.size = WT_ROW_REF_SIZE(rref);
		} else {
			rp = WT_ROW_REF_PAGE(rref);
			switch (F_ISSET(rp, WT_PAGE_REC_MASK)) {
			case WT_PAGE_REC_EMPTY:
				/* Check overflow keys for deleted pages. */
				if (cell != NULL)
					WT_RET(__rec_track_cell(
					    session, page, unpack));
				continue;
			case WT_PAGE_REC_REPLACE:
				val->off.addr = rp->modify->u.write_off.addr;
				val->off.size = rp->modify->u.write_off.size;
				break;
			case WT_PAGE_REC_SPLIT:
				r->merge_ref = rref;

				WT_RET(__rec_row_merge(session,
				    rp->modify == NULL ?
				    rp : rp->modify->u.write_split));
				continue;
			default:
				val->off.addr = WT_ROW_REF_ADDR(rref);
				val->off.size = WT_ROW_REF_SIZE(rref);
				break;
			}
		}

		/*
		 * Build key cell.
		 *
		 * If the key is an overflow item, assume prefix compression
		 * won't make things better, and simply copy it.
		 *
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		if (cell != NULL) {
			key->buf.data = cell;
			key->buf.size = unpack->len;
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;
		} else
			WT_RET(__rec_cell_build_key(session,
			    WT_IKEY_DATA(ikey),
			    r->cell_zero ? 1 : ikey->size, 1, &ovfl_key));
		r->cell_zero = 0;

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, turn off compression (until a full key
		 * is written to the page), change to a non-prefix-compressed
		 * key.
		 */
		while (key->len + val->len > r->space_avail) {
			/*
			 * We have to have a copy of any overflow key because
			 * we're about to promote it.
			 */
			if (ovfl_key && cell != NULL)
				WT_RET(__wt_cell_copy(session, cell, r->cur));
			WT_RET(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_RET(__rec_cell_build_key(
				    session, NULL, 0, 1, &ovfl_key));
		}

		/* Copy the key onto the page. */
		__rec_copy_incr(session, r, key);

		/*
		 * Copy the off-page reference onto the page; any off-page
		 * reference must be a valid disk address.
		 */
		WT_ASSERT_RET(session, val->off.addr != WT_ADDR_INVALID);
		__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_row_merge --
 *	Recursively walk a row-store internal tree of merge pages.
 */
static int
__rec_row_merge(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;
	int ovfl_key;

	WT_BSTAT_INCR(session, rec_page_merge);

	r = session->btree->reconcile;
	key = &r->k;
	val = &r->v;

	/* For each entry in the in-memory page... */
	WT_ROW_REF_FOREACH(page, rref, i) {
		/*
		 * The page may be deleted or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 */
		if (WT_ROW_REF_STATE(rref) == WT_REF_DISK) {
			val->off.addr = WT_ROW_REF_ADDR(rref);
			val->off.size = WT_ROW_REF_SIZE(rref);
		} else {
			rp = WT_ROW_REF_PAGE(rref);
			switch (F_ISSET(rp, WT_PAGE_REC_MASK)) {
			case WT_PAGE_REC_EMPTY:
				continue;
			case WT_PAGE_REC_REPLACE:
				val->off.addr = rp->modify->u.write_off.addr;
				val->off.size = rp->modify->u.write_off.size;
				break;
			case WT_PAGE_REC_SPLIT:
				WT_RET(__rec_row_merge(session,
				    rp->modify == NULL ?
				    rp : rp->modify->u.write_split));
				continue;
			default:
				val->off.addr = WT_ROW_REF_ADDR(rref);
				val->off.size = WT_ROW_REF_SIZE(rref);
				break;
			}
		}

		/*
		 * Build the key cell.  If this is the first key in a "to be
		 * merged" subtree, use the merge correction key saved in the
		 * top-level parent page when this function was called.
		 *
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		ikey = r->merge_ref == NULL ? rref->key : r->merge_ref->key;
		r->merge_ref = NULL;
		WT_RET(__rec_cell_build_key(session, WT_IKEY_DATA(ikey),
		    r->cell_zero ? 1 : ikey->size, 1, &ovfl_key));
		r->cell_zero = 0;

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, turn off compression (until a full key
		 * is written to the page), change to a non-prefix-compressed
		 * key.
		 */
		while (key->len + val->len > r->space_avail) {
			WT_RET(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_RET(__rec_cell_build_key(
				    session, NULL, 0, 1, &ovfl_key));
		}

		/* Copy the key onto the page. */
		__rec_copy_incr(session, r, key);

		/*
		 * Copy the off-page reference onto the page; any off-page
		 * reference must be a valid disk address.
		 */
		WT_ASSERT_RET(session, val->off.addr != WT_ADDR_INVALID);
		__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	return (0);
}

/*
 * __rec_row_leaf --
 *	Reconcile a row-store leaf page.
 */
static int
__rec_row_leaf(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_BUF *tmp;
	WT_CELL *cell, *val_cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_KV *key, *val;
	WT_RECONCILE *r;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint64_t slvg_skip;
	uint32_t i;
	int ovfl_key, ret;

	r = session->btree->reconcile;
	tmp = NULL;
	unpack = &_unpack;
	slvg_skip = salvage == NULL ? 0 : salvage->skip;
	ret = 0;

	key = &r->k;
	val = &r->v;

	WT_RET(__rec_split_init(session,
	    page, 0ULL, session->btree->maxleafpage));

	/*
	 * Bulk-loaded pages are just an insert list and nothing more.  As
	 * row-store leaf pages already have to deal with insert lists, it's
	 * pretty easy to hack into that path.
	 */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD)) {
		WT_RET(__rec_row_leaf_insert(session, page->u.bulk.ins));
		return (__rec_split_finish(session));
	}

	/*
	 * Write any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT_SMALLEST(page))) != NULL)
		WT_RET(__rec_row_leaf_insert(session, ins));

	/*
	 * A temporary buffer in which to instantiate any uninstantiated keys.
	 * From this point on, we need to jump to the err label on error so the
	 * buffer is discarded.
	 */
	WT_RET(__wt_scr_alloc(session, 0, &tmp));

	/* For each entry in the page... */
	WT_ROW_FOREACH(page, rip, i) {
		/*
		 * The salvage code, on some rare occasions, wants to reconcile
		 * a page but skip some leading records on the page.  Because
		 * the row-store leaf reconciliation function copies keys from
		 * the original disk page, this is non-trivial -- just changing
		 * the in-memory pointers isn't sufficient, we have to change
		 * the WT_CELL structures on the disk page, too.  It's ugly, but
		 * we pass in a value that tells us how many records to skip in
		 * this case.
		 */
		if (slvg_skip != 0) {
			--slvg_skip;
			continue;
		}

		/*
		 * Set the WT_IKEY reference (if the key was instantiated), and
		 * the key cell reference.
		 */
		if (__wt_off_page(page, rip->key)) {
			ikey = rip->key;
			cell = WT_REF_OFFSET(page, ikey->cell_offset);
		} else {
			ikey = NULL;
			cell = rip->key;
		}
							/* Build value cell. */
		if ((val_cell = __wt_row_value(page, rip)) != NULL)
			__wt_cell_unpack(val_cell, unpack);
		if ((upd = WT_ROW_UPDATE(page, rip)) == NULL) {
			/*
			 * Copy the item off the page -- however, when the page
			 * was read into memory, there may not have been a value
			 * item, that is, it may have been zero length.
			 */
			if (val_cell == NULL)
				val->buf.size = 0;
			else {
				val->buf.data = val_cell;
				val->buf.size = unpack->len;
			}
			val->cell_len = 0;
			val->len = val->buf.size;
		} else {
			/*
			 * If we updated an overflow value, free the underlying
			 * file space.
			 */
			if (val_cell != NULL)
				WT_ERR(__rec_track_cell(session, page, unpack));

			/*
			 * If this key/value pair was deleted, we're done.  If
			 * we deleted an overflow key, free the underlying file
			 * space.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				__wt_cell_unpack(cell, unpack);
				WT_ERR(__rec_track_cell(session, page, unpack));
				goto leaf_insert;
			}

			/*
			 * If no value, nothing needs to be copied.  Otherwise,
			 * build the value's WT_CELL chunk from the most recent
			 * update value.
			 */
			if (upd->size == 0)
				val->cell_len = val->len = val->buf.size = 0;
			else
				WT_ERR(__rec_cell_build_val(
				    session, WT_UPDATE_DATA(upd),
				    upd->size, (uint64_t)0));
		}

		/*
		 * Build key cell.
		 *
		 * If the key is an overflow item, assume prefix compression
		 * won't make things better, and simply copy it.
		 */
		__wt_cell_unpack(cell, unpack);
		if (unpack->type == WT_CELL_KEY_OVFL) {
			key->buf.data = cell;
			key->buf.size = unpack->len;
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;
		} else if (ikey != NULL)
			WT_ERR(__rec_cell_build_key(session,
			    WT_IKEY_DATA(ikey), ikey->size, 0, &ovfl_key));
		else {
			WT_ERR(__wt_row_key(session, page, rip, tmp));
			WT_ERR(__rec_cell_build_key(
			    session, tmp->data, tmp->size, 0, &ovfl_key));
		}

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, switch to the non-prefix-compressed key
		 * and turn off compression until a full key is written to the
		 * new page.
		 *
		 * We write a trailing key cell on the page after the K/V pairs
		 * (see WT_TRAILING_KEY_CELL for more information).
		 */
		while (key->len +
		    val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
			/*
			 * We have to have a copy of any overflow key because
			 * we're about to promote it.
			 */
			if (ovfl_key && unpack->type == WT_CELL_KEY_OVFL)
				WT_RET(__wt_cell_unpack_copy(
				    session, unpack, r->cur));
			WT_ERR(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_ERR(__rec_cell_build_key(
				    session, NULL, 0, 0, &ovfl_key));
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len != 0)
			__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);

leaf_insert:	/* Write any K/V pairs inserted into the page after this key. */
		if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT(page, rip))) != NULL)
			WT_ERR(__rec_row_leaf_insert(session, ins));
	}

	/* Write the remnant page. */
	ret = __rec_split_finish(session);

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __rec_row_leaf_insert --
 *	Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	WT_KV *key, *val;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	int ovfl_key;

	r = session->btree->reconcile;
	key = &r->k;
	val = &r->v;

	for (; ins != NULL; ins = WT_SKIP_NEXT(ins)) {
		upd = ins->upd;				/* Build value cell. */
		if (WT_UPDATE_DELETED_ISSET(upd))
			continue;
		if (upd->size == 0)
			val->len = 0;
		else
			WT_RET(__rec_cell_build_val(session,
			    WT_UPDATE_DATA(upd), upd->size, (uint64_t)0));

		WT_RET(__rec_cell_build_key(session,	/* Build key cell. */
		    WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), 0, &ovfl_key));

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, switch to the non-prefix-compressed key
		 * and turn off compression until a full key is written to the
		 * new page.
		 *
		 * We write a trailing key cell on the page after the K/V pairs
		 * (see WT_TRAILING_KEY_CELL for more information).
		 */
		while (key->len +
		    val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
			WT_RET(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_RET(__rec_cell_build_key(
				    session, NULL, 0, 0, &ovfl_key));
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len != 0)
			__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	return (0);
}

/*
 * __rec_write_wrapup  --
 *	Finish the reconciliation.
 */
static int
__rec_write_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_MODIFY *mod;
	WT_RECONCILE *r;
	uint32_t i;
	int ret;

	r = session->btree->reconcile;
	mod = page->modify;
	ret = 0;

	if (F_ISSET(page, WT_PAGE_FORCE_EVICT))
		__wt_evict_force_clear(session, page);

	/*
	 * This page may have previously been reconciled, and that information
	 * is now about to be replaced.  Make sure it's discarded at some point,
	 * and clear the underlying modification information, we're creating a
	 * new reality.
	 */
	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page deleted */
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		WT_RET(mod->u.write_split->type == WT_PAGE_ROW_INT ?
		    __rec_track_restart_row(session, page) :
		    __rec_track_restart_col(session, page));
		__wt_page_out(session, mod->u.write_split, 0);
		break;
	case WT_PAGE_REC_REPLACE:			/* 1-for-1 page swap */
		WT_RET(__wt_rec_track(session, page, WT_PT_BLOCK,
		    NULL, mod->u.write_off.addr, mod->u.write_off.size));
		break;
	default:
		break;
	}
	F_CLR(page, WT_PAGE_REC_MASK);

	switch (r->bnd_next) {
	case 0:						/* Page delete */
		WT_VERBOSE(session, reconcile, "page %p empty", page);

		WT_BSTAT_INCR(session, rec_page_delete);

		/*
		 * If the page was empty, we want to discard it from the tree
		 * by discarding the parent's key when evicting the parent.
		 * Mark the page as deleted, then return success, leaving the
		 * page in memory.  If the page is subsequently modified, that
		 * is OK, we'll just reconcile it again.
		 */
		F_SET(page, WT_PAGE_REC_EMPTY);
		break;
	case 1:						/* 1-for-1 page swap */
		/*
		 * Because WiredTiger's pages grow without splitting, we're
		 * replacing a single page with another single page most of
		 * the time.
		 */
		bnd = &r->bnd[0];
		WT_VERBOSE(session, reconcile,
		    "page %p replace to %" PRIu32 ", (%" PRIu32
		    " %s %" PRIu32 ")",
		    page, bnd->off.addr, WT_PSIZE(page),
		    WT_PSIZE(page) == bnd->off.size ? "==" :
		    (WT_PSIZE(page) < bnd->off.size ? ">>" : "<<"),
		    bnd->off.size);

		mod->u.write_off = bnd->off;
		F_SET(page, WT_PAGE_REC_REPLACE);
		break;
	default:					/* Page split */
		WT_VERBOSE(session, reconcile,
		    "page %p split into %" PRIu32 " pages",
		    page, r->bnd_next);

		switch (page->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			WT_BSTAT_INCR(session, rec_split_intl);
			break;
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			WT_BSTAT_INCR(session, rec_split_leaf);
			break;
		WT_ILLEGAL_FORMAT(session);
		}

#ifdef HAVE_VERBOSE
		if (WT_VERBOSE_ISSET(session, reconcile)) {
			WT_BUF *tkey;
			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_ROW_LEAF)
				WT_RET(__wt_scr_alloc(session, 0, &tkey));
			for (bnd = r->bnd, i = 0; i < r->bnd_next; ++bnd, ++i)
				switch (page->type) {
				case WT_PAGE_ROW_INT:
				case WT_PAGE_ROW_LEAF:
					WT_ERR(__wt_buf_set_printable(
					    session, tkey,
					    bnd->key.data, bnd->key.size));
					WT_VERBOSE(session, reconcile,
					    "split: starting key "
					    "%.*s",
					    (int)tkey->size,
					    (char *)tkey->data);
					break;
				case WT_PAGE_COL_FIX:
				case WT_PAGE_COL_INT:
				case WT_PAGE_COL_VAR:
					WT_VERBOSE(session, reconcile,
					    "split: starting recno %" PRIu64,
					    bnd->recno);
					break;
				WT_ILLEGAL_FORMAT(session);
				}
err:			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_ROW_LEAF)
				__wt_scr_free(&tkey);
		}
#endif
		switch (page->type) {
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			WT_RET(__rec_split_row(
			    session, page, &mod->u.write_split));
			break;
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			WT_RET(__rec_split_col(
			    session, page, &mod->u.write_split));
			break;
		WT_ILLEGAL_FORMAT(session);
		}

		F_SET(page, WT_PAGE_REC_SPLIT);
		break;
	}

	return (ret);
}

/*
 * __rec_cell_build_key --
 *	Process a key and return a WT_CELL structure and byte string to be
 * stored on the page.
 */
static int
__rec_cell_build_key(WT_SESSION_IMPL *session,
    const void *data, uint32_t size, int is_internal, int *is_ovflp)
{
	WT_BTREE *btree;
	WT_KV *key;
	WT_RECONCILE *r;
	uint32_t pfx_max;
	uint8_t pfx;
	const uint8_t *a, *b;

	r = session->btree->reconcile;
	btree = session->btree;
	key = &r->k;
	*is_ovflp = 0;

	pfx = 0;
	if (data == NULL)
		/*
		 * When data is NULL, our caller has a prefix compressed key
		 * they can't use (probably because they just crossed a split
		 * point).  Use the full key saved when last called, instead.
		 */
		WT_RET(__wt_buf_set(
		    session, &key->buf, r->cur->data, r->cur->size));
	else {
		/*
		 * Save a copy of the key for later reference: we use the full
		 * key for prefix-compression comparisons, and if we are, for
		 * any reason, unable to use the compressed key we generate.
		 */
		WT_RET(__wt_buf_set(session, r->cur, data, size));

		/*
		 * Do prefix compression on the key.  We know by definition the
		 * previous key sorts before the current key, which means the
		 * keys must differ and we just need to compare up to the
		 * shorter of the two keys.   Also, we can't compress out more
		 * than 256 bytes, limit the comparison to that.
		 */
		if (r->key_pfx_compress) {
			pfx_max = UINT8_MAX;
			if (size < pfx_max)
				pfx_max = size;
			if (r->last->size < pfx_max)
				pfx_max = r->last->size;
			for (a = data, b = r->last->data; pfx < pfx_max; ++pfx)
				if (*a++ != *b++)
					break;
		}

		/* Copy the non-prefix bytes into the key buffer. */
		WT_RET(__wt_buf_set(
		    session, &key->buf, (uint8_t *)data + pfx, size - pfx));
	}

	/* Optionally compress the value using the Huffman engine. */
	if (btree->huffman_key != NULL)
		WT_RET(__wt_huffman_encode(session, btree->huffman_key,
		    key->buf.data, key->buf.size, &key->buf));

	/* Create an overflow object if the data won't fit. */
	if (key->buf.size >
	    (is_internal ? btree->maxintlitem : btree->maxleafitem)) {
		WT_BSTAT_INCR(session, rec_ovfl_key);

		/*
		 * Overflow objects aren't prefix compressed -- rebuild any
		 * object that was prefix compressed.
		 *
		 * !!!
		 * Currently, we rewrite the key overflow values every damned
		 * time -- change NULL to "data" if you want to take another
		 * behavior out for a spin.
		 */
		if (pfx == 0) {
			*is_ovflp = 1;
			return (__rec_cell_build_ovfl(session,
			    NULL, key, WT_CELL_KEY_OVFL, (uint64_t)0));
		}
		return (__rec_cell_build_key(
		    session, NULL, 0, is_internal, is_ovflp));
	}

	key->cell_len = __wt_cell_pack_key(&key->cell, pfx, key->buf.size);
	key->len = key->cell_len + key->buf.size;

	return (0);
}

/*
 * __rec_cell_build_val --
 *	Process a data item and return a WT_CELL structure and byte string to
 * be stored on the page.
 */
static int
__rec_cell_build_val(
    WT_SESSION_IMPL *session, const void *data, uint32_t size, uint64_t rle)
{
	WT_BTREE *btree;
	WT_KV *val;
	WT_RECONCILE *r;

	r = session->btree->reconcile;
	btree = session->btree;
	val = &r->v;

	/*
	 * We don't copy the data into the buffer, it's not necessary; just
	 * re-point the buffer's data/length fields.
	 */
	val->buf.data = data;
	val->buf.size = size;

	/* Handle zero-length cells quickly. */
	if (size != 0) {
		/* Optionally compress the data using the Huffman engine. */
		if (btree->huffman_value != NULL)
			WT_RET(__wt_huffman_encode(
			    session, btree->huffman_value,
			    val->buf.data, val->buf.size, &val->buf));

		/* Create an overflow object if the data won't fit. */
		if (val->buf.size > btree->maxleafitem) {
			WT_BSTAT_INCR(session, rec_ovfl_value);

			return (__rec_cell_build_ovfl(session,
			    data, val, WT_CELL_VALUE_OVFL, rle));
		}
	}
	val->cell_len = __wt_cell_pack_data(&val->cell, rle, val->buf.size);
	val->len = val->cell_len + val->buf.size;

	return (0);
}

/*
 * __rec_cell_build_ovfl --
 *	Store bulk-loaded overflow items in the file, returning the WT_OFF.
 */
static int
__rec_cell_build_ovfl(WT_SESSION_IMPL *session,
    const void *orig_data, WT_KV *kv, uint8_t type, uint64_t rle)
{
	WT_BUF *tmp;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t size;
	int ret;

	r = session->btree->reconcile;
	page = r->page;
	tmp = NULL;
	ret = 0;

	/*
	 * Check to see if this overflow record has already been written and
	 * reuse it if possible.
	 *
	 * !!!
	 * The error that scares me a non-unique value for "orig_data", that's
	 * going to fail badly.  Assert that's not the case.
	 */
	if (__rec_track_ovfl_active(session,
	    page, orig_data, &kv->off.addr, &kv->off.size))
		goto reuse;

	/*
	 * Allocate a scratch buffer big enough to hold the overflow chunk.
	 *
	 * Initialize the disk header and overflow chunk, write it.
	 */
	size = WT_DISK_REQUIRED(session, kv->buf.size);
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	dsk = tmp->mem;
	memset(dsk, 0, WT_PAGE_DISK_SIZE);
	dsk->type = WT_PAGE_OVFL;
	dsk->u.datalen = kv->buf.size;
	memcpy(WT_PAGE_DISK_BYTE(dsk), kv->buf.data, kv->buf.size);
	tmp->size = WT_PAGE_DISK_SIZE + kv->buf.size;
	WT_ERR(__wt_block_write(session, tmp, &kv->off.addr, &kv->off.size));

	/* Track the new overflow item. */
	WT_RET(__wt_rec_track(session, page,
	    WT_PT_OVFL, orig_data, kv->off.addr, kv->off.size));

reuse:	/* Set the callers K/V to reference the WT_OFF structure. */
	kv->buf.data = &kv->off;
	kv->buf.size = sizeof(kv->off);
	kv->cell_len = __wt_cell_pack_type(&kv->cell, type, rle);
	kv->len = kv->cell_len + kv->buf.size;

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __rec_split_row --
 *	Update a row-store parent page's reference when a page is split.
 */
static int
__rec_split_row(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_PAGE **splitp)
{
	WT_BOUNDARY *bnd;
	WT_PAGE *page;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;
	int ret;

	r = session->btree->reconcile;
	ret = 0;

	/* Allocate a row-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(
	    session, (size_t)r->bnd_next, &page->u.row_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->entries = r->bnd_next;
	page->type = WT_PAGE_ROW_INT;

	/*
	 * Newly created internal pages are not persistent as we don't want the
	 * tree to deepen whenever a leaf page splits.  Flag the page for merge
	 * into its parent when the parent is reconciled.   We set the flag on
	 * the original page (so that future reconciliations of its parent see
	 * and merge the split pages), and on the newly created split page (so
	 * that after eviction, when the split page replaces the original page,
	 * its parents see and merge the split pages).  As they say, if it's not
	 * confusing, you don't understand it.
	 */
	F_SET(page, WT_PAGE_REC_SPLIT);

	/* Enter each split page into the new, internal page. */
	for (rref = page->u.row_int.t,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++rref, ++bnd, ++i) {
		WT_ERR(__wt_row_ikey_alloc(session, 0,
		    bnd->key.data, bnd->key.size, (WT_IKEY **)&rref->key));
		WT_ROW_REF_ADDR(rref) = bnd->off.addr;
		WT_ROW_REF_SIZE(rref) = bnd->off.size;

		WT_ROW_REF_PAGE(rref) = NULL;
		WT_ROW_REF_STATE(rref) = WT_REF_DISK;
	}

	*splitp = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}

/*
 * __rec_split_col --
 *	Update a column-store parent page's reference when a page is split.
 */
static int
__rec_split_col(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_PAGE **splitp)
{
	WT_BOUNDARY *bnd;
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_RECONCILE *r;
	uint32_t i;
	int ret;

	r = session->btree->reconcile;
	ret = 0;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(
	    session, (size_t)r->bnd_next, &page->u.col_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->u.col_int.recno = r->bnd[0].recno;
	page->entries = r->bnd_next;
	page->type = WT_PAGE_COL_INT;

	/*
	 * Newly created internal pages are not persistent as we don't want the
	 * tree to deepen whenever a leaf page splits.  Flag the page for merge
	 * into its parent when the parent is reconciled.   We set the flag on
	 * the original page (so that future reconciliations of its parent see
	 * and merge the split pages), and on the newly created split page (so
	 * that after eviction, when the split page replaces the original page,
	 * its parents see and merge the split pages).  As they say, if it's not
	 * confusing, you don't understand it.
	 */
	F_SET(page, WT_PAGE_REC_SPLIT);

	/* Enter each split page into the new, internal page. */
	for (cref = page->u.col_int.t,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++cref, ++bnd, ++i) {
		cref->recno = bnd->recno;
		WT_COL_REF_ADDR(cref) = bnd->off.addr;
		WT_COL_REF_SIZE(cref) = bnd->off.size;

		WT_COL_REF_PAGE(cref) = NULL;
		WT_COL_REF_STATE(cref) = WT_REF_DISK;
	}

	*splitp = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}

/*
 * __wt_rec_track --
 *	Add a page or addr/size pair to the page's list of tracked objects.
 */
int
__wt_rec_track(WT_SESSION_IMPL *session, WT_PAGE *page,
    __wt_pt_type_t type, const void *ref, uint32_t addr, uint32_t size)
{
	WT_PAGE_MODIFY *mod;
	size_t bytes_allocated;

	mod = page->modify;

	if (mod->track_next == mod->track_entries) {
		/*
		 * The __wt_realloc() function uses the "bytes allocated" value
		 * to figure out how much of the memory it needs to clear (see
		 * the function for an explanation of why the memory is cleared,
		 * it's a security thing).  We can calculate the bytes allocated
		 * so far, which saves a size_t in the WT_PAGE_MODIFY structure.
		 * That's worth a little dance, we have one of them per modified
		 * page.
		 */
		bytes_allocated = mod->track_entries * sizeof(*mod->track);
		WT_RET(__wt_realloc(session, &bytes_allocated,
		    (mod->track_entries + 20) * sizeof(*mod->track),
		    &mod->track));
		mod->track_entries += 20;
	}
	mod->track[mod->track_next].type = type;
	mod->track[mod->track_next].ref = ref;
	mod->track[mod->track_next].addr = addr;
	mod->track[mod->track_next].size = size;
#ifdef HAVE_VERBOSE
	__rec_track_verbose(session, page, &mod->track[mod->track_next]);
#endif
	++mod->track_next;
	return (0);
}

#ifdef HAVE_VERBOSE
/*
 * __rec_track_verbose --
 *	Display an entry being tracked.
 */
static void
__rec_track_verbose(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_TRACK *track)
{
	const char *onoff;

	switch (track->type) {
	case WT_PT_BLOCK:
		WT_VERBOSE(session, reconcile,
		    "page %p tracking block (%" PRIu32 "/%" PRIu32 ")",
		    page, track->addr, track->size);
		return;
	case WT_PT_OVFL:
		onoff = "ON";
		break;
	case WT_PT_OVFL_DISCARD:
		onoff = "OFF";
		break;
	case WT_PT_EMPTY:			/* Odd, but possible. */
	default:				/* Not possible. */
		return;
	}
	WT_VERBOSE(session, reconcile,
	    "page %p tracking overflow %s (%p, %" PRIu32 "/%" PRIu32 ")",
	    page, onoff, track->ref, track->addr, track->size);
}
#endif

/*
 * __rec_track_cell --
 *	If the cell references an overflow chunk, add it to the page's list.
 */
static int
__rec_track_cell(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack)
{
	return (unpack->ovfl ? __wt_rec_track(session, page,
	    WT_PT_BLOCK, NULL, unpack->off.addr, unpack->off.size) : 0);
}

/*
 * __rec_track_ovfl_active --
 *	Search for an overflow record and reactivate it.
 */
static int
__rec_track_ovfl_active(WT_SESSION_IMPL *session,
    WT_PAGE *page, const void *orig_data, uint32_t *addrp, uint32_t *sizep)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	WT_PAGE_MODIFY *mod;

	/*
	 * XXX
	 * We don't currently track overflow keys.
	 */
	if (orig_data == NULL)
		return (0);

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_next; ++track, ++i)
		if ((track->type == WT_PT_OVFL ||
		    track->type == WT_PT_OVFL_DISCARD) &&
		    track->ref == orig_data) {
			/*
			 * We better not see more than a single request for any
			 * specific overflow item, that would be a coding error.
			 */
			WT_ASSERT(session, track->type != WT_PT_OVFL);

			*addrp = track->addr;
			*sizep = track->size;
			track->type = WT_PT_OVFL;
#ifdef HAVE_VERBOSE
			__rec_track_verbose(session, page, track);
#endif
			return (1);
		}
	return (0);
}

/*
 * __rec_track_restart_ovfl --
 *	Cleanup the tracking information each time we write a page.
 */
static void
__rec_track_restart_ovfl(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;

	/*
	 * Mark all overflow references "discarded" at the start of a write,
	 * we'll reactivate ones we are using again as we process the page.
	 */
	for (track = mod->track, i = 0; i < mod->track_next; ++track, ++i) {
		if (track->type == WT_PT_OVFL)
			track->type = WT_PT_OVFL_DISCARD;
#ifdef HAVE_VERBOSE
		__rec_track_verbose(session, page, track);
#endif
	}
}

/*
 * __rec_track_restart_row --
 *	Cleanup the tracking information from a row-store split page.
 */
static int
__rec_track_restart_row(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_ROW_REF *rref;
	uint32_t i;

	WT_ROW_REF_FOREACH(page->modify->u.write_split, rref, i)
		WT_RET(__wt_rec_track(
		    session, page, WT_PT_BLOCK, NULL,
		    WT_ROW_REF_ADDR(rref), WT_ROW_REF_SIZE(rref)));
	return (0);
}

/*
 * __rec_track_restart_col --
 *	Cleanup the tracking information from a column-store split page.
 */
static int
__rec_track_restart_col(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	uint32_t i;

	WT_COL_REF_FOREACH(page->modify->u.write_split, cref, i)
		WT_RET(__wt_rec_track(
		    session, page, WT_PT_BLOCK, NULL,
		    WT_COL_REF_ADDR(cref), WT_COL_REF_SIZE(cref)));
	return (0);
}

/*
 * __wt_rec_discard_track --
 *	Process the page's list of tracked objects.
 */
int
__wt_rec_discard_track(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	for (track = page->modify->track,
	    i = 0; i < page->modify->track_next; ++track, ++i) {
		switch (track->type) {
		case WT_PT_EMPTY:
			continue;
		case WT_PT_BLOCK:
			WT_VERBOSE(session, reconcile,
			    "page %p discard block %" PRIu32 "/%" PRIu32,
			    page, track->addr, track->size);
			break;
		case WT_PT_OVFL_DISCARD:
			WT_VERBOSE(session, reconcile,
			    "page %p discard overflow %" PRIu32 "/%" PRIu32,
			    page, track->addr, track->size);
			break;
		case WT_PT_OVFL:
			__rec_track_verbose(session, page, track);
			continue;
		}
		WT_RET(__wt_block_free(session, track->addr, track->size));

		track->type = WT_PT_EMPTY;
		track->ref = NULL;
		track->addr = WT_ADDR_INVALID;
		track->size = 0;
	}
	return (0);
}
