// barry_capture: records the AYN Thor's top screen to an MP4, for the
// Record button on Barry Launcher's quick settings (barry_quick.py starts
// it and stops it with SIGINT).
//
// The picture is gamescope's own PipeWire stream (the node Steam records
// from too: the game, or Steam while it is in front), taken as NV12 in shared
// memory; the sound is everything playing (the default output's monitor).
// gamescope sends a frame only when something on screen changed, so
// videorate makes a steady 30 frames a second of it (the encoder's most at
// 1080p) for the Iris hardware encoder (v4l2h264enc); Opus sound. The MP4 is
// fragmented, so a recording cut off by a crash or power loss still plays.
//
// Usage: barry_capture <node serial> <out.mp4> [--no-audio]
// Exit: 0 saved; 1 bad use or no stream; 2 GStreamer error; 3 stopped as
// the disk filled (less than MIN_FREE left); 4 no frame came at all.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>

#define FPS 30
#define BITRATE 16000000
#define PEAK_BITRATE 24000000
#define MIN_FREE (1024ull * 1024 * 1024)
#define CHECK_EVERY 150  // frames between free-space checks

struct capture {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook listener;
    struct spa_video_info_raw format;
    bool have_format;

    const char *out;
    bool audio;
    GstElement *pipeline;
    GstElement *src;
    GstVideoInfo info;
    guint64 frames;
    bool stopping;
    int status;
    pid_t parent;
};

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "barry_capture: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void quit(struct capture *c, int status)
{
    if (!c->status)
        c->status = status;
    pw_main_loop_quit(c->loop);
}

// EOS or an error, from a GStreamer thread: back to the main loop.
static GstBusSyncReply on_bus(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus;
    struct capture *c = data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        quit(c, 0);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        log_msg("%s: %s (%s)", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err->message, dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        quit(c, 2);
        break;
    }
    default:
        break;
    }
    return GST_BUS_DROP;
}

// Every source ends its stream; the muxer then finishes the file and EOS
// comes back on the bus.
static void stop(struct capture *c, int status)
{
    if (c->stopping)
        return;
    c->stopping = true;
    c->status = status;
    if (!c->pipeline) {
        quit(c, c->status ? c->status : 4);
        return;
    }
    gst_element_send_event(c->pipeline, gst_event_new_eos());
}

