/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * D-Bus integration tests: the service on a private test bus, wired to the
 * device, the controls and the AAP link as in main.c, over the fake
 * Bluetooth transport. Calls methods like the extension does and checks
 * the packets sent to the AirPods, the properties and the signals.
 */

#include <gio/gio.h>
#include <string.h>

#include "aap_link.h"
#include "config.h"
#include "controls.h"
#include "dbus_service.h"
#include "device.h"
#include "fake_bluetooth.h"

#define BUS_NAME "io.github.anoryth.EarPort"
#define OBJECT_PATH "/io/github/anoryth/EarPort"
#define INTERFACE "io.github.anoryth.EarPort1"
#define ADDRESS "00:11:22:33:44:55"

/* Same value as passed by meson.build */
#define GAP_MS 2

typedef struct {
    AirPodsState state;
    DbusService *dbus;
    Device device;
    Controls controls;
    AapLink *link;

    GDBusConnection *client;
    guint signal_id;
    GPtrArray *signals;          /* Names of the signals received */
    bool speaking;
} Fixture;

static GTestDBus *test_bus;

/* ============================================================================
 * Wiring, as in main.c
 * ========================================================================== */

static void on_link_connected(const char *address, const char *name, void *user_data)
{
    Fixture *f = user_data;
    device_session_started(&f->device, address, name);
    controls_send_saved_settings(&f->controls, address);
}

static void on_link_disconnected(void *user_data)
{
    Fixture *f = user_data;
    device_session_ended(&f->device);
}

static void on_link_packet(const AapParsedPacket *packet, void *user_data)
{
    Fixture *f = user_data;
    device_handle_packet(&f->device, packet);
}

/* ============================================================================
 * Helpers
 * ========================================================================== */

#define RUN_UNTIL(cond) do { \
    gint64 deadline_ = g_get_monotonic_time() + 2 * G_USEC_PER_SEC; \
    while (!(cond)) { \
        g_assert_cmpint(g_get_monotonic_time(), <, deadline_); \
        g_main_context_iteration(NULL, FALSE); \
        g_usleep(500); \
    } \
} while (0)

static gboolean set_flag(gpointer user_data)
{
    *(bool *)user_data = true;
    return G_SOURCE_REMOVE;
}

static void run_for(guint ms)
{
    bool done = false;
    g_timeout_add(ms, set_flag, &done);
    while (!done)
        g_main_context_iteration(NULL, TRUE);
}

static bool name_has_owner(Fixture *f)
{
    GVariant *reply = g_dbus_connection_call_sync(f->client, "org.freedesktop.DBus",
                                                  "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                  "NameHasOwner", g_variant_new("(s)", BUS_NAME),
                                                  G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
                                                  -1, NULL, NULL);
    gboolean owned = FALSE;
    g_variant_get(reply, "(b)", &owned);
    g_variant_unref(reply);
    return owned;
}

/* Calls run asynchronously while we iterate: the service answers from
 * this same main loop */
typedef struct {
    bool done;
    GVariant *reply;
    GError *error;
} CallResult;

static void on_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    CallResult *result = user_data;
    result->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &result->error);
    result->done = true;
}

static CallResult call_full(Fixture *f, const char *interface, const char *method,
                            GVariant *parameters)
{
    CallResult result = { 0 };
    g_dbus_connection_call(f->client, BUS_NAME, OBJECT_PATH, interface, method, parameters,
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_call_done, &result);
    RUN_UNTIL(result.done);
    return result;
}

/* Call a method the way the extension does */
static GError *call(Fixture *f, const char *method, GVariant *parameters)
{
    CallResult result = call_full(f, INTERFACE, method, parameters);
    if (result.reply != NULL)
        g_variant_unref(result.reply);
    return result.error;
}

static void call_ok(Fixture *f, const char *method, GVariant *parameters)
{
    GError *error = call(f, method, parameters);
    g_assert_no_error(error);
}

