/******************************************************************************
 * test.c
 * 
 * Test code for all the various frontends; split from kernel.c
 * 
 * Copyright (c) 2002-2003, K A Fraser & R Neugebauer
 * Copyright (c) 2005, Grzegorz Milos, Intel Research Cambridge
 * Copyright (c) 2006, Robert Kaiser, FH Wiesbaden
 * 
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

#include <mini-os/os.h>
#include <mini-os/hypervisor.h>
#include <mini-os/mm.h>
#include <mini-os/events.h>
#include <mini-os/time.h>
#include <mini-os/types.h>
#include <mini-os/lib.h>
#include <mini-os/sched.h>
#include <mini-os/xenbus.h>
#include <mini-os/gnttab.h>
#include <mini-os/netfront.h>
#include <mini-os/blkfront.h>
#include <mini-os/fbfront.h>
#include <mini-os/pcifront.h>
#include <mini-os/xmalloc.h>
#include <fcntl.h>
#include <xen/features.h>
#include <xen/version.h>
#include <xen/io/xs_wire.h>

#ifdef CONFIG_XENBUS
static unsigned int do_shutdown = 0;
static unsigned int shutdown_reason;
static DECLARE_WAIT_QUEUE_HEAD(shutdown_queue);
#endif

#ifdef CONFIG_XENBUS
/* Send a debug message to xenbus.  Can block. */
static void xenbus_debug_msg(const char *msg)
{
    int len = strlen(msg);
    struct write_req req[] = {
        { "print", sizeof("print") },
        { msg, len },
        { "", 1 }};
    struct xsd_sockmsg *reply;

    reply = xenbus_msg_reply(XS_DEBUG, 0, req, ARRAY_SIZE(req));
    printk("Got a reply, type %d, id %d, len %d.\n",
           reply->type, reply->req_id, reply->len);
}

static void do_ls_test(const char *pre)
{
    char **dirs, *msg;
    int x;

    printk("ls %s...\n", pre);
    msg = xenbus_ls(XBT_NIL, pre, &dirs);
    if ( msg )
    {
        printk("Error in xenbus ls: %s\n", msg);
        free(msg);
        return;
    }

    for ( x = 0; dirs[x]; x++ )
    {
        printk("ls %s[%d] -> %s\n", pre, x, dirs[x]);
        free(dirs[x]);
    }

    free(dirs);
}

static void do_read_test(const char *path)
{
    char *res, *msg;

    printk("Read %s...\n", path);
    msg = xenbus_read(XBT_NIL, path, &res);
    if ( msg )
    {
        printk("Error in xenbus read: %s\n", msg);
        free(msg);
        return;
    }
    printk("Read %s -> %s.\n", path, res);
    free(res);
}

static void do_write_test(const char *path, const char *val)
{
    char *msg;

    printk("Write %s to %s...\n", val, path);
    msg = xenbus_write(XBT_NIL, path, val);
    if ( msg )
    {
        printk("Result %s\n", msg);
        free(msg);
    }
    else
        printk("Success.\n");
}

static void do_rm_test(const char *path)
{
    char *msg;

    printk("rm %s...\n", path);
    msg = xenbus_rm(XBT_NIL, path);
    if ( msg )
    {
        printk("Result %s\n", msg);
        free(msg);
    }
    else
        printk("Success.\n");
}

static void xenbus_tester(void *p)
{
    printk("Doing xenbus test.\n");
    xenbus_debug_msg("Testing xenbus...\n");

    printk("Doing ls test.\n");
    do_ls_test("device");
    do_ls_test("device/vif");
    do_ls_test("device/vif/0");

    printk("Doing read test.\n");
    do_read_test("device/vif/0/mac");
    do_read_test("device/vif/0/backend");

    printk("Doing write test.\n");
    do_write_test("device/vif/0/flibble", "flobble");
    do_read_test("device/vif/0/flibble");
    do_write_test("device/vif/0/flibble", "widget");
    do_read_test("device/vif/0/flibble");

    printk("Doing rm test.\n");
    do_rm_test("device/vif/0/flibble");
    do_read_test("device/vif/0/flibble");
    printk("(Should have said ENOENT)\n");
}
#endif

