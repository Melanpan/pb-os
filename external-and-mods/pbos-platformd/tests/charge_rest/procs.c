// Fake processes for tests/charge_rest, one binary under its copies' names:
//   reaper SteamLaunch AppId=1 -- GAME ARGS   forks GAME and waits (Steam's reaper)
//   game [N]                                  forks N more games (one level), then sleeps
//   other                                     sleeps (not under the reaper)
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
    if (!strcmp(me, "game") && argc > 1) {
        for (int n = atoi(argv[1]); n > 0; --n)
            if (fork() == 0) {
                execl(argv[0], argv[0], (char*)0);
                _exit(127);
            }
    }
    for (;;) pause();
}
