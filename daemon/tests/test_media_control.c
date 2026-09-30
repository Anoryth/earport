/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Pause/resume on ear detection, against fake MPRIS players on a private
 * bus. media_control.c calls the players synchronously, so each fake
 * player answers from its own thread.
 */

#include <gio/gio.h>

#include "media_control.h"

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"

static GTestDBus *test_bus;

/* ============================================================================
 * Fake MPRIS player
 * ========================================================================== */

static const char player_xml[] =
    "<node>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='Play'/>"
    "    <method name='Pause'/>"
    "    <property name='PlaybackStatus' type='s' access='read'/>"
    "  </interface>"
    "</node>";

typedef struct {
    char *name;
    GThread *thread;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *connection;

    GMutex lock;
    GCond ready_cond;
    bool ready;
    char *status;                /* "Playing", "Paused", "Stopped" */
    int plays;
    int pauses;
} FakePlayer;

static void set_status(FakePlayer *p, const char *status)
{
    g_mutex_lock(&p->lock);
    g_free(p->status);
    p->status = g_strdup(status);
    g_mutex_unlock(&p->lock);
}

static char *get_status(FakePlayer *p)
{
    g_mutex_lock(&p->lock);
    char *status = g_strdup(p->status);
    g_mutex_unlock(&p->lock);
    return status;
}

static void on_player_method(GDBusConnection *connection, const char *sender, const char *path,
                             const char *interface, const char *method, GVariant *parameters,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)parameters;
    FakePlayer *p = user_data;

    g_mutex_lock(&p->lock);
    if (g_strcmp0(method, "Play") == 0) {
        p->plays++;
        g_free(p->status);
        p->status = g_strdup("Playing");
    } else {
        p->pauses++;
        g_free(p->status);
        p->status = g_strdup("Paused");
    }
    g_mutex_unlock(&p->lock);
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *on_player_get_property(GDBusConnection *connection, const char *sender,
                                        const char *path, const char *interface,
                                        const char *property, GError **error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)property;
    (void)error;
    FakePlayer *p = user_data;
    g_autofree char *status = get_status(p);
    return g_variant_new_string(status);
}

static const GDBusInterfaceVTable player_vtable = {
    .method_call = on_player_method,
    .get_property = on_player_get_property,
};

static void on_player_name_acquired(GDBusConnection *connection, const char *name,
                                    gpointer user_data)
{
    (void)connection;
    (void)name;
    FakePlayer *p = user_data;
    g_mutex_lock(&p->lock);
    p->ready = true;
    g_cond_signal(&p->ready_cond);
    g_mutex_unlock(&p->lock);
}

static gpointer player_thread(gpointer user_data)
{
    FakePlayer *p = user_data;
    g_main_context_push_thread_default(p->context);

    p->connection = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(test_bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_assert_nonnull(p->connection);

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(player_xml, NULL);
    g_dbus_connection_register_object(p->connection, MPRIS_PATH, info->interfaces[0],
                                      &player_vtable, p, NULL, NULL);
    g_dbus_node_info_unref(info);
    guint owner = g_bus_own_name_on_connection(p->connection, p->name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                               on_player_name_acquired, NULL, p, NULL);

    g_main_loop_run(p->loop);

    g_bus_unown_name(owner);
    g_dbus_connection_flush_sync(p->connection, NULL, NULL);
    g_object_unref(p->connection);
    g_main_context_pop_thread_default(p->context);
    return NULL;
}

static FakePlayer *fake_player_new(const char *id, const char *status)
{
    FakePlayer *p = g_new0(FakePlayer, 1);
    p->name = g_strdup_printf("org.mpris.MediaPlayer2.%s", id);
    p->status = g_strdup(status);
    p->context = g_main_context_new();
    p->loop = g_main_loop_new(p->context, FALSE);
    g_mutex_init(&p->lock);
    g_cond_init(&p->ready_cond);
    p->thread = g_thread_new(id, player_thread, p);

    g_mutex_lock(&p->lock);
    while (!p->ready)
        g_cond_wait(&p->ready_cond, &p->lock);
    g_mutex_unlock(&p->lock);
    return p;
}

static void fake_player_free(FakePlayer *p)
{
    g_main_loop_quit(p->loop);
    g_thread_join(p->thread);
    g_main_loop_unref(p->loop);
    g_main_context_unref(p->context);
    g_mutex_clear(&p->lock);
    g_cond_clear(&p->ready_cond);
    g_free(p->status);
    g_free(p->name);
    g_free(p);
}

/* The player starts playing and tells it, as real players do */
static void fake_player_start(FakePlayer *p)
{
    set_status(p, "Playing");
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", "PlaybackStatus", g_variant_new_string("Playing"));
    g_dbus_connection_emit_signal(p->connection, NULL, MPRIS_PATH, "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)", PLAYER_INTERFACE, &changed, NULL),
                                  NULL);
}

