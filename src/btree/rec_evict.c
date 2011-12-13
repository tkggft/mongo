/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __hazard_bsearch_cmp(const void *, const void *);
static void __hazard_copy(WT_SESSION_IMPL *);
static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static int  __hazard_qsort_cmp(const void *, const void *);
static int  __rec_discard_page(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_parent_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_parent_dirty_update(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_review(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_root_split(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_discard_col(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_discard_row(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_sub_excl_clear(
    WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE *, uint32_t);
static int  __rec_sub_excl_col(
    WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **, uint32_t);
static int __rec_sub_excl_col_clear(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE *);
static int  __rec_sub_excl_page(
    WT_SESSION_IMPL *, WT_REF *, WT_PAGE *, uint32_t);
static int  __rec_sub_excl_row(
    WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **, uint32_t);
static int __rec_sub_excl_row_clear(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE *);

/*
 * __wt_rec_evict --
 *	Reconciliation plus eviction.
 */
int
__wt_rec_evict(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	int ret;

	conn = S2C(session);

	WT_VERBOSE(session, evict,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	/*
	 * You cannot evict pages merge-split pages (that is, internal pages
	 * that are a result of a split of another page).  They can only be
	 * evicted as a result of evicting their parents, else we would lose
	 * the merge flag and they would be written separately, permanently
	 * deepening the tree.  Should the eviction server request eviction
	 * of a merge-split page, ignore the request (but unlock the page and
	 * bump the read generation to ensure it isn't selected again).
	 */
	if (F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE)) {
		page->read_gen = __wt_cache_read_gen(session);
		page->parent_ref->state = WT_REF_MEM;
		return (0);
	}

	/*
	 * If eviction of a page needs to be forced, wait for the page to
	 * become available.
	 */
	if (F_ISSET(page, WT_PAGE_FORCE_EVICT)) {
		LF_SET(WT_REC_WAIT);
		__wt_evict_force_clear(session, page);
	}

	/*
	 * Get exclusive access to the page and review the page and its subtree
	 * for conditions that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * not disallowed anywhere.
	 */
	WT_RET(__rec_review(session, page, flags));

	/* If the page is dirty, write it. */
	if (__wt_page_is_modified(page))
		WT_ERR(__wt_rec_write(session, page, NULL));

	/* Update the parent and discard the page. */
	if (F_ISSET(page, WT_PAGE_REC_MASK) == 0) {
		WT_STAT_INCR(conn->stats, cache_evict_unmodified);
		WT_RET(__rec_parent_clean_update(session, page));
	} else {
		WT_STAT_INCR(conn->stats, cache_evict_modified);
		WT_RET(__rec_parent_dirty_update(session, page, flags));
	}

	return (0);

err:	__rec_sub_excl_clear(session, page, NULL, flags);
	return (ret);
}

/*
 * __rec_parent_clean_update  --
 *	Update a parent page's reference for an evicted, clean page.
 */
static int
__rec_parent_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	page->parent_ref->page = NULL;
	page->parent_ref->state = WT_REF_DISK;

	/* Discard the page. */
	return (__rec_discard_page(session, page));
}

/*
 * __rec_parent_dirty_update --
 *	Update a parent page's reference for an evicted, dirty page.
 */
static int
__rec_parent_dirty_update(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	mod = page->modify;
	parent_ref = page->parent_ref;

	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		/*
		 * Special case the root page: if the root page is empty, we
		 * reset the root address and discard the tree.
		 */
		if (WT_PAGE_IS_ROOT(page)) {
			parent_ref->addr = WT_ADDR_INVALID;
			parent_ref->page = NULL;
			/*
			 * Publish: a barrier to ensure the structure fields are
			 * set before the state change makes the page available
			 * to readers.
			 */
			WT_PUBLISH(parent_ref->state, WT_REF_DISK);
			break;
		}
		/*
		 * We're not going to evict this page after all, instead we'll
		 * merge it into its parent when that page is evicted.  Release
		 * our exclusive reference to it, as well as any pages below it
		 * we locked down, and return it into use.
		 */
		__rec_sub_excl_clear(session, page, NULL, flags);
		return (0);
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
		/*
		 * Special case the root page: none, we just wrote a new root
		 * page, updating the parent is all that's necessary.
		 *
		 * Update the parent to reference the replacement page.
		 */
		parent_ref->addr = mod->u.write_off.addr;
		parent_ref->size = mod->u.write_off.size;
		parent_ref->page = NULL;

		/*
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		/* Special case the root page: see below. */
		if (WT_PAGE_IS_ROOT(page)) {
			WT_VERBOSE(session,evict,
			    "root page split %p -> %p",
			    page, mod->u.write_split);
			/*
			 * Newly created internal pages are normally merged into
			 * their parent when the parent is evicted.  Newly split
			 * root pages can't be merged, they have no parent and
			 * the new root page must be written.  We also have to
			 * write the root page immediately, as the sync or close
			 * that triggered the split won't see our new root page
			 * during its traversal.
			 */
			WT_RET(__rec_root_split(session, mod->u.write_split));

			/*
			 * Publish: a barrier to ensure the structure fields are
			 * set before the state change makes the page available
			 * to readers.
			 */
			WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		} else {
			/*
			 * Update the parent to reference new internal page(s).
			 *
			 * Publish: a barrier to ensure the structure fields are
			 * set before the state change makes the page available
			 * to readers.
			 */
			parent_ref->page = mod->u.write_split;
			WT_PUBLISH(parent_ref->state, WT_REF_MEM);
		}
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * Eviction:
	 *
	 * Discard pages which were merged into this page during reconciliation,
	 * then discard the page itself.
	 */
	WT_RET(__rec_sub_discard(session, page));
	WT_RET(__rec_discard_page(session, page));

	return (0);
}

/*
 * __rec_root_split --
 *	Root splits.
 */
static int
__rec_root_split(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_PAGE *next;

	btree = session->btree;

	/*
	 * Make the new root page look like a normal page that's been modified,
	 * write it out and discard it.  Keep doing that and eventually we'll
	 * perform a simple replacement (as opposed to another level of split),
	 * allowing us to can update the tree's root information and quit.  The
	 * only time we see multiple splits in here is when we've bulk-loaded
	 * something huge, and now we're evicting the index page that references
	 * all of those leaf pages.
	 */
	for (; page != NULL; page = next) {
		WT_RET(__wt_page_set_modified(session, page));
		F_CLR(page, WT_PAGE_REC_MASK);

		WT_RET(__wt_rec_write(session, page, NULL));

		switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
		case WT_PAGE_REC_REPLACE: 		/* 1-for-1 page swap */
			btree->root_page.addr = page->modify->u.write_off.addr;
			btree->root_page.size = page->modify->u.write_off.size;
			btree->root_page.page = NULL;
			next = NULL;			/* terminate loop */
			break;
		case WT_PAGE_REC_SPLIT:			/* Page split */
			next = page->modify->u.write_split;
			break;
		WT_ILLEGAL_VALUE(session);
		}

		WT_RET(__rec_discard_page(session, page));
	}

	return (0);
}

/*
 * __rec_review --
 *	Get exclusive access to the page and review the page and its subtree
 * for conditions that would block our eviction of the page.
 */
static int
__rec_review(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_PAGE *last_page;
	int ret;

	ret = 0;

	/*
	 * Attempt exclusive access to the page if our caller doesn't have the
	 * tree locked down.
	 */
	if (!LF_ISSET(WT_REC_SINGLE)) {
		WT_RET(__hazard_exclusive(
		    session, page->parent_ref, LF_ISSET(WT_REC_WAIT) ? 1 : 0));

		last_page = page;
	}

	/*
	 * Walk the page's subtree and make sure we can evict this page.
	 *
	 * When evicting a page, it may reference deleted or split pages which
	 * will be merged into the evicted page.
	 *
	 * If we find an in-memory page, we're done: you can't evict a page that
	 * references other in-memory pages, those pages must be evicted first.
	 * While the test is necessary, it shouldn't happen much: reading any
	 * internal page increments its read generation, and so internal pages
	 * shouldn't be selected for eviction until after any children have been
	 * evicted.
	 *
	 * If we find a split page, get exclusive access to the page and then
	 * continue, the split page will be merged into our page.
	 *
	 * If we find a deleted page, get exclusive access to the page and then
	 * check its status.  If still deleted, we can continue, the page will
	 * be merged into our page.  However, another thread of control might
	 * have inserted new material and the page is no longer deleted, which
	 * means the reconciliation fails.
	 *
	 * If reconciliation isn't going to be possible, we have to clear any
	 * pages we locked while we were looking.  We keep track of the last
	 * page we successfully locked, and traverse the tree in the same order
	 * to clear locks, stopping when we reach the last locked page.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		ret = __rec_sub_excl_col(session, page, &last_page, flags);
		break;
	case WT_PAGE_ROW_INT:
		ret = __rec_sub_excl_row(session, page, &last_page, flags);
		break;
	default:
		break;
	}

	/* If can't evict this page, release our exclusive reference(s). */
	if (ret != 0)
		__rec_sub_excl_clear(session, page, last_page, flags);

	return (ret);
}

/*
 * __rec_sub_excl_clear --
 *     Discard exclusive access and return a page to availability.
 */
static void
__rec_sub_excl_clear(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_PAGE *last_page, uint32_t flags)
{
	if (LF_ISSET(WT_REC_SINGLE))
		return;

	WT_ASSERT(session, page->parent_ref->state == WT_REF_LOCKED);

	/*
	 * Take care to unlock pages in the same order we locked them.
	 * Otherwise, tracking the last successfully locked page is meaningless.
	 */
	page->parent_ref->state = WT_REF_MEM;
	if (page == last_page)
		return;

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__rec_sub_excl_col_clear(session, page, last_page);
		break;
	case WT_PAGE_ROW_INT:
		__rec_sub_excl_row_clear(session, page, last_page);
		break;
	default:
		break;
	}
}

/*
 * __rec_sub_excl_col --
 *	Walk a column-store internal page's subtree, handling deleted and split
 *	pages.
 */
static int
__rec_sub_excl_col(WT_SESSION_IMPL *session,
    WT_PAGE *parent, WT_PAGE **last_pagep, uint32_t flags)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i) {
		switch (WT_COL_REF_STATE(cref)) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
		case WT_REF_READING:			/* Being read */
			return (WT_ERROR);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = WT_COL_REF_PAGE(cref);

		WT_RET(__rec_sub_excl_page(session, &cref->ref, page, flags));

		*last_pagep = page;

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_COL_INT)
			WT_RET(__rec_sub_excl_col(
			    session, page, last_pagep, flags));
	}
	return (0);
}

