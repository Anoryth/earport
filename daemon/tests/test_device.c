/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Replay of a session with AirPods Pro 2 USB-C (2026-09-30): the packets
 * the parser handles, in the order they arrived, applied to the state as
 * the daemon does. Addresses, keys and serial number are left out.
 */

#include <glib.h>
#include <string.h>

#include "device.h"

typedef struct {
    AirPodsState state;
    DbusService *dbus;   /* Not on a bus: signals go nowhere */
    Device device;
} Fixture;

static void fixture_setup(Fixture *f, gconstpointer data)
{
    (void)data;
    airpods_state_init(&f->state);
    f->dbus = dbus_service_new(&f->state);
    device_init(&f->device, &f->state, f->dbus, NULL);
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
    (void)data;
    dbus_service_free(f->dbus);
    airpods_state_cleanup(&f->state);
}

static void replay(Fixture *f, const uint8_t *data, size_t len)
{
    AapParsedPacket packet;
    g_assert_cmpint(aap_parse_packet(data, len, &packet), ==, AAP_PARSE_OK);
    device_handle_packet(&f->device, &packet);
}

#define REPLAY(f, ...) do { \
    const uint8_t pkt_[] = { __VA_ARGS__ }; \
    replay((f), pkt_, sizeof(pkt_)); \
} while (0)

static void replay_metadata(Fixture *f)
{
    const uint8_t header[] = { 0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xDD, 0x00, 0x04, 0x00 };
    const char strings[] = "AirPods Pro\0A3048\0Apple Inc.\0SERIAL0000";
    uint8_t pkt[sizeof(header) + sizeof(strings)];

    memcpy(pkt, header, sizeof(header));
    memcpy(pkt + sizeof(header), strings, sizeof(strings));
    replay(f, pkt, sizeof(pkt));
}

/* What the AirPods send right after the handshake */
static void replay_connection(Fixture *f)
{
    device_session_started(&f->device, "00:11:22:33:44:55", "AirPods Pro");

    replay_metadata(f);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00);  /* ANC */
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x01, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00);  /* CA on */
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1B, 0x02, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x35, 0x01, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
              0x04, 0x01, 0x53, 0x02, 0x01,
              0x02, 0x01, 0x53, 0x02, 0x01,
              0x08, 0x01, 0x00, 0x04, 0x01);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00);  /* Both in */
}

static void test_connection(Fixture *f, gconstpointer data)
{
    (void)data;
    uint8_t value;

    replay_connection(f);

    g_assert_true(f->state.connected);
    g_assert_cmpstr(f->state.device_address, ==, "00:11:22:33:44:55");
    g_assert_cmpint(f->state.model, ==, AIRPODS_MODEL_PRO_2_USBC);
    g_assert_cmpint(f->state.noise_control_mode, ==, NOISE_CONTROL_ANC);
    g_assert_true(f->state.conversational_awareness);

    g_assert_cmpint(f->state.battery.left.level, ==, 83);
    g_assert_cmpint(f->state.battery.right.level, ==, 83);
    g_assert_true(f->state.ear_detection.left_in_ear);
    g_assert_true(f->state.ear_detection.right_in_ear);

    /* Settings announced on connection, booleans 1 = on, 2 = off */
    g_assert_true(airpods_state_get_setting(&f->state, 0x1B, &value));
    g_assert_cmpuint(value, ==, 0x02);
    g_assert_true(airpods_state_get_setting(&f->state, 0x35, &value));
    g_assert_cmpuint(value, ==, 0x01);
    g_assert_true(airpods_state_get_setting(&f->state, 0x23, &value));
    g_assert_cmpuint(value, ==, 0x00);
}

static void test_battery_update(Fixture *f, gconstpointer data)
{
    (void)data;

    replay_connection(f);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
              0x04, 0x01, 0x53, 0x02, 0x01,
              0x02, 0x01, 0x52, 0x02, 0x01,
              0x08, 0x01, 0x00, 0x04, 0x01);

    g_assert_cmpint(f->state.battery.left.level, ==, 83);
    g_assert_cmpint(f->state.battery.right.level, ==, 82);
}

static void test_ear_removal(Fixture *f, gconstpointer data)
{
    (void)data;

    replay_connection(f);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x01, 0x00);  /* Primary out */
    g_assert_cmpint(f->state.ear_detection.left_in_ear + f->state.ear_detection.right_in_ear, ==, 1);

    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00);
    g_assert_true(f->state.ear_detection.left_in_ear);
    g_assert_true(f->state.ear_detection.right_in_ear);
}

static void test_conversation(Fixture *f, gconstpointer data)
{
    (void)data;

    replay_connection(f);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x01);
    g_assert_true(f->device.ca_speaking);

    /* Intermediate levels keep the volume as is */
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x03);
    g_assert_true(f->device.ca_speaking);

    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x08);
    g_assert_false(f->device.ca_speaking);
}

static void test_disconnection(Fixture *f, gconstpointer data)
{
    (void)data;
    uint8_t value;

    replay_connection(f);
    REPLAY(f, 0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x01);

    device_session_ended(&f->device);

    g_assert_false(f->state.connected);
    /* Clients get the volume back even if the conversation never ended */
    g_assert_false(f->device.ca_speaking);
    /* Settings of the next AirPods are not known yet */
    g_assert_false(airpods_state_get_setting(&f->state, 0x1B, &value));
}

int main(int argc, char *argv[])
{
    /* The device profile and learned discharge rate go to a temporary
     * configuration directory */
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

#define ADD(path, func) \
    g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

    ADD("/device/connection", test_connection);
    ADD("/device/battery-update", test_battery_update);
    ADD("/device/ear-removal", test_ear_removal);
    ADD("/device/conversation", test_conversation);
    ADD("/device/disconnection", test_disconnection);

    return g_test_run();
}
