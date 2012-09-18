/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_dhandle_open_lock --
 *	Spin on the current data handle until either (a) it is open, read
 *	locked; or (b) it is closed, write locked.  If exclusive access is
 *	requested and cannot be granted immediately, fail with EBUSY.
 */
static int
__conn_dhandle_open_lock(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, uint32_t flags)
{
	/*
	 * Check that the handle is open.  We've already incremented
	 * the reference count, so once the handle is open it won't be
	 * closed by another thread.
	 *
	 * If we can see the WT_DHANDLE_OPEN flag set while holding a
	 * lock on the handle, then it's really open and we can start
	 * using it.  Alternatively, if we can get an exclusive lock
	 * and WT_DHANDLE_OPEN is still not set, we need to do the open.
	 */
	for (;;) {
		if (!LF_ISSET(WT_DHANDLE_EXCLUSIVE) &&
		    F_ISSET((WT_BTREE *)dhandle, WT_BTREE_SPECIAL_FLAGS))
			return (EBUSY);

		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    !LF_ISSET(WT_DHANDLE_EXCLUSIVE)) {
			__wt_readlock(session, dhandle->rwlock);
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
				return (0);
			__wt_rwunlock(session, dhandle->rwlock);
		}

		/*
		 * It isn't open or we want it exclusive: try to get an
		 * exclusive lock.  There is some subtlety here: if we race
		 * with another thread that successfully opens the file, we
		 * don't want to block waiting to get exclusive access.
		 */
		if (__wt_try_writelock(session, dhandle->rwlock) == 0) {
			/*
			 * If it was opened while we waited, drop the write
			 * lock and get a read lock instead.
			 */
			if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
			    !LF_ISSET(WT_DHANDLE_EXCLUSIVE)) {
				__wt_rwunlock(session, dhandle->rwlock);
				continue;
			}

			/* We have an exclusive lock, we're done. */
			F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
			return (0);
		} else if (LF_ISSET(WT_DHANDLE_EXCLUSIVE))
			return (EBUSY);

		/* Give other threads a chance to make progress. */
		__wt_yield();
	}
}

/*
 * __conn_dhandle_get --
 *	Find an open btree file handle, otherwise create a new one and link it
 *	into the connection's list.  If successful, it returns with either
 *	(a) an open handle, read locked (if WT_DHANDLE_EXCLUSIVE is set); or
 *	(b) an open handle, write locked (if WT_DHANDLE_EXCLUSIVE is set), or
 *	(c) a closed handle, write locked.
 */
static int
__conn_dhandle_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int matched;

	conn = S2C(session);

	/* We must be holding the schema lock at a higher level. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/* Increment the reference count if we already have the btree open. */
	matched = 0;
	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (strcmp(name, dhandle->name) == 0 &&
		    ((ckpt == NULL && dhandle->checkpoint == NULL) ||
		    (ckpt != NULL && dhandle->checkpoint != NULL &&
		    strcmp(ckpt, dhandle->checkpoint) == 0))) {
			++dhandle->refcnt;
			session->btree = (WT_BTREE *)dhandle;
			matched = 1;
			break;
		}
	}
	if (matched)
		return (__conn_dhandle_open_lock(session, dhandle, flags));

	/*
	 * Allocate the WT_BTREE structure, its lock, and set the name so we
	 * can put the handle into the list.
	 */
	btree = NULL;
	WT_RET(__wt_calloc_def(session, 1, &btree));
	dhandle = &btree->dhandle;
	if ((ret = __wt_rwlock_alloc(
		session, "btree handle", &dhandle->rwlock)) == 0 &&
	    (ret = __wt_strdup(session, name, &dhandle->name)) == 0 &&
	    (ckpt == NULL ||
	    (ret = __wt_strdup(session, ckpt, &dhandle->checkpoint)) == 0)) {
		/* Lock the handle before it is inserted in the list. */
		__wt_writelock(session, dhandle->rwlock);
		F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);

		/* Add to the connection list. */
		dhandle->refcnt = 1;
		TAILQ_INSERT_TAIL(&conn->dhqh, dhandle, q);
		++conn->btqcnt;
	}

	if (ret == 0)
		session->btree = btree;
	else {
		if (dhandle->rwlock != NULL)
			__wt_rwlock_destroy(session, &dhandle->rwlock);
		__wt_free(session, dhandle->name);
		__wt_free(session, dhandle->checkpoint);
		__wt_overwrite_and_free(session, btree);
	}

	return (ret);
}

/*
 * __wt_conn_btree_sync_and_close --
 *	Sync and close the underlying btree handle.
 */
