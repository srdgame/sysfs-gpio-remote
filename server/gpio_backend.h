/*
 * sysfs-gpio-remote server - libgpiod backend interface
 *
 * Ported from sysfs-gpio-shim (libgpiod v2 API).
 */
#ifndef GPIO_BACKEND_H
#define GPIO_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

/*
 * Called by the edge detection thread for each detected edge on pin n.
 * Only fires for pins with edge detection enabled.
 */
typedef void (*edge_callback_t)(int n, void *user_data);

/* Scan gpiochips, init per-pin locks, start edge detection thread.
 * Returns 0 on success, -1 on failure. */
int backend_init(int num_pins, edge_callback_t edge_cb, void *user_data);

int pin_export(int n);
int pin_unexport(int n);
bool pin_is_exported(int n);

int pin_set_direction(int n, uint8_t dir);
int pin_set_edge(int n, uint8_t edge);
int pin_set_active_low(int n, uint8_t val);
int pin_set_value(int n, uint8_t value);
int pin_get_value(int n, uint8_t *value_out);

#endif /* GPIO_BACKEND_H */