static void call_fails(Fixture *f, const char *method, GVariant *parameters, GDBusError code)
{
    GError *error = call(f, method, parameters);
    g_assert_error(error, G_DBUS_ERROR, (int)code);
    g_error_free(error);
}

/* Current value of a property (unref it) */
static GVariant *get_property(Fixture *f, const char *name)
{
    CallResult result = call_full(f, "org.freedesktop.DBus.Properties", "Get",
                                  g_variant_new("(ss)", INTERFACE, name));
    g_assert_no_error(result.error);
    GVariant *value = NULL;
    g_variant_get(result.reply, "(v)", &value);
    g_variant_unref(result.reply);
    return value;
}

static bool get_bool(Fixture *f, const char *name)
{
    GVariant *value = get_property(f, name);
    bool b = g_variant_get_boolean(value);
    g_variant_unref(value);
    return b;
}

static int get_int(Fixture *f, const char *name)
{
    GVariant *value = get_property(f, name);
    int i = g_variant_get_int32(value);
    g_variant_unref(value);
    return i;
}

static char *get_string(Fixture *f, const char *name)
{
    GVariant *value = get_property(f, name);
    char *s = g_variant_dup_string(value, NULL);
    g_variant_unref(value);
    return s;
}

/* A key of the Settings dictionary, NULL if absent (unref it) */
static GVariant *get_setting(Fixture *f, const char *key)
{
    GVariant *settings = get_property(f, "Settings");
    GVariant *value = g_variant_lookup_value(settings, key, NULL);
    g_variant_unref(settings);
    return value;
}

static void on_signal(GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *signal, GVariant *parameters,
                      gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    Fixture *f = user_data;

    g_ptr_array_add(f->signals, g_strdup(signal));
    if (g_strcmp0(signal, "SpeakingChanged") == 0)
        g_variant_get(parameters, "(b)", &f->speaking);
}

static bool received(Fixture *f, const char *signal)
{
    for (guint i = 0; i < f->signals->len; i++) {
        if (g_strcmp0(f->signals->pdata[i], signal) == 0)
            return true;
    }
    return false;
}

static void assert_sent(const uint8_t *packet, size_t len)
{
    RUN_UNTIL(fake_bt_count_sent(packet, len) > 0);
}

#define ASSERT_SENT(...) do { \
    const uint8_t pkt_[] = { __VA_ARGS__ }; \
    assert_sent(pkt_, sizeof(pkt_)); \
} while (0)

#define RECEIVE(...) do { \
    const uint8_t pkt_[] = { __VA_ARGS__ }; \
    fake_bt_receive(pkt_, sizeof(pkt_)); \
} while (0)

/* ============================================================================
 * Fixture
 * ========================================================================== */

static void fixture_setup(Fixture *f, gconstpointer data)
{
    (void)data;
    static const AapLinkCallbacks link_callbacks = {
        .connected = on_link_connected,
        .disconnected = on_link_disconnected,
        .packet = on_link_packet,
    };

    fake_bt_reset();
    *f = (Fixture) { 0 };
    f->signals = g_ptr_array_new_with_free_func(g_free);

    airpods_state_init(&f->state);
    f->dbus = dbus_service_new(&f->state);
    device_init(&f->device, &f->state, f->dbus, NULL);
    f->link = aap_link_new(&link_callbacks, f);
    controls_init(&f->controls, &f->device, f->link, f->dbus);

    f->client = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(test_bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_assert_nonnull(f->client);
    f->signal_id = g_dbus_connection_signal_subscribe(f->client, NULL, INTERFACE, NULL, OBJECT_PATH,
                                                      NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                      on_signal, f, NULL);

    g_assert_true(dbus_service_start(f->dbus));
    RUN_UNTIL(name_has_owner(f));
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
    (void)data;

    controls_cleanup(&f->controls);
    aap_link_free(f->link);
    dbus_service_free(f->dbus);
    RUN_UNTIL(!name_has_owner(f));

    g_dbus_connection_signal_unsubscribe(f->client, f->signal_id);
    g_object_unref(f->client);
    airpods_state_cleanup(&f->state);
    g_ptr_array_unref(f->signals);
}

/* ============================================================================
 * Tests
 * ========================================================================== */

/* What AirPods Pro 2 USB-C send after the handshake (see test_device.c) */
static void connect_airpods(Fixture *f)
{
    const uint8_t header[] = { 0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xDD, 0x00, 0x04, 0x00 };
    const char strings[] = "AirPods Pro\0A3048\0Apple Inc.\0SERIAL0000";
    uint8_t metadata[sizeof(header) + sizeof(strings)];

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");

    memcpy(metadata, header, sizeof(header));
    memcpy(metadata + sizeof(header), strings, sizeof(strings));
    fake_bt_receive(metadata, sizeof(metadata));

    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00);  /* ANC */
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x01, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00);  /* CA on */
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1B, 0x02, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x35, 0x01, 0x00, 0x00, 0x00);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 0x32, 0x00, 0x00, 0x00);  /* Level 50 */
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
            0x04, 0x01, 0x53, 0x02, 0x01,
            0x02, 0x01, 0x53, 0x02, 0x01,
            0x08, 0x01, 0x00, 0x04, 0x01);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00);  /* Both in */
}

