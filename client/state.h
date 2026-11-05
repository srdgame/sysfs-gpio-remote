/*
 * sysfs-gpio-remote client - pin state cache and reconnect recovery
 */
#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

struct pin_state {
    bool exported;
    uint8_t direction;
    uint8_t edge;
    uint8_t active_low;
    int last_value;      /* only meaningful when direction is output */
};

/* Init state for num_pins; hook called after each (re)connection. */
void state_init(int num_pins);

/* Track successful operations so reconnection can replay them. */
void state_on_export(int pin);
void state_on_unexport(int pin);
void state_on_direction(int pin, uint8_t dir);
void state_on_edge(int pin, uint8_t edge);
void state_on_active_low(int pin, uint8_t val);
void state_on_value(int pin, uint8_t value);
void state_on_connected(void);   /* triggers background recovery replay */

/*
 * Wait until no recovery replay is in progress (up to RECOVERY_WAIT_SEC).
 * Returns 0 if ready, -1 on timeout. Callers should fail with EIO on -1.
 */
int state_wait_ready(void);

/* Whether pin was exported through this client (local view). */
bool state_is_exported(int pin);

/* Cached attribute reads (client mirrors server state; single client). */
uint8_t state_get_direction(int pin);
uint8_t state_get_edge(int pin);
uint8_t state_get_active_low(int pin);

#endif /* STATE_H */
