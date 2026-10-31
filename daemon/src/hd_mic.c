/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

/* PipeWire's headers use POSIX 2008 (locale_t) */
#define _GNU_SOURCE

#include "hd_mic.h"

#ifndef HAVE_HD_MIC

HdMic *hd_mic_new(const HdMicCallbacks *callbacks, void *user_data)
{
    (void)callbacks;
    (void)user_data;
    return NULL;
}

void hd_mic_free(HdMic *mic) { (void)mic; }
void hd_mic_set_enabled(HdMic *mic, bool enabled) { (void)mic; (void)enabled; }
void hd_mic_set_name(HdMic *mic, const char *name) { (void)mic; (void)name; }
void hd_mic_set_connected(HdMic *mic, bool connected) { (void)mic; (void)connected; }
void hd_mic_audio(HdMic *mic, const AapMicAudio *audio) { (void)mic; (void)audio; }

#else

#include <errno.h>
#include <glib.h>
#include <glib-unix.h>
#include <libavcodec/avcodec.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdlib.h>
#include <string.h>

#define RATE 64000
/* AudioSpecificConfig: AAC-ELD (object type 39), 48 kHz index, mono, 480
 * samples a frame. The decoder's rate doesn't matter: the AirPods send a
 * frame every 7.5 ms, so the samples are 64 kHz. */
static const uint8_t ELD_ASC[] = { 0xF8, 0xE6, 0x30, 0x00 };
/* The raw stream is quiet (soft speech around -45 dBFS): automatic gain up
 * to +24 dB, aiming the peaks at -6 dBFS. Lowered at once on a frame that
 * would clip (measured before amplifying it), raised back over ~2 s. */
#define MAX_GAIN 16.0f
#define TARGET_PEAK 0.5f
#define GAIN_RELEASE 1.0105f   /* A frame (7.5 ms) each: x16 in 2 s */
/* The packets come in bursts (4 frames every 30 ms): keep some ahead */
#define RING_SIZE (RATE / 2)
#define PRIME_SAMPLES (RATE * 40 / 1000)
#define MAX_LATENCY_SAMPLES (RATE * 200 / 1000)
#define RETRY_S 5           /* PipeWire went away: try again */

struct HdMic {
    HdMicCallbacks callbacks;
    void *user_data;
    bool enabled;
    bool connected;
    char *description;      /* "<AirPods' name> (HD)" */

    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;

    struct pw_loop *loop;
    guint loop_source;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    guint retry_id;

    GHashTable *links;      /* Links leaving our source (ids) */
    bool capturing;

    float ring[RING_SIZE];
    size_t ring_read;
    size_t ring_fill;
    bool primed;
    float gain;
};

static void update(HdMic *mic);

/* ============================================================================
 * Capture
 * ========================================================================== */

static void start_capture(HdMic *mic)
{
    mic->capturing = true;
    mic->ring_fill = 0;
    mic->primed = false;
    mic->gain = MAX_GAIN;
    avcodec_flush_buffers(mic->codec);
    g_message("HD microphone: recording, starting the AirPods' stream");
    mic->callbacks.send(AAP_PKT_MIC_START, AAP_MIC_START_SIZE, mic->user_data);
    mic->callbacks.capture_changed(true, mic->user_data);
}

static void stop_capture(HdMic *mic)
{
    if (!mic->capturing)
        return;
    mic->capturing = false;
    g_message("HD microphone: no more recording, stopping the AirPods' stream");
    if (mic->connected)
        mic->callbacks.send(AAP_PKT_MIC_STOP, AAP_MIC_STOP_SIZE, mic->user_data);
    mic->callbacks.capture_changed(false, mic->user_data);
}

float hd_mic_next_gain(float gain, float peak)
{
    float wanted = peak > TARGET_PEAK / MAX_GAIN ? TARGET_PEAK / peak : MAX_GAIN;
    if (wanted < 1.0f)
        wanted = 1.0f;
    return wanted < gain ? wanted : MIN(wanted, gain * GAIN_RELEASE);
}

static void ring_push(HdMic *mic, const float *samples, size_t n)
{
    float peak = 0;
    for (size_t i = 0; i < n; i++)
        peak = MAX(peak, fabsf(samples[i]));
    mic->gain = hd_mic_next_gain(mic->gain, peak);

    for (size_t i = 0; i < n; i++) {
        if (mic->ring_fill == RING_SIZE) {
            mic->ring_read = (mic->ring_read + 1) % RING_SIZE;
            mic->ring_fill--;
        }
        float s = samples[i] * mic->gain;
        mic->ring[(mic->ring_read + mic->ring_fill) % RING_SIZE] = s > 1 ? 1 : s < -1 ? -1 : s;
        mic->ring_fill++;
    }
    /* The AirPods' clock runs a little ahead of PipeWire's: drop the excess */
    if (mic->ring_fill > MAX_LATENCY_SAMPLES) {
        size_t drop = mic->ring_fill - PRIME_SAMPLES;
        mic->ring_read = (mic->ring_read + drop) % RING_SIZE;
        mic->ring_fill -= drop;
    }
}