#ifndef HAVE_LIBC
/* Should be random enough for our uses */
int rand(void)
{
    static unsigned int previous;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    previous += tv.tv_sec + tv.tv_usec;
    previous *= RAND_MIX;
    return previous;
}
#endif

static void periodic_thread(void *p)
{
    struct timeval tv;
    printk("Periodic thread started.\n");
    for(;;)
    {
        gettimeofday(&tv, NULL);
        printk("T(s=%ld us=%ld)\n", tv.tv_sec, tv.tv_usec);
        sanity_check();
        msleep(1000);
    }
}

#ifdef CONFIG_NETFRONT
static struct netfront_dev *net_dev;
static struct semaphore net_sem = __SEMAPHORE_INITIALIZER(net_sem, 0);

static void netfront_thread(void *p)
{
    net_dev = init_netfront(NULL, NULL, NULL, NULL);
    up(&net_sem);
}
#endif

#ifdef CONFIG_BLKFRONT
static struct blkfront_dev *blk_dev;
static struct blkfront_info blk_info;
static uint64_t blk_size_read;
static uint64_t blk_size_write;
static struct semaphore blk_sem = __SEMAPHORE_INITIALIZER(blk_sem, 0);;

struct blk_req {
    struct blkfront_aiocb aiocb;
    int rand_value;
    struct blk_req *next;
};

#ifdef BLKTEST_WRITE
static struct blk_req *blk_to_read;
#endif

static struct blk_req *blk_alloc_req(uint64_t sector)
{
    struct blk_req *req = xmalloc(struct blk_req);
    req->aiocb.aio_dev = blk_dev;
    req->aiocb.aio_buf = _xmalloc(blk_info.sector_size, blk_info.sector_size);
    req->aiocb.aio_nbytes = blk_info.sector_size;
    req->aiocb.aio_offset = sector * blk_info.sector_size;
    req->aiocb.data = req;
    req->next = NULL;
    return req;
}

static void blk_read_completed(struct blkfront_aiocb *aiocb, int ret)
{
    struct blk_req *req = aiocb->data;
    if (ret)
        printk("got error code %d when reading at offset %ld\n", ret, (long) aiocb->aio_offset);
    else
        blk_size_read += blk_info.sector_size;
    free(aiocb->aio_buf);
    free(req);
}

static void blk_read_sector(uint64_t sector)
{
    struct blk_req *req;

    req = blk_alloc_req(sector);
    req->aiocb.aio_cb = blk_read_completed;

    blkfront_aio_read(&req->aiocb);
}

#ifdef BLKTEST_WRITE
static void blk_write_read_completed(struct blkfront_aiocb *aiocb, int ret)
{
    struct blk_req *req = aiocb->data;
    int rand_value;
    int i;
    int *buf;

    if (ret) {
        printk("got error code %d when reading back at offset %ld\n", ret, aiocb->aio_offset);
        free(aiocb->aio_buf);
        free(req);
        return;
    }
    blk_size_read += blk_info.sector_size;
    buf = (int*) aiocb->aio_buf;
    rand_value = req->rand_value;
    for (i = 0; i < blk_info.sector_size / sizeof(int); i++) {
        if (buf[i] != rand_value) {
            printk("bogus data at offset %ld\n", aiocb->aio_offset + i);
            break;
        }
        rand_value *= RAND_MIX;
    }
    free(aiocb->aio_buf);
    free(req);
}

static void blk_write_completed(struct blkfront_aiocb *aiocb, int ret)
{
    struct blk_req *req = aiocb->data;
    if (ret) {
        printk("got error code %d when writing at offset %ld\n", ret, aiocb->aio_offset);
        free(aiocb->aio_buf);
        free(req);
        return;
    }
    blk_size_write += blk_info.sector_size;
    /* Push write check */
    req->next = blk_to_read;
    blk_to_read = req;
}

