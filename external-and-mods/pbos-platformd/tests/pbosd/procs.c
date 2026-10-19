// Fake processes for tests/pbosd's game boost case, one binary under the
// names it is copied to (comm is the file's name):
//   reaper SteamLaunch AppId=1 -- GAME   forks GAME and waits (Steam's reaper)
//   game / gamescope-wl / steam          two threads at once, a third after
//                                        $PROCS_LATE_S seconds, then sleeps
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void* idle(void* _) {
    (void)_;
    for (;;) pause();
    return 0;
}

int main(int argc, char** argv) {
    const char* me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(me, "reaper")) {
        int i = 1;
        while (i < argc && strcmp(argv[i], "--")) ++i;
        if (i + 1 >= argc) return 2;
        pid_t p = fork();
        if (p == 0) {
            execv(argv[i + 1], argv + i + 1);
            _exit(127);
        }
        waitpid(p, 0, 0);
        return 0;
    }
    pthread_t t;
    pthread_create(&t, 0, idle, 0);
    pthread_create(&t, 0, idle, 0);
    const char* late = getenv("PROCS_LATE_S");
    sleep(late ? (unsigned)atoi(late) : 3);
    pthread_create(&t, 0, idle, 0);
    for (;;) pause();
}