void hd_mic_audio(HdMic *mic, const AapMicAudio *audio)
{
    if (mic == NULL || !mic->capturing)
        return;

    size_t offset = 0, len;
    const uint8_t *frame;
    uint8_t buf[256 + AV_INPUT_BUFFER_PADDING_SIZE];
    while (aap_mic_audio_next_frame(audio, &offset, &frame, &len)) {
        memcpy(buf, frame, len);
        memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        mic->packet->data = buf;
        mic->packet->size = (int)len;
        if (avcodec_send_packet(mic->codec, mic->packet) < 0 ||
            avcodec_receive_frame(mic->codec, mic->frame) < 0)
            continue;   /* A lost frame: a 7.5 ms gap */
        /* The native decoder gives planar float */
        ring_push(mic, (const float *)mic->frame->data[0], (size_t)mic->frame->nb_samples);
    }
}

/* ============================================================================
 * PipeWire
 * ========================================================================== */

static void on_process(void *data)
{
    HdMic *mic = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(mic->stream);
    if (b == NULL)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    float *dst = d->data;
    if (dst == NULL)
        return;

    size_t n = d->maxsize / sizeof(float);
    if (b->requested > 0 && b->requested < n)
        n = (size_t)b->requested;
    if (!mic->primed && mic->ring_fill >= PRIME_SAMPLES)
        mic->primed = true;
    size_t got = mic->primed ? MIN(n, mic->ring_fill) : 0;
    for (size_t i = 0; i < got; i++)
        dst[i] = mic->ring[(mic->ring_read + i) % RING_SIZE];
    mic->ring_read = (mic->ring_read + got) % RING_SIZE;
    mic->ring_fill -= got;
    if (got < n) {
        /* Ran dry: silence, and wait to have some ahead again */
        memset(dst + got, 0, (n - got) * sizeof(float));
        mic->primed = false;
    }

    d->chunk->offset = 0;
    d->chunk->stride = sizeof(float);
    d->chunk->size = (uint32_t)(n * sizeof(float));
    pw_stream_queue_buffer(mic->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static void on_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                      uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    HdMic *mic = data;
    if (strcmp(type, PW_TYPE_INTERFACE_Link) != 0 || props == NULL || mic->stream == NULL)
        return;
    const char *output = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
    if (output == NULL || strtoul(output, NULL, 10) != pw_stream_get_node_id(mic->stream))
        return;
    g_hash_table_add(mic->links, GUINT_TO_POINTER(id));
    if (!mic->capturing)
        start_capture(mic);
}

static void on_global_remove(void *data, uint32_t id)
{
    HdMic *mic = data;
    if (g_hash_table_remove(mic->links, GUINT_TO_POINTER(id)) &&
        g_hash_table_size(mic->links) == 0)
        stop_capture(mic);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

static void pipewire_close(HdMic *mic);

static gboolean retry_cb(gpointer user_data)
{
    HdMic *mic = user_data;
    mic->retry_id = 0;
    pipewire_close(mic);
    update(mic);
    return G_SOURCE_REMOVE;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    (void)seq;
    HdMic *mic = data;
    if (id != PW_ID_CORE || res != -EPIPE)
        return;
    /* PipeWire restarted or stopped: the source is gone with it. Closed
     * from the retry, not from within PipeWire's own callback. */
    g_message("HD microphone: lost PipeWire (%s), retrying in %d s", message, RETRY_S);
    if (mic->retry_id == 0)
        mic->retry_id = g_timeout_add_seconds(RETRY_S, retry_cb, mic);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static gboolean on_loop_ready(int fd, GIOCondition condition, gpointer user_data)
{
    (void)fd;
    (void)condition;
    HdMic *mic = user_data;
    pw_loop_iterate(mic->loop, 0);
    return G_SOURCE_CONTINUE;
}

static void pipewire_close(HdMic *mic)
{
    stop_capture(mic);
    g_hash_table_remove_all(mic->links);
    if (mic->stream != NULL) {
        pw_stream_destroy(mic->stream);
        mic->stream = NULL;
    }
    if (mic->registry != NULL) {
        spa_hook_remove(&mic->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)mic->registry);
        mic->registry = NULL;
    }
    if (mic->core != NULL) {
        spa_hook_remove(&mic->core_listener);
        pw_core_disconnect(mic->core);
        mic->core = NULL;
    }
}

static bool pipewire_open(HdMic *mic)
{
    mic->core = pw_context_connect(mic->context, NULL, 0);
    if (mic->core == NULL) {
        g_message("HD microphone: PipeWire not running");
        return false;
    }
    pw_core_add_listener(mic->core, &mic->core_listener, &core_events, mic);

    mic->stream = pw_stream_new(mic->core, "EarPort HD microphone",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CLASS, "Audio/Source",
                          PW_KEY_NODE_NAME, "earport_hd_mic",
                          PW_KEY_NODE_DESCRIPTION, mic->description,
                          PW_KEY_MEDIA_ICON_NAME, "audio-headset",
                          NULL));
    pw_stream_add_listener(mic->stream, &mic->stream_listener, &stream_events, mic);

    uint8_t pod[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = RATE,
        .channels = 1,
        .position = { SPA_AUDIO_CHANNEL_MONO },
    };
    const struct spa_pod *params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info),
    };
    if (pw_stream_connect(mic->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        g_warning("HD microphone: could not create the PipeWire source");
        pipewire_close(mic);
        return false;
    }

    /* After the stream: its node id is known when its links show up */
    mic->registry = pw_core_get_registry(mic->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(mic->registry, &mic->registry_listener, &registry_events, mic);
    g_message("HD microphone: source ready");
    return true;
}

/* The source exists while the option is on and the AirPods connected */
static void update(HdMic *mic)
{
    bool wanted = mic->enabled && mic->connected;
    if (wanted && mic->core == NULL)
        pipewire_open(mic);
    else if (!wanted)
        pipewire_close(mic);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

static bool open_decoder(HdMic *mic)
{
    const AVCodec *aac = avcodec_find_decoder(AV_CODEC_ID_AAC);
    mic->codec = aac != NULL ? avcodec_alloc_context3(aac) : NULL;
    mic->packet = av_packet_alloc();
    mic->frame = av_frame_alloc();
    if (mic->codec == NULL || mic->packet == NULL || mic->frame == NULL)
        return false;

    mic->codec->extradata = av_mallocz(sizeof(ELD_ASC) + AV_INPUT_BUFFER_PADDING_SIZE);
    if (mic->codec->extradata == NULL)
        return false;
    memcpy(mic->codec->extradata, ELD_ASC, sizeof(ELD_ASC));
    mic->codec->extradata_size = sizeof(ELD_ASC);
    mic->codec->sample_rate = 48000;
    av_channel_layout_default(&mic->codec->ch_layout, 1);
    return avcodec_open2(mic->codec, aac, NULL) == 0 &&
           mic->codec->sample_fmt == AV_SAMPLE_FMT_FLTP;
}

HdMic *hd_mic_new(const HdMicCallbacks *callbacks, void *user_data)
{
    HdMic *mic = g_new0(HdMic, 1);
    mic->callbacks = *callbacks;
    mic->user_data = user_data;
    mic->links = g_hash_table_new(NULL, NULL);
    mic->description = g_strdup("AirPods (HD)");

    if (!open_decoder(mic)) {
        g_message("HD microphone unavailable: no AAC-ELD decoder in FFmpeg");
        hd_mic_free(mic);
        return NULL;
    }

    pw_init(NULL, NULL);
    mic->loop = pw_loop_new(NULL);
    mic->context = mic->loop != NULL ? pw_context_new(mic->loop, NULL, 0) : NULL;
    if (mic->context == NULL) {
        g_warning("HD microphone unavailable: no PipeWire context");
        hd_mic_free(mic);
        return NULL;
    }
    /* Everything runs in the GLib main loop, PipeWire's callbacks too */
    pw_loop_enter(mic->loop);
    mic->loop_source = g_unix_fd_add(pw_loop_get_fd(mic->loop), G_IO_IN, on_loop_ready, mic);
    return mic;
}

void hd_mic_free(HdMic *mic)
{
    if (mic == NULL)
        return;
    if (mic->retry_id > 0)
        g_source_remove(mic->retry_id);
    pipewire_close(mic);
    if (mic->loop_source > 0)
        g_source_remove(mic->loop_source);
    if (mic->context != NULL)
        pw_context_destroy(mic->context);
    if (mic->loop != NULL) {
        if (mic->loop_source > 0)
            pw_loop_leave(mic->loop);
        pw_loop_destroy(mic->loop);
    }
    avcodec_free_context(&mic->codec);
    av_packet_free(&mic->packet);
    av_frame_free(&mic->frame);
    g_hash_table_unref(mic->links);
    g_free(mic->description);
    g_free(mic);
}

void hd_mic_set_name(HdMic *mic, const char *name)
{
    if (mic == NULL)
        return;
    char *description = g_strdup_printf("%s (HD)", name != NULL && name[0] != '\0' ? name : "AirPods");
    if (g_strcmp0(description, mic->description) == 0) {
        g_free(description);
        return;
    }
    g_free(mic->description);
    mic->description = description;
    if (mic->stream != NULL) {
        const struct spa_dict_item item = SPA_DICT_ITEM_INIT(PW_KEY_NODE_DESCRIPTION, description);
        pw_stream_update_properties(mic->stream, &SPA_DICT_INIT(&item, 1));
    }
}

void hd_mic_set_enabled(HdMic *mic, bool enabled)
{
    if (mic == NULL)
        return;
    mic->enabled = enabled;
    update(mic);
}

void hd_mic_set_connected(HdMic *mic, bool connected)
{
    if (mic == NULL)
        return;
    mic->connected = connected;
    update(mic);
}

#endif /* HAVE_HD_MIC */