static void blk_write_sector(uint64_t sector)
{
    struct blk_req *req;
    int rand_value;
    int i;
    int *buf;

    req = blk_alloc_req(sector);
    req->aiocb.aio_cb = blk_write_completed;
    req->rand_value = rand_value = rand();

    buf = (int*) req->aiocb.aio_buf;
    for (i = 0; i < blk_info.sector_size / sizeof(int); i++) {
        buf[i] = rand_value;
        rand_value *= RAND_MIX;
    }

    blkfront_aio_write(&req->aiocb);
}
#endif

static void blkfront_thread(void *p)
{
    time_t lasttime = 0;

    blk_dev = init_blkfront(NULL, &blk_info);
    if (!blk_dev) {
        up(&blk_sem);
        return;
    }

    if (blk_info.info & VDISK_CDROM)
        printk("Block device is a CDROM\n");
    if (blk_info.info & VDISK_REMOVABLE)
        printk("Block device is removable\n");
    if (blk_info.info & VDISK_READONLY)
        printk("Block device is read-only\n");

#ifdef BLKTEST_WRITE
    if (blk_info.mode == O_RDWR) {
        blk_write_sector(0);
        blk_write_sector(blk_info.sectors-1);
    } else
#endif
    {
        blk_read_sector(0);
        blk_read_sector(blk_info.sectors-1);
    }

    while (!do_shutdown) {
        uint64_t sector = rand() % blk_info.sectors;
        struct timeval tv;
#ifdef BLKTEST_WRITE
        if (blk_info.mode == O_RDWR)
            blk_write_sector(sector);
        else
#endif
            blk_read_sector(sector);
        blkfront_aio_poll(blk_dev);
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > lasttime + 10) {
            printk("%llu read, %llu write\n",
                    (unsigned long long) blk_size_read,
                    (unsigned long long) blk_size_write);
            lasttime = tv.tv_sec;
        }

#ifdef BLKTEST_WRITE
        while (blk_to_read) {
            struct blk_req *req = blk_to_read;
            blk_to_read = blk_to_read->next;
            req->aiocb.aio_cb = blk_write_read_completed;
            blkfront_aio_read(&req->aiocb);
        }
#endif
    }
    up(&blk_sem);
}
#endif

#if defined(CONFIG_FBFRONT) && defined(CONFIG_KBDFRONT)
#define WIDTH 800
#define HEIGHT 600
#define DEPTH 32

static uint32_t *fb;
static int refresh_period = 50;
static struct fbfront_dev *fb_dev;
static struct semaphore fbfront_sem = __SEMAPHORE_INITIALIZER(fbfront_sem, 0);

static void fbfront_drawvert(int x, int y1, int y2, uint32_t color)
{
    int y;
    if (x < 0)
        return;
    if (x >= WIDTH)
        return;
    if (y1 < 0)
        y1 = 0;
    if (y2 >= HEIGHT)
        y2 = HEIGHT-1;
    for (y = y1; y <= y2; y++)
        fb[x + y*WIDTH] ^= color;
}

static void fbfront_drawhoriz(int x1, int x2, int y, uint32_t color)
{
    int x;
    if (y < 0)
        return;
    if (y >= HEIGHT)
        return;
    if (x1 < 0)
        x1 = 0;
    if (x2 >= WIDTH)
        x2 = WIDTH-1;
    for (x = x1; x <= x2; x++)
        fb[x + y*WIDTH] ^= color;
}

static void fbfront_thread(void *p)
{
    size_t line_length = WIDTH * (DEPTH / 8);
    size_t memsize = HEIGHT * line_length;
    unsigned long *mfns;
    int i, n = (memsize + PAGE_SIZE-1) / PAGE_SIZE;

    memsize = n * PAGE_SIZE;
    fb = _xmalloc(memsize, PAGE_SIZE);
    memset(fb, 0, memsize);
    mfns = xmalloc_array(unsigned long, n);
    for (i = 0; i < n; i++)
        mfns[i] = virtual_to_mfn((char *) fb + i * PAGE_SIZE);
    fb_dev = init_fbfront(NULL, mfns, WIDTH, HEIGHT, DEPTH, line_length, n);
    xfree(mfns);
    if (!fb_dev) {
        xfree(fb);
    }
    up(&fbfront_sem);
}

