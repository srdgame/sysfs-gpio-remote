/*
 * sysfs-gpio-remote - frame send/recv over a TCP socket
 */
#ifndef PROTO_IO_H
#define PROTO_IO_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

/* result codes for send/recv */
#define PIO_OK          0
#define PIO_EOF        -1   /* peer closed cleanly */
#define PIO_ERROR      -2   /* socket error (errno set by syscall) */
#define PIO_PROTOERR   -3   /* protocol violation (bad magic, oversize) */

/*
 * Register a hook checked on EINTR inside blocking read/write loops.
 * When it returns nonzero, the operation aborts with an error instead of
 * retrying. Used by the server so SIGTERM can unblock a live connection.
 * Call with NULL to clear. Not thread-safe (call once at startup).
 */
void proto_io_set_interrupt_check(int (*check)(void));

/*
 * Send one frame. payload may be NULL when payload_len == 0.
 * Returns PIO_OK or an error code.
 */
int send_frame(int fd, uint16_t type, uint16_t status, uint32_t seq,
               const void *payload, uint32_t payload_len);

/*
 * Receive one frame. On success, header fields are filled in and payload
 * (if payload_len > 0) is copied into buf of size buf_size. If the incoming
 * payload exceeds buf_size, PIO_PROTOERR is returned.
 * payload may be NULL together with *buf_size = 0 to skip payload capture.
 */
int recv_frame(int fd, struct frame_header *h,
               void *buf, size_t *buf_size);

#endif /* PROTO_IO_H */
