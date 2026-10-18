/* Prints the default sink's volume (every channel, raw) and mute state each
 * time it changes: the trace the equivalence test compares. */
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>

static char last[256];

static void on_sink(pa_context *c, const pa_sink_info *i, int eol, void *u) {
    (void)c; (void)u;
    if (eol || !i) return;
    char s[256];
    int n = 0;
    for (unsigned k = 0; k < i->volume.channels; ++k) n += snprintf(s + n, sizeof s - n, "%u ", i->volume.values[k]);
    snprintf(s + n, sizeof s - n, "mute=%d", i->mute);
    if (strcmp(s, last)) {
        strcpy(last, s);
        printf("%s\n", s);
        fflush(stdout);
    }
}

static void ask(pa_context *c) { pa_operation_unref(pa_context_get_sink_info_by_name(c, "@DEFAULT_SINK@", on_sink, NULL)); }

static void on_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *u) {
    (void)t; (void)idx; (void)u;
    ask(c);
}

static void on_state(pa_context *c, void *u) {
    pa_mainloop *ml = u;
    pa_context_state_t s = pa_context_get_state(c);
    if (s == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(c, on_event, NULL);
        pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK, NULL, NULL));
        ask(c);
    } else if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
        pa_mainloop_quit(ml, 1);
    }
}

int main(void) {
    pa_mainloop *ml = pa_mainloop_new();
    pa_context *c = pa_context_new(pa_mainloop_get_api(ml), "sinkrec");
    pa_context_set_state_callback(c, on_state, ml);
    pa_context_connect(c, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);
    int r = 0;
    pa_mainloop_run(ml, &r);
    return r;
}