/*
 * __rec_sub_excl_col_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static int
__rec_sub_excl_col_clear(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_PAGE *last_page)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i) {
		WT_ASSERT(session, WT_COL_REF_STATE(cref) == WT_REF_LOCKED);
		WT_COL_REF_STATE(cref) = WT_REF_MEM;

		/* Recurse down the tree. */
		page = WT_COL_REF_PAGE(cref);
		if (page == last_page)
			return (1);
		if (page->type == WT_PAGE_COL_INT)
			if (__rec_sub_excl_col_clear(
			    session, page, last_page))
				return (1);
	}

	return (0);
}

/*
 * __rec_sub_excl_row --
 *	Walk a row-store internal page's subtree, and acquiring exclusive access
 * as necessary and checking if the subtree can be evicted.
 */
static int
__rec_sub_excl_row(WT_SESSION_IMPL *session,
    WT_PAGE *parent, WT_PAGE **last_pagep, uint32_t flags)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i) {
		switch (WT_ROW_REF_STATE(rref)) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
		case WT_REF_READING:			/* Being read */
			return (WT_ERROR);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = WT_ROW_REF_PAGE(rref);

		WT_RET(__rec_sub_excl_page(session, &rref->ref, page, flags));

		*last_pagep = page;

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_ROW_INT)
			WT_RET(__rec_sub_excl_row(
			    session, page, last_pagep, flags));
	}
	return (0);
}

