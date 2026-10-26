#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int mode_is(const char *s) {
    const char *m = getenv("H1_FAULT_MODE");
    return m && !strcmp(m, s);
}

static int ack_failure(int fd) {
    struct stat st;
    const char *p = getenv("H1_FAULT_OUTPUT");
    return mode_is("ack") && p && !access(p, F_OK) && !fstat(fd, &st) && S_ISSOCK(st.st_mode);
}

int fsync(int fd) {
    static int (*real_fn)(int);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "fsync");
    if (mode_is("fsync")) {
        fprintf(stderr, "H1_FAULT fsync EIO\n");
        errno = EIO;
        return -1;
    }
    return real_fn(fd);
}

int close(int fd) {
    static int (*real_fn)(int);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "close");
    char p[64], b[4096];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, b, sizeof(b) - 1);
    if (n >= 0) b[n] = 0; else b[0] = 0;
    int inject = mode_is("close") && strstr(b, ".lssd_recv_");
    int rc = real_fn(fd);
    if (inject) {
        fprintf(stderr, "H1_FAULT close EIO\n");
        errno = EIO;
        return -1;
    }
    return rc;
}

int rename(const char *a, const char *b) {
    static int (*real_fn)(const char *, const char *);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "rename");
    if (mode_is("rename")) {
        fprintf(stderr, "H1_FAULT rename EIO\n");
        errno = EIO;
        return -1;
    }
    return real_fn(a, b);
}

ssize_t write(int fd, const void *b, size_t n) {
    static ssize_t (*real_fn)(int, const void *, size_t);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "write");
    if (ack_failure(fd)) {
        fprintf(stderr, "H1_FAULT ACK socket write EPIPE\n");
        errno = EPIPE;
        return -1;
    }
    return real_fn(fd, b, n);
}

ssize_t send(int fd, const void *b, size_t n, int flags) {
    static ssize_t (*real_fn)(int, const void *, size_t, int);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "send");
    if (ack_failure(fd)) {
        fprintf(stderr, "H1_FAULT ACK socket send EPIPE\n");
        errno = EPIPE;
        return -1;
    }
    ssize_t rc = real_fn(fd, b, n, flags);
    if (mode_is("backpressure") && rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        fprintf(stderr, "H1_FAULT backpressure EAGAIN observed\n");
    }
    return rc;
}

int socket(int domain, int type, int protocol) {
    static int (*real_fn)(int, int, int);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "socket");
    int fd = real_fn(domain, type, protocol);
    if (mode_is("backpressure") && fd >= 0) {
        int value = 4096;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));
    }
    return fd;
}
