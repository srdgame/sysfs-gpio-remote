/*
 * sysfs-gpio-remote client - pin state cache and reconnect recovery
 *
 * On reconnection, replays export + configuration for every pin that was
 * exported before the connection dropped. FUSE operations wait for the
 * replay to finish (bounded) so they do not observe half-recovered pins.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>

#include "state.h"
#include "rpc.h"
#include "log.h"

#define RECOVERY_WAIT_SEC 10

static struct pin_state *pins = NULL;
static int num_pins = 0;
static bool recovering = false;
static unsigned recovery_seq = 0;   /* bumped per recovery; guards overlaps */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;

void state_init(int npins) {
    num_pins = npins;
    pins = calloc(npins, sizeof(*pins));
    if (!pins) {
        log_error("state: cannot alloc pin state");
        exit(1);
    }
}

void state_on_export(int pin) {
    pthread_mutex_lock(&state_lock);
    pins[pin].exported = true;
    snprintf(pins[pin].name, sizeof(pins[pin].name), "gpio%d", pin);
    pins[pin].direction = DIR_IN;
    pins[pin].edge = EDGE_NONE;
    pins[pin].active_low = 0;
    pthread_mutex_unlock(&state_lock);
}

void state_on_export_named(int pin, const char *name) {
    pthread_mutex_lock(&state_lock);
    pins[pin].exported = true;
    snprintf(pins[pin].name, sizeof(pins[pin].name), "%s", name);
    pins[pin].direction = DIR_IN;
    pins[pin].edge = EDGE_NONE;
    pins[pin].active_low = 0;
    pthread_mutex_unlock(&state_lock);
}

void state_on_unexport(int pin) {
    pthread_mutex_lock(&state_lock);
    pins[pin].exported = false;
    pins[pin].name[0] = '\0';
    pthread_mutex_unlock(&state_lock);
}

void state_on_direction(int pin, uint8_t dir) {
    pthread_mutex_lock(&state_lock);
    pins[pin].direction = dir;
    pthread_mutex_unlock(&state_lock);
}

void state_on_edge(int pin, uint8_t edge) {
    pthread_mutex_lock(&state_lock);
    pins[pin].edge = edge;
    pthread_mutex_unlock(&state_lock);
}

void state_on_active_low(int pin, uint8_t val) {
    pthread_mutex_lock(&state_lock);
    pins[pin].active_low = val;
    pthread_mutex_unlock(&state_lock);
}

void state_on_value(int pin, uint8_t value) {
    pthread_mutex_lock(&state_lock);
    pins[pin].last_value = value;
    pthread_mutex_unlock(&state_lock);
}

/* Send one call, used only by the recovery path. arg < 0 means no arg. */
static int replay_call(uint16_t type, int pin, int arg) {
    uint8_t payload[5];
    uint32_t be = htonl((uint32_t)pin);
    memcpy(payload, &be, 4);
    size_t len = 4;
    if (arg >= 0) {
        payload[4] = (uint8_t)arg;
        len = 5;
    }
    uint8_t resp[FRAME_MAX_PAYLOAD];
    size_t resp_len = sizeof(resp);
    uint16_t status = 0;
    int rc = rpc_call(type, payload, len, resp, &resp_len, &status);
    if (rc != 0)
        return rc;
    return status;
}

/* Whether the snapshot name is the default numeric name for this slot
 * (i.e. a numeric export, replayed via MSG_REQ_EXPORT). */
static bool pin_name_is_numeric(int pin, const char *name) {
    char expect[33];
    snprintf(expect, sizeof(expect), "gpio%d", pin);
    return strcmp(name, expect) == 0;
}

/* Whether this recovery generation is still the latest one. Older
 * generations must not mutate pin state (a newer recovery owns it). */
static bool recovery_is_current(unsigned gen) {
    pthread_mutex_lock(&state_lock);
    bool cur = gen == recovery_seq;
    pthread_mutex_unlock(&state_lock);
    return cur;
}

/* Clear the local export flag for a failed replay, but only when the live
 * entry at the snapshot slot is still this snapshot entry (a concurrent
 * named remap may have moved a different pin into this slot). */
static void replay_failure_clear(int slot, const char *name) {
    pthread_mutex_lock(&state_lock);
    if (pins[slot].exported && strcmp(pins[slot].name, name) == 0)
        pins[slot].exported = false;
    pthread_mutex_unlock(&state_lock);
}

static void replay_pin(int pin, struct pin_state st, unsigned gen) {
    int rc;
    if (pin_name_is_numeric(pin, st.name)) {
        rc = replay_call(MSG_REQ_EXPORT, pin, -1);
        if (rc != 0) {
            log_warn("state: recovery export GPIO%d failed: %s", pin,
                     strerror(rc));
            if (recovery_is_current(gen))
                replay_failure_clear(pin, st.name);
            return;
        }
    } else {
        /* named export: server assigns the slot; adopt it if it moved */
        int slot = -1;
        rc = rpc_export_name(st.name, &slot);
        if (rc != 0) {
            log_warn("state: recovery export %s failed: %s", st.name,
                     strerror(rc));
            if (recovery_is_current(gen))
                replay_failure_clear(pin, st.name);
            return;
        }
        if (slot != pin) {
            pthread_mutex_lock(&state_lock);
            pins[slot] = pins[pin];
            memset(&pins[pin], 0, sizeof(pins[pin]));
            pthread_mutex_unlock(&state_lock);
            pin = slot;
        }
    }
    if (st.direction != DIR_IN) {
        rc = replay_call(MSG_REQ_SET_DIRECTION, pin, st.direction);
        if (rc != 0)
            log_warn("state: recovery direction GPIO%d failed: %s", pin,
                     strerror(rc));
    }
    if (st.edge != EDGE_NONE) {
        rc = replay_call(MSG_REQ_SET_EDGE, pin, st.edge);
        if (rc != 0)
            log_warn("state: recovery edge GPIO%d failed: %s", pin,
                     strerror(rc));
    }
    if (st.active_low) {
        rc = replay_call(MSG_REQ_SET_ACTIVE_LOW, pin, 1);
        if (rc != 0)
            log_warn("state: recovery active_low GPIO%d failed: %s", pin,
                     strerror(rc));
    }
    if (st.direction == DIR_OUT || st.direction == DIR_OUT_LOW ||
        st.direction == DIR_OUT_HIGH) {
        rc = replay_call(MSG_REQ_SET_VALUE, pin, (int)st.last_value);
        if (rc != 0)
            log_warn("state: recovery value GPIO%d failed: %s", pin,
                     strerror(rc));
    }
}

