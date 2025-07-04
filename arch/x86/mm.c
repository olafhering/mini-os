/* 
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: mm.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: Grzegorz Milos
 *              
 *        Date: Aug 2003, chages Aug 2005
 * 
 * Environment: Xen Minimal OS
 * Description: memory management related functions
 *              contains buddy page allocator from Xen.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <mini-os/errno.h>
#include <mini-os/os.h>
#include <mini-os/hypervisor.h>
#include <mini-os/balloon.h>
#include <mini-os/mm.h>
#include <mini-os/paravirt.h>
#include <mini-os/types.h>
#include <mini-os/lib.h>
#include <mini-os/kexec.h>
#include <mini-os/xmalloc.h>
#include <mini-os/e820.h>
#include <xen/memory.h>
#include <xen/arch-x86/hvm/start_info.h>

#ifdef MM_DEBUG
#define DEBUG(_f, _a...) \
    printk("MINI_OS(file=mm.c, line=%d) " _f "\n", __LINE__, ## _a)
#else
#define DEBUG(_f, _a...)    ((void)0)
#endif

unsigned long *phys_to_machine_mapping;
EXPORT_SYMBOL(phys_to_machine_mapping);
unsigned long mfn_zero;
pgentry_t *pt_base;
EXPORT_SYMBOL(pt_base);
static unsigned long first_free_pfn;
static unsigned long last_free_pfn;
static unsigned long virt_kernel_area_end = VIRT_KERNEL_AREA;

extern char stack[];
extern void page_walk(unsigned long va);

#ifdef CONFIG_PARAVIRT
void arch_mm_preinit(void *p)
{
    start_info_t *si = p;

    phys_to_machine_mapping = (unsigned long *)si->mfn_list;
    pt_base = (pgentry_t *)si->pt_base;
    first_free_pfn = PFN_UP(to_phys(pt_base)) + si->nr_pt_frames;
    last_free_pfn = si->nr_pages;
    balloon_set_nr_pages(last_free_pfn, last_free_pfn);
}
#else
#include <mini-os/desc.h>
user_desc gdt[NR_GDT_ENTRIES] =
{
    [GDTE_CS64_DPL0] = INIT_GDTE_SYM(0, 0xfffff, COMMON, CODE, DPL0, R, L),
    [GDTE_CS32_DPL0] = INIT_GDTE_SYM(0, 0xfffff, COMMON, CODE, DPL0, R, D),
    [GDTE_DS32_DPL0] = INIT_GDTE_SYM(0, 0xfffff, COMMON, DATA, DPL0, B, W),

    [GDTE_CS64_DPL3] = INIT_GDTE_SYM(0, 0xfffff, COMMON, CODE, DPL3, R, L),
    [GDTE_CS32_DPL3] = INIT_GDTE_SYM(0, 0xfffff, COMMON, CODE, DPL3, R, D),
    [GDTE_DS32_DPL3] = INIT_GDTE_SYM(0, 0xfffff, COMMON, DATA, DPL3, B, W),

    /* [GDTE_TSS]     */
    /* [GDTE_TSS + 1] */
};

desc_ptr gdt_ptr =
{
    .limit = sizeof(gdt) - 1,
    .base = (unsigned long)&gdt,
};

gate_desc idt[256] = { };

desc_ptr idt_ptr =
{
    .limit = sizeof(idt) - 1,
    .base = (unsigned long)&idt,
};

void arch_mm_preinit(void *p)
{
    unsigned int pages;
    struct hvm_start_info *hsi = p;

    if ( hsi->version >= 1 && hsi->memmap_entries > 0 )
        e820_init_memmap((struct hvm_memmap_table_entry *)(unsigned long)
                         hsi->memmap_paddr, hsi->memmap_entries);
    else
        e820_init_memmap(NULL, 0);

    pt_base = page_table_base;
    first_free_pfn = PFN_UP(to_phys(&_end));
    pages = e820_get_current_pages();
    last_free_pfn = e820_get_maxpfn(pages);
    balloon_set_nr_pages(pages, last_free_pfn);
}
#endif

static const struct {
    unsigned int shift;
    unsigned int entries;
    pgentry_t prot;
} ptdata[PAGETABLE_LEVELS + 1] = {
    { 0, 0, 0 },
    { L1_PAGETABLE_SHIFT, L1_PAGETABLE_ENTRIES, L1_PROT },
    { L2_PAGETABLE_SHIFT, L2_PAGETABLE_ENTRIES, L2_PROT },
    { L3_PAGETABLE_SHIFT, L3_PAGETABLE_ENTRIES, L3_PROT },
#if defined(__x86_64__)
    { L4_PAGETABLE_SHIFT, L4_PAGETABLE_ENTRIES, L4_PROT },
#endif
};

