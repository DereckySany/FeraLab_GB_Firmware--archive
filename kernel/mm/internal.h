/* internal.h: mm/ internal definitions
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef __MM_INTERNAL_H
#define __MM_INTERNAL_H

#include <linux/mm.h>

void free_pgtables(struct mmu_gather *tlb, struct vm_area_struct *start_vma,
		unsigned long floor, unsigned long ceiling);

extern void prep_compound_page(struct page *page, unsigned long order);
extern void prep_compound_gigantic_page(struct page *page, unsigned long order);

static inline void set_page_count(struct page *page, int v)
{
	atomic_set(&page->_count, v);
}

/*
 * Turn a non-refcounted page (->_count == 0) into refcounted with
 * a count of one.
 */
static inline void set_page_refcounted(struct page *page)
{
	VM_BUG_ON(PageTail(page));
	VM_BUG_ON(atomic_read(&page->_count));
	set_page_count(page, 1);
}

static inline void __put_page(struct page *page)
{
	atomic_dec(&page->_count);
}

/*
 * in mm/vmscan.c:
 */
extern int isolate_lru_page(struct page *page);
extern void putback_lru_page(struct page *page);

/*
 * in mm/page_alloc.c
 */
extern unsigned long highest_memmap_pfn;
extern void __free_pages_bootmem(struct page *page, unsigned int order);

/*
 * function for dealing with page's order in buddy system.
 * zone->lock is already acquired when we use these.
 * So, we don't need atomic page->flags operations here.
 */
static inline unsigned long page_order(struct page *page)
{
	VM_BUG_ON(!PageBuddy(page));
	return page_private(page);
}

extern long mlock_vma_pages_range(struct vm_area_struct *vma,
			unsigned long start, unsigned long end);
extern void munlock_vma_pages_range(struct vm_area_struct *vma,
			unsigned long start, unsigned long end);
static inline void munlock_vma_pages_all(struct vm_area_struct *vma)
{
	munlock_vma_pages_range(vma, vma->vm_start, vma->vm_end);
}

#ifdef CONFIG_UNEVICTABLE_LRU
/*
 * unevictable_migrate_page() called only from migrate_page_copy() to
 * migrate unevictable flag to new page.
 * Note that the old page has been isolated from the LRU lists at this
 * point so we don't need to worry about LRU statistics.
 */
static inline void unevictable_migrate_page(struct page *new, struct page *old)
{
	if (TestClearPageUnevictable(old))
		SetPageUnevictable(new);
}
#else
static inline void unevictable_migrate_page(struct page *new, struct page *old)
{
}
#endif

#ifdef CONFIG_UNEVICTABLE_LRU
/*
 * Called only in fault path via page_evictable() for a new page
 * to determine if it's being mapped into a LOCKED vma.
 * If so, mark page as mlocked.
 */
static inline int is_mlocked_vma(struct vm_area_struct *vma, struct page *page)
{
	VM_BUG_ON(PageLRU(page));

	if (likely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) != VM_LOCKED))
		return 0;

	if (!TestSetPageMlocked(page)) {
		inc_zone_page_state(page, NR_MLOCK);
		count_vm_event(UNEVICTABLE_PGMLOCKED);
	}
	return 1;
}

extern void mlock_vma_page(struct page *page);
extern void munlock_vma_page(struct page *page);

/*
 * Clear the page's PageMlocked().  This can be useful in a situation where
 * we want to unconditionally remove a page from the pagecache -- e.g.,
 * on truncation or freeing.
 *
 * It is legal to call this function for any page, mlocked or not.
 * If called for a page that is still mapped by mlocked vmas, all we do
 * is revert to lazy LRU behaviour -- semantics are not broken.
 */
extern void __clear_page_mlock(struct page *page);
static inline void clear_page_mlock(struct page *page)
{
	if (unlikely(TestClearPageMlocked(page)))
		__clear_page_mlock(page);
}

/*
 * mlock_migrate_page - called only from migrate_page_copy() to
 * migrate the Mlocked page flag; update statistics.
 */
static inline void mlock_migrate_page(struct page *newpage, struct page *page)
{
	if (TestClearPageMlocked(page)) {
		unsigned long flags;

		local_irq_save(flags);
		__dec_zone_page_state(page, NR_MLOCK);
		SetPageMlocked(newpage);
		__inc_zone_page_state(newpage, NR_MLOCK);
		local_irq_restore(flags);
	}
}

