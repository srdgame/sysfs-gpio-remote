/*
 * sysfs-gpio-remote server - edge detection thread interface
 */
#ifndef EDGE_THREAD_H
#define EDGE_THREAD_H

#include <gpiod.h>
#include "gpio_backend.h"

/* Start the epoll-based edge detection thread. Returns 0 on success. */
int edge_thread_start(edge_callback_t cb, void *user_data);

/* (De)register a gpiod line request fd with the epoll loop. */
int edge_thread_add_pin(struct gpiod_line_request *req, int pin);
void edge_thread_remove_pin(struct gpiod_line_request *req);

#endif /* EDGE_THREAD_H */
