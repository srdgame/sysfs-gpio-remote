/*
 * sysfs-gpio-remote server - libgpiod backend
 *
 * Ported from sysfs-gpio-shim by Florian Wesch (MIT licensed).
 */
#define _GNU_SOURCE
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <sys/epoll.h>

#include "gpio_backend.h"
#include "edge_thread.h"
#include "log.h"

struct pin_s {
    int n;
    bool in_use;
    enum gpiod_line_direction direction;
    enum gpiod_line_edge edge;
    bool active_low;
    pthread_mutex_t lock;
    struct gpiod_chip *gpio_chip;
    struct gpiod_line_request *gpio_line_request;
    unsigned int gpio_line_offset;
};

static struct pin_s *pins = NULL;
static int num_pins = 0;

/////////////////////////////////////////////////////////////////////

static struct gpiod_chip **chips = NULL;
static int num_chips = 0;

static int chip_dir_filter(const struct dirent *entry) {
    char path[NAME_MAX + 6];
    snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
    struct stat sb;
    return lstat(path, &sb) == 0 &&
        !S_ISLNK(sb.st_mode) &&
        gpiod_is_gpiochip_device(path);
}

static int get_chips(struct gpiod_chip ***chips_ptr) {
    int n, i;
    struct dirent **entries;
    struct gpiod_chip **found;
    n = scandir("/dev/", &entries, chip_dir_filter, versionsort);
    if (n <= 0)
        return 0;
    found = calloc(n, sizeof(*found));
    if (!found)
        return 0;
    for (i = 0; i < n; i++) {
        char path[NAME_MAX + 6];
        snprintf(path, sizeof(path), "/dev/%s", entries[i]->d_name);
        found[i] = gpiod_chip_open(path);
    }
    *chips_ptr = found;
    for (i = 0; i < n; i++)
        free(entries[i]);
    free(entries);
    return n;
}

static bool find_gpio_by_name(
    const char *name, struct gpiod_chip **found_chip, unsigned int *found_offset
) {
    for (int i = 0; i < num_chips; i++) {
        struct gpiod_chip *chip = chips[i];
        int offset = gpiod_chip_get_line_offset_from_name(chip, name);
        if (offset == -1)
            continue;
        struct gpiod_chip_info *info = gpiod_chip_get_info(chip);
        log_debug("backend: %s -> %s @ chip %s offset %d", name,
                  gpiod_chip_info_get_name(info),
                  gpiod_chip_get_path(chip), offset);
        gpiod_chip_info_free(info);
        *found_chip = chip;
        *found_offset = offset;
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////

static bool pin_request_new(struct pin_s *pin,
                            struct gpiod_line_config *line_cfg) {
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
        log_error("backend: cannot alloc request config");
        return false;
    }
    gpiod_request_config_set_consumer(req_cfg, "sysfs-gpio-remote");
    pin->gpio_line_request = gpiod_chip_request_lines(
        pin->gpio_chip, req_cfg, line_cfg);
    gpiod_request_config_free(req_cfg);
    if (!pin->gpio_line_request) {
        log_error("backend: request GPIO%d failed: %s",
                  pin->n, strerror(errno));
        return false;
    }
    return true;
}

static bool pin_reconfigure_line(struct pin_s *pin) {
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings) {
        log_error("backend: cannot alloc settings");
        return false;
    }

    gpiod_line_settings_set_direction(settings, pin->direction);
    gpiod_line_settings_set_active_low(settings, pin->active_low);
    /* Only apply event settings when edge detection is enabled: a debounced
     * request returns the debounced value on reads, which lags the real
     * line by the debounce period - sysfs value reads must be raw. */
    if (pin->direction == GPIOD_LINE_DIRECTION_INPUT &&
        pin->edge != GPIOD_LINE_EDGE_NONE) {
        gpiod_line_settings_set_debounce_period_us(settings, 300);
        gpiod_line_settings_set_event_clock(settings, GPIOD_LINE_CLOCK_MONOTONIC);
        gpiod_line_settings_set_edge_detection(settings, pin->edge);
    }

    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        log_error("backend: cannot alloc line config");
        gpiod_line_settings_free(settings);
        return false;
    }

    bool ok = true;
    if (gpiod_line_config_add_line_settings(
            line_cfg, &pin->gpio_line_offset, 1, settings))
        ok = false;

    if (ok && !pin->gpio_line_request) {
        ok = pin_request_new(pin, line_cfg);
    } else if (ok) {
        if (gpiod_line_request_reconfigure_lines(
                pin->gpio_line_request, line_cfg) == 0)
            goto out;
        /* Some kernels reject reconfigure when input event attributes
         * (debounce/event clock/edge) change. Fall back to a full
         * release + re-request, which always works. */
        log_debug("backend: reconfigure GPIO%d failed (%s), re-requesting",
                  pin->n, strerror(errno));
        gpiod_line_request_release(pin->gpio_line_request);
        pin->gpio_line_request = NULL;
        ok = pin_request_new(pin, line_cfg);
    }

out:
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    return ok;
}

static int check_pin(int n, bool require_use) {
    if (n < 0 || n >= num_pins)
        return EINVAL;
    if (require_use && !pins[n].in_use)
        return EINVAL;
    return 0;
}

