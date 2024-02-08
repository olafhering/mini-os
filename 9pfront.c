/*
 * Minimal 9pfs PV frontend for Mini-OS.
 * Copyright (c) 2023 Juergen Gross, SUSE Software Solution GmbH
 */

#include <mini-os/os.h>
#include <mini-os/lib.h>
#include <mini-os/events.h>
#include <mini-os/fcntl.h>
#include <mini-os/gnttab.h>
#include <mini-os/semaphore.h>
#include <mini-os/wait.h>
#include <mini-os/xenbus.h>
#include <mini-os/xmalloc.h>
#include <errno.h>
#include <xen/io/9pfs.h>
#include <mini-os/9pfront.h>

#ifdef HAVE_LIBC

#define N_REQS   64

struct dev_9pfs {
    int id;
    char nodename[20];
    unsigned int dom;
    char *backend;

    char *tag;
    const char *mnt;
    unsigned int msize_max;

    struct xen_9pfs_data_intf *intf;
    struct xen_9pfs_data data;
    RING_IDX prod_pvt_out;
    RING_IDX cons_pvt_in;

    grant_ref_t ring_ref;
    evtchn_port_t evtchn;
    unsigned int ring_order;
    xenbus_event_queue events;

    unsigned int free_reqs;
    struct req {
        unsigned int id;
        unsigned int next_free;     /* N_REQS == end of list. */
        unsigned int cmd;
        int result;
        bool inflight;
        unsigned char *data;        /* Returned data. */
    } req[N_REQS];

    struct wait_queue_head waitq;
    struct semaphore ring_out_sem;
    struct semaphore ring_in_sem;

    unsigned long long fid_mask;              /* Bit mask for free fids. */
};

struct file_9pfs {
    uint32_t fid;
    struct dev_9pfs *dev;
    bool append;
};

#define DEFAULT_9PFS_RING_ORDER  4

/* P9 protocol commands (response is either cmd+1 or P9_CMD_ERROR). */
#define P9_CMD_VERSION    100
#define P9_CMD_ATTACH     104
#define P9_CMD_ERROR      107
#define P9_CMD_WALK       110
#define P9_CMD_OPEN       112
#define P9_CMD_CREATE     114
#define P9_CMD_READ       116
#define P9_CMD_WRITE      118
#define P9_CMD_CLUNK      120
#define P9_CMD_STAT       124

/* P9 protocol open flags. */
#define P9_OREAD            0   /* read */
#define P9_OWRITE           1   /* write */
#define P9_ORDWR            2   /* read and write */
#define P9_OTRUNC          16   /* or'ed in, truncate file first */

#define P9_QID_SIZE    13

struct p9_header {
    uint32_t size;
    uint8_t cmd;
    uint16_t tag;
} __attribute__((packed));

struct p9_stat {
    uint16_t size;
    uint16_t type;
    uint32_t dev;
    uint8_t qid[P9_QID_SIZE];
    uint32_t mode;
    uint32_t atime;
    uint32_t mtime;
    uint64_t length;
    char *name;
    char *uid;
    char *gid;
    char *muid;
    char *extension;
    uint32_t n_uid;
    uint32_t n_gid;
    uint32_t n_muid;
};

#define P9_VERSION        "9P2000.u"
#define P9_ROOT_FID       0

static unsigned int ftype_9pfs;

static void free_stat(struct p9_stat *stat)
{
    free(stat->name);
    free(stat->uid);
    free(stat->gid);
    free(stat->muid);
    free(stat->extension);
}

static unsigned int get_fid(struct dev_9pfs *dev)
{
    unsigned int fid;

    fid = ffs(dev->fid_mask);
    if ( fid )
        dev->fid_mask &= ~(1ULL << (fid - 1));

     return fid;
}

static void put_fid(struct dev_9pfs *dev, unsigned int fid)
{
    if ( fid )
        dev->fid_mask |= 1ULL << (fid - 1);
}

static struct req *get_free_req(struct dev_9pfs *dev)
{
    struct req *req;

    if ( dev->free_reqs == N_REQS )
        return NULL;

    req = dev->req + dev->free_reqs;
    dev->free_reqs = req->next_free;

    return req;
}

static void put_free_req(struct dev_9pfs *dev, struct req *req)
{
    req->next_free = dev->free_reqs;
    req->inflight = false;
    req->data = NULL;
    dev->free_reqs = req->id;
}

