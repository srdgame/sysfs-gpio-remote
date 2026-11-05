/*
 * sysfs-gpio-remote client - FUSE operations
 *
 * Ported from sysfs-gpio-shim; local gpiod calls replaced by RPC.
 */
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>

#include "fuse_ops.h"
#include "rpc.h"
#include "state.h"
#include "log.h"
#include <arpa/inet.h>
#include <sys/stat.h>

#define FILE2FH(p) ((uint64_t)(uintptr_t)(p))
#define FH2FILE(u) ((gpio_file_t*)(uintptr_t)(u))

#define str_equal(a, b) (strcmp(a, b) == 0)
#define str_startswith(a, b) (strncmp(a, b, sizeof(b) - 1) == 0)

static int str_buf_equal(const char *buf, size_t size, const char *value) {
    size_t value_len = strlen(value);
    if (size < value_len)
        return 0;
    size_t buf_len = 0;
    for (; buf_len < size; buf_len++) {
        if (buf[buf_len] == '\0' || buf[buf_len] == '\n')
            break;
    }
    if (buf_len != value_len)
        return 0;
    return strncmp(buf, value, buf_len) == 0;
}

static inline int min_int(int a, int b) {
    return a < b ? a : b;
}

/////////////////////////////////////////////////////////////////////
/* file handles */

typedef struct {
    int pin;
    const char *suffix;   /* NULL for pin dir, else "/value" etc. */
    int buf_fill;
    char buf[16];
} gpio_file_t;

static int num_pins = 0;

/* mirror of shim's gpio_file(): parse "/gpioN[/suffix]" or "/name[/suffix]"
 * for named exports */
static gpio_file_t *gpio_file(const char *path) {
    int pin = -1;
    const char *rest = NULL;

    if (str_startswith(path, "/gpio")) {
        /* numeric fast path */
        const char *p_start = path + 5;
        char *p_end = NULL;
        long int p = strtol(p_start, &p_end, 10);
        if (p_start != p_end && p >= 0 && p < num_pins) {
            pin = (int)p;
            rest = p_end;
        }
    }
    if (pin < 0) {
        /* named export: first path component resolved via state */
        if (path[0] != '/')
            return NULL;
        const char *end = strchr(path + 1, '/');
        size_t name_len = end ? (size_t)(end - path - 1) : strlen(path + 1);
        if (name_len < 1 || name_len > GPIO_NAME_MAX)
            return NULL;
        char name[GPIO_NAME_MAX + 1];
        memcpy(name, path + 1, name_len);
        name[name_len] = '\0';
        pin = state_find_by_name(name);
        if (pin < 0)
            return NULL;
        rest = end ? end : path + strlen(path);
    }
    if (!state_is_exported(pin))
        return NULL;

    gpio_file_t *file = calloc(1, sizeof(*file));
    if (!file)
        return NULL;
    file->pin = pin;
    file->suffix = *rest == '\0' ? NULL : strdup(rest);
    return file;
}

static void gpio_file_free(gpio_file_t *file) {
    assert(file);
    free((void *)file->suffix);
    free(file);
}

static int pin_from_num(const char *buf, size_t size, int *pin_out) {
    if (size == 0 || size > 10)
        return 0;
    char tmp[11];
    memcpy(tmp, buf, size);
    tmp[size] = '\0';
    char *end = NULL;
    long p = strtol(tmp, &end, 10);
    if (end == tmp || p < 0 || p >= num_pins)
        return 0;
    *pin_out = (int)p;
    return 1;
}

/* Parse a pin name from an export/unexport write ("pcie_on\n").
 * Returns 1 and fills out (NUL-terminated) on success. */
static int name_from_buf(const char *buf, size_t size,
                         char *out, size_t out_size) {
    if (size > 0 && buf[size - 1] == '\n')
        size--;
    if (size < 1 || size > GPIO_NAME_MAX || size >= out_size)
        return 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '/' || !isprint(c))
            return 0;
    }
    memcpy(out, buf, size);
    out[size] = '\0';
    return 1;
}

/////////////////////////////////////////////////////////////////////
/* RPC helpers; return errno to be negated for FUSE, 0 on success */

static int rpc_ready(void) {
    if (!rpc_is_connected())
        return EIO;
    if (state_wait_ready() != 0)
        return EIO;
    return 0;
}

