/*
 * Minimal 9pfs PV frontend for Mini-OS.
 * Copyright (c) 2023 Juergen Gross, SUSE Software Solution GmbH
 */

#include <mini-os/os.h>
#include <mini-os/lib.h>
#include <mini-os/events.h>
#include <mini-os/gnttab.h>
#include <mini-os/xenbus.h>
#include <mini-os/xmalloc.h>
#include <errno.h>
#include <xen/io/9pfs.h>
#include <mini-os/9pfront.h>

#ifdef HAVE_LIBC
struct dev_9pfs {
    int id;
    char nodename[20];
    unsigned int dom;
    char *backend;

    char *tag;
    const char *mnt;

    struct xen_9pfs_data_intf *intf;
    struct xen_9pfs_data data;
    RING_IDX prod_pvt_out;
    RING_IDX cons_pvt_in;

    grant_ref_t ring_ref;
    evtchn_port_t evtchn;
    unsigned int ring_order;
    xenbus_event_queue events;
};

#define DEFAULT_9PFS_RING_ORDER  4

static unsigned int ftype_9pfs;

static void intr_9pfs(evtchn_port_t port, struct pt_regs *regs, void *data)
{
}

static int open_9pfs(struct mount_point *mnt, const char *pathname, int flags,
                     mode_t mode)
{
    errno = ENOSYS;

    return -1;
}

static void free_9pfront(struct dev_9pfs *dev)
{
    unsigned int i;

    if ( dev->data.in && dev->intf )
    {
        for ( i = 0; i < (1 << dev->ring_order); i++ )
            gnttab_end_access(dev->intf->ref[i]);
        free_pages(dev->data.in, dev->ring_order);
    }
    unbind_evtchn(dev->evtchn);
    gnttab_end_access(dev->ring_ref);
    free_page(dev->intf);
    free(dev->backend);
    free(dev->tag);
    free(dev);
}