static bool build_pipeline(struct capture *c)
{
    const int w = c->format.size.width, h = c->format.size.height;
    gst_video_info_set_format(&c->info, GST_VIDEO_FORMAT_NV12, w, h);
    gchar *desc = g_strdup_printf(
        "appsrc name=v is-live=true do-timestamp=true format=time "
        "caps=video/x-raw,format=NV12,width=%d,height=%d,framerate=0/1,colorimetry=bt709 "
        "! queue max-size-buffers=8 leaky=downstream ! videorate "
        "! video/x-raw,framerate=%d/1 "
        "! v4l2h264enc extra-controls=\"encode,video_bitrate_mode=0,video_bitrate=%d,video_peak_bitrate=%d\" "
        "! video/x-h264,profile=(string)high ! h264parse ! queue "
        "! mp4mux name=mux fragment-duration=1000 ! filesink location=\"%s\" "
        "%s",
        w, h, FPS, BITRATE, PEAK_BITRATE, c->out,
        c->audio ? "pulsesrc device=@DEFAULT_MONITOR@ ! audioconvert ! audioresample "
                   "! opusenc bitrate=128000 ! queue ! mux." : "");
    GError *err = NULL;
    c->pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!c->pipeline) {
        log_msg("pipeline: %s", err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }
    c->src = gst_bin_get_by_name(GST_BIN(c->pipeline), "v");
    GstBus *bus = gst_element_get_bus(c->pipeline);
    gst_bus_set_sync_handler(bus, on_bus, c, NULL);
    gst_object_unref(bus);
    if (gst_element_set_state(c->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        log_msg("cannot start the pipeline");
        return false;
    }
    log_msg("recording %dx%d to %s", w, h, c->out);
    return true;
}

static bool disk_full(const char *path)
{
    gchar *dir = g_path_get_dirname(path);
    struct statvfs st;
    const bool full = statvfs(dir, &st) == 0 && (unsigned long long)st.f_bavail * st.f_frsize < MIN_FREE;
    g_free(dir);
    return full;
}

static void on_process(void *data)
{
    struct capture *c = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    if (c->have_format && !c->stopping && d->data && d->chunk && d->chunk->size > 0) {
        if (!c->pipeline && !build_pipeline(c)) {
            pw_stream_queue_buffer(c->stream, b);
            quit(c, 2);
            return;
        }
        // gamescope: the luma rows, then the chroma rows, each `stride` apart.
        const int w = c->format.size.width, h = c->format.size.height;
        const int stride = d->chunk->stride > 0 ? d->chunk->stride : w;
        const guint8 *src = (const guint8 *)d->data + d->chunk->offset;
        if ((size_t)stride * (h + (h + 1) / 2) <= d->maxsize - d->chunk->offset) {
            GstBuffer *gb = gst_buffer_new_allocate(NULL, GST_VIDEO_INFO_SIZE(&c->info), NULL);
            GstMapInfo map;
            if (gst_buffer_map(gb, &map, GST_MAP_WRITE)) {
                guint8 *y = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&c->info, 0);
                guint8 *uv = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&c->info, 1);
                const int ys = GST_VIDEO_INFO_PLANE_STRIDE(&c->info, 0);
                const int uvs = GST_VIDEO_INFO_PLANE_STRIDE(&c->info, 1);
                for (int r = 0; r < h; r++)
                    memcpy(y + (size_t)r * ys, src + (size_t)r * stride, w);
                const guint8 *srcuv = src + (size_t)stride * h;
                for (int r = 0; r < (h + 1) / 2; r++)
                    memcpy(uv + (size_t)r * uvs, srcuv + (size_t)r * stride, w);
                gst_buffer_unmap(gb, &map);
                gst_app_src_push_buffer(GST_APP_SRC(c->src), gb);
                if (++c->frames % CHECK_EVERY == 0 && disk_full(c->out)) {
                    log_msg("less than 1 GB free: stopping");
                    stop(c, 3);
                }
            } else {
                gst_buffer_unref(gb);
            }
        }
    }
    pw_stream_queue_buffer(c->stream, b);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct capture *c = data;
    if (!param || id != SPA_PARAM_Format)
        return;
    struct spa_video_info_raw f;
    if (spa_format_video_raw_parse(param, &f) < 0 || f.format != SPA_VIDEO_FORMAT_NV12)
        return;
    if (c->pipeline && (f.size.width != c->format.size.width || f.size.height != c->format.size.height)) {
        // The MP4's picture size is fixed: a new size ends this recording.
        log_msg("the stream changed size: stopping");
        stop(c, 0);
        return;
    }
    c->format = f;
    c->have_format = true;

    uint8_t buf[256];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    pw_stream_update_params(c->stream, params, 1);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    (void)old;
    struct capture *c = data;
    if (state == PW_STREAM_STATE_ERROR) {
        log_msg("stream: %s", error ? error : "error");
        stop(c, 1);
    } else if (state == PW_STREAM_STATE_UNCONNECTED && !c->stopping) {
        log_msg("the stream went away");
        stop(c, 0);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void on_signal(void *data, int sig)
{
    (void)sig;
    stop(data, 0);
}

// The recording ends with the one who started it (barry_launcher_statsd):
// checked every second. Not PR_SET_PDEATHSIG, which fires when the starting
// *thread* ends: statsd starts it from the thread of one HTTP request, and
// every recording stopped a second in.
static void on_parent_check(void *data, uint64_t expirations)
{
    (void)expirations;
    struct capture *c = data;
    if (getppid() != c->parent) {
        log_msg("barry_launcher_statsd went away: stopping");
        stop(c, 0);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--no-audio") != 0)) {
        fprintf(stderr, "usage: barry_capture <node serial> <out.mp4> [--no-audio]\n");
        return 1;
    }
    struct capture c = { .out = argv[2], .audio = argc < 4, .parent = getppid() };
    gst_init(&argc, &argv);
    pw_init(&argc, &argv);
    c.loop = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(c.loop);
    pw_loop_add_signal(loop, SIGINT, on_signal, &c);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &c);
    struct spa_source *parent_check = pw_loop_add_timer(loop, on_parent_check, &c);
    struct timespec every = { .tv_sec = 1 };
    pw_loop_update_timer(loop, parent_check, &every, &every, false);

    c.stream = pw_stream_new_simple(loop, "Barry Launcher recording",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT, argv[1], NULL),
        &stream_events, &c);

    uint8_t buf[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(2, 2), &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(0, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)),
        SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_BT709),
        SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_16_235));

    if (pw_stream_connect(c.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        log_msg("cannot connect to node %s", argv[1]);
        return 1;
    }
    pw_main_loop_run(c.loop);

    if (c.pipeline) {
        gst_element_set_state(c.pipeline, GST_STATE_NULL);
        gst_object_unref(c.src);
        gst_object_unref(c.pipeline);
    } else if (!c.status) {
        c.status = 4;
    }
    pw_stream_destroy(c.stream);
    pw_main_loop_destroy(c.loop);
    pw_deinit();
    if (c.status == 4)
        log_msg("no frame came from the stream");
    else if (c.status == 0 || c.status == 3)
        log_msg("saved %s (%llu frames in)", c.out, (unsigned long long)c.frames);
    return c.status;
}
