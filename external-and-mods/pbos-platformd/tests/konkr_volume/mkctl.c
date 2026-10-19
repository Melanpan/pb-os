/* Gives a card (snd-dummy in the test VM) the Pocket FIT's speaker controls
 * as user controls: "WSA_RX0/RX1 Digital Volume" (mono, 0..DMAX) and
 * "Speakers Volume" (stereo, 0..31). Ones left from an earlier run are
 * replaced. Usage: mkctl <card> <dmax> */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

static int add(snd_ctl_t *ctl, const char *name, unsigned count, long max) {
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_remove(ctl, id);
    int r = snd_ctl_elem_add_integer(ctl, id, count, 0, max, 1);
    if (r < 0) fprintf(stderr, "%s: %s\n", name, snd_strerror(r));
    return r;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    snd_ctl_t *ctl;
    char dev[32];
    snprintf(dev, sizeof dev, "hw:%s", argv[1]);
    if (snd_ctl_open(&ctl, dev, 0) < 0) return 1;
    long dmax = atol(argv[2]);
    int r = add(ctl, "WSA_RX0 Digital Volume", 1, dmax) | add(ctl, "WSA_RX1 Digital Volume", 1, dmax) |
            add(ctl, "Speakers Volume", 2, 31);
    snd_ctl_close(ctl);
    return r < 0;
}