int
__wt_conn_btree_sync_and_close(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	btree = session->btree;
	dhandle = &btree->dhandle;

	if (!F_ISSET(dhandle, WT_DHANDLE_OPEN))
		return (0);

	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		ret = __wt_checkpoint(session, NULL);

	WT_TRET(__wt_btree_close(session));
	F_CLR(dhandle, WT_DHANDLE_OPEN);
	F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);

	return (ret);
}

/*
 * __conn_btree_open --
 *	Open the current btree handle.
 */
static int
__conn_btree_open(WT_SESSION_IMPL *session,
    const char *config, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_ITEM(addr);
	WT_DECL_RET;

	btree = session->btree;
	dhandle = &btree->dhandle;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) &&
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) &&
	    !LF_ISSET(WT_DHANDLE_LOCK_ONLY));

	/* Open the underlying file, free any old config. */
	__wt_free(session, dhandle->config);
	dhandle->config = config;

	/*
	 * If the handle is already open, it has to be closed so it can be
	 * reopened with a new configuration.  We don't need to check again:
	 * this function isn't called if the handle is already open in the
	 * required mode.
	 */
	if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
		WT_RET(__wt_conn_btree_sync_and_close(session));

	WT_RET(__wt_scr_alloc(session, WT_BTREE_MAX_ADDR_COOKIE, &addr));

	/* Set any special flags on the handle. */
	F_SET(btree, LF_ISSET(WT_BTREE_SPECIAL_FLAGS));

	do {
		WT_ERR(__wt_meta_checkpoint_addr(
		    session, dhandle->name, dhandle->checkpoint, addr));
		WT_ERR(__wt_btree_open(session, addr->data, addr->size, cfg,
		    dhandle->checkpoint == NULL ? 0 : 1));
		F_SET(dhandle, WT_DHANDLE_OPEN);

		/* Drop back to a readlock if that is all that was needed. */
		if (!LF_ISSET(WT_DHANDLE_EXCLUSIVE)) {
			F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
			__wt_rwunlock(session, dhandle->rwlock);
			WT_ERR(
			    __conn_dhandle_open_lock(session, dhandle, flags));
		}
	} while (!F_ISSET(dhandle, WT_DHANDLE_OPEN));

	if (0) {
err:		(void)__wt_conn_btree_close(session, 1);
	}

	__wt_scr_free(&addr);
	return (ret);
}

/*
 * __wt_conn_btree_get --
 *	Get an open btree file handle, otherwise open a new one.
 */
int
__wt_conn_btree_get(WT_SESSION_IMPL *session,
    const char *name, const char *ckpt, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const char *treeconf;

	conn = S2C(session);

	WT_STAT_INCR(conn->stats, file_open);

	WT_RET(__conn_dhandle_get(session, name, ckpt, flags));
	btree = session->btree;
	dhandle = &btree->dhandle;

	if (!LF_ISSET(WT_DHANDLE_LOCK_ONLY) &&
	    (!F_ISSET(dhandle, WT_DHANDLE_OPEN) ||
	    LF_ISSET(WT_BTREE_SPECIAL_FLAGS))) {
		if ((ret = __wt_metadata_read(session, name, &treeconf)) != 0) {
			if (ret == WT_NOTFOUND)
				ret = ENOENT;
			goto err;
		}
		ret = __conn_btree_open(session, treeconf, cfg, flags);
	}

err:	if (ret != 0) {
		F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
		__wt_rwunlock(session, dhandle->rwlock);
	}

	WT_ASSERT(session, ret != 0 ||
	    LF_ISSET(WT_DHANDLE_EXCLUSIVE) ==
	    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));

	return (ret);
}

/*
 * __wt_conn_btree_apply --
 *	Apply a function to all open btree handles apart from the metadata
 * file.
 */
int
__wt_conn_btree_apply(WT_SESSION_IMPL *session,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_BTREE *btree, *saved_btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	conn = S2C(session);
	saved_btree = session->btree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	TAILQ_FOREACH(dhandle, &conn->dhqh, q)
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    !F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) &&
		    WT_PREFIX_MATCH(dhandle->name, "file:")) {
			btree = (WT_BTREE *)dhandle;
			if (WT_IS_METADATA(btree))
				continue;
			/*
			 * We have the connection spinlock, which prevents
			 * handles being opened or closed, so there is no need
			 * for additional handle locking here, or pulling every
			 * tree into this session's handle cache.
			 */
			session->btree = btree;
			WT_ERR(func(session, cfg));
		}

err:	session->btree = saved_btree;
	return (ret);
}

/*
 * __wt_conn_btree_close --
 *	Discard a reference to an open btree file handle.
 */