static void clip_cursor(int *x, int *y)
{
    if (*x < 0)
        *x = 0;
    if (*x >= WIDTH)
        *x = WIDTH - 1;
    if (*y < 0)
        *y = 0;
    if (*y >= HEIGHT)
        *y = HEIGHT - 1;
}

static void refresh_cursor(int new_x, int new_y)
{
    static int old_x = -1, old_y = -1;

    if (!refresh_period)
        return;

    if (old_x != -1 && old_y != -1) {
        fbfront_drawvert(old_x, old_y + 1, old_y + 8, 0xffffffff);
        fbfront_drawhoriz(old_x + 1, old_x + 8, old_y, 0xffffffff);
        fbfront_update(fb_dev, old_x, old_y, 9, 9);
    }
    old_x = new_x;
    old_y = new_y;
    fbfront_drawvert(new_x, new_y + 1, new_y + 8, 0xffffffff);
    fbfront_drawhoriz(new_x + 1, new_x + 8, new_y, 0xffffffff);
    fbfront_update(fb_dev, new_x, new_y, 9, 9);
}

static struct kbdfront_dev *kbd_dev;
static struct semaphore kbd_sem = __SEMAPHORE_INITIALIZER(kbd_sem, 0);
static void kbdfront_thread(void *p)
{
    DEFINE_WAIT(w);
    DEFINE_WAIT(w2);
    DEFINE_WAIT(w3);
    int x = WIDTH / 2, y = HEIGHT / 2, z = 0;

    kbd_dev = init_kbdfront(NULL, 1);
    down(&fbfront_sem);
    if (!kbd_dev) {
        up(&kbd_sem);
        return;
    }

    refresh_cursor(x, y);
    while (1) {
        union xenkbd_in_event kbdevent;
        union xenfb_in_event fbevent;
        int sleep = 1;

        add_waiter(w, kbdfront_queue);
        add_waiter(w2, fbfront_queue);
        add_waiter(w3, shutdown_queue);

        rmb();
        if (do_shutdown)
            break;

        while (kbdfront_receive(kbd_dev, &kbdevent, 1) != 0) {
            sleep = 0;
            switch(kbdevent.type) {
            case XENKBD_TYPE_MOTION:
                printk("motion x:%d y:%d z:%d\n",
                        kbdevent.motion.rel_x,
                        kbdevent.motion.rel_y,
                        kbdevent.motion.rel_z);
                x += kbdevent.motion.rel_x;
                y += kbdevent.motion.rel_y;
                z += kbdevent.motion.rel_z;
                clip_cursor(&x, &y);
                refresh_cursor(x, y);
                break;
            case XENKBD_TYPE_POS:
                printk("pos x:%d y:%d dz:%d\n",
                        kbdevent.pos.abs_x,
                        kbdevent.pos.abs_y,
                        kbdevent.pos.rel_z);
                x = kbdevent.pos.abs_x;
                y = kbdevent.pos.abs_y;
                z = kbdevent.pos.rel_z;
                clip_cursor(&x, &y);
                refresh_cursor(x, y);
                break;
            case XENKBD_TYPE_KEY:
                printk("key %d %s\n",
                        kbdevent.key.keycode,
                        kbdevent.key.pressed ? "pressed" : "released");
                if (kbdevent.key.keycode == BTN_LEFT) {
                    printk("mouse %s at (%d,%d,%d)\n",
                            kbdevent.key.pressed ? "clic" : "release", x, y, z);
                    if (kbdevent.key.pressed) {
                        uint32_t color = rand();
                        fbfront_drawvert(x - 16, y - 16, y + 15, color);
                        fbfront_drawhoriz(x - 16, x + 15, y + 16, color);
                        fbfront_drawvert(x + 16, y - 15, y + 16, color);
                        fbfront_drawhoriz(x - 15, x + 16, y - 16, color);
                        fbfront_update(fb_dev, x - 16, y - 16, 33, 33);
                    }
                } else if (kbdevent.key.keycode == KEY_Q) {
                    shutdown_reason = SHUTDOWN_poweroff;
                    wmb();
                    do_shutdown = 1;
                    wmb();
                    wake_up(&shutdown_queue);
                }
                break;
            }
        }
        while (fbfront_receive(fb_dev, &fbevent, 1) != 0) {
            sleep = 0;
            switch(fbevent.type) {
            case XENFB_TYPE_REFRESH_PERIOD:
                refresh_period = fbevent.refresh_period.period;
                printk("refresh period %d\n", refresh_period);
                refresh_cursor(x, y);
                break;
            }
        }
        if (sleep)
            schedule();
        remove_waiter(w3, shutdown_queue);
        remove_waiter(w2, fbfront_queue);
        remove_waiter(w, kbdfront_queue);
    }
    up(&kbd_sem);
}
#endif