static inline unsigned int idx_from_va_lvl(unsigned long va, unsigned int lvl)
{
    return (va >> ptdata[lvl].shift) & (ptdata[lvl].entries - 1);
}

/*
 * Make pt_pfn a new 'level' page table frame and hook it into the page
 * table at offset in previous level MFN (pref_l_mfn). pt_pfn is a guest
 * PFN.
 */
static void new_pt_frame(unsigned long *pt_pfn, unsigned long prev_l_mfn, 
                         unsigned long offset, unsigned long level)
{   
    pgentry_t *tab;
    unsigned long pt_page = (unsigned long)pfn_to_virt(*pt_pfn); 
#ifdef CONFIG_PARAVIRT
    mmu_update_t mmu_updates[1];
    int rc;
#endif
    
    DEBUG("Allocating new L%d pt frame for pfn=%lx, "
          "prev_l_mfn=%lx, offset=%lx", 
          level, *pt_pfn, prev_l_mfn, offset);

    /* We need to clear the page, otherwise we might fail to map it
       as a page table page */
    memset((void*) pt_page, 0, PAGE_SIZE);  

    ASSERT(level >= 1 && level <= PAGETABLE_LEVELS);

#ifdef CONFIG_PARAVIRT
    /* Make PFN a page table page */
    tab = pt_base;
#if defined(__x86_64__)
    tab = pte_to_virt(tab[l4_table_offset(pt_page)]);
#endif
    tab = pte_to_virt(tab[l3_table_offset(pt_page)]);

    mmu_updates[0].ptr = (tab[l2_table_offset(pt_page)] & PAGE_MASK) + 
        sizeof(pgentry_t) * l1_table_offset(pt_page);
    mmu_updates[0].val = (pgentry_t)pfn_to_mfn(*pt_pfn) << PAGE_SHIFT | 
        (ptdata[level].prot & ~_PAGE_RW);
    
    if ( (rc = HYPERVISOR_mmu_update(mmu_updates, 1, NULL, DOMID_SELF)) < 0 )
    {
        printk("ERROR: PTE for new page table page could not be updated\n");
        printk("       mmu_update failed with rc=%d\n", rc);
        do_exit();
    }

    /* Hook the new page table page into the hierarchy */
    mmu_updates[0].ptr =
        ((pgentry_t)prev_l_mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
    mmu_updates[0].val = (pgentry_t)pfn_to_mfn(*pt_pfn) << PAGE_SHIFT |
        ptdata[level + 1].prot;

    if ( (rc = HYPERVISOR_mmu_update(mmu_updates, 1, NULL, DOMID_SELF)) < 0 ) 
    {
        printk("ERROR: mmu_update failed with rc=%d\n", rc);
        do_exit();
    }
#else
    tab = mfn_to_virt(prev_l_mfn);
    tab[offset] = (*pt_pfn << PAGE_SHIFT) | ptdata[level + 1].prot;
#endif

    *pt_pfn += 1;
}

#ifdef CONFIG_PARAVIRT
static mmu_update_t mmu_updates[L1_PAGETABLE_ENTRIES + 1];
#endif

/*
 * Walk recursively through all PTEs calling a specified function. The function
 * is allowed to change the PTE, the walker will follow the new value.
 * The walk will cover the virtual address range [from_va .. to_va].
 * The supplied function will be called with the following parameters:
 * va: base virtual address of the area covered by the current PTE
 * lvl: page table level of the PTE (1 = lowest level, PAGETABLE_LEVELS =
 *      PTE in page table addressed by %cr3)
 * is_leaf: true if PTE doesn't address another page table (it is either at
 *          level 1, or invalid, or has its PSE bit set)
 * pte: address of the PTE
 * par: parameter, passed to walk_pt() by caller
 * Return value of func() being non-zero will terminate walk_pt(), walk_pt()
 * will return that value in this case, zero else.
 */
static int walk_pt(unsigned long from_va, unsigned long to_va,
                   int (func)(unsigned long va, unsigned int lvl,
                              bool is_leaf, pgentry_t *pte, void *par),
                   void *par)
{
    unsigned int lvl = PAGETABLE_LEVELS;
    unsigned int ptindex[PAGETABLE_LEVELS + 1];
    unsigned long va = round_pgdown(from_va);
    unsigned long va_lvl;
    pgentry_t *tab[PAGETABLE_LEVELS + 1];
    pgentry_t *pte;
    bool is_leaf;
    int ret;

    /* Start at top level page table. */
    tab[lvl] = pt_base;
    ptindex[lvl] = idx_from_va_lvl(va, lvl);

    while ( va < (to_va | (PAGE_SIZE - 1)) )
    {
        pte = tab[lvl] + ptindex[lvl];
        is_leaf = (lvl == L1_FRAME) || (*pte & _PAGE_PSE) ||
                  !(*pte & _PAGE_PRESENT);
        va_lvl = va & ~((1UL << ptdata[lvl].shift) - 1);
        ret = func(va_lvl, lvl, is_leaf, pte, par);
        if ( ret )
            return ret;

        /* PTE might have been modified by func(), reevaluate leaf state. */
        is_leaf = (lvl == L1_FRAME) || (*pte & _PAGE_PSE) ||
                  !(*pte & _PAGE_PRESENT);

        if ( is_leaf )
        {
            /* Reached a leaf PTE. Advance to next page. */
            va += 1UL << ptdata[lvl].shift;
            ptindex[lvl]++;

            /* Check for the need to traverse up again. */
            while ( ptindex[lvl] == ptdata[lvl].entries )
            {
                /* End of virtual address space? */
                if ( lvl == PAGETABLE_LEVELS )
                    return 0;
                /* Reached end of current page table, one level up. */
                lvl++;
                ptindex[lvl]++;
            }
        }
        else
        {
            /* Not a leaf, walk one level down. */
            lvl--;
            tab[lvl] = mfn_to_virt(pte_to_mfn(*pte));
            ptindex[lvl] = idx_from_va_lvl(va, lvl);
        }
    }

    return 0;
}

/*
 * Build the initial pagetable.
 */
static void build_pagetable(unsigned long *start_pfn, unsigned long *max_pfn)
{
    unsigned long start_address, end_address;
    unsigned long pfn_to_map, pt_pfn = *start_pfn;
    pgentry_t *tab = pt_base, page;
    unsigned long pt_mfn = pfn_to_mfn(virt_to_pfn(pt_base));
    unsigned long offset;
#ifdef CONFIG_PARAVIRT
    int count = 0;
    int rc;
#endif

    /* Be conservative: even if we know there will be more pages already
       mapped, start the loop at the very beginning. */
    pfn_to_map = *start_pfn;

#ifdef CONFIG_PARAVIRT
    if ( *max_pfn >= virt_to_pfn(HYPERVISOR_VIRT_START) )
    {
        printk("WARNING: Mini-OS trying to use Xen virtual space. "
               "Truncating memory from %luMB to ",
               ((unsigned long)pfn_to_virt(*max_pfn) -
                (unsigned long)&_text)>>20);
        *max_pfn = virt_to_pfn(HYPERVISOR_VIRT_START - PAGE_SIZE);
        printk("%luMB\n",
               ((unsigned long)pfn_to_virt(*max_pfn) - 
                (unsigned long)&_text)>>20);
    }
#else
    /* Round up to next 2MB boundary as we are using 2MB pages on HVMlite. */
    pfn_to_map = (pfn_to_map + L1_PAGETABLE_ENTRIES - 1) &
                 ~(L1_PAGETABLE_ENTRIES - 1);
#endif

    start_address = (unsigned long)pfn_to_virt(pfn_to_map);
    end_address = (unsigned long)pfn_to_virt(*max_pfn);

    /* We worked out the virtual memory range to map, now mapping loop */
    printk("Mapping memory range 0x%lx - 0x%lx\n", start_address, end_address);

    while ( start_address < end_address )
    {
        tab = pt_base;
        pt_mfn = pfn_to_mfn(virt_to_pfn(pt_base));

#if defined(__x86_64__)
        offset = l4_table_offset(start_address);
        /* Need new L3 pt frame */
        if ( !(tab[offset] & _PAGE_PRESENT) )
            new_pt_frame(&pt_pfn, pt_mfn, offset, L3_FRAME);

        page = tab[offset];
        pt_mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(pt_mfn) << PAGE_SHIFT);
#endif
        offset = l3_table_offset(start_address);
        /* Need new L2 pt frame */
        if ( !(tab[offset] & _PAGE_PRESENT) )
            new_pt_frame(&pt_pfn, pt_mfn, offset, L2_FRAME);

        page = tab[offset];
        pt_mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(pt_mfn) << PAGE_SHIFT);
        offset = l2_table_offset(start_address);        
#ifdef CONFIG_PARAVIRT
        /* Need new L1 pt frame */
        if ( !(tab[offset] & _PAGE_PRESENT) )
            new_pt_frame(&pt_pfn, pt_mfn, offset, L1_FRAME);

        page = tab[offset];
        pt_mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(pt_mfn) << PAGE_SHIFT);
        offset = l1_table_offset(start_address);

        if ( !(tab[offset] & _PAGE_PRESENT) )
        {
            mmu_updates[count].ptr =
                ((pgentry_t)pt_mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
            mmu_updates[count].val =
                (pgentry_t)pfn_to_mfn(pfn_to_map) << PAGE_SHIFT | L1_PROT;
            count++;
        }
        pfn_to_map++;
        if ( count == L1_PAGETABLE_ENTRIES ||
             (count && pfn_to_map == *max_pfn) )
        {
            rc = HYPERVISOR_mmu_update(mmu_updates, count, NULL, DOMID_SELF);
            if ( rc < 0 )
            {
                printk("ERROR: build_pagetable(): PTE could not be updated\n");
                printk("       mmu_update failed with rc=%d\n", rc);
                do_exit();
            }
            count = 0;
        }
        start_address += PAGE_SIZE;
#else
        if ( !(tab[offset] & _PAGE_PRESENT) )
            tab[offset] = (pgentry_t)pfn_to_map << PAGE_SHIFT |
                          L2_PROT | _PAGE_PSE;
        start_address += 1UL << L2_PAGETABLE_SHIFT;
#endif
    }

    *start_pfn = pt_pfn;
}

