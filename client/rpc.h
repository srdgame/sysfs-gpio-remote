/*
 * sysfs-gpio-remote client - RPC layer interface
 */
#ifndef RPC_H
#define RPC_H

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

/*
 * Called on the RPC receive thread when the server pushes an edge event.
 */
typedef void (*rpc_edge_cb_t)(int pin, void *user_data);

/*
 * Called whenever the connection state changes (connected=1/0).
 */
typedef void (*rpc_conn_cb_t)(int connected, void *user_data);

/*
 * Initialize the RPC layer and start receiver/reconnect threads.
 * Reconnect attempts run in the background until rpc_shutdown().
 */
int rpc_init(const char *host, int port, int num_pins,
             rpc_edge_cb_t edge_cb, void *edge_data,
             rpc_conn_cb_t conn_cb, void *conn_data);

/* Synchronous request-response. Returns errno-style code:
 *   0        success (status also valid, 0 = server-side OK)
 *   EIO      not connected / connection lost during call
 *   others   protocol/socket errors
 * On success, resp payload is copied into resp_buf (<= *resp_len),
 * *resp_len is updated to actual size.
 */
int rpc_call(uint16_t type, const void *req, size_t req_len,
             void *resp_buf, size_t *resp_len, uint16_t *status);

int rpc_is_connected(void);

void rpc_shutdown(void);

#endif /* RPC_H */