static int rpc_pin_call(uint16_t type, int pin, int arg) {
    int ready = rpc_ready();
    if (ready != 0)
        return ready;
    uint8_t payload[5];
    uint32_t be = htonl((uint32_t)pin);
    memcpy(payload, &be, 4);
    size_t len = 4;
    if (arg >= 0) {
        payload[4] = (uint8_t)arg;
        len = 5;
    }
    uint8_t resp[FRAME_MAX_PAYLOAD];
    size_t resp_len = sizeof(resp);
    uint16_t status = 0;
    int rc = rpc_call(type, payload, len, resp, &resp_len, &status);
    if (rc != 0)
        return rc;
    return status;
}

static int rpc_get_value(int pin, uint8_t *value) {
    int ready = rpc_ready();
    if (ready != 0)
        return ready;
    uint8_t payload[4];
    uint32_t be = htonl((uint32_t)pin);
    memcpy(payload, &be, 4);
    uint8_t resp[FRAME_MAX_PAYLOAD];
    size_t resp_len = sizeof(resp);
    uint16_t status = 0;
    int rc = rpc_call(MSG_REQ_GET_VALUE, payload, 4, resp, &resp_len, &status);
    if (rc != 0)
        return rc;
    if (status != 0)
        return status;
    if (resp_len != 1)
        return EPROTO;
    *value = resp[0];
    return 0;
}

/////////////////////////////////////////////////////////////////////
/* poll waiters */

struct poll_waiter {
    gpio_file_t *file;
    struct fuse_pollhandle *ph;
    bool edge_detected;
    struct poll_waiter *next;
};

static pthread_mutex_t poll_lock = PTHREAD_MUTEX_INITIALIZER;
static struct poll_waiter *poll_waiters = NULL;

void fuse_ops_edge_event(int n) {
    pthread_mutex_lock(&poll_lock);
    for (struct poll_waiter *w = poll_waiters; w; w = w->next) {
        if (w->file->pin != n || !w->ph)
            continue;
        w->edge_detected = true;
        fuse_notify_poll(w->ph);
        fuse_pollhandle_destroy(w->ph);
        w->ph = NULL;
    }
    pthread_mutex_unlock(&poll_lock);
}

static void poll_waiter_remove(gpio_file_t *file) {
    pthread_mutex_lock(&poll_lock);
    struct poll_waiter **pp = &poll_waiters;
    while (*pp) {
        if ((*pp)->file == file) {
            struct poll_waiter *dead = *pp;
            *pp = dead->next;
            if (dead->ph)
                fuse_pollhandle_destroy(dead->ph);
            free(dead);
            continue;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&poll_lock);
}

/* REQ_POLL: returns 0 (errno-style) and sets *pending from server response */
static int rpc_poll(int pin, bool new_session, uint8_t *pending) {
    int ready = rpc_ready();
    if (ready != 0)
        return ready;
    uint8_t payload[5];
    uint32_t be = htonl((uint32_t)pin);
    memcpy(payload, &be, 4);
    payload[4] = new_session ? MSG_POLL_F_NEW_SESSION : 0;
    uint8_t resp[FRAME_MAX_PAYLOAD];
    size_t resp_len = sizeof(resp);
    uint16_t status = 0;
    int rc = rpc_call(MSG_REQ_POLL, payload, sizeof(payload), resp, &resp_len, &status);
    if (rc != 0)
        return rc;
    if (status != 0)
        return status;
    if (resp_len != 1)
        return EPROTO;
    *pending = resp[0];
    return 0;
}

/////////////////////////////////////////////////////////////////////
/* FUSE operations */

static int gpio_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (str_equal(path, "/")) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    if (str_equal(path, "/export") || str_equal(path, "/unexport")) {
        stbuf->st_mode = S_IFREG | 0220;
        stbuf->st_nlink = 1;
        stbuf->st_size = 4096;
        return 0;
    }

    gpio_file_t *file = gpio_file(path);
    if (!file)
        return -ENOENT;
    if (!file->suffix) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0660;
        stbuf->st_nlink = 1;
        stbuf->st_size = 4096;
    }
    gpio_file_free(file);
    return 0;
}