/*
 * Mark portion of the address space read only.
 */
extern struct shared_info shared_info;

struct change_readonly_par {
    unsigned long etext;
#ifdef CONFIG_PARAVIRT
    unsigned int count;
#endif
    bool readonly;
};

static int change_readonly_func(unsigned long va, unsigned int lvl,
                                bool is_leaf, pgentry_t *pte, void *par)
{
    struct change_readonly_par *ro = par;
    pgentry_t newval;

    if ( !is_leaf )
        return 0;

    if ( va + (1UL << ptdata[lvl].shift) > ro->etext )
        return 1;

    if ( va == (unsigned long)&shared_info )
    {
        printk("skipped %lx\n", va);
        return 0;
    }

    newval = ro->readonly ? (*pte & ~_PAGE_RW) : (*pte | _PAGE_RW);

#ifdef CONFIG_PARAVIRT
    mmu_updates[ro->count].ptr = virt_to_mach(pte);
    mmu_updates[ro->count].val = newval;
    ro->count++;

    if ( ro->count == L1_PAGETABLE_ENTRIES )
    {
         if ( HYPERVISOR_mmu_update(mmu_updates, ro->count, NULL,
                                    DOMID_SELF) < 0 )
             BUG();
         ro->count = 0;
    }
#else
    *pte = newval;
#endif

    return 0;
}