static int plays(FakePlayer *p)
{
    g_mutex_lock(&p->lock);
    int n = p->plays;
    g_mutex_unlock(&p->lock);
    return n;
}

static int pauses(FakePlayer *p)
{
    g_mutex_lock(&p->lock);
    int n = p->pauses;
    g_mutex_unlock(&p->lock);
    return n;
}

/* ============================================================================
 * Tests
 * ========================================================================== */

static void ears(MediaControl *mc, bool left, bool right)
{
    media_control_on_ear_detection_changed(mc, left, right);
}

static void test_one_out(void)
{
    FakePlayer *player = fake_player_new("one", "Playing");
    MediaControl *mc = media_control_new();

    ears(mc, true, true);
    ears(mc, false, true);
    g_assert_cmpint(pauses(player), ==, 1);

    ears(mc, true, true);
    g_assert_cmpint(plays(player), ==, 1);

    media_control_free(mc);
    fake_player_free(player);
}

/* A player the user had paused stays paused */
static void test_resumes_only_what_it_paused(void)
{
    FakePlayer *music = fake_player_new("music", "Playing");
    FakePlayer *video = fake_player_new("video", "Paused");
    MediaControl *mc = media_control_new();

    ears(mc, true, true);
    ears(mc, true, false);
    ears(mc, true, true);

    g_assert_cmpint(pauses(music), ==, 1);
    g_assert_cmpint(plays(music), ==, 1);
    g_assert_cmpint(pauses(video), ==, 0);
    g_assert_cmpint(plays(video), ==, 0);

    media_control_free(mc);
    fake_player_free(video);
    fake_player_free(music);
}

static void test_both_out(void)
{
    FakePlayer *player = fake_player_new("both", "Playing");
    MediaControl *mc = media_control_new();
    media_control_set_ear_pause_mode(mc, EAR_PAUSE_BOTH_OUT);

    ears(mc, true, true);
    ears(mc, false, true);
    g_assert_cmpint(pauses(player), ==, 0);

    ears(mc, false, false);
    g_assert_cmpint(pauses(player), ==, 1);

    /* One back in is enough to listen again */
    ears(mc, false, true);
    g_assert_cmpint(plays(player), ==, 1);

    media_control_free(mc);
    fake_player_free(player);
}

static void test_disabled(void)
{
    FakePlayer *player = fake_player_new("disabled", "Playing");
    MediaControl *mc = media_control_new();
    media_control_set_ear_pause_mode(mc, EAR_PAUSE_DISABLED);

    ears(mc, true, true);
    ears(mc, false, false);
    ears(mc, true, true);
    g_assert_cmpint(pauses(player), ==, 0);
    g_assert_cmpint(plays(player), ==, 0);

    media_control_free(mc);
    fake_player_free(player);
}

/* Only changes count: AirPods connecting with a pod out don't pause */
static void test_first_state_is_not_a_change(void)
{
    FakePlayer *player = fake_player_new("first", "Playing");
    MediaControl *mc = media_control_new();

    ears(mc, false, true);
    g_assert_cmpint(pauses(player), ==, 0);

    media_control_free(mc);
    fake_player_free(player);
}

/* Taken out again after the user resumed by hand: paused again */
static void test_pause_after_manual_resume(void)
{
    FakePlayer *player = fake_player_new("manual", "Playing");
    MediaControl *mc = media_control_new();

    ears(mc, true, true);
    ears(mc, false, true);
    set_status(player, "Playing");  /* Resumed by hand, pod still out */
    ears(mc, true, true);
    ears(mc, false, true);
    g_assert_cmpint(pauses(player), ==, 2);

    media_control_free(mc);
    fake_player_free(player);
}

static void on_playback_started(void *user_data)
{
    (*(int *)user_data)++;
}

static void test_playback_started(void)
{
    FakePlayer *player = fake_player_new("started", "Stopped");
    MediaControl *mc = media_control_new();
    int started = 0;
    media_control_set_playback_started_callback(mc, on_playback_started, &started);

    fake_player_start(player);
    gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (started == 0) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(NULL, FALSE);
        g_usleep(500);
    }
    g_assert_cmpint(started, ==, 1);

    media_control_free(mc);
    fake_player_free(player);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    g_test_add_func("/media/one-out", test_one_out);
    g_test_add_func("/media/resumes-only-what-it-paused", test_resumes_only_what_it_paused);
    g_test_add_func("/media/both-out", test_both_out);
    g_test_add_func("/media/disabled", test_disabled);
    g_test_add_func("/media/first-state-is-not-a-change", test_first_state_is_not_a_change);
    g_test_add_func("/media/pause-after-manual-resume", test_pause_after_manual_resume);
    g_test_add_func("/media/playback-started", test_playback_started);

    int result = g_test_run();

    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
    return result;
}
