/* 
 ****************************************************************************
 * (C) 2006 - Cambridge University
 ****************************************************************************
 *
 *        File: gnttab.c
 *      Author: Steven Smith (sos22@cam.ac.uk) 
 *     Changes: Grzegorz Milos (gm281@cam.ac.uk)
 *              
 *        Date: July 2006
 * 
 * Environment: Xen Minimal OS
 * Description: Simple grant tables implementation. About as stupid as it's
 *  possible to be and still work.
 *
 ****************************************************************************
 */
#include <mini-os/os.h>
#include <mini-os/mm.h>
#include <mini-os/gnttab.h>
#include <mini-os/semaphore.h>

#define NR_RESERVED_ENTRIES 8

/* NR_GRANT_FRAMES must be less than or equal to that configured in Xen */
#define NR_GRANT_FRAMES 4
#define NR_GRANT_ENTRIES (NR_GRANT_FRAMES * PAGE_SIZE / sizeof(grant_entry_v1_t))

static grant_entry_v1_t *gnttab_table;
static grant_ref_t gnttab_list[NR_GRANT_ENTRIES];
#ifdef GNT_DEBUG
static char inuse[NR_GRANT_ENTRIES];
#endif
static __DECLARE_SEMAPHORE_GENERIC(gnttab_sem, 0);

static void
put_free_entry(grant_ref_t ref)
{
    unsigned long flags;
    local_irq_save(flags);
#ifdef GNT_DEBUG
    BUG_ON(!inuse[ref]);
    inuse[ref] = 0;
#endif
    gnttab_list[ref] = gnttab_list[0];
    gnttab_list[0]  = ref;
    local_irq_restore(flags);
    up(&gnttab_sem);
}

static grant_ref_t
get_free_entry(void)
{
    unsigned int ref;
    unsigned long flags;
    down(&gnttab_sem);
    local_irq_save(flags);
    ref = gnttab_list[0];
    BUG_ON(ref < NR_RESERVED_ENTRIES || ref >= NR_GRANT_ENTRIES);
    gnttab_list[0] = gnttab_list[ref];
#ifdef GNT_DEBUG
    BUG_ON(inuse[ref]);
    inuse[ref] = 1;
#endif
    local_irq_restore(flags);
    return ref;
}

grant_ref_t
gnttab_grant_access(domid_t domid, unsigned long frame, int readonly)
{
    grant_ref_t ref;

    ref = get_free_entry();
    gnttab_table[ref].frame = frame;
    gnttab_table[ref].domid = domid;
    wmb();
    readonly *= GTF_readonly;
    gnttab_table[ref].flags = GTF_permit_access | readonly;

    return ref;
}
EXPORT_SYMBOL(gnttab_grant_access);

grant_ref_t
gnttab_grant_transfer(domid_t domid, unsigned long pfn)
{
    grant_ref_t ref;

    ref = get_free_entry();
    gnttab_table[ref].frame = pfn;
    gnttab_table[ref].domid = domid;
    wmb();
    gnttab_table[ref].flags = GTF_accept_transfer;

    return ref;
}
EXPORT_SYMBOL(gnttab_grant_transfer);

int
gnttab_end_access(grant_ref_t ref)
{
    uint16_t flags, nflags;

    BUG_ON(ref >= NR_GRANT_ENTRIES || ref < NR_RESERVED_ENTRIES);

    nflags = gnttab_table[ref].flags;
    do {
        if ((flags = nflags) & (GTF_reading|GTF_writing)) {
            printk("WARNING: g.e. still in use! (%x)\n", flags);
            return 0;
        }
    } while ((nflags = synch_cmpxchg(&gnttab_table[ref].flags, flags, 0)) !=
            flags);

    put_free_entry(ref);
    return 1;
}
EXPORT_SYMBOL(gnttab_end_access);

unsigned long
gnttab_end_transfer(grant_ref_t ref)
{
    unsigned long frame;
    uint16_t flags;

    BUG_ON(ref >= NR_GRANT_ENTRIES || ref < NR_RESERVED_ENTRIES);

    while (!((flags = gnttab_table[ref].flags) & GTF_transfer_committed)) {
        if (synch_cmpxchg(&gnttab_table[ref].flags, flags, 0) == flags) {
            printk("Release unused transfer grant.\n");
            put_free_entry(ref);
            return 0;
        }
    }

    /* If a transfer is in progress then wait until it is completed. */
    while (!(flags & GTF_transfer_completed)) {
        flags = gnttab_table[ref].flags;
    }

    /* Read the frame number /after/ reading completion status. */
    rmb();
    frame = gnttab_table[ref].frame;

    put_free_entry(ref);

    return frame;
}
EXPORT_SYMBOL(gnttab_end_transfer);

grant_ref_t
gnttab_alloc_and_grant(void **map)
{
    unsigned long mfn;
    grant_ref_t gref;

    *map = (void *)alloc_page();
    mfn = virt_to_mfn(*map);
    gref = gnttab_grant_access(0, mfn, 0);
    return gref;
}
EXPORT_SYMBOL(gnttab_alloc_and_grant);

static const char * const gnttabop_error_msgs[] = GNTTABOP_error_msgs;

const char *
gnttabop_error(int16_t status)
{
    status = -status;
    if (status < 0 || status >= ARRAY_SIZE(gnttabop_error_msgs))
        return "bad status";
    else
        return gnttabop_error_msgs[status];
}
EXPORT_SYMBOL(gnttabop_error);

void
init_gnttab(void)
{
    int i;

#ifdef GNT_DEBUG
    memset(inuse, 1, sizeof(inuse));
#endif
    for (i = NR_RESERVED_ENTRIES; i < NR_GRANT_ENTRIES; i++)
        put_free_entry(i);

    gnttab_table = arch_init_gnttab(NR_GRANT_FRAMES);
    printk("gnttab_table mapped at %p.\n", gnttab_table);
}

void
fini_gnttab(void)
{
    struct gnttab_setup_table setup;

    setup.dom = DOMID_SELF;
    setup.nr_frames = 0;

    HYPERVISOR_grant_table_op(GNTTABOP_setup_table, &setup, 1);
}

void suspend_gnttab(void)
{
    arch_suspend_gnttab(gnttab_table, NR_GRANT_FRAMES);
}

void resume_gnttab(void)
{
    arch_resume_gnttab(gnttab_table, NR_GRANT_FRAMES);
}