static unsigned int ring_out_free(struct dev_9pfs *dev)
{
    RING_IDX ring_size = XEN_FLEX_RING_SIZE(dev->ring_order);
    unsigned int queued;

    queued = xen_9pfs_queued(dev->prod_pvt_out, dev->intf->out_cons, ring_size);
    rmb();

    return ring_size - queued;
}

static unsigned int ring_in_data(struct dev_9pfs *dev)
{
    RING_IDX ring_size = XEN_FLEX_RING_SIZE(dev->ring_order);
    unsigned int queued;

    queued = xen_9pfs_queued(dev->intf->in_prod, dev->cons_pvt_in, ring_size);
    rmb();

    return queued;
}

static void copy_to_ring(struct dev_9pfs *dev, void *data, unsigned int len)
{
    RING_IDX ring_size = XEN_FLEX_RING_SIZE(dev->ring_order);
    RING_IDX prod = xen_9pfs_mask(dev->prod_pvt_out, ring_size);
    RING_IDX cons = xen_9pfs_mask(dev->intf->out_cons, ring_size);

    xen_9pfs_write_packet(dev->data.out, data, len, &prod, cons, ring_size);
    dev->prod_pvt_out += len;
}

static void copy_from_ring(struct dev_9pfs *dev, void *data, unsigned int len)
{
    RING_IDX ring_size = XEN_FLEX_RING_SIZE(dev->ring_order);
    RING_IDX prod = xen_9pfs_mask(dev->intf->in_prod, ring_size);
    RING_IDX cons = xen_9pfs_mask(dev->cons_pvt_in, ring_size);

    xen_9pfs_read_packet(data, dev->data.in, len, prod, &cons, ring_size);
    dev->cons_pvt_in += len;
}

/*
 * send_9p() and rcv_9p() are using a special format string for specifying
 * the kind of data sent/expected. Each data item is represented by a single
 * character:
 * b: 1 byte unsigned integer (uint8_t)
 *    Only valid for sending.
 * u: 2 byte unsigned integer (uint16_t)
 * U: 4 byte unsigned integer (uint32_t)
 * L: 8 byte unsigned integer (uint64_t)
 * S: String (2 byte length + <length> characters)
 *    in the rcv_9p() case the data for string is allocated (length omitted,
 *    string terminated by a NUL character)
 * D: Binary data (4 byte length + <length> bytes of data), requires a length
 *    and a buffer pointer parameter.
 * Q: A 13 byte "qid", consisting of 1 byte file type, 4 byte file version
 *    and 8 bytes unique file id. Only valid for receiving.
 */
static void send_9p(struct dev_9pfs *dev, struct req *req, const char *fmt, ...)
{
    struct p9_header hdr;
    va_list ap, aq;
    const char *f;
    uint64_t longval;
    uint32_t intval;
    uint16_t shortval;
    uint16_t len;
    uint8_t byte;
    uint8_t *data;
    char *strval;

    hdr.size = sizeof(hdr);
    hdr.cmd = req->cmd;
    hdr.tag = req->id;

    va_start(ap, fmt);

    va_copy(aq, ap);
    for ( f = fmt; *f; f++ )
    {
        switch ( *f )
        {
        case 'b':
            hdr.size += 1;
            byte = va_arg(aq, unsigned int);
            break;
        case 'u':
            hdr.size += 2;
            shortval = va_arg(aq, unsigned int);
            break;
        case 'U':
            hdr.size += 4;
            intval = va_arg(aq, unsigned int);
            break;
        case 'L':
            hdr.size += 8;
            longval = va_arg(aq, uint64_t);
            break;
        case 'S':
            hdr.size += 2;
            strval = va_arg(aq, char *);
            hdr.size += strlen(strval);
            break;
        case 'D':
            hdr.size += 4;
            intval = va_arg(aq, unsigned int);
            hdr.size += intval;
            data = va_arg(aq, uint8_t *);
            break;
        default:
            printk("send_9p: unknown format character %c\n", *f);
            break;
        }
    }
    va_end(aq);

    /*
     * Waiting for free space must be done in the critical section!
     * Otherwise we might get overtaken by other short requests.
     */
    down(&dev->ring_out_sem);

    wait_event(dev->waitq, ring_out_free(dev) >= hdr.size);

    copy_to_ring(dev, &hdr, sizeof(hdr));
    for ( f = fmt; *f; f++ )
    {
        switch ( *f )
        {
        case 'b':
            byte = va_arg(ap, unsigned int);
            copy_to_ring(dev, &byte, sizeof(byte));
            break;
        case 'u':
            shortval = va_arg(ap, unsigned int);
            copy_to_ring(dev, &shortval, sizeof(shortval));
            break;
        case 'U':
            intval = va_arg(ap, unsigned int);
            copy_to_ring(dev, &intval, sizeof(intval));
            break;
        case 'L':
            longval = va_arg(ap, uint64_t);
            copy_to_ring(dev, &longval, sizeof(longval));
            break;
        case 'S':
            strval = va_arg(ap, char *);
            len = strlen(strval);
            copy_to_ring(dev, &len, sizeof(len));
            copy_to_ring(dev, strval, len);
            break;
        case 'D':
            intval = va_arg(ap, unsigned int);
            copy_to_ring(dev, &intval, sizeof(intval));
            data = va_arg(ap, uint8_t *);
            copy_to_ring(dev, data, intval);
            break;
        }
    }

    wmb();   /* Data on ring must be seen before updating index. */
    dev->intf->out_prod = dev->prod_pvt_out;
    req->inflight = true;

    up(&dev->ring_out_sem);

    va_end(ap);

    notify_remote_via_evtchn(dev->evtchn);
}

