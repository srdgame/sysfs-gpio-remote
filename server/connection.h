/*
 * sysfs-gpio-remote server - client connection handling
 */
#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdint.h>
#include "gpio_backend.h"

/* Allocate per-pin subscription state. Call once after backend_init. */
int connection_setup(int num_pins);

/* Edge callback for backend_init (defined in connection.c). */
edge_callback_t connection_edge_callback(void);

/*
 * Atomically claim the single client slot. Returns 1 if the fd was
 * registered, 0 if another client is currently served.
 */
int connection_try_claim(int fd);

/*
 * Serve a claimed client connection (blocking; returns when the client
 * disconnects or a protocol error occurs).
 */
void connection_serve(int fd);

#endif /* CONNECTION_H */