static void test_disconnected(Fixture *f, gconstpointer data)
{
    (void)data;

    g_assert_false(get_bool(f, "Connected"));
    g_assert_cmpint(get_int(f, "BatteryLeft"), ==, -1);

    /* Nothing announced: no setting can be changed */
    call_fails(f, "SetSetting", g_variant_new("(sv)", "OneBudANC", g_variant_new_boolean(TRUE)),
               G_DBUS_ERROR_NOT_SUPPORTED);
}

static void test_connection(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    RUN_UNTIL(received(f, "DeviceConnected"));
    RUN_UNTIL(received(f, "BatteryChanged"));
    RUN_UNTIL(received(f, "EarDetectionChanged"));

    g_assert_true(get_bool(f, "Connected"));
    g_autofree char *model = get_string(f, "DeviceModel");
    g_assert_cmpstr(model, ==, "AirPods Pro 2 (USB-C)");
    g_autofree char *mode = get_string(f, "NoiseControlMode");
    g_assert_cmpstr(mode, ==, "anc");
    g_assert_cmpint(get_int(f, "BatteryLeft"), ==, 83);
    g_assert_true(get_bool(f, "ConversationalAwareness"));

    /* Settings the AirPods announced, booleans as b, choices as i */
    g_autoptr(GVariant) one_bud = get_setting(f, "OneBudANC");
    g_assert_nonnull(one_bud);
    g_assert_false(g_variant_get_boolean(one_bud));
    g_autoptr(GVariant) press_speed = get_setting(f, "PressSpeed");
    g_assert_cmpint(g_variant_get_int32(press_speed), ==, 0);
}

static void test_noise_control(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    call_ok(f, "SetNoiseControlMode", g_variant_new("(s)", "transparency"));
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x03, 0x00, 0x00, 0x00);

    /* The AirPods confirm the change */
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x03, 0x00, 0x00, 0x00);
    RUN_UNTIL(received(f, "NoiseControlModeChanged"));
    g_autofree char *mode = get_string(f, "NoiseControlMode");
    g_assert_cmpstr(mode, ==, "transparency");
}

/* A typo must not turn noise control off */
static void test_noise_control_unknown(Fixture *f, gconstpointer data)
{
    (void)data;
    const uint8_t off[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01 };

    connect_airpods(f);
    call_fails(f, "SetNoiseControlMode", g_variant_new("(s)", "anx"), G_DBUS_ERROR_INVALID_ARGS);
    run_for(10 * GAP_MS);
    g_assert_cmpuint(fake_bt_count_sent(off, sizeof(off)), ==, 0);
}