/* Ingest the server's exported-pin listing: slots the server still has
 * but the client does not (server residue / client restart) are added to
 * the local view. Existing local exports win and are replayed afterwards. */
static int list_ingest_cb(uint32_t pin, const char *name, void *ctx) {
    (void)ctx;
    if (pin >= (uint32_t)num_pins)
        return 0;
    pthread_mutex_lock(&state_lock);
    if (!pins[pin].exported) {
        pins[pin].exported = true;
        snprintf(pins[pin].name, sizeof(pins[pin].name), "%s", name);
        pins[pin].direction = DIR_IN;
        pins[pin].edge = EDGE_NONE;
        pins[pin].active_low = 0;
        log_info("state: adopted server-residue pin %u (%s)", pin, name);
    }
    pthread_mutex_unlock(&state_lock);
    return 0;
}

static void *recovery_thread(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    pthread_mutex_lock(&state_lock);
    struct pin_state *snapshot = malloc(num_pins * sizeof(*snapshot));
    if (!snapshot) {
        pthread_mutex_unlock(&state_lock);
        return NULL;
    }
    for (int i = 0; i < num_pins; i++)
        snapshot[i] = pins[i];
    pthread_mutex_unlock(&state_lock);

    /* A newer recovery (rapid reconnect) supersedes this one: stop
     * touching pin state and leave recovering for the newer thread. */
    if (recovery_is_current(gen)) {
        /* 1) adopt server-residue exports first so replayed exports do
         *    not collide with slots the server still holds */
        int lrc = rpc_list(list_ingest_cb, NULL);
        if (lrc != 0)
            log_warn("state: LIST of server exports failed: %s",
                     strerror(lrc));
    }

    /* 2) replay exports/config for everything the client had exported */
    for (int i = 0; i < num_pins; i++) {
        if (!snapshot[i].exported)
            continue;
        if (!recovery_is_current(gen))
            break;
        replay_pin(i, snapshot[i], gen);
    }
    free(snapshot);

    pthread_mutex_lock(&state_lock);
    if (gen == recovery_seq)
        recovering = false;
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_lock);
    log_info("state: recovery replay finished");
    return NULL;
}

void state_on_connected(void) {
    pthread_mutex_lock(&state_lock);
    unsigned gen = ++recovery_seq;
    recovering = true;
    log_info("state: starting recovery replay");
    pthread_t t;
    if (pthread_create(&t, NULL, recovery_thread,
                       (void *)(uintptr_t)gen) == 0)
        pthread_detach(t);
    else {
        recovering = false;
        log_error("state: cannot start recovery thread");
    }
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_lock);
}

int state_wait_ready(void) {
    pthread_mutex_lock(&state_lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += RECOVERY_WAIT_SEC;
    int wrc = 0;
    while (recovering && wrc == 0)
        wrc = pthread_cond_timedwait(&state_cond, &state_lock, &ts);
    int ret = recovering ? -1 : 0;
    pthread_mutex_unlock(&state_lock);
    return ret;
}

bool state_is_exported(int pin) {
    pthread_mutex_lock(&state_lock);
    bool ret = pin >= 0 && pin < num_pins && pins[pin].exported;
    pthread_mutex_unlock(&state_lock);
    return ret;
}

int state_find_by_name(const char *name) {
    pthread_mutex_lock(&state_lock);
    int found = -1;
    for (int i = 0; i < num_pins; i++) {
        if (pins[i].exported && strcmp(pins[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    pthread_mutex_unlock(&state_lock);
    return found;
}

void state_name_of(int pin, char *buf, size_t size) {
    pthread_mutex_lock(&state_lock);
    const char *name = (pin >= 0 && pin < num_pins && pins[pin].exported &&
                        pins[pin].name[0])
                           ? pins[pin].name
                           : "";
    snprintf(buf, size, "%s", name);
    pthread_mutex_unlock(&state_lock);
}

uint8_t state_get_direction(int pin) {
    pthread_mutex_lock(&state_lock);
    uint8_t ret = pins[pin].direction;
    pthread_mutex_unlock(&state_lock);
    return ret;
}

uint8_t state_get_edge(int pin) {
    pthread_mutex_lock(&state_lock);
    uint8_t ret = pins[pin].edge;
    pthread_mutex_unlock(&state_lock);
    return ret;
}

uint8_t state_get_active_low(int pin) {
    pthread_mutex_lock(&state_lock);
    uint8_t ret = pins[pin].active_low;
    pthread_mutex_unlock(&state_lock);
    return ret;
}
