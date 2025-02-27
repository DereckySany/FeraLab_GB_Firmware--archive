#ifndef PAGE_FLAGS_H
#define PAGE_FLAGS_H

#include <linux/types.h>
#ifndef __GENERATING_BOUNDS_H
#include <linux/mm_types.h>
#include <linux/bounds.h>
#endif /* !__GENERATING_BOUNDS_H */

enum pageflags {
	PG_locked,		/* Page is locked. Don't touch. */
	PG_error,
	PG_referenced,
	PG_uptodate,
	PG_dirty,
	PG_lru,
	PG_active,
	PG_slab,
	PG_owner_priv_1,	/* Owner use. If pagecache, fs may use*/
	PG_arch_1,
	PG_reserved,
	PG_private,		/* If pagecache, has fs-private data */
	PG_writeback,		/* Page is under writeback */
#ifdef CONFIG_PAGEFLAGS_EXTENDED
	PG_head,		/* A head page */
	PG_tail,		/* A tail page */
#else
	PG_compound,		/* A compound page */
#endif
	PG_swapcache,		/* Swap page: swp_entry_t in private */
	PG_mappedtodisk,	/* Has blocks allocated on-disk */
	PG_reclaim,		/* To be reclaimed asap */
	PG_buddy,		/* Page is free, on buddy lists */
	PG_swapbacked,		/* Page is backed by RAM/swap */
#ifdef CONFIG_UNEVICTABLE_LRU
	PG_unevictable,		/* Page is "unevictable"  */
	PG_mlocked,		/* Page is vma mlocked */
#endif
#ifdef CONFIG_IA64_UNCACHED_ALLOCATOR
	PG_uncached,		/* Page has been mapped as uncached */
#endif
#ifdef CONFIG_CLEANCACHE
	PG_was_active,
#endif
	__NR_PAGEFLAGS,

	/* Filesystems */
	PG_checked = PG_owner_priv_1,

	/* XEN */
	PG_pinned = PG_owner_priv_1,
	PG_savepinned = PG_dirty,

	/* SLOB */
	PG_slob_page = PG_active,
	PG_slob_free = PG_private,

	/* SLUB */
	PG_slub_frozen = PG_active,
	PG_slub_debug = PG_error,
};

#ifndef __GENERATING_BOUNDS_H

/*
 * Macros to create function definitions for page flags
 */