static int gpio_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    if (str_equal(path, "/")) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        filler(buf, "export", NULL, 0, 0);
        filler(buf, "unexport", NULL, 0, 0);
        for (int p = 0; p < num_pins; p++) {
            if (!state_is_exported(p))
                continue;
            char name[64];
            state_name_of(p, name, sizeof(name));
            filler(buf, name, NULL, 0, 0);
        }
        return 0;
    }

    gpio_file_t *file = gpio_file(path);
    if (!file)
        return -ENOENT;
    if (file->suffix) {
        gpio_file_free(file);
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, "direction", NULL, 0, 0);
    filler(buf, "value", NULL, 0, 0);
    filler(buf, "edge", NULL, 0, 0);
    filler(buf, "active_low", NULL, 0, 0);
    gpio_file_free(file);
    return 0;
}

static int gpio_open(const char *path, struct fuse_file_info *fi) {
    if (str_equal(path, "/export") || str_equal(path, "/unexport")) {
        if ((fi->flags & O_ACCMODE) != O_WRONLY)
            return -EACCES;
        return 0;
    }

    gpio_file_t *file = gpio_file(path);
    if (!file)
        return -ENOENT;
    if (!file->suffix ||
        (!str_equal(file->suffix, "/direction") &&
         !str_equal(file->suffix, "/value") &&
         !str_equal(file->suffix, "/edge") &&
         !str_equal(file->suffix, "/active_low"))) {
        gpio_file_free(file);
        return -ENOENT;
    }

    fi->fh = FILE2FH(file);
    fi->direct_io = 1;
    return 0;
}

static int gpio_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    if (fi->fh) {
        gpio_file_t *file = FH2FILE(fi->fh);
        poll_waiter_remove(file);
        gpio_file_free(file);
    }
    return 0;
}

#define buf_update_maybe(file, offset, fmt, value) do {                      \
    if (file->buf_fill == 0 || offset == 0) {                                \
        file->buf_fill = snprintf(file->buf, sizeof(file->buf), fmt, value); \
    }                                                                        \
} while (0)

static int gpio_read(const char *path, char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
    (void)path;
    if (!fi->fh)
        return -EINVAL;
    gpio_file_t *file = FH2FILE(fi->fh);
    assert(file->suffix);

    if (str_equal(file->suffix, "/value")) {
        uint8_t value = 0;
        int rc = rpc_get_value(file->pin, &value);
        if (rc != 0)
            return -rc;
        buf_update_maybe(file, offset, "%d", value);
    } else if (str_equal(file->suffix, "/direction")) {
        uint8_t d = state_get_direction(file->pin);
        buf_update_maybe(file, offset, "%s",
            d == DIR_IN ? "in" : "out");
    } else if (str_equal(file->suffix, "/edge")) {
        uint8_t e = state_get_edge(file->pin);
        buf_update_maybe(file, offset, "%s",
            e == EDGE_NONE    ? "none" :
            e == EDGE_RISING  ? "rising" :
            e == EDGE_FALLING ? "falling" :
                                "both");
    } else if (str_equal(file->suffix, "/active_low")) {
        buf_update_maybe(file, offset, "%d", state_get_active_low(file->pin));
    } else {
        return -EINVAL;
    }

    assert(file->buf_fill > 0);
    if (offset >= file->buf_fill)
        return 0;
    int readable = min_int((int)size, file->buf_fill - (int)offset);
    memcpy(buf, file->buf + offset, readable);
    return readable;
}