#ifdef CONFIG_PCIFRONT
static struct pcifront_dev *pci_dev;
static struct semaphore pci_sem = __SEMAPHORE_INITIALIZER(pci_sem, 0);

static void print_pcidev(unsigned int domain, unsigned int bus, unsigned int slot, unsigned int fun)
{
    unsigned int vendor, device, rev, class;

    pcifront_conf_read(pci_dev, domain, bus, slot, fun, 0x00, 2, &vendor);
    pcifront_conf_read(pci_dev, domain, bus, slot, fun, 0x02, 2, &device);
    pcifront_conf_read(pci_dev, domain, bus, slot, fun, 0x08, 1, &rev);
    pcifront_conf_read(pci_dev, domain, bus, slot, fun, 0x0a, 2, &class);

    printk("%04x:%02x:%02x.%02x %04x: %04x:%04x (rev %02x)\n", domain, bus, slot, fun, class, vendor, device, rev);
}

static void pcifront_thread(void *p)
{
    pcifront_watches(NULL);
    pci_dev = init_pcifront(NULL);
    if (!pci_dev) {
        up(&pci_sem);
        return;
    }
    printk("PCI devices:\n");
    pcifront_scan(pci_dev, print_pcidev);
    up(&pci_sem);
}
#endif

void shutdown_frontends(void)
{
#ifdef CONFIG_NETFRONT
    down(&net_sem);
    if (net_dev)
        shutdown_netfront(net_dev);
#endif

#ifdef CONFIG_BLKFRONT
    down(&blk_sem);
    if (blk_dev)
        shutdown_blkfront(blk_dev);
#endif

#if defined(CONFIG_FBFRONT) && defined(CONFIG_KBDFRONT)
    if (fb_dev)
        shutdown_fbfront(fb_dev);

    down(&kbd_sem);
    if (kbd_dev)
        shutdown_kbdfront(kbd_dev);
#endif

#ifdef CONFIG_PCIFRONT
    down(&pci_sem);
    if (pci_dev)
        shutdown_pcifront(pci_dev);
#endif
}

#ifdef CONFIG_XENBUS
void app_shutdown(unsigned reason)
{
    shutdown_reason = reason;
    wmb();
    do_shutdown = 1;
    wmb();
    wake_up(&shutdown_queue);
}

static void shutdown_thread(void *p)
{
    DEFINE_WAIT(w);

    while (1) {
        add_waiter(w, shutdown_queue);
        rmb();
        if (do_shutdown) {
            rmb();
            break;
        }
        schedule();
        remove_waiter(w, shutdown_queue);
    }

    shutdown_frontends();

    HYPERVISOR_shutdown(shutdown_reason);
}
#endif

int app_main(void *p)
{
    printk("Test main: par=%p\n", p);
#ifdef CONFIG_XENBUS
    create_thread("xenbus_tester", xenbus_tester, p);
#endif
    create_thread("periodic_thread", periodic_thread, p);
#ifdef CONFIG_NETFRONT
    create_thread("netfront", netfront_thread, p);
#endif
#ifdef CONFIG_BLKFRONT
    create_thread("blkfront", blkfront_thread, p);
#endif
#if defined(CONFIG_FBFRONT) && defined(CONFIG_KBDFRONT)
    create_thread("fbfront", fbfront_thread, p);
    create_thread("kbdfront", kbdfront_thread, p);
#endif
#ifdef CONFIG_PCIFRONT
    create_thread("pcifront", pcifront_thread, p);
#endif
#ifdef CONFIG_XENBUS
    create_thread("shutdown", shutdown_thread, p);
#endif
    return 0;
}