#define TESTPAGEFLAG(uname, lname)					\
static inline int Page##uname(struct page *page) 			\
			{ return test_bit(PG_##lname, &page->flags); }

#define SETPAGEFLAG(uname, lname)					\
static inline void SetPage##uname(struct page *page)			\
			{ set_bit(PG_##lname, &page->flags); }

#define CLEARPAGEFLAG(uname, lname)					\
static inline void ClearPage##uname(struct page *page)			\
			{ clear_bit(PG_##lname, &page->flags); }

#define __SETPAGEFLAG(uname, lname)					\
static inline void __SetPage##uname(struct page *page)			\
			{ __set_bit(PG_##lname, &page->flags); }

#define __CLEARPAGEFLAG(uname, lname)					\
static inline void __ClearPage##uname(struct page *page)		\
			{ __clear_bit(PG_##lname, &page->flags); }

#define TESTSETFLAG(uname, lname)					\
static inline int TestSetPage##uname(struct page *page)			\
		{ return test_and_set_bit(PG_##lname, &page->flags); }

#define TESTCLEARFLAG(uname, lname)					\
static inline int TestClearPage##uname(struct page *page)		\
		{ return test_and_clear_bit(PG_##lname, &page->flags); }


#define PAGEFLAG(uname, lname) TESTPAGEFLAG(uname, lname)		\
	SETPAGEFLAG(uname, lname) CLEARPAGEFLAG(uname, lname)

#define __PAGEFLAG(uname, lname) TESTPAGEFLAG(uname, lname)		\
	__SETPAGEFLAG(uname, lname)  __CLEARPAGEFLAG(uname, lname)

#define PAGEFLAG_FALSE(uname) 						\
static inline int Page##uname(struct page *page) 			\
			{ return 0; }

#define TESTSCFLAG(uname, lname)					\
	TESTSETFLAG(uname, lname) TESTCLEARFLAG(uname, lname)

#define SETPAGEFLAG_NOOP(uname)						\
static inline void SetPage##uname(struct page *page) {  }

#define CLEARPAGEFLAG_NOOP(uname)					\
static inline void ClearPage##uname(struct page *page) {  }

#define __CLEARPAGEFLAG_NOOP(uname)					\
static inline void __ClearPage##uname(struct page *page) {  }

#define TESTCLEARFLAG_FALSE(uname)					\
static inline int TestClearPage##uname(struct page *page) { return 0; }

struct page;	/* forward declaration */

TESTPAGEFLAG(Locked, locked)
PAGEFLAG(Error, error)
PAGEFLAG(Referenced, referenced) TESTCLEARFLAG(Referenced, referenced)
PAGEFLAG(Dirty, dirty) TESTSCFLAG(Dirty, dirty) __CLEARPAGEFLAG(Dirty, dirty)
PAGEFLAG(LRU, lru) __CLEARPAGEFLAG(LRU, lru)
PAGEFLAG(Active, active) __CLEARPAGEFLAG(Active, active)
	TESTCLEARFLAG(Active, active)
__PAGEFLAG(Slab, slab)
PAGEFLAG(Checked, checked)		/* Used by some filesystems */
PAGEFLAG(Pinned, pinned) TESTSCFLAG(Pinned, pinned)	/* Xen */
PAGEFLAG(SavePinned, savepinned);			/* Xen */
PAGEFLAG(Reserved, reserved) __CLEARPAGEFLAG(Reserved, reserved)
PAGEFLAG(Private, private) __CLEARPAGEFLAG(Private, private)
	__SETPAGEFLAG(Private, private)
PAGEFLAG(SwapBacked, swapbacked) __CLEARPAGEFLAG(SwapBacked, swapbacked)

__PAGEFLAG(SlobPage, slob_page)
__PAGEFLAG(SlobFree, slob_free)

__PAGEFLAG(SlubFrozen, slub_frozen)
__PAGEFLAG(SlubDebug, slub_debug)

#ifdef CONFIG_CLEANCACHE
PAGEFLAG(WasActive, was_active)
#endif

/*
 * Only test-and-set exist for PG_writeback.  The unconditional operators are
 * risky: they bypass page accounting.
 */
TESTPAGEFLAG(Writeback, writeback) TESTSCFLAG(Writeback, writeback)
__PAGEFLAG(Buddy, buddy)
PAGEFLAG(MappedToDisk, mappedtodisk)

/* PG_readahead is only used for file reads; PG_reclaim is only for writes */
PAGEFLAG(Reclaim, reclaim) TESTCLEARFLAG(Reclaim, reclaim)
PAGEFLAG(Readahead, reclaim)		/* Reminder to do async read-ahead */

#ifdef CONFIG_HIGHMEM
/*
 * Must use a macro here due to header dependency issues. page_zone() is not
 * available at this point.
 */
#define PageHighMem(__p) is_highmem(page_zone(__p))
#else
PAGEFLAG_FALSE(HighMem)
#endif

#ifdef CONFIG_SWAP
PAGEFLAG(SwapCache, swapcache)
#else
PAGEFLAG_FALSE(SwapCache)
	SETPAGEFLAG_NOOP(SwapCache) CLEARPAGEFLAG_NOOP(SwapCache)
#endif

#ifdef CONFIG_UNEVICTABLE_LRU
PAGEFLAG(Unevictable, unevictable) __CLEARPAGEFLAG(Unevictable, unevictable)
	TESTCLEARFLAG(Unevictable, unevictable)

#define MLOCK_PAGES 1
PAGEFLAG(Mlocked, mlocked) __CLEARPAGEFLAG(Mlocked, mlocked)
	TESTSCFLAG(Mlocked, mlocked)

#else

#define MLOCK_PAGES 0
PAGEFLAG_FALSE(Mlocked)
	SETPAGEFLAG_NOOP(Mlocked) TESTCLEARFLAG_FALSE(Mlocked)

PAGEFLAG_FALSE(Unevictable) TESTCLEARFLAG_FALSE(Unevictable)
	SETPAGEFLAG_NOOP(Unevictable) CLEARPAGEFLAG_NOOP(Unevictable)
	__CLEARPAGEFLAG_NOOP(Unevictable)
#endif

#ifdef CONFIG_IA64_UNCACHED_ALLOCATOR
PAGEFLAG(Uncached, uncached)
#else
PAGEFLAG_FALSE(Uncached)
#endif

static inline int PageUptodate(struct page *page)
{
	int ret = test_bit(PG_uptodate, &(page)->flags);

	/*
	 * Must ensure that the data we read out of the page is loaded
	 * _after_ we've loaded page->flags to check for PageUptodate.
	 * We can skip the barrier if the page is not uptodate, because
	 * we wouldn't be reading anything from it.
	 *
	 * See SetPageUptodate() for the other side of the story.
	 */
	if (ret)
		smp_rmb();

	return ret;
}

static inline void __SetPageUptodate(struct page *page)
{
	smp_wmb();
	__set_bit(PG_uptodate, &(page)->flags);
}

static inline void SetPageUptodate(struct page *page)
{
#ifdef CONFIG_S390
	if (!test_and_set_bit(PG_uptodate, &page->flags))
		page_clear_dirty(page);
#else
	/*
	 * Memory barrier must be issued before setting the PG_uptodate bit,
	 * so that all previous stores issued in order to bring the page
	 * uptodate are actually visible before PageUptodate becomes true.
	 *
	 * s390 doesn't need an explicit smp_wmb here because the test and
	 * set bit already provides full barriers.
	 */
	smp_wmb();
	set_bit(PG_uptodate, &(page)->flags);
#endif
}

CLEARPAGEFLAG(Uptodate, uptodate)

extern void cancel_dirty_page(struct page *page, unsigned int account_size);

int test_clear_page_writeback(struct page *page);
int test_set_page_writeback(struct page *page);

static inline void set_page_writeback(struct page *page)
{
	test_set_page_writeback(page);
}

#ifdef CONFIG_PAGEFLAGS_EXTENDED
/*
 * System with lots of page flags available. This allows separate
 * flags for PageHead() and PageTail() checks of compound pages so that bit
 * tests can be used in performance sensitive paths. PageCompound is
 * generally not used in hot code paths.
 */
__PAGEFLAG(Head, head)
__PAGEFLAG(Tail, tail)

static inline int PageCompound(struct page *page)
{
	return page->flags & ((1L << PG_head) | (1L << PG_tail));

}
#else
/*
 * Reduce page flag use as much as possible by overlapping
 * compound page flags with the flags used for page cache pages. Possible
 * because PageCompound is always set for compound pages and not for
 * pages on the LRU and/or pagecache.
 */
TESTPAGEFLAG(Compound, compound)
__PAGEFLAG(Head, compound)

/*
 * PG_reclaim is used in combination with PG_compound to mark the
 * head and tail of a compound page. This saves one page flag
 * but makes it impossible to use compound pages for the page cache.
 * The PG_reclaim bit would have to be used for reclaim or readahead
 * if compound pages enter the page cache.
 *
 * PG_compound & PG_reclaim	=> Tail page
 * PG_compound & ~PG_reclaim	=> Head page
 */
#define PG_head_tail_mask ((1L << PG_compound) | (1L << PG_reclaim))

static inline int PageTail(struct page *page)
{
	return ((page->flags & PG_head_tail_mask) == PG_head_tail_mask);
}

static inline void __SetPageTail(struct page *page)
{
	page->flags |= PG_head_tail_mask;
}

static inline void __ClearPageTail(struct page *page)
{
	page->flags &= ~PG_head_tail_mask;
}

#endif /* !PAGEFLAGS_EXTENDED */

#ifdef CONFIG_UNEVICTABLE_LRU
#define __PG_UNEVICTABLE	(1 << PG_unevictable)
#define __PG_MLOCKED		(1 << PG_mlocked)
#else
#define __PG_UNEVICTABLE	0
#define __PG_MLOCKED		0
#endif

/*
 * Flags checked when a page is freed.  Pages being freed should not have
 * these flags set.  It they are, there is a problem.
 */
#define PAGE_FLAGS_CHECK_AT_FREE \
	(1 << PG_lru   | 1 << PG_private   | 1 << PG_locked | \
	 1 << PG_buddy | 1 << PG_writeback | 1 << PG_reserved | \
	 1 << PG_slab  | 1 << PG_swapcache | 1 << PG_active | \
	 __PG_UNEVICTABLE | __PG_MLOCKED)

/*
 * Flags checked when a page is prepped for return by the page allocator.
 * Pages being prepped should not have any flags set.  It they are set,
 * there has been a kernel bug or struct page corruption.
 */
#define PAGE_FLAGS_CHECK_AT_PREP	((1 << NR_PAGEFLAGS) - 1)

#endif /* !__GENERATING_BOUNDS_H */
#endif	/* PAGE_FLAGS_H */