int pin_export(int n) {
    if (n < 0 || n >= num_pins)
        return EINVAL;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    int ret = 0;
    if (pin->in_use)
        goto out;

    char name[20];
    snprintf(name, sizeof(name), "GPIO%d", n);
    if (!find_gpio_by_name(name, &pin->gpio_chip, &pin->gpio_line_offset)) {
        ret = EBUSY;
        goto out;
    }

    pin->direction = GPIOD_LINE_DIRECTION_INPUT;
    pin->edge = GPIOD_LINE_EDGE_NONE;
    pin->active_low = false;
    pin->gpio_line_request = NULL;
    if (!pin_reconfigure_line(pin)) {
        ret = EBUSY;
        goto out;
    }
    pin->in_use = true;
    log_info("backend: exported GPIO%d", n);
out:
    pthread_mutex_unlock(&pin->lock);
    return ret;
}

int pin_unexport(int n) {
    int chk = check_pin(n, true);
    if (chk != 0)
        return chk;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    if (pin->edge != GPIOD_LINE_EDGE_NONE)
        edge_thread_remove_pin(pin->gpio_line_request);
    gpiod_line_request_release(pin->gpio_line_request);
    pin->gpio_line_request = NULL;
    pin->in_use = false;
    pthread_mutex_unlock(&pin->lock);
    log_info("backend: unexported GPIO%d", n);
    return 0;
}

bool pin_is_exported(int n) {
    if (n < 0 || n >= num_pins)
        return false;
    return pins[n].in_use;
}

int pin_set_direction(int n, uint8_t dir) {
    int chk = check_pin(n, true);
    if (chk != 0)
        return chk;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    enum gpiod_line_direction d = GPIOD_LINE_DIRECTION_INPUT;
    int initial = -1;
    switch (dir) {
    case DIR_IN:       d = GPIOD_LINE_DIRECTION_INPUT;  break;
    case DIR_OUT:      d = GPIOD_LINE_DIRECTION_OUTPUT; initial = 0; break;
    case DIR_OUT_LOW:  d = GPIOD_LINE_DIRECTION_OUTPUT; initial = 0; break;
    case DIR_OUT_HIGH: d = GPIOD_LINE_DIRECTION_OUTPUT; initial = 1; break;
    default:
        pthread_mutex_unlock(&pin->lock);
        return EINVAL;
    }
    int ret = 0;
    if (pin->direction != d || initial >= 0) {
        pin->direction = d;
        if (!pin_reconfigure_line(pin))
            ret = EIO;
    }
    if (ret == 0 && initial >= 0)
        gpiod_line_request_set_value(pin->gpio_line_request,
                                     pin->gpio_line_offset, initial);
    pthread_mutex_unlock(&pin->lock);
    return ret;
}

int pin_set_edge(int n, uint8_t edge) {
    int chk = check_pin(n, true);
    if (chk != 0)
        return chk;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    enum gpiod_line_edge e;
    switch (edge) {
    case EDGE_NONE:    e = GPIOD_LINE_EDGE_NONE;    break;
    case EDGE_RISING:  e = GPIOD_LINE_EDGE_RISING;  break;
    case EDGE_FALLING: e = GPIOD_LINE_EDGE_FALLING; break;
    case EDGE_BOTH:    e = GPIOD_LINE_EDGE_BOTH;    break;
    default:
        pthread_mutex_unlock(&pin->lock);
        return EINVAL;
    }
    int ret = 0;
    if (pin->edge != e) {
        if (pin->edge != GPIOD_LINE_EDGE_NONE)
            edge_thread_remove_pin(pin->gpio_line_request);
        pin->edge = e;
        if (!pin_reconfigure_line(pin))
            ret = EIO;
        else if (pin->edge != GPIOD_LINE_EDGE_NONE)
            edge_thread_add_pin(pin->gpio_line_request, pin->n);
    }
    pthread_mutex_unlock(&pin->lock);
    return ret;
}

int pin_set_active_low(int n, uint8_t val) {
    int chk = check_pin(n, true);
    if (chk != 0)
        return chk;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    bool al = (val != 0);
    int ret = 0;
    if (pin->active_low != al) {
        pin->active_low = al;
        if (!pin_reconfigure_line(pin))
            ret = EIO;
    }
    pthread_mutex_unlock(&pin->lock);
    return ret;
}

int pin_set_value(int n, uint8_t value) {
    int chk = check_pin(n, true);
    if (chk != 0)
        return chk;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    int ret = 0;
    if (pin->direction != GPIOD_LINE_DIRECTION_OUTPUT) {
        ret = EPERM;
    } else {
        gpiod_line_request_set_value(pin->gpio_line_request,
                                     pin->gpio_line_offset, value ? 1 : 0);
    }
    pthread_mutex_unlock(&pin->lock);
    return ret;
}

int pin_get_value(int n, uint8_t *value_out) {
    int chk = check_pin(n, true);
    if (chk != 0)
        return chk;
    struct pin_s *pin = &pins[n];
    pthread_mutex_lock(&pin->lock);
    int value = gpiod_line_request_get_value(pin->gpio_line_request,
                                             pin->gpio_line_offset);
    pthread_mutex_unlock(&pin->lock);
    *value_out = (uint8_t)value;
    return 0;
}

/////////////////////////////////////////////////////////////////////

int backend_init(int npins, edge_callback_t edge_cb, void *user_data) {
    num_pins = npins;
    pins = calloc(num_pins, sizeof(*pins));
    if (!pins) {
        log_error("backend: cannot alloc pins");
        return -1;
    }
    for (int p = 0; p < num_pins; p++) {
        pins[p].n = p;
        pthread_mutex_init(&pins[p].lock, NULL);
    }
    num_chips = get_chips(&chips);
    if (num_chips == 0) {
        log_warn("backend: no gpiochip devices found in /dev");
    } else {
        log_info("backend: found %d gpiochip(s)", num_chips);
    }
    if (edge_thread_start(edge_cb, user_data) != 0)
        return -1;
    return 0;
}
