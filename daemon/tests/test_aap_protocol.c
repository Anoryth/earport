/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * AAP parser tests, using packets captured from real AirPods
 */

#include <glib.h>
#include <string.h>

#include "aap_protocol.h"
#include "airpods_state.h"

#define PARSE(pkt, result) aap_parse_packet((pkt), sizeof(pkt), (result))

/* ============================================================================
 * Battery
 * ========================================================================== */

static void test_battery_in_ears(void)
{
    /* Both pods in ears, case disconnected (level 0, status 0x04) */
    const uint8_t pkt[] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
        0x02, 0x01, 0x0D, 0x02, 0x01,
        0x04, 0x01, 0x10, 0x02, 0x01,
        0x08, 0x01, 0x00, 0x04, 0x01,
    };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_BATTERY);
    g_assert_cmpint(p.data.battery.left_level, ==, 16);
    g_assert_cmpint(p.data.battery.right_level, ==, 13);
    g_assert_cmpint(p.data.battery.left_status, ==, BATTERY_STATUS_DISCHARGING);
    g_assert_cmpint(p.data.battery.right_status, ==, BATTERY_STATUS_DISCHARGING);
    /* A disconnected case must be unavailable, not 0% */
    g_assert_cmpint(p.data.battery.case_level, ==, -1);
    g_assert_cmpint(p.data.battery.case_status, ==, BATTERY_STATUS_DISCONNECTED);
}

static void test_battery_in_case(void)
{
    /* Pods charging in the case, components in a different order */
    const uint8_t pkt[] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
        0x04, 0x01, 0x10, 0x01, 0x01,
        0x02, 0x01, 0x0D, 0x01, 0x01,
        0x08, 0x01, 0x1F, 0x02, 0x01,
    };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.data.battery.left_level, ==, 16);
    g_assert_cmpint(p.data.battery.right_level, ==, 13);
    g_assert_cmpint(p.data.battery.case_level, ==, 31);
    g_assert_cmpint(p.data.battery.left_status, ==, BATTERY_STATUS_CHARGING);
    g_assert_cmpint(p.data.battery.right_status, ==, BATTERY_STATUS_CHARGING);
    g_assert_cmpint(p.data.battery.case_status, ==, BATTERY_STATUS_DISCHARGING);
}

static void test_battery_unknown_level(void)
{
    /* Case level 0xFF right after a pod is put back in the case */
    const uint8_t pkt[] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
        0x02, 0x01, 0x3D, 0x01, 0x01,
        0x04, 0x01, 0x3F, 0x02, 0x01,
        0x08, 0x01, 0xFF, 0x04, 0x01,
    };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.data.battery.right_level, ==, 61);
    g_assert_cmpint(p.data.battery.left_level, ==, 63);
    g_assert_cmpint(p.data.battery.case_level, ==, -1);
}

static void test_battery_malformed(void)
{
    const uint8_t zero_count[] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const uint8_t truncated[] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
        0x02, 0x01, 0x0D, 0x02, 0x01,
    };
    const uint8_t too_short[] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01,
    };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(zero_count, &p), ==, AAP_PARSE_MALFORMED);
    g_assert_cmpint(PARSE(truncated, &p), ==, AAP_PARSE_INCOMPLETE);
    g_assert_cmpint(PARSE(too_short, &p), ==, AAP_PARSE_INCOMPLETE);
}

/* ============================================================================
 * Ear detection
 * ========================================================================== */

static void test_ear_detection(void)
{
    const uint8_t both_in[] = { 0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00 };
    const uint8_t primary_out[] = { 0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x01, 0x00 };
    const uint8_t both_in_case[] = { 0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x02, 0x02 };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(both_in, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_EAR_DETECTION);
    g_assert_true(p.data.ear_detection.primary_in_ear);
    g_assert_true(p.data.ear_detection.secondary_in_ear);

    g_assert_cmpint(PARSE(primary_out, &p), ==, AAP_PARSE_OK);
    g_assert_false(p.data.ear_detection.primary_in_ear);
    g_assert_true(p.data.ear_detection.secondary_in_ear);

    g_assert_cmpint(PARSE(both_in_case, &p), ==, AAP_PARSE_OK);
    g_assert_false(p.data.ear_detection.primary_in_ear);
    g_assert_false(p.data.ear_detection.secondary_in_ear);
}

/* ============================================================================
 * Control commands
 * ========================================================================== */

static void test_noise_control(void)
{
    const struct {
        uint8_t byte;
        NoiseControlMode mode;
    } cases[] = {
        { 0x01, NOISE_CONTROL_OFF },
        { 0x02, NOISE_CONTROL_ANC },
        { 0x03, NOISE_CONTROL_TRANSPARENCY },
        { 0x04, NOISE_CONTROL_ADAPTIVE },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        uint8_t pkt[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, cases[i].byte, 0x00, 0x00, 0x00 };
        AapParsedPacket p;

        g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
        g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_NOISE_CONTROL);
        g_assert_cmpint(p.data.noise_control, ==, cases[i].mode);
    }
}

