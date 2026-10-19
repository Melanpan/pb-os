// LD_PRELOAD for tests/pbosd: every write() to a file under /sys, as
// "path<TAB>value" lines in $PBOSD_WLOG (the daemons write each sysfs value
// with one write()).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

ssize_t write(int fd, const void* buf, size_t n) {
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t len = readlink(link, path, sizeof path - 1);
    long r = syscall(SYS_write, fd, buf, n);
    const char* log = getenv("PBOSD_WLOG");
    if (len > 5 && log && !strncmp(path, "/sys/", 5)) {
        path[len] = 0;
        char line[8192];
        size_t k = n < 2048 ? n : 2048;
        int m = snprintf(line, sizeof line, "%s\t%.*s%s\n", path, (int)k, (const char*)buf, r < 0 ? "\t(failed)" : "");
        for (int i = (int)strlen(path) + 1; i < m - 1; ++i)
            if (line[i] == '\n') line[i] = ' ';
        int lfd = (int)syscall(SYS_openat, AT_FDCWD, log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (lfd >= 0) {
            syscall(SYS_write, lfd, line, (size_t)m);
            syscall(SYS_close, lfd);
        }
    }
    return r;
}