/*
 * free_page_mlock() -- clean up attempts to free and mlocked() page.
 * Page should not be on lru, so no need to fix that up.
 * free_pages_check() will verify...
 */
static inline void free_page_mlock(struct page *page)
{
	if (unlikely(TestClearPageMlocked(page))) {
		unsigned long flags;

		local_irq_save(flags);
		__dec_zone_page_state(page, NR_MLOCK);
		__count_vm_event(UNEVICTABLE_MLOCKFREED);
		local_irq_restore(flags);
	}
}

#else /* CONFIG_UNEVICTABLE_LRU */
static inline int is_mlocked_vma(struct vm_area_struct *v, struct page *p)
{
	return 0;
}
static inline void clear_page_mlock(struct page *page) { }
static inline void mlock_vma_page(struct page *page) { }
static inline void mlock_migrate_page(struct page *new, struct page *old) { }
static inline void free_page_mlock(struct page *page) { }

#endif /* CONFIG_UNEVICTABLE_LRU */

/*
 * Return the mem_map entry representing the 'offset' subpage within
 * the maximally aligned gigantic page 'base'.  Handle any discontiguity
 * in the mem_map at MAX_ORDER_NR_PAGES boundaries.
 */
static inline struct page *mem_map_offset(struct page *base, int offset)
{
	if (unlikely(offset >= MAX_ORDER_NR_PAGES))
		return pfn_to_page(page_to_pfn(base) + offset);
	return base + offset;
}

/*
 * Iterator over all subpages withing the maximally aligned gigantic
 * page 'base'.  Handle any discontiguity in the mem_map.
 */
static inline struct page *mem_map_next(struct page *iter,
						struct page *base, int offset)
{
	if (unlikely((offset & (MAX_ORDER_NR_PAGES - 1)) == 0)) {
		unsigned long pfn = page_to_pfn(base) + offset;
		if (!pfn_valid(pfn))
			return NULL;
		return pfn_to_page(pfn);
	}
	return iter + 1;
}

/*
 * FLATMEM and DISCONTIGMEM configurations use alloc_bootmem_node,
 * so all functions starting at paging_init should be marked __init
 * in those cases. SPARSEMEM, however, allows for memory hotplug,
 * and alloc_bootmem_node is not used.
 */
#ifdef CONFIG_SPARSEMEM
#define __paginginit __meminit
#else
#define __paginginit __init
#endif

/* Memory initialisation debug and verification */
enum mminit_level {
	MMINIT_WARNING,
	MMINIT_VERIFY,
	MMINIT_TRACE
};

#ifdef CONFIG_DEBUG_MEMORY_INIT

extern int mminit_loglevel;

#define mminit_dprintk(level, prefix, fmt, arg...) \
do { \
	if (level < mminit_loglevel) { \
		printk(level <= MMINIT_WARNING ? KERN_WARNING : KERN_DEBUG); \
		printk(KERN_CONT "mminit::" prefix " " fmt, ##arg); \
	} \
} while (0)

extern void mminit_verify_pageflags_layout(void);
extern void mminit_verify_page_links(struct page *page,
		enum zone_type zone, unsigned long nid, unsigned long pfn);
extern void mminit_verify_zonelist(void);

#else

static inline void mminit_dprintk(enum mminit_level level,
				const char *prefix, const char *fmt, ...)
{
}

static inline void mminit_verify_pageflags_layout(void)
{
}

static inline void mminit_verify_page_links(struct page *page,
		enum zone_type zone, unsigned long nid, unsigned long pfn)
{
}

static inline void mminit_verify_zonelist(void)
{
}
#endif /* CONFIG_DEBUG_MEMORY_INIT */

/* mminit_validate_memmodel_limits is independent of CONFIG_DEBUG_MEMORY_INIT */
#if defined(CONFIG_SPARSEMEM)
extern void mminit_validate_memmodel_limits(unsigned long *start_pfn,
				unsigned long *end_pfn);
#else
static inline void mminit_validate_memmodel_limits(unsigned long *start_pfn,
				unsigned long *end_pfn)
{
}
#endif /* CONFIG_SPARSEMEM */

#define GUP_FLAGS_WRITE                  0x1
#define GUP_FLAGS_FORCE                  0x2
#define GUP_FLAGS_IGNORE_VMA_PERMISSIONS 0x4
#define GUP_FLAGS_IGNORE_SIGKILL         0x8

int __get_user_pages(struct task_struct *tsk, struct mm_struct *mm,
		     unsigned long start, int len, int flags,
		     struct page **pages, struct vm_area_struct **vmas);

#endif
