/*
 * sysfs-gpio-remote - frame send/recv over a TCP socket
 */
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "proto_io.h"

/* Optional hook checked on EINTR: when it returns nonzero, blocked
 * reads/writes abort instead of retrying (used for server shutdown). */
static int (*interrupt_check)(void) = NULL;

void proto_io_set_interrupt_check(int (*check)(void)) {
    interrupt_check = check;
}

void frame_header_pack(uint8_t *buf, const struct frame_header *h) {
    uint32_t m32 = htonl(h->magic);
    uint16_t m16;

    memcpy(buf + 0, &m32, 4);
    m16 = htons(h->type);
    memcpy(buf + 4, &m16, 2);
    m16 = htons(h->status);
    memcpy(buf + 6, &m16, 2);
    m32 = htonl(h->seq);
    memcpy(buf + 8, &m32, 4);
    m32 = htonl(h->payload_len);
    memcpy(buf + 12, &m32, 4);
}

int frame_header_unpack(const uint8_t *buf, struct frame_header *h) {
    uint32_t m32;
    uint16_t m16;

    memcpy(&m32, buf + 0, 4);
    h->magic = ntohl(m32);
    memcpy(&m16, buf + 4, 2);
    h->type = ntohs(m16);
    memcpy(&m16, buf + 6, 2);
    h->status = ntohs(m16);
    memcpy(&m32, buf + 8, 4);
    h->seq = ntohl(m32);
    memcpy(&m32, buf + 12, 4);
    h->payload_len = ntohl(m32);

    if (h->magic != FRAME_MAGIC)
        return -1;
    if (h->payload_len > FRAME_MAX_PAYLOAD)
        return -1;
    return 0;
}

/* write exactly size bytes; returns 0 or -1 (errno set) */
static int write_all(int fd, const uint8_t *buf, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, buf + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                if (interrupt_check && interrupt_check())
                    return -1;
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* read exactly size bytes; returns 0 / PIO_EOF / PIO_ERROR */
static int read_all(int fd, uint8_t *buf, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(fd, buf + got, size - got, 0);
        if (n == 0)
            return PIO_EOF;
        if (n < 0) {
            if (errno == EINTR) {
                if (interrupt_check && interrupt_check())
                    return PIO_ERROR;
                continue;
            }
            return PIO_ERROR;
        }
        got += (size_t)n;
    }
    return PIO_OK;
}

int send_frame(int fd, uint16_t type, uint16_t status, uint32_t seq,
               const void *payload, uint32_t payload_len) {
    if (payload_len > FRAME_MAX_PAYLOAD)
        return PIO_PROTOERR;

    struct frame_header h = {
        .magic = FRAME_MAGIC,
        .type = type,
        .status = status,
        .seq = seq,
        .payload_len = payload_len,
    };
    uint8_t hdr[FRAME_HEADER_SIZE];
    frame_header_pack(hdr, &h);

    if (write_all(fd, hdr, sizeof(hdr)) != 0)
        return PIO_ERROR;
    if (payload_len > 0) {
        if (write_all(fd, payload, payload_len) != 0)
            return PIO_ERROR;
    }
    return PIO_OK;
}

int recv_frame(int fd, struct frame_header *h, void *buf, size_t *buf_size) {
    uint8_t hdr[FRAME_HEADER_SIZE];
    int rc = read_all(fd, hdr, sizeof(hdr));
    if (rc != PIO_OK)
        return rc;

    if (frame_header_unpack(hdr, h) != 0)
        return PIO_PROTOERR;

    if (h->payload_len > 0) {
        size_t cap = buf_size ? *buf_size : 0;
        if (h->payload_len > cap)
            return PIO_PROTOERR;
        rc = read_all(fd, buf, h->payload_len);
        if (rc != PIO_OK)
            return rc;
    }
    if (buf_size)
        *buf_size = h->payload_len;
    return PIO_OK;
}
