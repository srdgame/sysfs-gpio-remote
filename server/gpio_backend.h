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

/*
 * Export a pin by name (OpenWrt gpio-export style or a gpiod line name).
 * Server assigns a free slot and stores it in *slot_out.
 * Lookup order: gpiod line name -> full-featured pin; else
 * <sysfs_dir>/<name>/value passthrough pin (value read/write only);
 * else ENOENT. Idempotent: exporting an already-bound name returns
 * its existing slot.
 */
int pin_export_named(const char *name, int *slot_out);

/*
 * Mirror pre-existing named entries in /sys/class/gpio (env override:
 * GPIO_REMOTE_SYSFS_DIR, for tests) as value-only pins. Called on every
 * client connect so server-residue exports appear in the client view.
 */
void backend_mirror_sysfs_named(void);

int pin_unexport(int n);
bool pin_is_exported(int n);

/* Copy the display name ("gpio%d" or requested name) of an exported pin. */
int pin_get_display_name(int n, char *buf, size_t size);

int pin_set_direction(int n, uint8_t dir);
int pin_set_edge(int n, uint8_t edge);
int pin_set_active_low(int n, uint8_t val);
int pin_set_value(int n, uint8_t value);
int pin_get_value(int n, uint8_t *value_out);

#endif /* GPIO_BACKEND_H */