static void test_conversational_awareness(void)
{
    const uint8_t enabled[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00 };
    const uint8_t disabled[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x02, 0x00, 0x00, 0x00 };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(enabled, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_CONV_AWARENESS);
    g_assert_true(p.data.conversational_awareness);

    g_assert_cmpint(PARSE(disabled, &p), ==, AAP_PARSE_OK);
    g_assert_false(p.data.conversational_awareness);
}

static void test_listening_modes(void)
{
    /* Transparency + ANC enabled, Off and Adaptive disabled */
    const uint8_t pkt[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1A, 0x06, 0x00, 0x00, 0x00 };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_LISTENING_MODES);
    g_assert_false(p.data.listening_modes.off_enabled);
    g_assert_true(p.data.listening_modes.anc_enabled);
    g_assert_true(p.data.listening_modes.transparency_enabled);
    g_assert_false(p.data.listening_modes.adaptive_enabled);
}

static void test_control_settings(void)
{
    /* Settings announced by AirPods Pro 2 USB-C right after the handshake */
    const struct {
        uint8_t id;
        uint8_t v0;
        uint8_t v1;
    } cases[] = {
        { 0x17, 0x00, 0x00 },  /* DOUBLE_CLICK_INTERVAL */
        { 0x18, 0x00, 0x00 },  /* CLICK_HOLD_INTERVAL */
        { 0x1B, 0x02, 0x00 },  /* ONE_BUD_ANC_MODE */
        { 0x1F, 0x50, 0x50 },  /* CHIME_VOLUME */
        { 0x23, 0x00, 0x00 },  /* VOLUME_SWIPE_INTERVAL */
        { 0x24, 0x00, 0x03 },  /* CALL_MANAGEMENT_CONFIG */
        { 0x25, 0x01, 0x00 },  /* VOLUME_SWIPE_MODE */
        { 0x26, 0x01, 0x00 },  /* ADAPTIVE_VOLUME_CONFIG */
        { 0x35, 0x01, 0x00 },  /* SLEEP_DETECTION_CONFIG */
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        uint8_t pkt[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, cases[i].id,
                          cases[i].v0, cases[i].v1, 0x00, 0x00 };
        AapParsedPacket p;

        g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
        g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_CONTROL_SETTING);
        g_assert_cmpuint(p.data.control_setting.id, ==, cases[i].id);
        g_assert_cmpuint(p.data.control_setting.value[0], ==, cases[i].v0);
        g_assert_cmpuint(p.data.control_setting.value[1], ==, cases[i].v1);
    }
}

static void test_control_setting_short(void)
{
    /* Value bytes missing from a short packet read as zero */
    const uint8_t pkt[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x02 };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_CONTROL_SETTING);
    g_assert_cmpuint(p.data.control_setting.value[0], ==, 0x02);
    g_assert_cmpuint(p.data.control_setting.value[1], ==, 0x00);
    g_assert_cmpuint(p.data.control_setting.value[3], ==, 0x00);
}

/* ============================================================================
 * Metadata
 * ========================================================================== */

static void test_metadata(void)
{
    const uint8_t header[] = { 0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xDD, 0x00, 0x04, 0x00 };
    const char strings[] = "CrazyMole\0A3047\0Apple Inc.\0H2FD9M6M95";
    uint8_t pkt[sizeof(header) + sizeof(strings)];
    AapParsedPacket p;

    memcpy(pkt, header, sizeof(header));
    memcpy(pkt + sizeof(header), strings, sizeof(strings));

    g_assert_cmpint(PARSE(pkt, &p), ==, AAP_PARSE_OK);
    g_assert_cmpint(p.type, ==, AAP_PKT_TYPE_METADATA);
    g_assert_cmpstr(p.data.metadata.device_name, ==, "CrazyMole");
    g_assert_cmpstr(p.data.metadata.model_number, ==, "A3047");
    g_assert_cmpstr(p.data.metadata.manufacturer, ==, "Apple Inc.");
}

/* ============================================================================
 * Non-AAP and unhandled packets
 * ========================================================================== */

