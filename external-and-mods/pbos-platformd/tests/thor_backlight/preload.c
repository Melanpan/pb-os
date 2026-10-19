// LD_PRELOAD for the daemon under test (both versions): the backlight core's
// answer to a refused brightness, and a log of every panel write.
//
// While /tmp/ctl/einval-top (or -bottom) exists, a write to that panel's
// brightness lands in the file and then fails with EINVAL, as the kernel's
// backlight_device_set_brightness keeps the value in sysfs when the
// driver's update_status refuses it. Every write to a panel's brightness or
// bl_power goes to /tmp/ctl/wlog: "<monotonic s> <file> <value>". And as
// with a sysfs attribute, each write to those files is the whole value
// (the file is cut to it), whatever the offset. While /tmp/ctl/slow-bottom
// holds a number, each write to the bottom panel first waits that many ms
// (the Thor's DSI panel waits for a frame).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* panel(int fd, int* refuse) {
    char p[64], l[512];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, l, sizeof l - 1);
    *refuse = 0;
    if (n <= 0) return 0;
    l[n] = 0;
    if (!strcmp(l, "/sys/class/backlight/ae96000.dsi.0/brightness")) {
        *refuse = access("/tmp/ctl/einval-top", F_OK) == 0;
        return "top";
    }
    if (!strcmp(l, "/sys/class/backlight/ae94000.dsi.0/brightness")) {
        *refuse = access("/tmp/ctl/einval-bottom", F_OK) == 0;
        return "bottom";
    }
    if (!strcmp(l, "/sys/class/backlight/ae94000.dsi.0/bl_power")) return "bl_power";
    return 0;
}

static ssize_t after(int fd, const void* buf, ssize_t r) {
    int saved = errno, refuse;
    const char* p = r > 0 ? panel(fd, &refuse) : 0;
    if (p) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        char line[128];
        int len = snprintf(line, sizeof line, "%ld.%06ld %s %.*s%s\n", (long)ts.tv_sec, ts.tv_nsec / 1000, p,
                           (int)(r > 32 ? 32 : r), (const char*)buf, refuse ? " EINVAL" : "");
        int lf = open("/tmp/ctl/wlog", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        static ssize_t (*real_write)(int, const void*, size_t);
        if (!real_write) real_write = (ssize_t (*)(int, const void*, size_t))dlsym(RTLD_NEXT, "write");
        if (lf >= 0) {
            real_write(lf, line, (size_t)len);
            close(lf);
        }
        if (refuse) {
            errno = EINVAL;
            return -1;
        }
    }
    errno = saved;
    return r;
}

typedef ssize_t (*write_fn)(int, const void*, size_t);
typedef ssize_t (*pwrite_fn)(int, const void*, size_t, off_t);

// A panel's file: the value replaces what was there, as a sysfs store.
static ssize_t store(int fd, const void* buf, size_t n) {
    static pwrite_fn real;
    int refuse;
    const char* p = panel(fd, &refuse);
    if (p && !strcmp(p, "bottom")) {
        char ms[16] = {0};
        int f = open("/tmp/ctl/slow-bottom", O_RDONLY | O_CLOEXEC);
        if (f >= 0) {
            if (read(f, ms, sizeof ms - 1) > 0) {
                struct timespec d = {0, atol(ms) * 1000000L};
                nanosleep(&d, 0);
            }
            close(f);
        }
    }
    if (!real) real = (pwrite_fn)dlsym(RTLD_NEXT, "pwrite");
    if (ftruncate(fd, 0) != 0) return -1;
    return after(fd, buf, real(fd, buf, n, 0));
}

ssize_t write(int fd, const void* buf, size_t n) {
    static write_fn real;
    int refuse;
    if (!real) real = (write_fn)dlsym(RTLD_NEXT, "write");
    return panel(fd, &refuse) ? store(fd, buf, n) : real(fd, buf, n);
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
    static pwrite_fn real;
    int refuse;
    if (!real) real = (pwrite_fn)dlsym(RTLD_NEXT, "pwrite");
    return panel(fd, &refuse) ? store(fd, buf, n) : real(fd, buf, n, off);
}

ssize_t pwrite64(int fd, const void* buf, size_t n, off_t off) { return pwrite(fd, buf, n, off); }