/*
 * __rec_sub_excl_row_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static int
__rec_sub_excl_row_clear(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_PAGE *last_page)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i) {
		WT_ASSERT(session, WT_ROW_REF_STATE(rref) == WT_REF_LOCKED);
		WT_ROW_REF_STATE(rref) = WT_REF_MEM;

		/* Recurse down the tree. */
		page = WT_ROW_REF_PAGE(rref);
		if (page == last_page)
			return (1);
		if (page->type == WT_PAGE_ROW_INT)
			if (__rec_sub_excl_row_clear(
			    session, page, last_page))
				return (1);
	}

	return (0);
}

/*
 * __rec_sub_excl_page --
 *	Acquire exclusive access to a page as necessary, and check if the page
 * can be evicted.
 */
static int
__rec_sub_excl_page(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE *page, uint32_t flags)
{
	/*
	 * An in-memory page: if the page can't be merged into its parent, then
	 * we can't evict the subtree.  This is not a problem, it just means we
	 * chose badly when selecting a page for eviction.
	 *
	 * First, a cheap test: if the child page doesn't at least have a chance
	 * of a merge, we can't evict the candidate page.
	 */
	if (!F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (1);

	/*
	 * Next, if our caller doesn't have the tree locked down, get exclusive
	 * access to the page and test again.
	 */
	if (!LF_ISSET(WT_REC_SINGLE))
		WT_RET(__hazard_exclusive(
		    session, ref, LF_ISSET(WT_REC_WAIT) ? 1 : 0));

	/*
	 * Second, a more careful test: merge-split pages are OK, no matter if
	 * they're clean or dirty, we can always merge them into the parent.
	 * Clean split or empty pages are OK too.  Dirty split or empty pages
	 * are not OK, they must be written first so we know what they're going
	 * to look like to the parent.
	 */
	if (F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE))
		return (0);
	if (F_ISSET(page, WT_PAGE_REC_SPLIT | WT_PAGE_REC_EMPTY))
		if (!__wt_page_is_modified(page))
			return (0);
	return (1);
}

