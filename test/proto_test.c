/*
 * Unit tests for frame header pack/unpack and frame send/recv over a socketpair.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "proto_io.h"
#include "log.h"

static int tests_run = 0;

#define TEST(name) do { printf("  ok: %s\n", name); tests_run++; } while (0)

static void test_header_roundtrip(void) {
    struct frame_header h = {
        .magic = FRAME_MAGIC,
        .type = MSG_REQ_SET_VALUE,
        .status = 0,
        .seq = 0xDEADBEEF,
        .payload_len = 5,
    };
    uint8_t buf[FRAME_HEADER_SIZE];
    frame_header_pack(buf, &h);

    /* verify network byte order layout */
    uint32_t m = ntohl(*(uint32_t *)buf);
    assert(m == FRAME_MAGIC);
    uint16_t t = ntohs(*(uint16_t *)(buf + 4));
    assert(t == MSG_REQ_SET_VALUE);

    struct frame_header out;
    assert(frame_header_unpack(buf, &out) == 0);
    assert(out.magic == FRAME_MAGIC);
    assert(out.type == MSG_REQ_SET_VALUE);
    assert(out.seq == 0xDEADBEEF);
    assert(out.payload_len == 5);
    TEST("header pack/unpack roundtrip");
}

static void test_header_bad_magic(void) {
    struct frame_header h = { .magic = 0x12345678, .payload_len = 0 };
    uint8_t buf[FRAME_HEADER_SIZE];
    frame_header_pack(buf, &h);
    struct frame_header out;
    assert(frame_header_unpack(buf, &out) == -1);
    TEST("bad magic rejected");
}

static void test_header_oversize_payload(void) {
    struct frame_header h = { .magic = FRAME_MAGIC,
                              .payload_len = FRAME_MAX_PAYLOAD + 1 };
    uint8_t buf[FRAME_HEADER_SIZE];
    frame_header_pack(buf, &h);
    struct frame_header out;
    assert(frame_header_unpack(buf, &out) == -1);
    TEST("oversize payload rejected");
}

static void test_send_recv_roundtrip(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    uint8_t payload[5] = { 0x00, 0x00, 0x00, 0x11, 0x01 }; /* pin 17, value 1 */
    assert(send_frame(sv[0], MSG_REQ_SET_VALUE, 0, 42, payload, 5) == PIO_OK);

    struct frame_header h;
    uint8_t rbuf[FRAME_MAX_PAYLOAD];
    size_t cap = sizeof(rbuf);
    assert(recv_frame(sv[1], &h, rbuf, &cap) == PIO_OK);
    assert(h.type == MSG_REQ_SET_VALUE);
    assert(h.seq == 42);
    assert(h.payload_len == 5);
    assert(cap == 5);
    assert(memcmp(rbuf, payload, 5) == 0);

    close(sv[0]);
    close(sv[1]);
    TEST("send/recv frame roundtrip with payload");
}

static void test_send_recv_empty(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    assert(send_frame(sv[0], MSG_REQ_PING, 0, 7, NULL, 0) == PIO_OK);

    struct frame_header h;
    size_t cap = 0;
    assert(recv_frame(sv[1], &h, NULL, &cap) == PIO_OK);
    assert(h.type == MSG_REQ_PING);
    assert(h.seq == 7);
    assert(h.payload_len == 0);

    close(sv[0]);
    close(sv[1]);
    TEST("send/recv frame without payload");
}

static void test_recv_truncated_payload_is_protoerr(void) {
    /* payload larger than buffer cap must fail with PIO_PROTOERR */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    uint8_t payload[10] = {0};
    assert(send_frame(sv[0], MSG_REQ_SET_EDGE, 0, 1, payload, 10) == PIO_OK);

    struct frame_header h;
    uint8_t rbuf[4];
    size_t cap = sizeof(rbuf);
    assert(recv_frame(sv[1], &h, rbuf, &cap) == PIO_PROTOERR);

    close(sv[0]);
    close(sv[1]);
    TEST("oversize incoming payload rejected");
}

static void test_recv_eof(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[0]); /* trigger EOF on peer */

    struct frame_header h;
    size_t cap = 0;
    assert(recv_frame(sv[1], &h, NULL, &cap) == PIO_EOF);

    close(sv[1]);
    TEST("EOF detected");
}

static void test_oversize_send_rejected(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    uint8_t payload[FRAME_MAX_PAYLOAD + 1] = {0};
    assert(send_frame(sv[0], MSG_REQ_PING, 0, 0, payload,
                      FRAME_MAX_PAYLOAD + 1) == PIO_PROTOERR);
    close(sv[0]);
    close(sv[1]);
    TEST("oversize outgoing payload rejected");
}

int main(void) {
    log_set_level(LOG_ERROR);

    test_header_roundtrip();
    test_header_bad_magic();
    test_header_oversize_payload();
    test_send_recv_roundtrip();
    test_send_recv_empty();
    test_recv_truncated_payload_is_protoerr();
    test_recv_eof();
    test_oversize_send_rejected();

    printf("%d proto tests passed\n", tests_run);
    return 0;
}
