// LD_PRELOAD for tests/charge_rest: a virtual CLOCK_MONOTONIC, so the
// Python and the C++ draw their pulse frames at the same times and must
// write the same values.
//
// It moves only through waits: a sleep or a timed poll/select moves it by
// exactly what was asked (while really waiting that times $CREST_VSCALE).
// A wait ended early by an event on a FIFO, a socket or inotify (a key, a
// uevent) also moves it by the whole timeout, so frames stay on their grid
// and only which frame an event lands on can differ; one ended by a pipe
// or a pidfd (a child's output or end, SIGUSR1's self-pipe) or a signal
// moves it by the real time passed (scaled), as a busy-waiting Python loop
// would see.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long virt = 1000LL * 1000000000LL;  // 1000 s
static double scale = -1;

static double sc(void) {
    if (scale < 0) {
        const char* s = getenv("CREST_VSCALE");
        scale = s ? atof(s) : 0.1;
    }
    return scale;
}
static long long real_ns(void) {
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static long long ns(const struct timespec* ts) { return (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec; }
static struct timespec tsof(long long v) {
    struct timespec t = {v / 1000000000LL, v % 1000000000LL};
    return t;
}
static long long scaled(long long v) { return (long long)(v * sc()); }
static long long unscale(long long real, long long cap) {
    long long v = (long long)(real / sc());
    return v < cap ? v : cap;
}

// A pipe or a pidfd: what was waited for is real work, not an event.
static int real_fd(int fd) {
    char link[64], path[256];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    return !strncmp(path, "pipe:", 5) || !strcmp(path, "anon_inode:[pidfd]");
}

int clock_gettime(clockid_t id, struct timespec* ts) {
    if (id != CLOCK_MONOTONIC) return (int)syscall(SYS_clock_gettime, id, ts);
    *ts = tsof(virt);
    return 0;
}

static int vsleep(long long dt, long long* done) {
    *done = 0;
    if (dt <= 0) return 0;
    struct timespec r = tsof(scaled(dt)), rem;
    long long t0 = real_ns();
    if (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &r, &rem) != 0) {
        *done = unscale(real_ns() - t0, dt);
        virt += *done;
        return EINTR;
    }
    virt += dt;
    *done = dt;
    return 0;
}

int clock_nanosleep(clockid_t id, int flags, const struct timespec* req, struct timespec* rem) {
    (void)id;
    long long dt = (flags & TIMER_ABSTIME) ? ns(req) - virt : ns(req), done;
    int r = vsleep(dt, &done);
    if (r && rem && !(flags & TIMER_ABSTIME)) *rem = tsof(dt - done);
    return r;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    long long done;
    int r = vsleep(ns(req), &done);
    if (r) {
        if (rem) *rem = tsof(ns(req) - done);
        errno = r;
        return -1;
    }
    return 0;
}

static void after_wait(long r, long long T, long long real, int any_real_fd) {
    if (r == 0) virt += T;
    else if (r > 0 && !any_real_fd) virt += T;
    else virt += unscale(real, T);
}

int ppoll(struct pollfd* fds, nfds_t n, const struct timespec* tmo, const sigset_t* mask) {
    if (!tmo) return (int)syscall(SYS_ppoll, fds, n, NULL, mask, 8);
    long long T = ns(tmo);
    struct timespec r = tsof(scaled(T));
    long long t0 = real_ns();
    long ret = syscall(SYS_ppoll, fds, n, &r, mask, 8);
    int err = errno;
    int rf = 0;
    if (ret > 0)
        for (nfds_t i = 0; i < n; ++i)
            if (fds[i].revents && real_fd(fds[i].fd)) rf = 1;
    after_wait(ret, T, real_ns() - t0, rf);
    errno = err;
    return (int)ret;
}

int poll(struct pollfd* fds, nfds_t n, int ms) {
    if (ms < 0) return ppoll(fds, n, NULL, NULL);
    struct timespec t = tsof((long long)ms * 1000000LL);
    return ppoll(fds, n, &t, NULL);
}

static int any_real_set(int n, fd_set* a, fd_set* b, fd_set* c) {
    for (int fd = 0; fd < n; ++fd)
        if (((a && FD_ISSET(fd, a)) || (b && FD_ISSET(fd, b)) || (c && FD_ISSET(fd, c))) && real_fd(fd)) return 1;
    return 0;
}

int pselect(int n, fd_set* rd, fd_set* wr, fd_set* ex, const struct timespec* tmo, const sigset_t* mask) {
    struct {
        const sigset_t* ss;
        size_t len;
    } data = {mask, 8};
    if (!tmo) return (int)syscall(SYS_pselect6, n, rd, wr, ex, NULL, mask ? &data : NULL);
    long long T = ns(tmo);
    struct timespec r = tsof(scaled(T));
    long long t0 = real_ns();
    long ret = syscall(SYS_pselect6, n, rd, wr, ex, &r, mask ? &data : NULL);
    int err = errno;
    after_wait(ret, T, real_ns() - t0, ret > 0 ? any_real_set(n, rd, wr, ex) : 0);
    errno = err;
    return (int)ret;
}

int select(int n, fd_set* rd, fd_set* wr, fd_set* ex, struct timeval* tv) {
    if (!tv) return pselect(n, rd, wr, ex, NULL, NULL);
    struct timespec t = {tv->tv_sec, tv->tv_usec * 1000};
    return pselect(n, rd, wr, ex, &t, NULL);
}