int
__wt_conn_btree_close(WT_SESSION_IMPL *session, int locked)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	int inuse;

	btree = session->btree;
	dhandle = &btree->dhandle;
	conn = S2C(session);

	if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
		WT_STAT_DECR(conn->stats, file_open);

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Decrement the reference count.  If we really are the last reference,
	 * get an exclusive lock on the handle so that we can close it.
	 */
	inuse = --dhandle->refcnt > 0;
	if (!inuse && !locked) {
		__wt_writelock(session, dhandle->rwlock);
		F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
	}

	if (!inuse) {
		/*
		 * We should only close the metadata file when closing the
		 * last session (i.e., the default session for the connection).
		 */
		WT_ASSERT(session,
		    btree != session->metafile ||
		    session == conn->default_session);

		if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
			WT_TRET(__wt_conn_btree_sync_and_close(session));
		if (!locked) {
			F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
			__wt_rwunlock(session, dhandle->rwlock);
		}
	}

	return (ret);
}

/*
 * __wt_conn_dhandle_close_all --
 *	Close all data handles handles with matching name (including all
 *	checkpoint handles).
 */
int
__wt_conn_dhandle_close_all(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE *btree, *saved_btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	conn = S2C(session);
	saved_btree = session->btree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (strcmp(dhandle->name, name) != 0)
			continue;
		btree = (WT_BTREE *)dhandle;

		/*
		 * The caller may have this tree locked to prevent
		 * concurrent schema operations.
		 */
		if (btree == saved_btree)
			WT_ASSERT(session,
			    F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));
		else {
			WT_ERR(__wt_try_writelock(session, dhandle->rwlock));
			F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
		}

		session->btree = btree;
		if (WT_META_TRACKING(session))
			WT_ERR(__wt_meta_track_handle_lock(session, 0));

		/*
		 * We have an exclusive lock, which means there are no
		 * cursors open at this point.  Close the handle, if
		 * necessary.
		 */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
			ret = __wt_meta_track_sub_on(session);
			if (ret == 0)
				ret = __wt_conn_btree_sync_and_close(session);

			/*
			 * If the close succeeded, drop any locks it
			 * acquired.  If there was a failure, this
			 * function will fail and the whole transaction
			 * will be rolled back.
			 */
			if (ret == 0)
				ret = __wt_meta_track_sub_off(session);
		}

		if (!WT_META_TRACKING(session))
			WT_TRET(__wt_session_release_btree(session));
		session->btree = NULL;

		WT_ERR(ret);
	}

err:	return (ret);
}

/*
 * __conn_btree_discard --
 *	Discard a single btree file handle structure.
 */
static int
__conn_btree_discard(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	dhandle = &btree->dhandle;

	if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
		WT_SET_BTREE_IN_SESSION(session, btree);
		WT_TRET(__wt_conn_btree_sync_and_close(session));
		WT_CLEAR_BTREE_IN_SESSION(session);
	}
	__wt_rwlock_destroy(session, &dhandle->rwlock);
	__wt_free(session, dhandle->config);
	__wt_free(session, dhandle->name);
	__wt_free(session, dhandle->checkpoint);
	__wt_overwrite_and_free(session, dhandle);

	return (ret);
}

/*
 * __wt_conn_btree_discard --
 *	Discard the btree file handle structures.
 */
int
__wt_conn_btree_discard(WT_CONNECTION_IMPL *conn)
{
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DATA_HANDLE_CACHE *dhandle_cache;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	/* Close is single-threaded, no need to get the lock for real. */
	F_SET(session, WT_SESSION_SCHEMA_LOCKED);

	/*
	 * Close open btree handles: first, everything but the metadata file
	 * (as closing a normal file may open and write the metadata file),
	 * then the metadata file.  This function isn't called often, and I
	 * don't want to "know" anything about the metadata file's position on
	 * the list, so we do it the hard way.
	 */
restart:
	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		btree = (WT_BTREE *)dhandle;
		if (WT_IS_METADATA(btree))
			continue;

		TAILQ_REMOVE(&conn->dhqh, dhandle, q);
		--conn->btqcnt;
		WT_TRET(__conn_btree_discard(session, btree));
		goto restart;
	}

	/*
	 * Closing the files may have resulted in entries on our session's list
	 * of open btree handles, specifically, we added the metadata file if
	 * any of the files were dirty.  Clean up that list before we shut down
	 * the metadata entry, for good.
	 */
	while ((dhandle_cache = TAILQ_FIRST(&session->dhandles)) != NULL)
		WT_TRET(__wt_session_discard_btree(session, dhandle_cache));

	/* Close the metadata file handle. */
	while ((dhandle = TAILQ_FIRST(&conn->dhqh)) != NULL) {
		TAILQ_REMOVE(&conn->dhqh, dhandle, q);
		--conn->btqcnt;
		btree = (WT_BTREE *)dhandle;
		WT_TRET(__conn_btree_discard(session, btree));
	}

	return (ret);
}