/*
 * Using an opportunistic approach for receiving data: in case multiple
 * requests are outstanding (which is very unlikely), we nevertheless need
 * to consume all data available until we reach the desired request.
 * For requests other than the one we are waiting for, we link the complete
 * data to the request via an intermediate buffer. For our own request we can
 * omit that buffer and directly fill the caller provided variables.
 *
 * Helper functions:
 *
 * copy_bufs(): copy raw data into a target buffer. There can be 2 source
 *   buffers involved (in case the copy is done from the ring and it is across
 *   the ring end). The buffer pointers and lengths are updated according to
 *   the number of bytes copied.
 *
 * rcv_9p_copy(): copy the data (without the generic header) of a 9p response
 *   to the specified variables using the specified format string for
 *   deciphering the single item types. The decision whether to copy from the
 *   ring or an allocated buffer is done via the "hdr" parameter, which is
 *   NULL in the buffer case (in that case the header is located at the start
 *   of the buffer).
 *
 * rcv_9p_one(): Checks for an already filled buffer with the correct tag in
 *   it. If none is found, consumes one response. It checks the tag of the
 *   response in order to decide whether to allocate a buffer for putting the
 *   data into, or to fill the user supplied variables. Return true, if the
 *   tag did match. Waits if no data is ready to be consumed.
 */
static void copy_bufs(unsigned char **buf1, unsigned char **buf2,
                      unsigned int *len1, unsigned int *len2,
                      void *target, unsigned int len)
{
    if ( len <= *len1 )
    {
        memcpy(target, *buf1, len);
        *buf1 += len;
        *len1 -= len;
    }
    else
    {
        memcpy(target, *buf1, *len1);
        target = (char *)target + *len1;
        len -= *len1;
        *buf1 = *buf2;
        *len1 = *len2;
        *buf2 = NULL;
        *len2 = 0;
        if ( len > *len1 )
        {
            printk("9pfs: short copy (dropping %u bytes)\n", len - *len1);
            len = *len1;
        }
        memcpy(target, *buf1, len);
        *buf1 += len;
        *len1 -= len;
    }
}

static void rcv_9p_copy(struct dev_9pfs *dev, struct req *req,
                        struct p9_header *hdr, const char *fmt, va_list ap)
{
    struct p9_header *h = hdr ? hdr : (void *)req->data;
    RING_IDX cons = dev->cons_pvt_in + h->size - sizeof(*h);
    RING_IDX ring_size = XEN_FLEX_RING_SIZE(dev->ring_order);
    unsigned char *buf1, *buf2;
    unsigned int len1, len2;
    const char *f;
    char *str;
    uint16_t len;
    uint32_t err;
    uint16_t *shortval;
    uint32_t *val;
    uint64_t *longval;
    uint8_t *data;
    char **strval;
    uint8_t *qval;

