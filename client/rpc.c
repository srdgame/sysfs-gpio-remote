/*
 * sysfs-gpio-remote client - RPC layer
 *
 * Serial request-response over a single TCP connection, with a receiver
 * thread handling async MSG_EDGE_EVENT frames and background reconnection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "rpc.h"
#include "proto_io.h"
#include "log.h"

#define RECONNECT_MIN_SEC       1
#define RECONNECT_MAX_SEC       10
#define RESPONSE_TIMEOUT_SEC    10

struct rpc_state {
    char host[128];
    int port;
    int num_pins;

    int sock;                       /* -1 when disconnected */
    pthread_mutex_t sock_lock;      /* guards sock + in-flight call */
    pthread_cond_t resp_cond;
    pthread_cond_t conn_cond;       /* signaled on connection state change */
    /* in-flight call state, owned under sock_lock */
    uint32_t next_seq;
    bool call_active;
    uint16_t call_type;
    int call_result;                /* PIO_* result of recv */
    struct frame_header call_hdr;
    uint8_t call_resp[FRAME_MAX_PAYLOAD];
    size_t call_resp_cap;

    bool connected;
    bool shutting_down;
    rpc_edge_cb_t edge_cb;
    void *edge_data;
    rpc_conn_cb_t conn_cb;
    void *conn_data;

    pthread_t recv_thread;
    pthread_t reconnect_thread;
};

static struct rpc_state rpc = {
    .sock = -1,
    .sock_lock = PTHREAD_MUTEX_INITIALIZER,
    .resp_cond = PTHREAD_COND_INITIALIZER,
    .conn_cond = PTHREAD_COND_INITIALIZER,
    .next_seq = 1,
};

int rpc_is_connected(void) {
    pthread_mutex_lock(&rpc.sock_lock);
    bool c = rpc.connected;
    pthread_mutex_unlock(&rpc.sock_lock);
    return c ? 1 : 0;
}

static void notify_conn(bool connected) {
    pthread_mutex_lock(&rpc.sock_lock);
    rpc.connected = connected;
    pthread_cond_broadcast(&rpc.conn_cond);
    pthread_mutex_unlock(&rpc.sock_lock);
    if (rpc.conn_cb)
        rpc.conn_cb(connected ? 1 : 0, rpc.conn_data);
}