/*
 * __rec_sub_discard --
 *	Discard any pages merged into the evicted page.
 */
static int
__rec_sub_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	switch (page->type) {
	case WT_PAGE_COL_INT:
		WT_RET(__rec_sub_discard_col(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__rec_sub_discard_row(session, page));
		break;
	default:
		break;
	}
	return (0);
}

/*
 * __rec_sub_discard_col --
 *	Discard any column-store pages we merged.
 */
static int
__rec_sub_discard_col(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i)
		if (WT_COL_REF_STATE(cref) != WT_REF_DISK) {
			page = WT_ROW_REF_PAGE(cref);

			/* Recurse down the tree. */
			if (page->type == WT_PAGE_COL_INT)
				WT_RET(__rec_sub_discard_col(session, page));

			WT_RET(__rec_discard_page(session, page));
		}
	return (0);
}

/*
 * __rec_sub_discard_row --
 *	Discard any row-store pages we merged.
 */
static int
__rec_sub_discard_row(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i)
		if (WT_ROW_REF_STATE(rref) != WT_REF_DISK) {
			page = WT_ROW_REF_PAGE(rref);

			/* Recurse down the tree. */
			if (page->type == WT_PAGE_ROW_INT)
				WT_RET(__rec_sub_discard_row(session, page));

			WT_RET(__rec_discard_page(session, page));
		}
	return (0);
}

/*
 * __rec_discard_page --
 *	Process the page's list of tracked objects, and discard it.
 */
static int
__rec_discard_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* If the page has tracked objects, resolve them. */
	if (page->modify != NULL)
		WT_RET(__wt_rec_track_discard(session, page, 1));

	/* Discard the page itself. */
	__wt_page_out(session, page, 0);

	return (0);
}

/*
 * __hazard_exclusive --
 *	Request exclusive access to a page.
 */
static int
__hazard_exclusive(WT_SESSION_IMPL *session, WT_REF *ref, int force)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	/* The page must be in memory, and we may already have it locked. */
	WT_ASSERT(session,
	    ref->state == WT_REF_MEM || ref->state == WT_REF_LOCKED);

	/*
	 * Hazard references are acquired down the tree, which means we can't
	 * deadlock.
	 *
	 * Request exclusive access to the page; no memory flush needed, the
	 * state field is declared volatile.  If another thread already has
	 * this page and we are not forcing the issue, give up.
	 */
	ref->state = WT_REF_LOCKED;

	/* Get a fresh copy of the hazard reference array. */
retry:	__hazard_copy(session);

	/* If we find a matching hazard reference, the page is still in use. */
	if (bsearch(ref->page, cache->hazard, cache->hazard_elem,
	    sizeof(WT_HAZARD), __hazard_bsearch_cmp) == NULL)
		return (0);

	WT_BSTAT_INCR(session, rec_hazard);

	/*
	 * If we have to get this hazard reference, spin and wait for it to
	 * become available.
	 */
	if (force) {
		__wt_yield();
		goto retry;
	}

	WT_VERBOSE(session, evict,
	    "page %p hazard request failed", ref->page);

	/* Return the page to in-use. */
	ref->state = WT_REF_MEM;

	return (1);
}

/*
 * __hazard_qsort_cmp --
 *	Qsort function: sort hazard list based on the page's address.
 */
static int
__hazard_qsort_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = ((WT_HAZARD *)a)->page;
	b_page = ((WT_HAZARD *)b)->page;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __hazard_copy --
 *	Copy the hazard array and prepare it for searching.
 */
static void
__hazard_copy(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint32_t elem, i, j;

	conn = S2C(session);
	cache = conn->cache;

	/* Copy the list of hazard references, compacting it as we go. */
	elem = conn->session_size * conn->hazard_size;
	for (i = j = 0; j < elem; ++j) {
		if (conn->hazard[j].page == NULL)
			continue;
		cache->hazard[i] = conn->hazard[j];
		++i;
	}
	elem = i;

	/* Sort the list by page address. */
	qsort(
	    cache->hazard, (size_t)elem, sizeof(WT_HAZARD), __hazard_qsort_cmp);
	cache->hazard_elem = elem;
}

/*
 * __hazard_bsearch_cmp --
 *	Bsearch function: search sorted hazard list.
 */
static int
__hazard_bsearch_cmp(const void *search, const void *b)
{
	void *entry;

	entry = ((WT_HAZARD *)b)->page;

	return (search > entry ? 1 : ((search < entry) ? -1 : 0));
}