    if ( hdr )
    {
        buf1 = xen_9pfs_get_ring_ptr(dev->data.in, dev->cons_pvt_in, ring_size);
        buf2 = xen_9pfs_get_ring_ptr(dev->data.in, 0,  ring_size);
        len1 = ring_size - xen_9pfs_mask(dev->cons_pvt_in, ring_size);
        if ( len1 > h->size - sizeof(*h) )
            len1 = h->size - sizeof(*h);
        len2 = h->size - sizeof(*h) - len1;
    }
    else
    {
        buf1 = req->data + sizeof(*h);
        buf2 = NULL;
        len1 = h->size - sizeof(*h);
        len2 = 0;
    }

    if ( h->cmd == P9_CMD_ERROR )
    {
        copy_bufs(&buf1, &buf2, &len1, &len2, &len, sizeof(len));
        str = malloc(len + 1);
        copy_bufs(&buf1, &buf2, &len1, &len2, str, len);
        str[len] = 0;
        printk("9pfs: request %u resulted in \"%s\"\n", req->cmd, str);
        free(str);
        err = EIO;
        copy_bufs(&buf1, &buf2, &len1, &len2, &err, sizeof(err));
        req->result = err;

        if ( hdr )
            dev->cons_pvt_in = cons;

        return;
    }

    if ( h->cmd != req->cmd + 1 )
    {
        req->result = EDOM;
        printk("9pfs: illegal response: wrong return type (%u instead of %u)\n",
               h->cmd, req->cmd + 1);

        if ( hdr )
            dev->cons_pvt_in = cons;

        return;
    }

    req->result = 0;

    for ( f = fmt; *f; f++ )
    {
        switch ( *f )
        {
        case 'u':
            shortval = va_arg(ap, uint16_t *);
            copy_bufs(&buf1, &buf2, &len1, &len2, shortval, sizeof(*shortval));
            break;
        case 'U':
            val = va_arg(ap, uint32_t *);
            copy_bufs(&buf1, &buf2, &len1, &len2, val, sizeof(*val));
            break;
        case 'L':
            longval = va_arg(ap, uint64_t *);
            copy_bufs(&buf1, &buf2, &len1, &len2, longval, sizeof(*longval));
            break;
        case 'S':
            strval = va_arg(ap, char **);
            copy_bufs(&buf1, &buf2, &len1, &len2, &len, sizeof(len));
            *strval = malloc(len + 1);
            copy_bufs(&buf1, &buf2, &len1, &len2, *strval, len);
            (*strval)[len] = 0;
            break;
        case 'D':
            val = va_arg(ap, uint32_t *);
            data = va_arg(ap, uint8_t *);
            copy_bufs(&buf1, &buf2, &len1, &len2, val, sizeof(*val));
            copy_bufs(&buf1, &buf2, &len1, &len2, data, *val);
            break;
        case 'Q':
            qval = va_arg(ap, uint8_t *);
            copy_bufs(&buf1, &buf2, &len1, &len2, qval, P9_QID_SIZE);
            break;
        default:
            printk("rcv_9p: unknown format character %c\n", *f);
            break;
        }
    }

    if ( hdr )
        dev->cons_pvt_in = cons;
}

static bool rcv_9p_one(struct dev_9pfs *dev, struct req *req, const char *fmt,
                       va_list ap)
{
    struct p9_header hdr;
    struct req *tmp;

    if ( req->data )
    {
        rcv_9p_copy(dev, req, NULL, fmt, ap);
        free(req->data);
        req->data = NULL;

        return true;
    }

    wait_event(dev->waitq, ring_in_data(dev) >= sizeof(hdr));

    copy_from_ring(dev, &hdr, sizeof(hdr));

    wait_event(dev->waitq, ring_in_data(dev) >= hdr.size - sizeof(hdr));

    tmp = dev->req + hdr.tag;
    if ( hdr.tag >= N_REQS || !tmp->inflight )
    {
        printk("9pfs: illegal response: %s\n",
               hdr.tag >= N_REQS ? "tag out of bounds" : "request not pending");
        dev->cons_pvt_in += hdr.size - sizeof(hdr);

        return false;
    }

    tmp->inflight = false;

    if ( tmp != req )
    {
        tmp->data = malloc(hdr.size);
        memcpy(tmp->data, &hdr, sizeof(hdr));
        copy_from_ring(dev, tmp->data + sizeof(hdr), hdr.size - sizeof(hdr));

        return false;
    }

    rcv_9p_copy(dev, req, &hdr, fmt, ap);

    return true;
}