#ifdef CONFIG_PARAVIRT
static void tlb_flush(void)
{
    mmuext_op_t op = { .cmd = MMUEXT_TLB_FLUSH_ALL };
    int count;

    HYPERVISOR_mmuext_op(&op, 1, &count, DOMID_SELF);
}
#else
static void tlb_flush(void)
{
    write_cr3((unsigned long)pt_base);
}
#endif

/*
 * get the PTE for virtual address va if it exists. Otherwise NULL.
 */
static int get_pgt_func(unsigned long va, unsigned int lvl, bool is_leaf,
                        pgentry_t *pte, void *par)
{
    pgentry_t **result;

    if ( !(*pte & _PAGE_PRESENT) && lvl > L1_FRAME )
        return -1;

    if ( lvl > L1_FRAME && !(*pte & _PAGE_PSE) )
        return 0;

    result = par;
    *result = pte;

    return 0;
}

static pgentry_t *get_pgt(unsigned long va)
{
    pgentry_t *tab = NULL;

    walk_pt(va, va, get_pgt_func, &tab);
    return tab;
}

void change_readonly(bool readonly)
{
    struct change_readonly_par ro = {
        .etext = (unsigned long)&_erodata,
        .readonly = readonly,
    };
    unsigned long start_address = PAGE_ALIGN((unsigned long)&_text);
#ifdef CONFIG_PARAVIRT
    pte_t nullpte = { };
    int rc;
#else
    pgentry_t *pgt = get_pgt((unsigned long)&_text);
#endif

    if ( readonly )
    {
#ifdef CONFIG_PARAVIRT
        if ( (rc = HYPERVISOR_update_va_mapping(0, nullpte, UVMF_INVLPG)) )
            printk("Unable to unmap NULL page. rc=%d\n", rc);
#else
        *pgt = 0;
        invlpg((unsigned long)&_text);
#endif
    }
    else
    {
#ifdef CONFIG_PARAVIRT
        /* No kexec support with PARAVIRT. */
        BUG();
#else
        *pgt = L1_PROT;
#endif
    }

    printk("setting %p-%p %s\n", &_text, &_erodata,
           readonly ? "readonly" : "writable");
    walk_pt(start_address, ro.etext, change_readonly_func, &ro);

#ifdef CONFIG_PARAVIRT
    if ( ro.count &&
         HYPERVISOR_mmu_update(mmu_updates, ro.count, NULL, DOMID_SELF) < 0)
        BUG();
#endif

    tlb_flush();
}

