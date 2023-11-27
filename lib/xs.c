/*
 * libxs-compatible layer
 *
 * Samuel Thibault <Samuel.Thibault@eu.citrix.net>, 2007-2008
 *
 * Mere wrapper around xenbus_*
 */

#ifdef HAVE_LIBC
#include <os.h>
#include <lib.h>
#include <xenstore.h>
#include <xenbus.h>
#include <stdlib.h>
#include <unistd.h>

static inline int _xs_fileno(struct xs_handle *h) {
    return (intptr_t) h;
}

static int xs_close_fd(struct file *file)
{
    struct xenbus_event *event, *next;

    for ( event = file->dev; event; event = next )
    {
        next = event->next;
        free(event);
    }

    return 0;
}

static bool xs_can_read(struct file *file)
{
    return file->dev;
}

static const struct file_ops xenbus_ops = {
    .name = "xenbus",
    .close = xs_close_fd,
    .select_rd = xs_can_read,
};

static unsigned int ftype_xenbus;

__attribute__((constructor))
static void xs_initialize(void)
{
    ftype_xenbus = alloc_file_type(&xenbus_ops);
}

struct xs_handle *xs_daemon_open()
{
    int fd;
    struct file *file;

    fd = alloc_fd(ftype_xenbus);
    file = get_file_from_fd(fd);
    if ( !file )
        return NULL;

    file->dev = NULL;
    printk("xs_daemon_open -> %d, %p\n", fd, &file->dev);
    return (void*)(intptr_t) fd;
}
EXPORT_SYMBOL(xs_daemon_open);

void xs_daemon_close(struct xs_handle *h)
{
    close(_xs_fileno(h));
}

int xs_fileno(struct xs_handle *h)
{
    return _xs_fileno(h);
}
EXPORT_SYMBOL(xs_fileno);

void *xs_read(struct xs_handle *h, xs_transaction_t t,
	     const char *path, unsigned int *len)
{
    char *value;
    char *msg;

    msg = xenbus_read(t, path, &value);
    if (msg) {
	printk("xs_read(%s): %s\n", path, msg);
	free(msg);
	return NULL;
    }

    if (len)
	*len = strlen(value);
    return value;
}
EXPORT_SYMBOL(xs_read);

bool xs_write(struct xs_handle *h, xs_transaction_t t,
	      const char *path, const void *data, unsigned int len)
{
    char value[len + 1];
    char *msg;

    memcpy(value, data, len);
    value[len] = 0;

    msg = xenbus_write(t, path, value);
    if (msg) {
	printk("xs_write(%s): %s\n", path, msg);
	free(msg);
	return false;
    }
    return true;
}
EXPORT_SYMBOL(xs_write);

static bool xs_bool(char *reply)
{
    if (!reply)
	return true;
    free(reply);
    return false;
}

bool xs_rm(struct xs_handle *h, xs_transaction_t t, const char *path)
{
    return xs_bool(xenbus_rm(t, path));
}
EXPORT_SYMBOL(xs_rm);

static void *xs_talkv(struct xs_handle *h, xs_transaction_t t,
		enum xsd_sockmsg_type type,
		struct write_req *iovec,
		unsigned int num_vecs,
		unsigned int *len)
{
    struct xsd_sockmsg *msg;
    void *ret;

    msg = xenbus_msg_reply(type, t, iovec, num_vecs);
    ret = malloc(msg->len);
    memcpy(ret, (char*) msg + sizeof(*msg), msg->len);
    if (len)
	*len = msg->len - 1;
    free(msg);
    return ret;
}

static void *xs_single(struct xs_handle *h, xs_transaction_t t,
		enum xsd_sockmsg_type type,
		const char *string,
		unsigned int *len)
{
    struct write_req iovec;

    iovec.data = (void *)string;
    iovec.len = strlen(string) + 1;

    return xs_talkv(h, t, type, &iovec, 1, len);
}

char *xs_get_domain_path(struct xs_handle *h, unsigned int domid)
{
    char domid_str[MAX_STRLEN(domid)];

    sprintf(domid_str, "%u", domid);

    return xs_single(h, XBT_NULL, XS_GET_DOMAIN_PATH, domid_str, NULL);
}
EXPORT_SYMBOL(xs_get_domain_path);

char **xs_directory(struct xs_handle *h, xs_transaction_t t,
		    const char *path, unsigned int *num)
{
    char *msg;
    char **entries, **res;
    char *entry;
    int i, n;
    int size;

    msg = xenbus_ls(t, path, &res);
    if (msg) {
	printk("xs_directory(%s): %s\n", path, msg);
	free(msg);
	return NULL;
    }

    size = 0;
    for (n = 0; res[n]; n++)
	size += strlen(res[n]) + 1;

    entries = malloc(n * sizeof(char *) + size);
    entry = (char *) (&entries[n]);

    for (i = 0; i < n; i++) {
	int l = strlen(res[i]) + 1;
	memcpy(entry, res[i], l);
	free(res[i]);
	entries[i] = entry;
	entry += l;
    }

    *num = n;
    free(res);
    return entries;
}
EXPORT_SYMBOL(xs_directory);

bool xs_watch(struct xs_handle *h, const char *path, const char *token)
{
    struct file *file = get_file_from_fd(_xs_fileno(h));

    printk("xs_watch(%s, %s)\n", path, token);
    return xs_bool(xenbus_watch_path_token(XBT_NULL, path, token,
                                           (xenbus_event_queue *)&file->dev));
}
EXPORT_SYMBOL(xs_watch);

char **xs_read_watch(struct xs_handle *h, unsigned int *num)
{
    struct xenbus_event *event;
    struct file *file = get_file_from_fd(_xs_fileno(h));

    event = file->dev;
    file->dev = event->next;
    printk("xs_read_watch() -> %s %s\n", event->path, event->token);
    *num = 2;
    return (char **) &event->path;
}
EXPORT_SYMBOL(xs_read_watch);

bool xs_unwatch(struct xs_handle *h, const char *path, const char *token)
{
    printk("xs_unwatch(%s, %s)\n", path, token);
    return xs_bool(xenbus_unwatch_path_token(XBT_NULL, path, token));
}
EXPORT_SYMBOL(xs_unwatch);
#endif