void *init_9pfront(unsigned int id, const char *mnt)
{
    struct dev_9pfs *dev;
    char *msg;
    char *reason = "";
    xenbus_transaction_t xbt;
    int retry = 1;
    char bepath[64] = { 0 };
    XenbusState state;
    unsigned int i;
    void *addr;
    char *version;
    char *v;

    printk("9pfsfront add %u, for mount at %s\n", id, mnt);
    dev = malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->nodename, sizeof(dev->nodename), "device/9pfs/%u", id);
    dev->id = id;

    msg = xenbus_read_unsigned(XBT_NIL, dev->nodename, "backend-id", &dev->dom);
    if ( msg )
        goto err;
    msg = xenbus_read_string(XBT_NIL, dev->nodename, "backend", &dev->backend);
    if ( msg )
        goto err;
    msg = xenbus_read_string(XBT_NIL, dev->nodename, "tag", &dev->tag);
    if ( msg )
        goto err;

    snprintf(bepath, sizeof(bepath), "%s/state", dev->backend);
    free(xenbus_watch_path_token(XBT_NIL, bepath, bepath, &dev->events));
    state = xenbus_read_integer(bepath);
    while ( msg == NULL && state < XenbusStateInitWait )
        msg = xenbus_wait_for_state_change(bepath, &state, &dev->events);
    if ( msg || state != XenbusStateInitWait )
    {
        reason = "illegal backend state";
        goto err;
    }

    msg = xenbus_read_unsigned(XBT_NIL, dev->backend, "max-ring-page-order",
                               &dev->ring_order);
    if ( msg )
        goto err;
    if ( dev->ring_order > DEFAULT_9PFS_RING_ORDER )
        dev->ring_order = DEFAULT_9PFS_RING_ORDER;

    msg = xenbus_read_string(XBT_NIL, dev->backend, "versions", &version);
    if ( msg )
        goto err;
    for ( v = version; *v; v++ )
    {
        if ( strtoul(v, &v, 10) == 1 && (*v == ',' || *v == 0) )
        {
            v = NULL;
            break;
        }
        if ( *v != ',' && *v != 0 )
        {
            reason = "backend published illegal version string";
            free(version);
            goto err;
        }
    }
    free(version);
    if ( v )
    {
        reason = "backend doesn't support version 1";
        goto err;
    }

    dev->ring_ref = gnttab_alloc_and_grant((void **)&dev->intf);
    memset(dev->intf, 0, PAGE_SIZE);
    if ( evtchn_alloc_unbound(dev->dom, intr_9pfs, dev, &dev->evtchn) )
    {
        reason = "no event channel";
        goto err;
    }
    dev->intf->ring_order = dev->ring_order;
    dev->data.in = (void *)alloc_pages(dev->ring_order);
    dev->data.out = dev->data.in + XEN_FLEX_RING_SIZE(dev->ring_order);
    for ( i = 0; i < (1 << dev->ring_order); i++ )
    {
        addr = dev->data.in + i * PAGE_SIZE;
        dev->intf->ref[i] = gnttab_grant_access(dev->dom, virt_to_mfn(addr), 0);
    }

    while ( retry )
    {
        msg = xenbus_transaction_start(&xbt);
        if ( msg )
        {
            free(msg);
            msg = NULL;
            reason = "starting transaction";
            goto err;
        }

        msg = xenbus_printf(xbt, dev->nodename, "version", "%u", 1);
        if ( msg )
            goto err_tr;
        msg = xenbus_printf(xbt, dev->nodename, "num-rings", "%u", 1);
        if ( msg )
            goto err_tr;
        msg = xenbus_printf(xbt, dev->nodename, "ring-ref0", "%u",
                            dev->ring_ref);
        if ( msg )
            goto err_tr;
        msg = xenbus_printf(xbt, dev->nodename, "event-channel-0", "%u",
                            dev->evtchn);
        if ( msg )
            goto err_tr;
        msg = xenbus_printf(xbt, dev->nodename, "state", "%u",
                            XenbusStateInitialised);
        if ( msg )
            goto err_tr;

        free(xenbus_transaction_end(xbt, 0, &retry));
    }

    state = xenbus_read_integer(bepath);
    while ( msg == NULL && state < XenbusStateConnected )
        msg = xenbus_wait_for_state_change(bepath, &state, &dev->events);
    if ( msg || state != XenbusStateConnected )
    {
        reason = "illegal backend state";
        goto err;
    }

    msg = xenbus_printf(XBT_NIL, dev->nodename, "state", "%u",
                        XenbusStateConnected);
    if ( msg )
        goto err;

    unmask_evtchn(dev->evtchn);

    dev->mnt = mnt;
    if ( mount(dev->mnt, dev, open_9pfs) )
    {
        reason = "mount failed";
        goto err;
    }

    return dev;

 err_tr:
    free(xenbus_transaction_end(xbt, 1, &retry));

 err:
    if ( bepath[0] )
        free(xenbus_unwatch_path_token(XBT_NIL, bepath, bepath));
    if ( msg )
        printk("9pfsfront add %u failed, error %s accessing Xenstore\n",
               id, msg);
    else
        printk("9pfsfront add %u failed, %s\n", id, reason);
    free_9pfront(dev);
    free(msg);
    return NULL;
}

void shutdown_9pfront(void *dev)
{
    struct dev_9pfs *dev9p = dev;
    char bepath[64];
    XenbusState state;
    char *msg;
    char *reason = "";

    umount(dev9p->mnt);
    snprintf(bepath, sizeof(bepath), "%s/state", dev9p->backend);

    msg = xenbus_printf(XBT_NIL, dev9p->nodename, "state", "%u",
                        XenbusStateClosing);
    if ( msg )
        goto err;

    state = xenbus_read_integer(bepath);
    while ( msg == NULL && state < XenbusStateClosing)
        msg = xenbus_wait_for_state_change(bepath, &state, &dev9p->events);
    if ( msg || state != XenbusStateClosing )
    {
        reason = "illegal backend state";
        goto err;
    }

    msg = xenbus_printf(XBT_NIL, dev9p->nodename, "state", "%u",
                        XenbusStateClosed);
    if ( msg )
        goto err;

    free_9pfront(dev9p);

    return;

 err:
    if ( msg )
        printk("9pfsfront shutdown %u failed, error %s accessing Xenstore\n",
               dev9p->id, msg);
    else
        printk("9pfsfront shutdown %u failed, %s\n", dev9p->id, reason);
    free(msg);
}

static const struct file_ops ops_9pfs = {
    .name = "9pfs",
};

__attribute__((constructor))
static void initialize_9pfs(void)
{
    ftype_9pfs = alloc_file_type(&ops_9pfs);
}

#endif /* HAVE_LIBC */