/*
 * return a valid PTE for a given virtual address. If PTE does not exist,
 * allocate page-table pages.
 */
static int need_pgt_func(unsigned long va, unsigned int lvl, bool is_leaf,
                         pgentry_t *pte, void *par)
{
    pgentry_t **result = par;
    unsigned long pt_mfn;
    unsigned long pt_pfn;
    unsigned int idx;

    if ( !is_leaf )
        return 0;

    if ( lvl == L1_FRAME || (*pte & _PAGE_PRESENT) )
    {
        /*
         * The PTE is not addressing a page table (is_leaf is true). If we are
         * either at the lowest level or we have a valid large page, we don't
         * need to allocate a page table.
         */
        ASSERT(lvl == L1_FRAME || (*pte & _PAGE_PSE));
        *result = pte;
        return 1;
    }

    pt_mfn = virt_to_mfn(pte);
    pt_pfn = virt_to_pfn(alloc_page());
    if ( !pt_pfn )
        return -1;
    idx = idx_from_va_lvl(va, lvl);
    new_pt_frame(&pt_pfn, pt_mfn, idx, lvl - 1);

    return 0;
}

pgentry_t *need_pgt(unsigned long va)
{
    pgentry_t *tab = NULL;

    walk_pt(va, va, need_pgt_func, &tab);
    return tab;
}
EXPORT_SYMBOL(need_pgt);

/*
 * Reserve an area of virtual address space for mappings and Heap
 */
static unsigned long demand_map_area_start;
static unsigned long demand_map_area_end;
#ifdef HAVE_LIBC
unsigned long heap, brk, heap_mapped, heap_end;
#endif

void arch_init_demand_mapping_area(void)
{
    demand_map_area_start = VIRT_DEMAND_AREA;
    demand_map_area_end = demand_map_area_start + DEMAND_MAP_PAGES * PAGE_SIZE;
    printk("Demand map pfns at %lx-%lx.\n", demand_map_area_start,
           demand_map_area_end);

#ifdef HAVE_LIBC
    heap_mapped = brk = heap = VIRT_HEAP_AREA;
    heap_end = heap_mapped + HEAP_PAGES * PAGE_SIZE;
    printk("Heap resides at %lx-%lx.\n", brk, heap_end);
#endif
}

unsigned long allocate_ondemand(unsigned long n, unsigned long alignment)
{
    unsigned long x;
    unsigned long y = 0;

    /* Find a properly aligned run of n contiguous frames */
    for ( x = 0;
          x <= DEMAND_MAP_PAGES - n; 
          x = (x + y + 1 + alignment - 1) & ~(alignment - 1) )
    {
        unsigned long addr = demand_map_area_start + x * PAGE_SIZE;
        pgentry_t *pgt = get_pgt(addr);
        for ( y = 0; y < n; y++, addr += PAGE_SIZE ) 
        {
            if ( !(addr & L1_MASK) )
                pgt = get_pgt(addr);
            if ( pgt )
            {
                if ( *pgt & _PAGE_PRESENT )
                    break;
                pgt++;
            }
        }
        if ( y == n )
            break;
    }
    if ( y != n )
    {
        printk("Failed to find %ld frames!\n", n);
        return 0;
    }
    return demand_map_area_start + x * PAGE_SIZE;
}