static void rcv_9p(struct dev_9pfs *dev, struct req *req, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);

    down(&dev->ring_in_sem);

    while ( !rcv_9p_one(dev, req, fmt, ap) );

    rmb(); /* Read all data before updating ring index. */
    dev->intf->in_cons = dev->cons_pvt_in;

    notify_remote_via_evtchn(dev->evtchn);

    up(&dev->ring_in_sem);

    va_end(ap);
}

static int p9_version(struct dev_9pfs *dev)
{
    unsigned int msize = XEN_FLEX_RING_SIZE(dev->ring_order) / 2;
    struct req *req = get_free_req(dev);
    char *verret;
    int ret;

    if ( !req )
        return EAGAIN;

    req->cmd = P9_CMD_VERSION;
    send_9p(dev, req, "US", msize, P9_VERSION);
    rcv_9p(dev, req, "US", &dev->msize_max, &verret);
    ret = req->result;

    put_free_req(dev, req);

    if ( ret )
        return ret;

    if ( strcmp(verret, P9_VERSION) )
        ret = ENOMSG;
    free(verret);

    return ret;
}

static int p9_attach(struct dev_9pfs *dev)
{
    uint32_t fid = P9_ROOT_FID;
    uint32_t afid = 0;
    uint32_t uid = 0;
    uint8_t qid[P9_QID_SIZE];
    struct req *req = get_free_req(dev);
    int ret;

    if ( !req )
        return EAGAIN;

    req->cmd = P9_CMD_ATTACH;
    send_9p(dev, req, "UUSSU", fid, afid, "root", "root", uid);
    rcv_9p(dev, req, "Q", qid);
    ret = req->result;

    put_free_req(dev, req);

    return ret;
}

static int p9_clunk(struct dev_9pfs *dev, uint32_t fid)
{
    struct req *req = get_free_req(dev);
    int ret;

    if ( !req )
        return EAGAIN;

    req->cmd = P9_CMD_CLUNK;
    send_9p(dev, req, "U", fid);
    rcv_9p(dev, req, "");
    ret = req->result;

    put_free_req(dev, req);

    return ret;
}

static int p9_walk(struct dev_9pfs *dev, uint32_t fid, uint32_t newfid,
                   char *name)
{
    struct req *req = get_free_req(dev);
    int ret;
    uint16_t nqid;
    uint8_t qid[P9_QID_SIZE];

    if ( !req )
        return EAGAIN;

    req->cmd = P9_CMD_WALK;
    if ( name[0] )
    {
        send_9p(dev, req, "UUuS", fid, newfid, 1, name);
        rcv_9p(dev, req, "uQ", &nqid, qid);
    }
    else
    {
        send_9p(dev, req, "UUu", fid, newfid, 0);
        rcv_9p(dev, req, "u", &nqid);
    }

    ret = req->result;

    put_free_req(dev, req);

    return ret;
}

static int p9_open(struct dev_9pfs *dev, uint32_t fid, uint8_t omode)
{
    struct req *req = get_free_req(dev);
    int ret;
    uint8_t qid[P9_QID_SIZE];
    uint32_t iounit;

    if ( !req )
        return EAGAIN;

    req->cmd = P9_CMD_OPEN;
    send_9p(dev, req, "Ub", fid, omode);
    rcv_9p(dev, req, "QU", qid, &iounit);

    ret = req->result;

    put_free_req(dev, req);

    return ret;
}

static int p9_create(struct dev_9pfs *dev, uint32_t fid, char *path,
                     uint32_t mode, uint8_t omode)
{
    struct req *req = get_free_req(dev);
    int ret;
    uint8_t qid[P9_QID_SIZE];
    uint32_t iounit;

    if ( !req )
        return EAGAIN;

    req->cmd = P9_CMD_CREATE;
    send_9p(dev, req, "USUbS", fid, path, mode, omode, "");
    rcv_9p(dev, req, "QU", qid, &iounit);

    ret = req->result;

    put_free_req(dev, req);

    return ret;
}

static int p9_stat(struct dev_9pfs *dev, uint32_t fid, struct p9_stat *stat)
{
    struct req *req = get_free_req(dev);
    uint16_t total;
    int ret;

    if ( !req )
        return EAGAIN;

    memset(stat, 0, sizeof(*stat));
    req->cmd = P9_CMD_STAT;
    send_9p(dev, req, "U", fid);
    rcv_9p(dev, req, "uuuUQUUULSSSSSUUU", &total, &stat->size, &stat->type,
           &stat->dev, stat->qid, &stat->mode, &stat->atime, &stat->mtime,
           &stat->length, &stat->name, &stat->uid, &stat->gid, &stat->muid,
           &stat->extension, &stat->n_uid, &stat->n_gid, &stat->n_muid);

    ret = req->result;
    if ( ret )
        free_stat(stat);

    put_free_req(dev, req);

    return ret;
}

