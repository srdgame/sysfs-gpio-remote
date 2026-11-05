/*
 * sysfs-gpio-remote client
 *
 * FUSE filesystem exposing remote GPIOs over TCP.
 */
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "fuse_ops.h"
#include "rpc.h"
#include "state.h"
#include "log.h"

#define DEFAULT_PORT     4242
#define DEFAULT_NUM_PINS 54
#define DEFAULT_HOST     "127.0.0.1"

struct client_options {
    char *host;
    int port;
    int num_pins;
    char *mountpoint;
    char *extra_opts;   /* passed to fusermount, e.g. -o args */
    int verbose;
};

static struct client_options opts;

/* edge events forwarded from rpc receive thread */
static void rpc_edge_cb(int pin, void *user_data) {
    (void)user_data;
    fuse_ops_edge_event(pin);
}

static void rpc_conn_cb(int connected, void *user_data) {
    (void)user_data;
    if (connected)
        state_on_connected();
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] MOUNTPOINT\n"
        "\n"
        "sysfs-gpio-remote client: map remote GPIOs into a FUSE\n"
        "filesystem compatible with /sys/class/gpio.\n"
        "\n"
        "Options:\n"
        "  -s HOST     server address (default: %s)\n"
        "  -p PORT     server port (default: %d)\n"
        "  -n PINS     number of GPIO pins (default: %d)\n"
        "  -o OPTS     extra FUSE mount options (e.g. allow_other)\n"
        "  -v          increase verbosity (repeatable)\n"
        "  -h          show this help\n"
        "\n"
        "Example (shadow /sys/class/gpio, needs root):\n"
        "  %s -s 192.168.1.50 -o allow_other,default_permissions \\\n"
        "     /sys/class/gpio\n",
        prog, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_NUM_PINS, prog);
}

static int parse_args(int argc, char **argv) {
    opts.host = (char *)DEFAULT_HOST;
    opts.port = DEFAULT_PORT;
    opts.num_pins = DEFAULT_NUM_PINS;
    opts.mountpoint = NULL;
    opts.extra_opts = NULL;
    opts.verbose = 0;

    int opt;
    while ((opt = getopt(argc, argv, "s:p:n:o:vh")) != -1) {
        switch (opt) {
        case 's': opts.host = optarg; break;
        case 'p': {
            char *end = NULL;
            opts.port = (int)strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' ||
                opts.port <= 0 || opts.port > 65535) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return -1;
            }
            break;
        }
        case 'n': {
            char *end = NULL;
            opts.num_pins = (int)strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' ||
                opts.num_pins <= 0 || opts.num_pins > 1024) {
                fprintf(stderr, "invalid pin count: %s\n", optarg);
                return -1;
            }
            break;
        }
        case 'o': opts.extra_opts = optarg; break;
        case 'v': opts.verbose++; break;
        case 'h': usage(argv[0]); exit(0);
        default:
            usage(argv[0]);
            return -1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "missing mountpoint\n");
        usage(argv[0]);
        return -1;
    }
    opts.mountpoint = argv[optind++];
    if (optind < argc) {
        fprintf(stderr, "too many arguments\n");
        return -1;
    }
    return 0;
}

/* fuse_main consumes argv; rebuild it with our mountpoint and options. */
static int run_fuse(void) {
    char *argv[10];
    int argc = 0;
    argv[argc++] = "gpio-remote-client";
    argv[argc++] = "-f";            /* foreground */
    argv[argc++] = opts.mountpoint;
    /* unmount even if the process dies without a chance to clean up */
    argv[argc++] = "-o";
    argv[argc++] = "auto_unmount";
    if (opts.extra_opts) {
        argv[argc++] = "-o";
        argv[argc++] = opts.extra_opts;
    }

    struct fuse_operations oper;
    fuse_ops_setup(&oper, opts.num_pins);
    return fuse_main(argc, argv, &oper, NULL);
}

int main(int argc, char **argv) {
    enum log_level level = LOG_WARN;

    if (parse_args(argc, argv) != 0)
        return 1;
    if (opts.verbose == 1)
        level = LOG_INFO;
    else if (opts.verbose >= 2)
        level = LOG_DEBUG;
    log_set_level(level);

    signal(SIGPIPE, SIG_IGN);

    state_init(opts.num_pins);
    if (rpc_init(opts.host, opts.port, opts.num_pins,
                 rpc_edge_cb, NULL, rpc_conn_cb, NULL) != 0) {
        log_error("rpc init failed");
        return 1;
    }

    log_info("client ready: server=%s:%d mount=%s pins=%d",
             opts.host, opts.port, opts.mountpoint, opts.num_pins);

    int ret = run_fuse();

    rpc_shutdown();
    return ret;
}
