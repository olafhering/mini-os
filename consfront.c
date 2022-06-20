#include <mini-os/types.h>
#include <mini-os/wait.h>
#include <mini-os/mm.h>
#include <mini-os/hypervisor.h>
#include <mini-os/events.h>
#include <mini-os/os.h>
#include <mini-os/lib.h>
#include <mini-os/console.h>
#include <mini-os/xenbus.h>
#include <xen/io/console.h>
#include <xen/io/protocols.h>
#include <xen/io/ring.h>
#include <mini-os/xmalloc.h>
#include <mini-os/gnttab.h>

void free_consfront(struct consfront_dev *dev)
{
    char* err = NULL;
    XenbusState state;

    char path[strlen(dev->backend) + strlen("/state") + 1];
    char nodename[strlen(dev->nodename) + strlen("/state") + 1];

    snprintf(path, sizeof(path), "%s/state", dev->backend);
    snprintf(nodename, sizeof(nodename), "%s/state", dev->nodename);

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateClosing)) != NULL) {
        printk("free_consfront: error changing state to %d: %s\n",
                XenbusStateClosing, err);
        goto close;
    }
    state = xenbus_read_integer(path);
    while (err == NULL && state < XenbusStateClosing)
        err = xenbus_wait_for_state_change(path, &state, &dev->events);
    free(err);

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateClosed)) != NULL) {
        printk("free_consfront: error changing state to %d: %s\n",
                XenbusStateClosed, err);
        goto close;
    }

close:
    free(err);
    err = xenbus_unwatch_path_token(XBT_NIL, path, path);
    free(err);

    mask_evtchn(dev->evtchn);
    unbind_evtchn(dev->evtchn);
    free(dev->backend);
    free(dev->nodename);

    gnttab_end_access(dev->ring_ref);

    free_page(dev->ring);
    free(dev);
}

struct consfront_dev *init_consfront(char *_nodename)
{
    xenbus_transaction_t xbt;
    char* err = NULL;
    char* message=NULL;
    int retry=0;
    char* msg = NULL;
    char nodename[256];
    char path[256];
    static int consfrontends = 3;
    struct consfront_dev *dev;
    int res;

    if (!_nodename)
        snprintf(nodename, sizeof(nodename), "device/console/%d", consfrontends);
    else {
        strncpy(nodename, _nodename, sizeof(nodename) - 1);
        nodename[sizeof(nodename) - 1] = 0;
    }

    printk("******************* CONSFRONT for %s **********\n\n\n", nodename);

    consfrontends++;
    dev = malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->nodename = strdup(nodename);
#ifdef HAVE_LIBC
    dev->fd = -1;
#endif

    snprintf(path, sizeof(path), "%s/backend-id", nodename);
    if ((res = xenbus_read_integer(path)) < 0) 
        goto error;
    else
        dev->dom = res;
    evtchn_alloc_unbound(dev->dom, console_handle_input, dev, &dev->evtchn);

    dev->ring = (struct xencons_interface *) alloc_page();
    memset(dev->ring, 0, PAGE_SIZE);
    dev->ring_ref = gnttab_grant_access(dev->dom, virt_to_mfn(dev->ring), 0);

    dev->events = NULL;

again:
    err = xenbus_transaction_start(&xbt);
    if (err) {
        printk("starting transaction\n");
        free(err);
    }

    err = xenbus_printf(xbt, nodename, "ring-ref","%u",
                dev->ring_ref);
    if (err) {
        message = "writing ring-ref";
        goto abort_transaction;
    }
    err = xenbus_printf(xbt, nodename,
                "port", "%u", dev->evtchn);
    if (err) {
        message = "writing event-channel";
        goto abort_transaction;
    }
    err = xenbus_printf(xbt, nodename,
                "protocol", "%s", XEN_IO_PROTO_ABI_NATIVE);
    if (err) {
        message = "writing protocol";
        goto abort_transaction;
    }

    snprintf(path, sizeof(path), "%s/state", nodename);
    err = xenbus_switch_state(xbt, path, XenbusStateConnected);
    if (err) {
        message = "switching state";
        goto abort_transaction;
    }


