/* the upper-most page table pointer */

#ifdef CONFIG_MMU

extern pmd_t *top_pmd;

#define TOP_PTE(x)	pte_offset_kernel(top_pmd, x)

static inline pmd_t *pmd_off(pgd_t *pgd, unsigned long virt)
{
	return pmd_offset(pgd, virt);
}

static inline pmd_t *pmd_off_k(unsigned long virt)
{
	return pmd_off(pgd_offset_k(virt), virt);
}

const struct mem_type *get_mem_type(unsigned int type);

#endif

struct map_desc;
struct meminfo;
struct pglist_data;

void __init create_mapping(struct map_desc *md);
void __init bootmem_init(void);
void reserve_node_zero(struct pglist_data *pgdat);
