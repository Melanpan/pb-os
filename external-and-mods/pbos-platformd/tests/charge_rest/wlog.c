// LD_PRELOAD for tests/charge_rest: what the program does to the (fake)
// system, in order, as "<real monotonic s>\t<what>" lines in $CREST_LOG:
//   W path = value            write() to a file under /sys, /run or /var/lib/pbos-utils
//   I2C path aa bb            write() to /dev/i2c-N (bytes in hex)
//   IOCTL path SLAVE_FORCE 0xNN   ioctl I2C_SLAVE_FORCE on /dev/i2c-N (faked: 0)
//   KILL pid [cmdline] signal
//   UNLINK path / RENAME a -> b  under those folders
// The stubs append their argv to the same file.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void emit(const char* what) {
    const char* log = getenv("CREST_LOG");
    if (!log) return;
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    char line[9000];
    int m = snprintf(line, sizeof line, "%ld.%06ld\t%s\n", (long)ts.tv_sec, ts.tv_nsec / 1000, what);
    int lfd = (int)syscall(SYS_openat, AT_FDCWD, log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (lfd >= 0) {
        syscall(SYS_write, lfd, line, (size_t)m);
        syscall(SYS_close, lfd);
    }
}

static int fd_path(int fd, char* out, size_t n) {
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t len = readlink(link, out, n - 1);
    if (len <= 0) return 0;
    out[len] = 0;
    return 1;
}

static int watched(const char* p) {
    return !strncmp(p, "/sys/", 5) || !strncmp(p, "/run/", 5) || !strncmp(p, "/var/lib/pbos-utils", 19);
}
static int i2c(const char* p) { return !strncmp(p, "/dev/i2c-", 9); }

ssize_t write(int fd, const void* buf, size_t n) {
    char path[4096];
    int known = fd_path(fd, path, sizeof path);
    long r = syscall(SYS_write, fd, buf, n);
    int err = errno;
    if (known && (watched(path) || i2c(path))) {
        char what[8500];
        int k = snprintf(what, sizeof what, "%s %s ", i2c(path) ? "I2C" : "W", path);
        if (i2c(path)) {
            for (size_t i = 0; i < n && i < 64; ++i)
                k += snprintf(what + k, sizeof what - k, "%02x ", ((const unsigned char*)buf)[i]);
        } else {
            k += snprintf(what + k, sizeof what - k, "= ");
            for (size_t i = 0; i < n && i < 4000 && k < (int)sizeof what - 8; ++i) {
                unsigned char c = ((const unsigned char*)buf)[i];
                if (c == '\n') k += snprintf(what + k, sizeof what - k, "\\n");
                else if (c < 0x20 || c >= 0x7f) k += snprintf(what + k, sizeof what - k, "\\x%02x", c);
                else what[k++] = (char)c;
            }
            what[k] = 0;
        }
        if (r < 0) snprintf(what + strlen(what), sizeof what - strlen(what), " (failed)");
        emit(what);
    }
    errno = err;
    return r;
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);
    char path[4096];
    if (req == 0x0706 && fd_path(fd, path, sizeof path) && i2c(path)) {
        char what[4200];
        snprintf(what, sizeof what, "IOCTL %s SLAVE_FORCE 0x%lx", path, arg);
        emit(what);
        return 0;
    }
    return (int)syscall(SYS_ioctl, fd, req, arg);
}

int kill(pid_t pid, int sig) {
    char p[64], cmd[512] = "?";
    snprintf(p, sizeof p, "/proc/%d/cmdline", (int)pid);
    int fd = (int)syscall(SYS_openat, AT_FDCWD, p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        long n = syscall(SYS_read, fd, cmd, sizeof cmd - 1);
        syscall(SYS_close, fd);
        if (n > 0) {
            cmd[n] = 0;
            for (long i = 0; i < n - 1; ++i)
                if (!cmd[i]) cmd[i] = ' ';
        }
    }
    long r = syscall(SYS_kill, pid, sig);
    int err = errno;
    char what[700];
    snprintf(what, sizeof what, "KILL %d [%s] %s%s", (int)pid, cmd, sigabbrev_np(sig) ? sigabbrev_np(sig) : "?",
             r < 0 ? " (failed)" : "");
    emit(what);
    errno = err;
    return (int)r;
}

int unlink(const char* path) {
    long r = syscall(SYS_unlinkat, AT_FDCWD, path, 0);
    int err = errno;
    if (watched(path)) {
        char what[4200];
        snprintf(what, sizeof what, "UNLINK %s%s", path, r < 0 ? " (failed)" : "");
        emit(what);
    }
    errno = err;
    return (int)r;
}

int rename(const char* a, const char* b) {
    long r = syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 0);
    int err = errno;
    if (watched(a) || watched(b)) {
        char what[8400];
        snprintf(what, sizeof what, "RENAME %s -> %s%s", a, b, r < 0 ? " (failed)" : "");
        emit(what);
    }
    errno = err;
    return (int)r;
}