static int gpio_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
    (void)offset;
    if (str_equal(path, "/export")) {
        int pin;
        if (pin_from_num(buf, size, &pin)) {
            int rc = rpc_pin_call(MSG_REQ_EXPORT, pin, -1);
            if (rc != 0)
                return -rc;
            /* idempotent re-export: keep the cached pin config */
            if (!state_is_exported(pin))
                state_on_export(pin);
            return (int)size;
        }
        /* named export (OpenWrt gpio-export style) */
        char name[GPIO_NAME_MAX + 1];
        if (!name_from_buf(buf, size, name, sizeof(name)))
            return -EINVAL;
        int slot = -1;
        int rc = rpc_export_name(name, &slot);
        if (rc != 0)
            return -rc;
        if (!state_is_exported(slot))
            state_on_export_named(slot, name);
        return (int)size;
    }
    if (str_equal(path, "/unexport")) {
        int pin;
        if (!pin_from_num(buf, size, &pin)) {
            char name[GPIO_NAME_MAX + 1];
            if (!name_from_buf(buf, size, name, sizeof(name)))
                return -EINVAL;
            pin = state_find_by_name(name);
            if (pin < 0)
                return -ENOENT;
        }
        int rc = rpc_pin_call(MSG_REQ_UNEXPORT, pin, -1);
        if (rc != 0)
            return -rc;
        state_on_unexport(pin);
        return (int)size;
    }

    if (!fi->fh)
        return -EINVAL;
    gpio_file_t *file = FH2FILE(fi->fh);
    assert(file->suffix);
    int rc;

    if (str_equal(file->suffix, "/value")) {
        rc = rpc_pin_call(MSG_REQ_SET_VALUE, file->pin,
                          str_buf_equal(buf, size, "0") ? 0 : 1);
        if (rc != 0)
            return -rc;
        state_on_value(file->pin, str_buf_equal(buf, size, "0") ? 0 : 1);
    } else if (str_equal(file->suffix, "/direction")) {
        uint8_t dir;
        if (str_buf_equal(buf, size, "out") || str_buf_equal(buf, size, "low"))
            dir = DIR_OUT;
        else if (str_buf_equal(buf, size, "high"))
            dir = DIR_OUT_HIGH;
        else if (str_buf_equal(buf, size, "in"))
            dir = DIR_IN;
        else
            return -EINVAL;
        rc = rpc_pin_call(MSG_REQ_SET_DIRECTION, file->pin, dir);
        if (rc != 0)
            return -rc;
        state_on_direction(file->pin, dir);
    } else if (str_equal(file->suffix, "/edge")) {
        uint8_t edge;
        if (str_buf_equal(buf, size, "none"))
            edge = EDGE_NONE;
        else if (str_buf_equal(buf, size, "rising"))
            edge = EDGE_RISING;
        else if (str_buf_equal(buf, size, "falling"))
            edge = EDGE_FALLING;
        else if (str_buf_equal(buf, size, "both"))
            edge = EDGE_BOTH;
        else
            return -EINVAL;
        rc = rpc_pin_call(MSG_REQ_SET_EDGE, file->pin, edge);
        if (rc != 0)
            return -rc;
        state_on_edge(file->pin, edge);
    } else if (str_equal(file->suffix, "/active_low")) {
        uint8_t val = str_buf_equal(buf, size, "0") ? 0 : 1;
        rc = rpc_pin_call(MSG_REQ_SET_ACTIVE_LOW, file->pin, val);
        if (rc != 0)
            return -rc;
        state_on_active_low(file->pin, val);
    } else {
        return -EINVAL;
    }
    return (int)size;
}

static int gpio_poll(const char *path, struct fuse_file_info *fi,
                     struct fuse_pollhandle *ph, unsigned *reventsp) {
    (void)path;
    if (!fi->fh)
        return -EINVAL;
    gpio_file_t *file = FH2FILE(fi->fh);
    assert(file->suffix);
    if (!str_equal(file->suffix, "/value"))
        return -EACCES;

    /* check local pending edge */
    pthread_mutex_lock(&poll_lock);
    for (struct poll_waiter *w = poll_waiters; w; w = w->next) {
        if (w->file != file)
            continue;
        if (w->edge_detected) {
            w->edge_detected = false;
            pthread_mutex_unlock(&poll_lock);
            *reventsp |= POLLPRI;
            return 0;
        }
        /* refresh pollhandle for this file */
        if (ph) {
            if (w->ph)
                fuse_pollhandle_destroy(w->ph);
            w->ph = ph;
            ph = NULL;
        }
        break;
    }
    pthread_mutex_unlock(&poll_lock);

    bool new_session = false;
    if (ph) {
        /* first poll on this file: register waiter */
        struct poll_waiter *w = calloc(1, sizeof(*w));
        if (!w)
            return -ENOMEM;
        w->file = file;
        w->ph = ph;
        pthread_mutex_lock(&poll_lock);
        w->next = poll_waiters;
        poll_waiters = w;
        pthread_mutex_unlock(&poll_lock);
        new_session = true;
    }

    /* subscribe on the server / check for pending edge there */
    uint8_t pending = 0;
    int rc = rpc_poll(file->pin, new_session, &pending);
    if (rc != 0)
        return -rc;
    if (pending)
        *reventsp |= POLLPRI;
    return 0;
}

/////////////////////////////////////////////////////////////////////

void fuse_ops_setup(struct fuse_operations *oper, int npins) {
    memset(oper, 0, sizeof(*oper));
    num_pins = npins;
    oper->getattr = gpio_getattr;
    oper->readdir = gpio_readdir;
    oper->open = gpio_open;
    oper->release = gpio_release;
    oper->read = gpio_read;
    oper->write = gpio_write;
    oper->poll = gpio_poll;
}