static void test_set_setting(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);

    call_ok(f, "SetSetting", g_variant_new("(sv)", "OneBudANC", g_variant_new_boolean(TRUE)));
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1B, 0x01, 0x00, 0x00, 0x00);
    /* The AirPods don't echo settings: the state is updated right away */
    g_autoptr(GVariant) one_bud = get_setting(f, "OneBudANC");
    g_assert_true(g_variant_get_boolean(one_bud));

    call_ok(f, "SetSetting", g_variant_new("(sv)", "PressSpeed", g_variant_new_int32(2)));
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x17, 0x02, 0x00, 0x00, 0x00);
}

static void test_set_setting_errors(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    call_fails(f, "SetSetting", g_variant_new("(sv)", "Nope", g_variant_new_boolean(TRUE)),
               G_DBUS_ERROR_INVALID_ARGS);
    call_fails(f, "SetSetting", g_variant_new("(sv)", "OneBudANC", g_variant_new_int32(1)),
               G_DBUS_ERROR_INVALID_ARGS);
    call_fails(f, "SetSetting", g_variant_new("(sv)", "PressSpeed", g_variant_new_int32(5)),
               G_DBUS_ERROR_INVALID_ARGS);

    /* Link down: the command can't be sent */
    fake_bt.refuse_connects = 100;
    fake_bt_peer_closes();
    call_fails(f, "SetSetting", g_variant_new("(sv)", "OneBudANC", g_variant_new_boolean(TRUE)),
               G_DBUS_ERROR_NOT_SUPPORTED);
}

static void test_conversation_awareness(Fixture *f, gconstpointer data)
{
    (void)data;
    DeviceProfile profile;

    connect_airpods(f);
    call_ok(f, "SetConversationalAwareness", g_variant_new("(b)", FALSE));
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x02, 0x00, 0x00, 0x00);
    g_assert_false(get_bool(f, "ConversationalAwareness"));

    /* Sent back on the next connection */
    g_assert_true(config_load_device_profile(ADDRESS, &profile));
    g_assert_false(profile.conversational_awareness);
}

static void test_adaptive_level(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    call_ok(f, "SetAdaptiveNoiseLevel", g_variant_new("(i)", 30));
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 0x1E, 0x00, 0x00, 0x00);
    g_assert_cmpint(get_int(f, "AdaptiveNoiseLevel"), ==, 30);
}

static void test_listening_modes(Fixture *f, gconstpointer data)
{
    (void)data;
    const uint8_t anc_only[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1A, 0x02 };

    connect_airpods(f);
    call_ok(f, "SetListeningModes", g_variant_new("(bbbb)", FALSE, TRUE, TRUE, FALSE));
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1A, 0x06, 0x00, 0x00, 0x00);
    g_assert_false(get_bool(f, "ListeningModeOff"));
    g_assert_true(get_bool(f, "ListeningModeANC"));

    /* The long press needs at least two modes to cycle through */
    call(f, "SetListeningModes", g_variant_new("(bbbb)", FALSE, FALSE, TRUE, FALSE));
    run_for(10 * GAP_MS);
    g_assert_cmpuint(fake_bt_count_sent(anc_only, sizeof(anc_only)), ==, 0);
    g_assert_true(get_bool(f, "ListeningModeTransparency"));
}

static void test_display_name(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    call_ok(f, "SetDisplayName", g_variant_new("(s)", "Mes AirPods"));
    g_autofree char *custom = get_string(f, "DisplayName");
    g_assert_cmpstr(custom, ==, "Mes AirPods");

    /* Empty: back to the model name */
    call_ok(f, "SetDisplayName", g_variant_new("(s)", ""));
    g_autofree char *model = get_string(f, "DisplayName");
    g_assert_cmpstr(model, ==, "AirPods Pro 2 (USB-C)");
}

