/*
 * sysfs-gpio-remote server - client connection handling
 *
 * Single-client request loop. Owns the per-connection edge subscription
 * table and the socket write lock (shared with the edge push path).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "connection.h"
#include "proto_io.h"
#include "log.h"

struct edge_waiter {
    bool subscribed;
    bool edge_detected;
};

/* single-client state */
static int client_fd = -1;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct edge_waiter *waiters = NULL; /* [num_pins] */
static int waiters_len = 0;

int connection_setup(int num_pins) {
    waiters = calloc(num_pins, sizeof(*waiters));
    if (!waiters)
        return -1;
    waiters_len = num_pins;
    return 0;
}

/* Edge callback: runs on the edge detection thread. */
static void connection_edge_event(int n, void *user_data) {
    (void)user_data;
    if (n < 0 || n >= waiters_len)
        return;

    pthread_mutex_lock(&state_lock);
    if (client_fd == -1) {
        pthread_mutex_unlock(&state_lock);
        return;
    }
    waiters[n].edge_detected = true;
    int fd = client_fd;
    pthread_mutex_unlock(&state_lock);

    uint8_t payload[4];
    uint32_t pin_be = htonl((uint32_t)n);
    memcpy(payload, &pin_be, 4);

    pthread_mutex_lock(&write_lock);
    int rc = send_frame(fd, MSG_EDGE_EVENT, 0, 0, payload, 4);
    pthread_mutex_unlock(&write_lock);
    if (rc != PIO_OK)
        log_warn("connection: failed to push edge event for GPIO%d", n);
}

int connection_try_claim(int fd) {
    pthread_mutex_lock(&state_lock);
    if (client_fd != -1) {
        pthread_mutex_unlock(&state_lock);
        return 0;
    }
    client_fd = fd;
    pthread_mutex_unlock(&state_lock);
    return 1;
}

static uint32_t payload_decode_pin(const uint8_t *buf) {
    uint32_t be;
    memcpy(&be, buf, 4);
    return ntohl(be);
}

/*
 * Dispatch a request. Returns errno to report in the response status and
 * fills resp/resp_len if a response payload is produced.
 */
static uint16_t dispatch_request(const struct frame_header *req,
                                 const uint8_t *payload,
                                 uint8_t *resp, size_t *resp_len) {
    *resp_len = 0;
    uint32_t pin = 0;
    uint8_t arg = 0;
    uint8_t poll_flags = 0;
    int ret = 0;

    switch (req->type) {
    case MSG_REQ_EXPORT:
    case MSG_REQ_UNEXPORT:
    case MSG_REQ_GET_VALUE:
        if (req->payload_len != 4)
            return EINVAL;
        pin = payload_decode_pin(payload);
        break;
    case MSG_REQ_POLL:
        if (req->payload_len != 5)
            return EINVAL;
        pin = payload_decode_pin(payload);
        poll_flags = payload[4];
        break;
    case MSG_REQ_SET_VALUE:
    case MSG_REQ_SET_DIRECTION:
    case MSG_REQ_SET_EDGE:
    case MSG_REQ_SET_ACTIVE_LOW:
        if (req->payload_len != 5)
            return EINVAL;
        pin = payload_decode_pin(payload);
        arg = payload[4];
        break;
    case MSG_REQ_PING:
        if (req->payload_len != 0)
            return EINVAL;
        return 0;
    default:
        return EINVAL;
    }

    if (pin >= (uint32_t)waiters_len)
        return EINVAL;
    struct edge_waiter *waiter = &waiters[pin];

    /* REQ_POLL only touches subscription state */
    if (req->type == MSG_REQ_POLL) {
        pthread_mutex_lock(&state_lock);
        /* a new poll session must not see edges from earlier sessions */
        if (poll_flags & MSG_POLL_F_NEW_SESSION)
            waiter->edge_detected = false;
        if (waiter->subscribed && waiter->edge_detected) {
            waiter->edge_detected = false;
            pthread_mutex_unlock(&state_lock);
            resp[0] = 1;
            *resp_len = 1;
            return 0;
        }
        waiter->subscribed = true;
        pthread_mutex_unlock(&state_lock);
        resp[0] = 0;
        *resp_len = 1;
        return 0;
    }

    switch (req->type) {
    case MSG_REQ_EXPORT:
        ret = pin_export((int)pin);
        break;
    case MSG_REQ_UNEXPORT:
        ret = pin_unexport((int)pin);
        if (ret == 0) {
            pthread_mutex_lock(&state_lock);
            waiter->subscribed = false;
            waiter->edge_detected = false;
            pthread_mutex_unlock(&state_lock);
        }
        break;
    case MSG_REQ_GET_VALUE:
        ret = pin_get_value((int)pin, &resp[0]);
        if (ret == 0)
            *resp_len = 1;
        break;
    case MSG_REQ_SET_VALUE:
        ret = pin_set_value((int)pin, arg);
        break;
    case MSG_REQ_SET_DIRECTION:
        ret = pin_set_direction((int)pin, arg);
        break;
    case MSG_REQ_SET_EDGE:
        ret = pin_set_edge((int)pin, arg);
        break;
    case MSG_REQ_SET_ACTIVE_LOW:
        ret = pin_set_active_low((int)pin, arg);
        break;
    }

    return (uint16_t)ret;
}

void connection_serve(int fd) {
    struct frame_header req;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    uint8_t resp[FRAME_MAX_PAYLOAD];
    uint32_t expect_seq = 1;

    log_info("connection: client connected");

    while (1) {
        size_t cap = sizeof(payload);
        int rc = recv_frame(fd, &req, payload, &cap);
        if (rc == PIO_EOF)
            break;
        if (rc == PIO_ERROR) {
            log_warn("connection: recv error: %s", strerror(errno));
            break;
        }
        if (rc == PIO_PROTOERR) {
            log_warn("connection: protocol violation, dropping client");
            break;
        }
        if (req.seq != expect_seq) {
            log_warn("connection: seq mismatch (got %u, want %u)",
                     req.seq, expect_seq);
            break;
        }

        size_t resp_len = 0;
        uint16_t status = dispatch_request(&req, payload, resp, &resp_len);

        pthread_mutex_lock(&write_lock);
        int rc2 = send_frame(fd, req.type, status, req.seq, resp, resp_len);
        pthread_mutex_unlock(&write_lock);
        if (rc2 != PIO_OK) {
            log_warn("connection: send error, dropping client");
            break;
        }
        expect_seq++;
    }

    /* clear subscriptions */
    pthread_mutex_lock(&state_lock);
    client_fd = -1;
    for (int i = 0; i < waiters_len; i++) {
        waiters[i].subscribed = false;
        waiters[i].edge_detected = false;
    }
    pthread_mutex_unlock(&state_lock);
    log_info("connection: client disconnected");
}

edge_callback_t connection_edge_callback(void) {
    return connection_edge_event;
}
