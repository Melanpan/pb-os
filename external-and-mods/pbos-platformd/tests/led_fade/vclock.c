// LD_PRELOAD for tests/led_fade: a virtual CLOCK_MONOTONIC that stands
// still but for sleeps, which pass at once and move it on by exactly what
// was asked. The Python and the C++ then see the same frame times and must
// write exactly the same values; their real timing is checked apart.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long virt = 1000LL * 1000000000LL;  // 1000 s

static void put(struct timespec* ts) {
    ts->tv_sec = virt / 1000000000LL;
    ts->tv_nsec = virt % 1000000000LL;
}
static long long ns(const struct timespec* ts) { return (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec; }

int clock_gettime(clockid_t id, struct timespec* ts) {
    if (id != CLOCK_MONOTONIC) {
        // The rest stay real (Python asks for others at start-up).
        return (int)syscall(SYS_clock_gettime, id, ts);
    }
    put(ts);
    return 0;
}

int clock_nanosleep(clockid_t id, int flags, const struct timespec* req, struct timespec* rem) {
    (void)id;
    if (flags & TIMER_ABSTIME) {
        if (ns(req) > virt) virt = ns(req);
    } else {
        virt += ns(req);
    }
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    virt += ns(req);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}
