// A process that only waits, copied to the names the daemon looks for
// (gamescope --steam, kwin_wayland): argv[0]'s base name is what counts.
#include <unistd.h>

int main(void) {
    for (;;) pause();
}
