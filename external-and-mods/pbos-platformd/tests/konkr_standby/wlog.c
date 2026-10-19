// LD_PRELOAD for tests/konkr_standby: every write() to a file under /sys
// and every kill(), as lines in $KSTANDBY_LOG ("<monotonic>\t<what>"), in
// the order they happen (the stubs append their argv to the same file).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void emit(const char* what) {
    const char* log = getenv("KSTANDBY_LOG");
    if (!log) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[8192];
    int m = snprintf(line, sizeof line, "%ld.%06ld\t%s\n", (long)ts.tv_sec, ts.tv_nsec / 1000, what);
    int lfd = (int)syscall(SYS_openat, AT_FDCWD, log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (lfd >= 0) {
        syscall(SYS_write, lfd, line, (size_t)m);
        syscall(SYS_close, lfd);
    }
}

ssize_t write(int fd, const void* buf, size_t n) {
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t len = readlink(link, path, sizeof path - 1);
    long r = syscall(SYS_write, fd, buf, n);
    if (len > 5 && !strncmp(path, "/sys/", 5)) {
        path[len] = 0;
        char what[6000];
        size_t k = n < 1024 ? n : 1024;
        snprintf(what, sizeof what, "write %s = %.*s%s", path, (int)k, (const char*)buf, r < 0 ? " (failed)" : "");
        emit(what);
    }
    return r;
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
    char what[700];
    snprintf(what, sizeof what, "kill %d [%s] %s%s", (int)pid, cmd, strsignal(sig), r < 0 ? " (failed)" : "");
    emit(what);
    return (int)r;
}