/*
 * Map an array of MFNs contiguously into virtual address space starting at
 * va. map f[i*stride]+i*increment for i in 0..n-1.
 */
#define MAP_BATCH ((STACK_SIZE / 2) / sizeof(mmu_update_t))
int do_map_frames(unsigned long va,
                  const unsigned long *mfns, unsigned long n,
                  unsigned long stride, unsigned long incr,
                  domid_t id, int *err, unsigned long prot)
{
    pgentry_t *pgt = NULL;
    unsigned long done = 0;

    if ( !mfns ) 
    {
        printk("do_map_frames: no mfns supplied\n");
        return -EINVAL;
    }
    DEBUG("va=%p n=0x%lx, mfns[0]=0x%lx stride=0x%lx incr=0x%lx prot=0x%lx\n",
          va, n, mfns[0], stride, incr, prot);

    if ( err )
        memset(err, 0x00, n * sizeof(int));
    while ( done < n )
    {
#ifdef CONFIG_PARAVIRT
        unsigned long i;
        int rc;
        unsigned long todo;

        if ( err )
            todo = 1;
        else
            todo = n - done;

        if ( todo > MAP_BATCH )
            todo = MAP_BATCH;

        {
            mmu_update_t mmu_updates[todo];

            for ( i = 0; i < todo; i++, va += PAGE_SIZE, pgt++) 
            {
                if ( !pgt || !(va & L1_MASK) )
                    pgt = need_pgt(va);
                if ( !pgt )
                    return -ENOMEM;

                mmu_updates[i].ptr = virt_to_mach(pgt) | MMU_NORMAL_PT_UPDATE;
                mmu_updates[i].val = ((pgentry_t)(mfns[(done + i) * stride] +
                                                  (done + i) * incr)
                                      << PAGE_SHIFT) | prot;
            }

            rc = HYPERVISOR_mmu_update(mmu_updates, todo, NULL, id);
            if ( rc < 0 )
            {
                if (err)
                    err[done * stride] = rc;
                else {
                    printk("Map %ld (%lx, ...) at %lx failed: %d.\n",
                           todo, mfns[done * stride] + done * incr, va, rc);
                    do_exit();
                }
            }
        }
        done += todo;
#else
        if ( !pgt || !(va & L1_MASK) )
            pgt = need_pgt(va & ~L1_MASK);
        if ( !pgt )
            return -ENOMEM;

        ASSERT(!(*pgt & _PAGE_PSE));
        pgt[l1_table_offset(va)] = (pgentry_t)
            (((mfns[done * stride] + done * incr) << PAGE_SHIFT) | prot);
        done++;
        va += PAGE_SIZE;
#endif
    }

    return 0;
}
EXPORT_SYMBOL(do_map_frames);

/*
 * Map an array of MFNs contiguous into virtual address space. Virtual
 * addresses are allocated from the on demand area.
 */
void *map_frames_ex(const unsigned long *mfns, unsigned long n, 
                    unsigned long stride, unsigned long incr,
                    unsigned long alignment,
                    domid_t id, int *err, unsigned long prot)
{
    unsigned long va = allocate_ondemand(n, alignment);

    if ( !va )
        return NULL;

    if ( do_map_frames(va, mfns, n, stride, incr, id, err, prot) )
        return NULL;

    return (void *)va;
}
EXPORT_SYMBOL(map_frames_ex);

/*
 * Unmap nun_frames frames mapped at virtual address va.
 */
#define UNMAP_BATCH ((STACK_SIZE / 2) / sizeof(multicall_entry_t))
int unmap_frames(unsigned long va, unsigned long num_frames)
{
#ifdef CONFIG_PARAVIRT
    int n = UNMAP_BATCH;
    multicall_entry_t call[n];
    int ret;
    int i;
#else
    pgentry_t *pgt;
#endif

    ASSERT(!((unsigned long)va & ~PAGE_MASK));

    DEBUG("va=%p, num=0x%lx\n", va, num_frames);

    while ( num_frames ) {
#ifdef CONFIG_PARAVIRT
        if ( n > num_frames )
            n = num_frames;

        for ( i = 0; i < n; i++ )
        {
            int arg = 0;
            /* simply update the PTE for the VA and invalidate TLB */
            call[i].op = __HYPERVISOR_update_va_mapping;
            call[i].args[arg++] = va;
            call[i].args[arg++] = 0;
#ifdef __i386__
            call[i].args[arg++] = 0;
#endif  
            call[i].args[arg++] = UVMF_INVLPG;

            va += PAGE_SIZE;
        }

        ret = HYPERVISOR_multicall(call, n);
        if ( ret )
        {
            printk("update_va_mapping hypercall failed with rc=%d.\n", ret);
            return -ret;
        }

        for ( i = 0; i < n; i++ )
        {
            if ( call[i].result ) 
            {
                printk("update_va_mapping failed for with rc=%d.\n", ret);
                return -(call[i].result);
            }
        }
        num_frames -= n;
#else
        pgt = get_pgt(va);
        if ( pgt )
        {
            ASSERT(!(*pgt & _PAGE_PSE));
            *pgt = 0;
            invlpg(va);
        }
        va += PAGE_SIZE;
        num_frames--;
#endif
    }
    return 0;
}
EXPORT_SYMBOL(unmap_frames);

