/*
 * poll_helper <path> [timeout_sec]
 *
 * Polls path for POLLPRI (edge event), then reads and prints the value.
 * Exit 0 on edge, 1 on timeout, 2 on error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path> [timeout_sec]\n", argv[0]);
        return 2;
    }
    int timeout_ms = argc > 2 ? atoi(argv[2]) * 1000 : 3000;

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    /* priming read: sysfs semantics require reading value before polling */
    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("read");
        close(fd);
        return 2;
    }
    printf("initial: %s", buf);

    struct pollfd pfd = { .fd = fd, .events = POLLPRI };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc == 0) {
        printf("timeout, no edge\n");
        close(fd);
        return 1;
    }
    if (rc < 0) {
        perror("poll");
        close(fd);
        return 2;
    }

    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("read after poll");
        close(fd);
        return 2;
    }
    printf("edge! value: %s", buf);
    close(fd);
    return 0;
}
