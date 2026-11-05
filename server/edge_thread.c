/*
 * sysfs-gpio-remote server - edge detection thread
 *
 * Ported from sysfs-gpio-shim: epoll loop over gpiod line request fds.
 * Detected edges are forwarded to a callback with the pin number.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "edge_thread.h"
#include "log.h"

#define MAX_EPOLL_EVENTS 10
#define MAX_EDGE_EVENTS  32

struct edge_watch {
    struct gpiod_line_request *req;
    int pin;
};

static int epoll_fd = -1;
static pthread_t edge_thread;
static edge_callback_t callback;
static void *callback_data;

static struct edge_watch *watches = NULL;
static int num_watches = 0;
static pthread_mutex_t watches_lock = PTHREAD_MUTEX_INITIALIZER;

static struct edge_watch *watch_find(struct gpiod_line_request *req) {
    for (int i = 0; i < num_watches; i++) {
        if (watches[i].req == req)
            return &watches[i];
    }
    return NULL;
}

static void *edge_detect_loop(void *data) {
    (void)data;
    struct gpiod_edge_event_buffer *edge_events =
        gpiod_edge_event_buffer_new(MAX_EDGE_EVENTS);
    if (!edge_events) {
        log_error("edge: cannot alloc event buffer");
        return NULL;
    }
    while (1) {
        struct epoll_event epoll_events[MAX_EPOLL_EVENTS];
        int num_epolls = epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_EVENTS, -1);
        if (num_epolls == -1) {
            if (errno == EINTR)
                continue;
            log_error("edge: epoll_wait failed");
            break;
        }
        for (int i = 0; i < num_epolls; i++) {
            struct gpiod_line_request *req = epoll_events[i].data.ptr;
            int num_edges = gpiod_line_request_read_edge_events(
                req, edge_events, MAX_EDGE_EVENTS);
            if (num_edges < 0) {
                log_error("edge: read_edge_events failed");
                continue;
            }
            if (num_edges == 0 || !callback)
                continue;
            pthread_mutex_lock(&watches_lock);
            struct edge_watch *w = watch_find(req);
            int pin = w ? w->pin : -1;
            pthread_mutex_unlock(&watches_lock);
            if (pin >= 0)
                callback(pin, callback_data);
        }
    }
    gpiod_edge_event_buffer_free(edge_events);
    return NULL;
}

int edge_thread_start(edge_callback_t cb, void *user_data) {
    callback = cb;
    callback_data = user_data;
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        log_error("edge: epoll_create1 failed");
        return -1;
    }
    if (pthread_create(&edge_thread, NULL, edge_detect_loop, NULL) != 0) {
        log_error("edge: cannot start thread");
        return -1;
    }
    return 0;
}

int edge_thread_add_pin(struct gpiod_line_request *req, int pin) {
    pthread_mutex_lock(&watches_lock);
    struct edge_watch *watches_new =
        realloc(watches, (num_watches + 1) * sizeof(*watches));
    if (!watches_new) {
        pthread_mutex_unlock(&watches_lock);
        log_error("edge: cannot alloc watch");
        return -1;
    }
    watches = watches_new;
    watches[num_watches].req = req;
    watches[num_watches].pin = pin;
    num_watches++;
    pthread_mutex_unlock(&watches_lock);

    int fd = gpiod_line_request_get_fd(req);
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = req;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        log_error("edge: epoll_ctl ADD failed");
        edge_thread_remove_pin(req);
        return -1;
    }
    return 0;
}

void edge_thread_remove_pin(struct gpiod_line_request *req) {
    int fd = gpiod_line_request_get_fd(req);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1)
        log_warn("edge: epoll_ctl DEL failed");

    pthread_mutex_lock(&watches_lock);
    for (int i = 0; i < num_watches; i++) {
        if (watches[i].req == req) {
            watches[i] = watches[num_watches - 1];
            num_watches--;
            break;
        }
    }
    pthread_mutex_unlock(&watches_lock);
}
