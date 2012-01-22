/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __hazard_bsearch_cmp(const void *, const void *);
static void __hazard_copy(WT_SESSION_IMPL *);
static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static int  __hazard_qsort_cmp(const void *, const void *);
static int  __rec_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_discard_page(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_excl(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __rec_excl_clear(WT_SESSION_IMPL *);
static int  __rec_excl_page(WT_SESSION_IMPL *, WT_REF *, uint32_t);
static int  __rec_page_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_page_dirty_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_review(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_root_addr_update(WT_SESSION_IMPL *, uint8_t *, uint32_t);
static int  __rec_root_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_root_dirty_update(WT_SESSION_IMPL *, WT_PAGE *);

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
		page->ref->state = WT_REF_MEM;
		return (0);
	}

	/*
	 * Get exclusive access to the page and review the page and its subtree
	 * for conditions that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * not disallowed anywhere.
	 */
	WT_ERR(__rec_review(session, page, flags));

	/* If the page is dirty, write it. */
	if (__wt_page_is_modified(page))
		WT_ERR(__wt_rec_write(session, page, NULL));

	/* Count evictions of internal pages during normal operation. */
	if (!LF_ISSET(WT_REC_SINGLE) &&
	    (page->type == WT_PAGE_COL_INT || page->type == WT_PAGE_ROW_INT))
		WT_STAT_INCR(conn->stats, cache_evict_internal);

	/* Update the parent and discard the page. */
	if (F_ISSET(page, WT_PAGE_REC_MASK) == 0) {
		WT_STAT_INCR(conn->stats, cache_evict_unmodified);

		if (WT_PAGE_IS_ROOT(page))
			WT_ERR(__rec_root_clean_update(session, page));
		else
			WT_ERR(__rec_page_clean_update(session, page));
	} else {
		WT_STAT_INCR(conn->stats, cache_evict_modified);

		if (WT_PAGE_IS_ROOT(page))
			WT_ERR(__rec_root_dirty_update(session, page));
		else
			WT_ERR(__rec_page_dirty_update(session, page));
	}

	return (0);

err:	/*
	 * If unable to evict this page, release exclusive reference(s) we've
	 * acquired.
	 */
	__rec_excl_clear(session);
	return (ret);
}

/*
 * __rec_page_clean_update  --
 *	Update a page's reference for an evicted, clean page.
 */
static int
__rec_page_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	page->ref->page = NULL;
	page->ref->state = WT_REF_DISK;

	return (__rec_discard_page(session, page));
}

/*
 * __rec_root_clean_update  --
 *	Update a page's reference for an evicted, clean page.
 */
static int
__rec_root_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;

	btree = session->btree;

	btree->root_page = NULL;

	return (__rec_discard_page(session, page));
}

/*
 * __rec_page_dirty_update --
 *	Update a page's reference for an evicted, dirty page.
 */
static int
__rec_page_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	mod = page->modify;
	parent_ref = page->ref;

	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		/*
		 * We're not going to evict this page after all, instead we'll
		 * merge it into its parent when that page is evicted.  Release
		 * our exclusive reference to it, as well as any pages below it
		 * we locked down, and return it into use.
		 */
		__rec_excl_clear(session);
		return (0);
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
		if (parent_ref->addr != NULL &&
		    __wt_off_page(page->parent, parent_ref->addr))
			__wt_free(session, parent_ref->addr);
		WT_RET(__wt_calloc(
		    session, 1, sizeof(WT_ADDR), &parent_ref->addr));

		((WT_ADDR *)parent_ref->addr)->addr = mod->u.replace.addr;
		((WT_ADDR *)parent_ref->addr)->size = mod->u.replace.size;
		parent_ref->page = NULL;

		/*
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		/*
		 * Update the parent to reference new internal page(s).
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		parent_ref->page = mod->u.split;
		WT_PUBLISH(parent_ref->state, WT_REF_MEM);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * Discard pages which were merged into this page during reconciliation,
	 * then discard the page itself.
	 */
	WT_RET(__rec_discard(session, page));

	return (0);
}

/*
 * __rec_root_addr_update --
 *	Update the root page's address.
 */
static int
__rec_root_addr_update(WT_SESSION_IMPL *session, uint8_t *addr, uint32_t size)
{
	WT_ADDR *root_addr;
	WT_BTREE *btree;

	btree = session->btree;
	root_addr = &btree->root_addr;

	/* Free any previously created root addresses. */
	if (root_addr->addr != NULL) {
		WT_RET(__wt_bm_free(session, root_addr->addr, root_addr->size));

		__wt_free(session, root_addr->addr);
	}
	btree->root_update = 1;

	root_addr->addr = addr;
	root_addr->size = size;

	return (0);
}

/*
 * __rec_root_dirty_update --
 *	Update the reference for an evicted, dirty root page.
 */