static void save_profile(bool listening_modes_set)
{
    DeviceProfile profile;

    config_get_default_profile(&profile);
    profile.listening_modes = (ListeningModesConfig) {
        .off_enabled = true, .transparency_enabled = true,
        .anc_enabled = true, .adaptive_enabled = false,
    };
    profile.listening_modes_set = listening_modes_set;
    profile.conversational_awareness = false;
    profile.adaptive_noise_level = 40;
    g_assert_true(config_save_device_profile(ADDRESS, &profile));
}

/* The long-press modes set in EarPort are sent back: the AirPods don't
 * announce them. Conversation awareness and the adaptive level are not:
 * the AirPods keep what was set on another device and announce it. */
static void test_saved_profile(Fixture *f, gconstpointer data)
{
    (void)data;
    const uint8_t ca_off[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28 };
    const uint8_t level[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E };

    save_profile(true);
    connect_airpods(f);
    ASSERT_SENT(0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1A, 0x07, 0x00, 0x00, 0x00);
    g_assert_true(get_bool(f, "ListeningModeOff"));

    run_for(100);
    g_assert_cmpuint(fake_bt_count_sent(ca_off, sizeof(ca_off)), ==, 0);
    g_assert_cmpuint(fake_bt_count_sent(level, sizeof(level)), ==, 0);
    g_assert_true(get_bool(f, "ConversationalAwareness"));
    g_assert_cmpint(get_int(f, "AdaptiveNoiseLevel"), ==, 50);
}

/* Profile saved for another setting: the defaults are not pushed */
static void test_defaults_not_pushed(Fixture *f, gconstpointer data)
{
    (void)data;
    const uint8_t modes[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1A };

    save_profile(false);
    connect_airpods(f);
    run_for(700);
    g_assert_cmpuint(fake_bt_count_sent(modes, sizeof(modes)), ==, 0);
}

/* Setting the modes here marks them for the next connections */
static void test_listening_modes_remembered(Fixture *f, gconstpointer data)
{
    (void)data;
    DeviceProfile profile;

    connect_airpods(f);
    call_ok(f, "SetListeningModes", g_variant_new("(bbbb)", TRUE, FALSE, TRUE, FALSE));
    g_assert_true(config_load_device_profile(ADDRESS, &profile));
    g_assert_true(profile.listening_modes_set);
    g_assert_true(profile.listening_modes.off_enabled);
}

static void test_conversation_signals(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x01);
    RUN_UNTIL(f->speaking);

    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x08);
    RUN_UNTIL(!f->speaking);
}

/* Lost while speaking: clients get the volume back */
static void test_disconnection(Fixture *f, gconstpointer data)
{
    (void)data;

    connect_airpods(f);
    RECEIVE(0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x01);
    RUN_UNTIL(f->speaking);

    aap_link_device_disconnected(f->link);
    RUN_UNTIL(received(f, "DeviceDisconnected"));
    RUN_UNTIL(!f->speaking);
    g_assert_false(get_bool(f, "Connected"));
    g_assert_null(get_setting(f, "OneBudANC"));
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
    /* Warnings are expected (commands while disconnected) */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

    test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

#define ADD(path, func) \
    g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

    ADD("/dbus/disconnected", test_disconnected);
    ADD("/dbus/connection", test_connection);
    ADD("/dbus/noise-control", test_noise_control);
    ADD("/dbus/noise-control-unknown", test_noise_control_unknown);
    ADD("/dbus/set-setting", test_set_setting);
    ADD("/dbus/set-setting-errors", test_set_setting_errors);
    ADD("/dbus/conversation-awareness", test_conversation_awareness);
    ADD("/dbus/adaptive-level", test_adaptive_level);
    ADD("/dbus/listening-modes", test_listening_modes);
    ADD("/dbus/display-name", test_display_name);
    ADD("/dbus/saved-profile", test_saved_profile);
    ADD("/dbus/defaults-not-pushed", test_defaults_not_pushed);
    ADD("/dbus/listening-modes-remembered", test_listening_modes_remembered);
    ADD("/dbus/conversation-signals", test_conversation_signals);
    ADD("/dbus/disconnection", test_disconnection);

    int result = g_test_run();

    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
    return result;
}