static int tcp_connect(void) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", rpc.port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(rpc.host, port_str, &hints, &res);
    if (rc != 0) {
        log_error("rpc: cannot resolve %s: %s", rpc.host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1)
        return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* Receiver thread: runs while connected. On disconnect returns. */
static void *recv_loop(void *arg) {
    (void)arg;
    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    size_t cap;

    while (1) {
        pthread_mutex_lock(&rpc.sock_lock);
        int fd = rpc.sock;
        bool shutdown = rpc.shutting_down;
        pthread_mutex_unlock(&rpc.sock_lock);
        if (fd == -1 || shutdown)
            break;

        cap = sizeof(payload);
        int rc = recv_frame(fd, &h, payload, &cap);
        if (rc != PIO_OK) {
            if (rc == PIO_PROTOERR)
                log_warn("rpc: protocol violation from server");
            break;
        }

        if (h.type == MSG_EDGE_EVENT && h.payload_len == 4) {
            uint32_t pin;
            memcpy(&pin, payload, 4);
            if (rpc.edge_cb)
                rpc.edge_cb((int)ntohl(pin), rpc.edge_data);
            continue;
        }

        /* response frame for the in-flight call */
        pthread_mutex_lock(&rpc.sock_lock);
        if (rpc.call_active && h.seq == rpc.call_hdr.seq) {
            rpc.call_result = PIO_OK;
            rpc.call_hdr = h;
            size_t n = h.payload_len;
            if (n > sizeof(rpc.call_resp))
                n = sizeof(rpc.call_resp);
            memcpy(rpc.call_resp, payload, n);
            rpc.call_resp_cap = n;
            rpc.call_active = false;
            pthread_cond_broadcast(&rpc.resp_cond);
        } else {
            log_warn("rpc: unexpected frame type=%u seq=%u", h.type, h.seq);
        }
        pthread_mutex_unlock(&rpc.sock_lock);
    }

    /* connection lost: wake any in-flight call */
    pthread_mutex_lock(&rpc.sock_lock);
    if (rpc.sock != -1) {
        close(rpc.sock);
        rpc.sock = -1;
    }
    if (rpc.call_active) {
        rpc.call_result = PIO_EOF;
        rpc.call_active = false;
        pthread_cond_broadcast(&rpc.resp_cond);
    }
    pthread_mutex_unlock(&rpc.sock_lock);
    notify_conn(false);
    log_info("rpc: disconnected from server");
    return NULL;
}

static void *reconnect_loop(void *arg) {
    (void)arg;
    int backoff = RECONNECT_MIN_SEC;

    while (1) {
        /* backoff sleep, waking immediately on shutdown or disconnect */
        pthread_mutex_lock(&rpc.sock_lock);
        if (rpc.shutting_down) {
            pthread_mutex_unlock(&rpc.sock_lock);
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += backoff;
        pthread_cond_timedwait(&rpc.conn_cond, &rpc.sock_lock, &ts);
        bool skip = rpc.shutting_down || rpc.sock != -1;
        pthread_mutex_unlock(&rpc.sock_lock);
        if (rpc.shutting_down)
            break;
        if (skip)
            continue;

        int fd = tcp_connect();
        if (fd == -1) {
            log_debug("rpc: reconnect to %s:%d failed, retry in %ds",
                      rpc.host, rpc.port, backoff);
            if (backoff < RECONNECT_MAX_SEC)
                backoff *= 2;
            continue;
        }
        log_info("rpc: connected to %s:%d", rpc.host, rpc.port);
        backoff = RECONNECT_MIN_SEC;

        pthread_mutex_lock(&rpc.sock_lock);
        rpc.sock = fd;
        rpc.next_seq = 1;   /* server expects seq to restart on each connection */
        pthread_mutex_unlock(&rpc.sock_lock);
        notify_conn(true);

        if (pthread_create(&rpc.recv_thread, NULL, recv_loop, NULL) != 0) {
            log_error("rpc: cannot start recv thread");
            pthread_mutex_lock(&rpc.sock_lock);
            close(rpc.sock);
            rpc.sock = -1;
            pthread_mutex_unlock(&rpc.sock_lock);
            notify_conn(false);
        }
        pthread_detach(rpc.recv_thread);
    }
    return NULL;
}

int rpc_init(const char *host, int port, int num_pins,
             rpc_edge_cb_t edge_cb, void *edge_data,
             rpc_conn_cb_t conn_cb, void *conn_data) {
    snprintf(rpc.host, sizeof(rpc.host), "%s", host);
    rpc.port = port;
    rpc.num_pins = num_pins;
    rpc.edge_cb = edge_cb;
    rpc.edge_data = edge_data;
    rpc.conn_cb = conn_cb;
    rpc.conn_data = conn_data;

    int fd = tcp_connect();
    if (fd == -1) {
        log_warn("rpc: initial connect to %s:%d failed, "
                 "reconnecting in background", host, port);
    } else {
        log_info("rpc: connected to %s:%d", host, port);
        pthread_mutex_lock(&rpc.sock_lock);
        rpc.sock = fd;
        rpc.connected = true;
        pthread_mutex_unlock(&rpc.sock_lock);
        if (pthread_create(&rpc.recv_thread, NULL, recv_loop, NULL) == 0)
            pthread_detach(rpc.recv_thread);
        else {
            log_error("rpc: cannot start recv thread");
            close(fd);
            rpc.sock = -1;
            rpc.connected = false;
        }
        if (rpc.conn_cb)
            rpc.conn_cb(1, rpc.conn_data);
    }

    if (pthread_create(&rpc.reconnect_thread, NULL, reconnect_loop, NULL) != 0) {
        log_error("rpc: cannot start reconnect thread");
        return -1;
    }
    return 0;
}

int rpc_call(uint16_t type, const void *req, size_t req_len,
             void *resp_buf, size_t *resp_len, uint16_t *status) {
    pthread_mutex_lock(&rpc.sock_lock);

    if (rpc.sock == -1) {
        pthread_mutex_unlock(&rpc.sock_lock);
        return EIO;
    }
    if (rpc.call_active) {
        /* should not happen with serialized callers */
        pthread_mutex_unlock(&rpc.sock_lock);
        return EIO;
    }

    uint32_t seq = rpc.next_seq++;
    rpc.call_active = true;
    rpc.call_type = type;
    rpc.call_hdr.seq = seq;
    rpc.call_result = -100; /* sentinel: no response yet */
    rpc.call_resp_cap = 0;

    int fd = rpc.sock;
    int rc = send_frame(fd, type, 0, seq, req, (uint32_t)req_len);
    if (rc != PIO_OK) {
        rpc.call_active = false;
        pthread_mutex_unlock(&rpc.sock_lock);
        return EIO;
    }
    pthread_mutex_unlock(&rpc.sock_lock);

    /* wait for response or disconnect */
    pthread_mutex_lock(&rpc.sock_lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += RESPONSE_TIMEOUT_SEC;
    int wrc = 0;
    while (rpc.call_result == -100 && wrc == 0) {
        wrc = pthread_cond_timedwait(&rpc.resp_cond, &rpc.sock_lock, &ts);
    }
    if (rpc.call_result == -100) {
        log_warn("rpc: response timeout for type=%u seq=%u", type, seq);
        rpc.call_active = false;
        pthread_mutex_unlock(&rpc.sock_lock);
        return EIO;
    }
    int result = rpc.call_result;
    bool connected = rpc.connected;
    rpc.call_active = false;
    pthread_mutex_unlock(&rpc.sock_lock);

    if (result != PIO_OK)
        return EIO;
    if (!connected)
        return EIO;

    *status = rpc.call_hdr.status;
    size_t n = rpc.call_resp_cap;
    if (n > *resp_len)
        n = *resp_len;
    memcpy(resp_buf, rpc.call_resp, n);
    *resp_len = n;
    return 0;
}

void rpc_shutdown(void) {
    pthread_mutex_lock(&rpc.sock_lock);
    rpc.shutting_down = true;
    pthread_cond_broadcast(&rpc.conn_cond);
    if (rpc.sock != -1) {
        shutdown(rpc.sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&rpc.sock_lock);
    pthread_join(rpc.reconnect_thread, NULL);
}
