/*
 * sysfs-gpio-remote client - pin state cache and reconnect recovery
 *
 * On reconnection, replays export + configuration for every pin that was
 * exported before the connection dropped. FUSE operations wait for the
 * replay to finish (bounded) so they do not observe half-recovered pins.
 */
#include <errno.h>
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
    pins[pin].direction = DIR_IN;
    pins[pin].edge = EDGE_NONE;
    pins[pin].active_low = 0;
    pthread_mutex_unlock(&state_lock);
}

void state_on_unexport(int pin) {
    pthread_mutex_lock(&state_lock);
    pins[pin].exported = false;
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

static void replay_pin(int pin, struct pin_state st) {
    int rc = replay_call(MSG_REQ_EXPORT, pin, -1);
    if (rc != 0) {
        log_warn("state: recovery export GPIO%d failed: %s", pin,
                 strerror(rc));
        pthread_mutex_lock(&state_lock);
        pins[pin].exported = false;
        pthread_mutex_unlock(&state_lock);
        return;
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

static void *recovery_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&state_lock);
    struct pin_state *snapshot = malloc(num_pins * sizeof(*snapshot));
    if (!snapshot) {
        pthread_mutex_unlock(&state_lock);
        return NULL;
    }
    for (int i = 0; i < num_pins; i++)
        snapshot[i] = pins[i];
    pthread_mutex_unlock(&state_lock);

    for (int i = 0; i < num_pins; i++) {
        if (!snapshot[i].exported)
            continue;
        replay_pin(i, snapshot[i]);
    }
    free(snapshot);

    pthread_mutex_lock(&state_lock);
    recovering = false;
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_lock);
    log_info("state: recovery replay finished");
    return NULL;
}

void state_on_connected(void) {
    pthread_mutex_lock(&state_lock);
    bool any = false;
    for (int i = 0; i < num_pins; i++) {
        if (pins[i].exported) {
            any = true;
            break;
        }
    }
    if (any) {
        recovering = true;
        log_info("state: starting recovery replay");
        pthread_t t;
        if (pthread_create(&t, NULL, recovery_thread, NULL) == 0)
            pthread_detach(t);
        else {
            recovering = false;
            log_error("state: cannot start recovery thread");
        }
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