    err = xenbus_transaction_end(xbt, 0, &retry);
    free(err);
    if (retry) {
            goto again;
        printk("completing transaction\n");
    }

    goto done;

abort_transaction:
    free(err);
    err = xenbus_transaction_end(xbt, 1, &retry);
    printk("Abort transaction %s\n", message);
    goto error;

done:

    snprintf(path, sizeof(path), "%s/backend", nodename);
    msg = xenbus_read(XBT_NIL, path, &dev->backend);
    if (msg) {
        printk("Error %s when reading the backend path %s\n", msg, path);
        goto error;
    }

    printk("backend at %s\n", dev->backend);

    {
        XenbusState state;
        char path[strlen(dev->backend) + strlen("/state") + 1];
        snprintf(path, sizeof(path), "%s/state", dev->backend);
        
	free(xenbus_watch_path_token(XBT_NIL, path, path, &dev->events));
        msg = NULL;
        state = xenbus_read_integer(path);
        while (msg == NULL && state < XenbusStateConnected)
            msg = xenbus_wait_for_state_change(path, &state, &dev->events);
        if (msg != NULL || state != XenbusStateConnected) {
            printk("backend not available, state=%d\n", state);
            err = xenbus_unwatch_path_token(XBT_NIL, path, path);
            goto error;
        }
    }
    unmask_evtchn(dev->evtchn);

    printk("**************************\n");

    return dev;

error:
    free(msg);
    free(err);
    free_consfront(dev);
    return NULL;
}

void fini_consfront(struct consfront_dev *dev)
{
    if (dev) free_consfront(dev);
}

#ifdef HAVE_LIBC
static int consfront_read(struct file *file, void *buf, size_t nbytes)
{
    int ret;
    DEFINE_WAIT(w);

    while ( 1 )
    {
        add_waiter(w, console_queue);
        ret = xencons_ring_recv(file->dev, buf, nbytes);
        if ( ret )
            break;
        schedule();
    }

    remove_waiter(w, console_queue);

    return ret;
}

static int savefile_write(struct file *file, const void *buf, size_t nbytes)
{
    int ret = 0, tot = nbytes;

    while ( nbytes > 0 )
    {
        ret = xencons_ring_send(file->dev, buf, nbytes);
        nbytes -= ret;
        buf = (char *)buf + ret;
    }

    return tot - nbytes;
}

static int console_write(struct file *file, const void *buf, size_t nbytes)
{
    console_print(file->dev, buf, nbytes);

    return nbytes;
}

static int consfront_close_fd(struct file *file)
{
    fini_consfront(file->dev);

    return 0;
}

static int consfront_fstat(struct file *file, struct stat *buf)
{
    buf->st_mode = S_IRUSR | S_IWUSR;
    buf->st_mode |= (file->type == FTYPE_CONSOLE) ? S_IFCHR : S_IFREG;
    buf->st_atime = buf->st_mtime = buf->st_ctime = time(NULL);

    return 0;
}

static bool consfront_select_rd(struct file *file)
{
    return xencons_ring_avail(file->dev);
}

static const struct file_ops savefile_ops = {
    .name = "savefile",
    .read = consfront_read,
    .write = savefile_write,
    .close = consfront_close_fd,
    .fstat = consfront_fstat,
    .select_rd = consfront_select_rd,
    .select_wr = select_yes,
};

const struct file_ops console_ops = {
    .name = "console",
    .read = consfront_read,
    .write = console_write,
    .close = consfront_close_fd,
    .fstat = consfront_fstat,
    .select_rd = consfront_select_rd,
    .select_wr = select_yes,
};

static unsigned int ftype_savefile;

__attribute__((constructor))
static void consfront_initialize(void)
{
    ftype_savefile = alloc_file_type(&savefile_ops);
}

int open_consfront(char *nodename)
{
    struct consfront_dev *dev;
    struct file *file;

    dev = init_consfront(nodename);
    if ( !dev )
        return -1;

    dev->fd = alloc_fd(nodename ? ftype_savefile : FTYPE_CONSOLE);
    file = get_file_from_fd(dev->fd);
    if ( !file )
    {
        fini_consfront(dev);
        return -1;
    }
    file->dev = dev;

    return dev->fd;
}
#endif