static void test_unhandled_packets(void)
{
    /* Handshake acknowledgement uses a different header */
    const uint8_t handshake_ack[] = {
        0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    /* SET_FEATURES acknowledgement (0x2B) is not parsed */
    const uint8_t features_ack[] = { 0x04, 0x00, 0x04, 0x00, 0x2B, 0x00, 0x01, 0x22, 0x00 };
    AapParsedPacket p;

    g_assert_cmpint(PARSE(handshake_ack, &p), ==, AAP_PARSE_INVALID_HEADER);
    g_assert_cmpint(PARSE(features_ack, &p), ==, AAP_PARSE_UNKNOWN_OPCODE);
    g_assert_cmpint(aap_parse_packet(features_ack, 3, &p), ==, AAP_PARSE_INVALID_HEADER);
}

/* ============================================================================
 * Command builders
 * ========================================================================== */

static void test_build_commands(void)
{
    uint8_t buf[AAP_CONTROL_CMD_SIZE];
    const uint8_t anc[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00 };
    const uint8_t modes[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1A, 0x06, 0x00, 0x00, 0x00 };

    aap_build_noise_control_cmd(NOISE_CONTROL_ANC, buf);
    g_assert_cmpmem(buf, sizeof(buf), anc, sizeof(anc));

    aap_build_listening_modes_cmd(AAP_LISTENING_MODE_ANC | AAP_LISTENING_MODE_TRANSPARENCY, buf);
    g_assert_cmpmem(buf, sizeof(buf), modes, sizeof(modes));

    aap_build_adaptive_level_cmd(50, buf);
    g_assert_cmpuint(buf[6], ==, AAP_CTRL_ADAPTIVE_LEVEL);
    g_assert_cmpuint(buf[7], ==, 50);

    /* Out-of-range levels are clamped */
    aap_build_adaptive_level_cmd(150, buf);
    g_assert_cmpuint(buf[7], ==, 100);
    aap_build_adaptive_level_cmd(-5, buf);
    g_assert_cmpuint(buf[7], ==, 0);
}

static void test_build_control_cmd(void)
{
    uint8_t buf[AAP_CONTROL_CMD_SIZE];
    const uint8_t one_byte[] = { 0x02 };
    const uint8_t two_bytes[] = { 0x00, 0x03 };
    const uint8_t pv_off[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x02, 0x00, 0x00, 0x00 };
    const uint8_t calls[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x24, 0x00, 0x03, 0x00, 0x00 };

    aap_build_control_cmd(0x26, one_byte, sizeof(one_byte), buf);
    g_assert_cmpmem(buf, sizeof(buf), pv_off, sizeof(pv_off));

    aap_build_control_cmd(0x24, two_bytes, sizeof(two_bytes), buf);
    g_assert_cmpmem(buf, sizeof(buf), calls, sizeof(calls));
}

/* ============================================================================
 * Model detection
 * ========================================================================== */

static void test_model_from_number(void)
{
    g_assert_cmpint(airpods_model_from_number("A3047"), ==, AIRPODS_MODEL_PRO_2_USBC);
    g_assert_cmpint(airpods_model_from_number("A3048"), ==, AIRPODS_MODEL_PRO_2_USBC);
    g_assert_cmpint(airpods_model_from_number("A3531"), ==, AIRPODS_MODEL_5);
    g_assert_cmpint(airpods_model_from_number("A3439"), ==, AIRPODS_MODEL_5);
    g_assert_cmpint(airpods_model_from_number("A3454"), ==, AIRPODS_MODEL_MAX_2);
    g_assert_cmpint(airpods_model_from_number("A9999"), ==, AIRPODS_MODEL_UNKNOWN);
    g_assert_cmpint(airpods_model_from_number(""), ==, AIRPODS_MODEL_UNKNOWN);
    g_assert_cmpint(airpods_model_from_number(NULL), ==, AIRPODS_MODEL_UNKNOWN);

    g_assert_true(airpods_model_is_headphones(AIRPODS_MODEL_MAX_2));
    g_assert_false(airpods_model_is_headphones(AIRPODS_MODEL_5));
    g_assert_true(airpods_model_supports_adaptive(AIRPODS_MODEL_5));
    g_assert_false(airpods_model_supports_adaptive(AIRPODS_MODEL_MAX_2));
    g_assert_false(airpods_model_supports_anc(AIRPODS_MODEL_4));
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/aap/battery/in-ears", test_battery_in_ears);
    g_test_add_func("/aap/battery/in-case", test_battery_in_case);
    g_test_add_func("/aap/battery/unknown-level", test_battery_unknown_level);
    g_test_add_func("/aap/battery/malformed", test_battery_malformed);
    g_test_add_func("/aap/ear-detection", test_ear_detection);
    g_test_add_func("/aap/control/noise-control", test_noise_control);
    g_test_add_func("/aap/control/conversational-awareness", test_conversational_awareness);
    g_test_add_func("/aap/control/listening-modes", test_listening_modes);
    g_test_add_func("/aap/control/settings", test_control_settings);
    g_test_add_func("/aap/control/setting-short", test_control_setting_short);
    g_test_add_func("/aap/metadata", test_metadata);
    g_test_add_func("/aap/unhandled", test_unhandled_packets);
    g_test_add_func("/aap/build-commands", test_build_commands);
    g_test_add_func("/aap/build-control-cmd", test_build_control_cmd);
    g_test_add_func("/model/from-number", test_model_from_number);

    return g_test_run();
}