static int p9_read(struct dev_9pfs *dev, uint32_t fid, uint64_t offset,
                   uint8_t *data, uint32_t len)
{
    struct req *req = get_free_req(dev);
    int ret = 0;
    uint32_t count, count_max;

    if ( !req )
    {
        errno = EAGAIN;
        return -1;
    }
    req->cmd = P9_CMD_READ;
    count_max = dev->msize_max - (sizeof(struct p9_header) + sizeof(uint32_t));

    while ( len )
    {
        count = len;
        if ( count > count_max )
            count = count_max;

        send_9p(dev, req, "ULU", fid, offset, count);
        rcv_9p(dev, req, "D", &count, data);

        if ( !count )
            break;
        if ( req->result )
        {
            ret = -1;
            errno = EIO;
            printk("9pfs: read got error %d\n", req->result);
            break;
        }
        ret += count;
        offset += count;
        data += count;
        len -= count;
    }

    put_free_req(dev, req);

    return ret;
}

static int p9_write(struct dev_9pfs *dev, uint32_t fid, uint64_t offset,
                    const uint8_t *data, uint32_t len)
{
    struct req *req = get_free_req(dev);
    int ret = 0;
    uint32_t count, count_max;

    if ( !req )
    {
        errno = EAGAIN;
        return -1;
    }
    req->cmd = P9_CMD_WRITE;
    count_max = dev->msize_max - (sizeof(struct p9_header) + sizeof(uint32_t) +
                                  sizeof(uint64_t) + sizeof(uint32_t));

    while ( len )
    {
        count = len;
        if ( count > count_max )
            count = count_max;

        send_9p(dev, req, "ULD", fid, offset, count, data);
        rcv_9p(dev, req, "U", &count);
        if ( req->result )
        {
            ret = -1;
            errno = EIO;
            printk("9pfs: write got error %d\n", req->result);
            break;
        }
        ret += count;
        offset += count;
        data += count;
        len -= count;
    }

    put_free_req(dev, req);

    return ret;
}

/*
 * Walk from root <steps> levels with the levels listed in <*paths> as a
 * sequence of names. Returns the number of steps not having been able to
 * walk, with <*paths> pointing at the name of the failing walk step.
 * <fid> will be associated with the last successful walk step. Note that
 * the first step should always succeed, as it is an empty walk in order
 * to start at the root (needed for creating new files in root).
 */
static unsigned int walk_9pfs(struct dev_9pfs *dev, uint32_t fid,
                              unsigned int steps, char **paths)
{
    uint32_t curr_fid = P9_ROOT_FID;
    int ret;

    while ( steps-- )
    {
        ret = p9_walk(dev, curr_fid, fid, *paths);
        if ( ret )
            return steps + 1;
        curr_fid = fid;
        *paths += strlen(*paths) + 1;
    }

    return 0;
}

static unsigned int split_path(const char *pathname, char **split_ptr)
{
    unsigned int parts = 1;
    char *p;

    *split_ptr = strdup(pathname);

    for ( p = strchr(*split_ptr, '/'); p; p = strchr(p + 1, '/') )
    {
        *p = 0;
        parts++;
    }

    return parts;
}

static bool path_canonical(const char *pathname)
{
    unsigned int len = strlen(pathname);
    const char *c;

    /* Empty path is allowed. */
    if ( !len )
        return true;

    /* No trailing '/'. */
    if ( pathname[len - 1] == '/' )
        return false;

    /* No self or parent references. */
    c = pathname;
    while ( (c = strstr(c, "/.")) != NULL )
    {
        if ( c[2] == '.' )
            c++;
        if ( c[2] == 0 || c[2] == '/' )
            return false;
        c += 2;
    }

    /* No "//". */
    if ( strstr(pathname, "//") )
        return false;

    return true;
}

static int connect_9pfs(struct dev_9pfs *dev)
{
    int ret;

    ret = p9_version(dev);
    if ( ret )
        return ret;

    return p9_attach(dev);
}