#ifdef CONFIG_PARAVIRT
void p2m_chk_pfn(unsigned long pfn)
{
    if ( (pfn >> L3_P2M_SHIFT) > 0 )
    {
        printk("Error: Too many pfns.\n");
        do_exit();
    }
}

void arch_init_p2m(unsigned long max_pfn)
{
    unsigned long *l2_list = NULL, *l3_list;
    unsigned long pfn;
    
    p2m_chk_pfn(max_pfn - 1);
    l3_list = (unsigned long *)alloc_page(); 
    for ( pfn = 0; pfn < max_pfn; pfn += P2M_ENTRIES )
    {
        if ( !(pfn % (P2M_ENTRIES * P2M_ENTRIES)) )
        {
            l2_list = (unsigned long*)alloc_page();
            l3_list[L3_P2M_IDX(pfn)] = virt_to_mfn(l2_list);
        }
        l2_list[L2_P2M_IDX(pfn)] = virt_to_mfn(phys_to_machine_mapping + pfn);
    }
    HYPERVISOR_shared_info->arch.pfn_to_mfn_frame_list_list = 
        virt_to_mfn(l3_list);
    HYPERVISOR_shared_info->arch.max_pfn = max_pfn;

    arch_remap_p2m(max_pfn);
}

void arch_mm_pre_suspend(void)
{
    /* TODO: Pre suspend arch specific operations. */
}

void arch_mm_post_suspend(int canceled)
{
    /* TODO: Post suspend arch specific operations. */
}
#else
void arch_mm_pre_suspend(void){ }

void arch_mm_post_suspend(int canceled){ }
#endif

void arch_init_mm(unsigned long* start_pfn_p, unsigned long* max_pfn_p)
{
    unsigned long start_pfn, max_pfn;

    printk("      _text: %p(VA)\n", &_text);
    printk("     _etext: %p(VA)\n", &_etext);
    printk("   _erodata: %p(VA)\n", &_erodata);
    printk("     _edata: %p(VA)\n", &_edata);
    printk("stack start: %p(VA)\n", stack);
    printk("       _end: %p(VA)\n", &_end);

    /* First page follows page table pages. */
    start_pfn = first_free_pfn;
    max_pfn = last_free_pfn;

    if ( max_pfn >= MAX_MEM_SIZE / PAGE_SIZE )
        max_pfn = MAX_MEM_SIZE / PAGE_SIZE - 1;

    printk("  start_pfn: %lx\n", start_pfn);
    printk("    max_pfn: %lx\n", max_pfn);

    build_pagetable(&start_pfn, &max_pfn);

    /* Prepare page 0 as CoW page. */
    memset(&_text, 0, PAGE_SIZE);
    mfn_zero = virt_to_mfn((unsigned long)&_text);

    change_readonly(true);

    *start_pfn_p = start_pfn;
    *max_pfn_p = max_pfn;

#ifndef CONFIG_PARAVIRT
#ifdef __x86_64__
    BUILD_BUG_ON(l4_table_offset(VIRT_KERNEL_AREA) != 1 ||
                 l3_table_offset(VIRT_KERNEL_AREA) != 0 ||
                 l2_table_offset(VIRT_KERNEL_AREA) != 0);
#else
    BUILD_BUG_ON(l3_table_offset(VIRT_KERNEL_AREA) != 0 ||
                 l2_table_offset(VIRT_KERNEL_AREA) == 0);
#endif
#endif
}

unsigned long alloc_virt_kernel(unsigned n_pages)
{
    unsigned long addr;

    addr = virt_kernel_area_end;
    virt_kernel_area_end += PAGE_SIZE * n_pages;
    ASSERT(virt_kernel_area_end <= VIRT_DEMAND_AREA);

    return addr;
}