static int
__rec_root_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_PAGE *next;
	WT_PAGE_MODIFY *mod;

	btree = session->btree;
	mod = page->modify;

	next = NULL;
	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		WT_VERBOSE(session, evict, "root page empty");

		/* If the root page is empty, clear the root address. */
		WT_RET(__rec_root_addr_update(session, NULL, 0));
		btree->root_page = NULL;
		break;
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
		WT_VERBOSE(session, evict, "root page replaced");

		/* Update the root to its replacement. */
		WT_RET(__rec_root_addr_update(
		    session, mod->u.replace.addr, mod->u.replace.size));
		btree->root_page = NULL;
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		WT_VERBOSE(session, evict,
		    "root page split %p -> %p", page, mod->u.split);

		next = mod->u.split;
		break;
	}

	/*
	 * Discard pages which were merged into this page during reconciliation,
	 * then discard the page itself.
	 */
	WT_RET(__rec_discard(session, page));

	if (next == NULL)
		return (0);

	/*
	 * Newly created internal pages are normally merged into their parent
	 * when the parent is evicted.  Newly split root pages can't be merged,
	 * they have no parent and the new root page must be written.  We also
	 * have to write the root page immediately, as the sync or close that
	 * triggered the split won't see our new root page during its traversal.
	 *
	 * Make the new root page look like a normal page that's been modified,
	 * write it out and discard it.  Keep doing that and eventually we'll
	 * perform a simple replacement (as opposed to another level of split),
	 * allowing us to can update the tree's root information and quit.  The
	 * only time we see multiple splits in here is when we've bulk-loaded
	 * something huge, and now we're evicting the index page referencing all
	 * of those leaf pages.
	 */
	WT_RET(__wt_page_modify_init(session, next));
	__wt_page_modify_set(next);
	F_CLR(next, WT_PAGE_REC_MASK);
	WT_RET(__wt_rec_write(session, next, NULL));
	return (__rec_root_dirty_update(session, next));
}

/*
 * __rec_review --
 *	Get exclusive access to the page and review the page and its subtree
 * for conditions that would block our eviction of the page.
 */
static int
__rec_review(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	session->excl_next = 0;			/* Track the pages we lock. */

	/*
	 * Get exclusive access to the page if our caller doesn't have the tree
	 * locked down.
	 */
	if (!LF_ISSET(WT_REC_SINGLE))
		WT_RET(__hazard_exclusive(
		    session, page->ref, LF_ISSET(WT_REC_WAIT) ? 1 : 0));

	/* Walk the page's subtree and make sure we can evict this page. */
	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		WT_RET(__rec_excl(session, page, flags));
		break;
	default:
		break;
	}

	return (0);
}

/*
 * __rec_excl --
 *	Walk an internal page's subtree, getting exclusive access as necessary,
 * and checking if the subtree can be evicted.
 */
static int
__rec_excl(WT_SESSION_IMPL *session, WT_PAGE *parent, uint32_t flags)
{
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* For each entry in the page... */
	WT_REF_FOREACH(parent, ref, i) {
		switch (ref->state) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_MEM:			/* In-memory */
			break;
		case WT_REF_LOCKED:			/* Being evicted */
		case WT_REF_READING:			/* Being read */
			return (WT_ERROR);
		}
		WT_RET(__rec_excl_page(session, ref, flags));

		/* Recurse down the tree. */
		page = ref->page;
		switch (page->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			WT_RET(__rec_excl(session, page, flags));
			break;
		default:
			break;
		}
	}
	return (0);
}

/*
 * __rec_excl_clear --
 *     Discard exclusive access and return a page's subtree to availability.
 */
static void
__rec_excl_clear(WT_SESSION_IMPL *session)
{
	WT_REF *ref;
	uint32_t i;

	for (i = 0; i < session->excl_next; ++i) {
		if ((ref = session->excl[i]) == NULL)
			break;
		WT_ASSERT(session, ref->state == WT_REF_LOCKED);
		ref->state = WT_REF_MEM;
	}
	session->excl_next = 0;
}

/*
 * __rec_excl_page --
 *	Acquire exclusive access to a page as necessary, and check if the page
 * can be evicted.
 */
static int
__rec_excl_page(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
	WT_PAGE *page;

	page = ref->page;

	/*
	 * If we find a page that can't be merged into its parent, we're done:
	 * you can't evict a page that references other in-memory pages, those
	 * pages must be evicted first.  While the test is necessary, it should
	 * not happen much: reading an internal page increments its read
	 * generation, and so internal pages shouldn't be selected for eviction
	 * until after their children have been evicted.
	 *
	 * A cheap test: if the child page doesn't at least have a chance of a
	 * merge, we're done.
	 */
	if (!F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (1);

	/*
	 * If the page is dirty, try and write it.   This is because once a page
	 * is flagged for a merge into its parent, the eviction server no longer
	 * makes any attempt to evict it, it only attempts to evict its parent.
	 * If a parent page is blocked from eviction because of a dirty child
	 * page, we would never write the child page and never evict the parent.
	 * This prevents that from happening.
	 */
	if (__wt_page_is_modified(page))
		WT_RET(__wt_rec_write(session, page, NULL));

	/* Repeat the cheap test: an empty page might no longer be "empty". */
	if (!F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (1);

	/*
	 * Next, if our caller doesn't have the tree locked down, get exclusive
	 * access to the page.
	 */
	if (!LF_ISSET(WT_REC_SINGLE))
		WT_RET(__hazard_exclusive(
		    session, ref, LF_ISSET(WT_REC_WAIT) ? 1 : 0));

	/*
	 * Finally, a more careful test: merge-split pages are OK, no matter if
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
 * __rec_discard --
 *	Discard any pages merged into an evicted page, then the page itself.
 */
static int
__rec_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_REF *ref;
	uint32_t i;

	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/* For each entry in the page... */
		WT_REF_FOREACH(page, ref, i)
			if (ref->state != WT_REF_DISK)
				WT_RET(__rec_discard(session, ref->page));
		/* FALLTHROUGH */
	default:
		WT_RET(__rec_discard_page(session, page));
		break;
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
		WT_RET(__wt_rec_track_wrapup(session, page, 1));

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
	if (session->excl_next * sizeof(WT_REF *) == session->excl_allocated)
		WT_RET(__wt_realloc(session, &session->excl_allocated,
		    (session->excl_next + 50) * sizeof(WT_REF *),
		    &session->excl));
	session->excl[session->excl_next++] = ref;
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

	WT_CSTAT_INCR(session, cache_evict_hazard);

	WT_VERBOSE(session, evict, "page %p hazard request failed", ref->page);

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
