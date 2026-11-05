/*
 * sysfs-gpio-remote client - FUSE operations interface
 */
#ifndef FUSE_OPS_H
#define FUSE_OPS_H

#include <fuse.h>
#include "protocol.h"

/* Populate a fuse_operations struct with the remote GPIO handlers. */
void fuse_ops_setup(struct fuse_operations *oper, int num_pins);

/* Called from rpc edge callback: notify poll waiters on pin n. */
void fuse_ops_edge_event(int n);

#endif /* FUSE_OPS_H */