unsigned long map_frame_virt(unsigned long mfn)
{
    unsigned long addr;

    addr = alloc_virt_kernel(1);
    if ( map_frame_rw(addr, mfn) )
        return 0;

    return addr;
}
EXPORT_SYMBOL(map_frame_virt);

#ifdef CONFIG_KEXEC
static unsigned long kexec_gdt;
static unsigned long kexec_idt;

static int move_pt(unsigned long va, unsigned int lvl, bool is_leaf,
                   pgentry_t *pte, void *par)
{
    unsigned long boundary_pfn = *(unsigned long *)par;
    unsigned long pfn;
    void *old_pg, *new_pg;

    if ( is_leaf )
        return 0;

    pfn = (lvl == PAGETABLE_LEVELS + 1) ? PHYS_PFN(*(unsigned long *)pte)
                                        : pte_to_mfn(*pte);
    if ( pfn >= boundary_pfn )
        return 0;

    new_pg = (void *)alloc_page();
    if ( !new_pg )
        return ENOMEM;
    old_pg = pfn_to_virt(pfn);
    memcpy(new_pg, old_pg, PAGE_SIZE);
    if ( lvl == PAGETABLE_LEVELS + 1 )
        *(pgentry_t **)pte = new_pg;
    else
        *pte = ((unsigned long)new_pg & PAGE_MASK) | ptdata[lvl].prot;

    tlb_flush();

    free_page(old_pg);

    return 0;
}

static int move_leaf(unsigned long va, unsigned int lvl, bool is_leaf,
                     pgentry_t *pte, void *par)
{
    unsigned long boundary_pfn = *(unsigned long *)par;
    unsigned long pfn;
    void *old_pg, *new_pg;

    if ( !is_leaf )
        return 0;

    /* No large page support, all pages must be valid. */
    if ( (*pte & _PAGE_PSE) || !(*pte & _PAGE_PRESENT) )
        return EINVAL;

    pfn = pte_to_mfn(*pte);
    if ( pfn >= boundary_pfn )
        return 0;

    new_pg = (void *)alloc_page();
    if ( !new_pg )
        return ENOMEM;
    old_pg = pfn_to_virt(pfn);
    memcpy(new_pg, old_pg, PAGE_SIZE);
    *pte = ((unsigned long)new_pg & PAGE_MASK) | ptdata[lvl].prot;

    invlpg(va);

    free_page(old_pg);

    return 0;
}

int kexec_move_used_pages(unsigned long boundary, unsigned long kernel,
                          unsigned long kernel_size)
{
    int ret;
    unsigned long boundary_pfn = PHYS_PFN(boundary);

    kexec_gdt = alloc_page();
    if ( !kexec_gdt )
        return ENOMEM;
    memcpy((char *)kexec_gdt, &gdt, sizeof(gdt));
    gdt_ptr.base = kexec_gdt;
    asm volatile("lgdt %0" : : "m" (gdt_ptr));

    kexec_idt = alloc_page();
    if ( !kexec_idt )
        return ENOMEM;
    memcpy((char *)kexec_idt, &idt, sizeof(idt));
    idt_ptr.base = kexec_idt;
    asm volatile("lidt %0" : : "m" (idt_ptr));

    /* Top level page table needs special handling. */
    ret = move_pt(0, PAGETABLE_LEVELS + 1, false, (pgentry_t *)(&pt_base),
                  &boundary_pfn);
    if ( ret )
        return ret;
    ret = walk_pt(0, ~0UL, move_pt, &boundary_pfn);
    if ( ret )
        return ret;

    /* Move new kernel image pages. */
    ret = walk_pt(kernel, kernel + kernel_size - 1, move_leaf, &boundary_pfn);
    if ( ret )
        return ret;

    return 0;
}

void kexec_move_used_pages_undo(void)
{
    if ( kexec_gdt )
    {
        gdt_ptr.base = (unsigned long)&gdt;
        asm volatile("lgdt %0" : : "m" (gdt_ptr));
        free_page((void *)kexec_gdt);
        kexec_gdt = 0;
    }

    if ( kexec_idt )
    {
        idt_ptr.base = (unsigned long)&idt;
        asm volatile("lidt %0" : : "m" (idt_ptr));
        free_page((void *)kexec_idt);
        kexec_idt = 0;
    }
}
#endif
