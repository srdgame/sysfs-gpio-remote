/*
 * sysfs-gpio-remote - wire protocol definitions
 *
 * Frame layout (all fields network byte order):
 *   [u32 magic][u16 type][u16 status][u32 seq][u32 payload_len][payload]
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_MAGIC         0x5247494Fu /* "RGOI" */
#define FRAME_HEADER_SIZE   16
#define FRAME_MAX_PAYLOAD   512

/* longest GPIO name carried by the protocol (excluding '/') */
#define GPIO_NAME_MAX       32

/* message types */
enum msg_type {
    MSG_REQ_EXPORT          = 1,
    MSG_REQ_UNEXPORT        = 2,
    MSG_REQ_GET_VALUE       = 3,
    MSG_REQ_SET_VALUE       = 4,
    MSG_REQ_SET_DIRECTION   = 5,
    MSG_REQ_SET_EDGE        = 6,
    MSG_REQ_SET_ACTIVE_LOW  = 7,
    MSG_REQ_POLL            = 8,
    MSG_REQ_PING            = 9,
    MSG_REQ_EXPORT_NAME     = 10,
    MSG_REQ_LIST            = 11,
    MSG_EDGE_EVENT          = 16,
};

/* direction values for MSG_REQ_SET_DIRECTION */
enum pin_direction {
    DIR_IN       = 0,
    DIR_OUT      = 1,
    DIR_OUT_LOW  = 2,
    DIR_OUT_HIGH = 3,
};

/* edge values for MSG_REQ_SET_EDGE */
enum pin_edge {
    EDGE_NONE    = 0,
    EDGE_RISING  = 1,
    EDGE_FALLING = 2,
    EDGE_BOTH    = 3,
};

/* flags byte for MSG_REQ_POLL */
#define MSG_POLL_F_NEW_SESSION  0x01  /* new poll session: drop stale pending edge */

/* payload sizes per message type (0 = empty).
 * MSG_REQ_EXPORT_NAME is variable: [u8 name_len][name...] (name_len 1..32).
 * MSG_REQ_LIST is variable: request empty; response
 * [u16 count][count × {u32 pin, u8 name_len, name}]. */
static const size_t msg_payload_size[] = {
    [MSG_REQ_EXPORT]         = 4,   /* u32 pin */
    [MSG_REQ_UNEXPORT]       = 4,
    [MSG_REQ_GET_VALUE]      = 4,
    [MSG_REQ_SET_VALUE]      = 5,   /* u32 pin, u8 value */
    [MSG_REQ_SET_DIRECTION]  = 5,   /* u32 pin, u8 dir */
    [MSG_REQ_SET_EDGE]       = 5,   /* u32 pin, u8 edge */
    [MSG_REQ_SET_ACTIVE_LOW] = 5,   /* u32 pin, u8 val */
    [MSG_REQ_POLL]           = 5,   /* u32 pin, u8 flags */
    [MSG_REQ_PING]           = 0,
    [MSG_EDGE_EVENT]         = 4,
};

/* u8 response payloads */
#define RESP_PAYLOAD_GET_VALUE  1   /* u8 value */
#define RESP_PAYLOAD_POLL       1   /* u8 edge_detected */

struct frame_header {
    uint32_t magic;
    uint16_t type;
    uint16_t status;
    uint32_t seq;
    uint32_t payload_len;
};

/* serialize header into network byte order; buf must be >= FRAME_HEADER_SIZE */
void frame_header_pack(uint8_t *buf, const struct frame_header *h);

/* parse from network byte order; returns 0 on success, -1 on invalid magic/len */
int frame_header_unpack(const uint8_t *buf, struct frame_header *h);

#endif /* PROTOCOL_H */
