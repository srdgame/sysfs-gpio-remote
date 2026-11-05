/*
 * sysfs-gpio-remote server
 *
 * Listens on TCP, serves a single client, exposes GPIOs via libgpiod.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "connection.h"
#include "proto_io.h"
#include "log.h"

#define DEFAULT_PORT     4242
#define DEFAULT_NUM_PINS 54

static volatile sig_atomic_t got_signal = 0;

static void on_signal(int sig) {
    (void)sig;
    got_signal = 1;
}

/* unblocks a live connection stuck in recv/send when we are shutting down */
static int shutdown_check(void) {
    return got_signal != 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "sysfs-gpio-remote server: expose GPIOs over TCP via libgpiod.\n"
        "\n"
        "Options:\n"
        "  -l ADDR     listen address (default: 0.0.0.0)\n"
        "  -p PORT     listen port (default: %d)\n"
        "  -n PINS     number of GPIO pins (default: %d)\n"
        "  -v          increase verbosity (repeatable)\n"
        "  -h          show this help\n",
        prog, DEFAULT_PORT, DEFAULT_NUM_PINS);
}

int main(int argc, char **argv) {
    const char *listen_addr = "0.0.0.0";
    int port = DEFAULT_PORT;
    int num_pins = DEFAULT_NUM_PINS;
    enum log_level level = LOG_WARN;

    int opt;
    while ((opt = getopt(argc, argv, "l:p:n:vh")) != -1) {
        switch (opt) {
        case 'l': listen_addr = optarg; break;
        case 'p': {
            char *end = NULL;
            port = (int)strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || port <= 0 || port > 65535) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'n': {
            char *end = NULL;
            num_pins = (int)strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || num_pins <= 0 ||
                num_pins > 1024) {
                fprintf(stderr, "invalid pin count: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'v':
            if (level < LOG_DEBUG)
                level++;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    log_set_level(level);

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* no SA_RESTART: accept() must return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    proto_io_set_interrupt_check(shutdown_check);

    if (backend_init(num_pins, connection_edge_callback(), NULL) != 0) {
        log_error("backend init failed");
        return 1;
    }
    if (connection_setup(num_pins) != 0) {
        log_error("cannot allocate connection state");
        return 1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        log_error("socket: %s", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, listen_addr, &addr.sin_addr) != 1) {
        log_error("invalid listen address: %s", listen_addr);
        return 1;
    }
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        log_error("bind %s:%d: %s", listen_addr, port, strerror(errno));
        return 1;
    }
    if (listen(listen_fd, 1) == -1) {
        log_error("listen: %s", strerror(errno));
        return 1;
    }
    log_info("server listening on %s:%d (%d pins)", listen_addr, port,
             num_pins);

    while (!got_signal) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client == -1) {
            if (errno == EINTR)
                continue;
            log_error("accept: %s", strerror(errno));
            break;
        }

        char peer_addr[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, peer_addr, sizeof(peer_addr));

        /* single-client policy: reject while another client is served.
         * connection_serve clears the registration on return, so client_fd
         * is only >= 0 while a connection is active. */
        if (connection_try_claim(client)) {
            log_info("accepted client %s:%d", peer_addr,
                     (int)ntohs(peer.sin_port));
            connection_serve(client);
            close(client);
            log_info("client %s:%d closed", peer_addr,
                     (int)ntohs(peer.sin_port));
        } else {
            log_warn("rejected client %s:%d: already serving",
                     peer_addr, (int)ntohs(peer.sin_port));
            send_frame(client, MSG_REQ_PING, EBUSY, 0, NULL, 0);
            close(client);
        }
    }

    close(listen_fd);
    log_info("server exiting");
    return 0;
}