static void intr_9pfs(evtchn_port_t port, struct pt_regs *regs, void *data)
{
    struct dev_9pfs *dev = data;

    wake_up(&dev->waitq);
}

static int read_9pfs(struct file *file, void *buf, size_t nbytes)
{
    struct file_9pfs *f9pfs = file->filedata;
    int ret;

    ret = p9_read(f9pfs->dev, f9pfs->fid, file->offset, buf, nbytes);
    if ( ret >= 0 )
        file->offset += ret;

    return ret;
}

static int write_9pfs(struct file *file, const void *buf, size_t nbytes)
{
    struct file_9pfs *f9pfs = file->filedata;
    struct p9_stat stat;
    int ret;

    if ( f9pfs->append )
    {
        ret = p9_stat(f9pfs->dev, f9pfs->fid, &stat);
        if ( ret )
        {
            errno = EIO;
            return -1;
        }
        file->offset = stat.length;
        free_stat(&stat);
    }

    ret = p9_write(f9pfs->dev, f9pfs->fid, file->offset, buf, nbytes);
    if ( ret >= 0 )
        file->offset += ret;

    return ret;
}

static int close_9pfs(struct file *file)
{
    struct file_9pfs *f9pfs = file->filedata;

    if ( f9pfs->fid != P9_ROOT_FID )
    {
        p9_clunk(f9pfs->dev, f9pfs->fid);
        put_fid(f9pfs->dev, f9pfs->fid);
    }

    free(f9pfs);

    return 0;
}

static int open_9pfs(struct mount_point *mnt, const char *pathname, int flags,
                     mode_t mode)
{
    int fd;
    char *path = NULL;
    char *p;
    struct file *file;
    struct file_9pfs *f9pfs;
    uint16_t nwalk;
    uint8_t omode;
    int ret;

    if ( !path_canonical(pathname) )
        return EINVAL;

    f9pfs = calloc(1, sizeof(*f9pfs));
    f9pfs->dev = mnt->dev;
    f9pfs->fid = P9_ROOT_FID;

    fd = alloc_fd(ftype_9pfs);
    file = get_file_from_fd(fd);
    file->filedata = f9pfs;

    switch ( flags & O_ACCMODE )
    {
    case O_RDONLY:
        omode = P9_OREAD;
        break;
    case O_WRONLY:
        omode = P9_OWRITE;
        break;
    case O_RDWR:
        omode = P9_ORDWR;
        break;
    default:
        ret = EINVAL;
        goto err;
    }

    if ( flags & O_TRUNC )
        omode |= P9_OTRUNC;
    f9pfs->append = flags & O_APPEND;

    nwalk = split_path(pathname, &path);

    f9pfs->fid = get_fid(mnt->dev);
    if ( !f9pfs->fid )
    {
        ret = ENFILE;
        goto err;
    }
    p = path;
    nwalk = walk_9pfs(mnt->dev, f9pfs->fid, nwalk, &p);
    if ( nwalk )
    {
        if ( nwalk > 1 || !(flags & O_CREAT) )
        {
            ret = ENOENT;
            goto err;
        }

        ret = p9_create(mnt->dev, f9pfs->fid, p, mode, omode);
        if ( ret )
            goto err;
        goto out;
    }

    ret = p9_open(mnt->dev, f9pfs->fid, omode);
    if ( ret )
        goto err;

 out:
    free(path);

    return fd;

 err:
    free(path);
    close(fd);
    errno = ret;

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
    init_waitqueue_head(&dev->waitq);
    init_SEMAPHORE(&dev->ring_out_sem, 1);
    init_SEMAPHORE(&dev->ring_in_sem, 1);
    dev->fid_mask = ~0ULL;

    for ( i = 0; i < N_REQS; i++ )
    {
        dev->req[i].id = i;
        dev->req[i].next_free = i + 1;
    }
    dev->free_reqs = 0;

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

    if ( connect_9pfs(dev) )
    {
        reason = "9pfs connect failed";
        goto err;
    }

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
EXPORT_SYMBOL(init_9pfront);

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
EXPORT_SYMBOL(shutdown_9pfront);

static const struct file_ops ops_9pfs = {
    .name = "9pfs",
    .read = read_9pfs,
    .write = write_9pfs,
    .close = close_9pfs,
};

__attribute__((constructor))
static void initialize_9pfs(void)
{
    ftype_9pfs = alloc_file_type(&ops_9pfs);
}

#endif /* HAVE_LIBC */
