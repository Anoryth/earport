/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "aap_protocol.h"
#include <glib.h>
#include <string.h>

/* Pre-built packets */
const uint8_t AAP_PKT_HANDSHAKE[AAP_HANDSHAKE_SIZE] = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_REQUEST_NOTIFICATIONS[AAP_REQUEST_NOTIF_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF
};

const uint8_t AAP_PKT_SET_FEATURES[AAP_SET_FEATURES_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_OFF[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_ANC[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_TRANSPARENCY[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x03, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_ADAPTIVE[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x04, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_CA_ENABLE[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_CA_DISABLE[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x02, 0x00, 0x00, 0x00
};

bool aap_has_valid_header(const uint8_t *data, size_t len)
{
    if (data == NULL || len < AAP_HEADER_SIZE)
        return false;

    return (data[0] == AAP_HEADER_BYTE0 &&
            data[1] == AAP_HEADER_BYTE1 &&
            data[2] == AAP_HEADER_BYTE2 &&
            data[3] == AAP_HEADER_BYTE3);
}

uint8_t aap_get_opcode(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 5)
        return 0;
    return data[4];
}

AapParseResult aap_parse_battery(const uint8_t *data, size_t len, AapBatteryData *battery)
{
    if (len < AAP_MIN_BATTERY_SIZE)
        return AAP_PARSE_INCOMPLETE;

    /* Header: 04 00 04 00 04 00 [count] ... */
    if (data[4] != AAP_OPCODE_BATTERY || data[5] != 0x00)
        return AAP_PARSE_MALFORMED;

    uint8_t count = data[6];
    if (count == 0 || count > 3)
        return AAP_PARSE_MALFORMED;

    /* Verify we have enough data: header (7) + count * 5 bytes per component */
    size_t expected_len = 7 + (count * 5);
    if (len < expected_len)
        return AAP_PARSE_INCOMPLETE;

    /* Initialize to unavailable */
    battery->left_level = -1;
    battery->right_level = -1;
    battery->case_level = -1;
    battery->left_status = BATTERY_STATUS_UNKNOWN;
    battery->right_status = BATTERY_STATUS_UNKNOWN;
    battery->case_status = BATTERY_STATUS_UNKNOWN;

    /* Parse each component (5 bytes: component, spacer, level, status, end_marker) */
    for (uint8_t i = 0; i < count; i++) {
        size_t offset = 7 + (i * 5);
        uint8_t component = data[offset];
        uint8_t level = data[offset + 2];
        uint8_t status = data[offset + 3];

        BatteryStatus bat_status;
        switch (status) {
        case 0x01:
            bat_status = BATTERY_STATUS_CHARGING;
            break;
        case 0x02:
            bat_status = BATTERY_STATUS_DISCHARGING;
            break;
        case 0x04:
            bat_status = BATTERY_STATUS_DISCONNECTED;
            break;
        default:
            bat_status = BATTERY_STATUS_UNKNOWN;
            break;
        }

        /* A disconnected component (e.g. the case once both pods are out)
         * reports level 0: treat it as unavailable rather than empty. */
        if (bat_status == BATTERY_STATUS_DISCONNECTED)
            level = 0xFF;

        switch (component) {
        case AAP_BATTERY_SINGLE:
            /* AirPods Max: single battery, store in left_level */
            battery->left_level = (level <= 100) ? (int8_t)level : -1;
            battery->left_status = bat_status;
            break;
        case AAP_BATTERY_LEFT:
            battery->left_level = (level <= 100) ? (int8_t)level : -1;
            battery->left_status = bat_status;
            break;
        case AAP_BATTERY_RIGHT:
            battery->right_level = (level <= 100) ? (int8_t)level : -1;
            battery->right_status = bat_status;
            break;
        case AAP_BATTERY_CASE:
            battery->case_level = (level <= 100) ? (int8_t)level : -1;
            battery->case_status = bat_status;
            break;
        }
    }

    return AAP_PARSE_OK;
}

AapParseResult aap_parse_ear_detection(const uint8_t *data, size_t len, AapEarDetectionData *ear)
{
    /* Packet: 04 00 04 00 06 00 [primary] [secondary] */
    if (len < 8)
        return AAP_PARSE_INCOMPLETE;

    if (data[4] != AAP_OPCODE_EAR_DETECTION || data[5] != 0x00)
        return AAP_PARSE_MALFORMED;

    uint8_t primary_status = data[6];
    uint8_t secondary_status = data[7];

    ear->primary_in_ear = (primary_status == AAP_EAR_IN_EAR);
    ear->secondary_in_ear = (secondary_status == AAP_EAR_IN_EAR);
    ear->primary_left = true;  /* Default, may need to track from battery order */

    return AAP_PARSE_OK;
}

AapParseResult aap_parse_noise_control(const uint8_t *data, size_t len, NoiseControlMode *mode)
{
    /* Control response: 04 00 04 00 09 00 0D [mode] ... */
    if (len < 8)
        return AAP_PARSE_INCOMPLETE;

    if (data[4] != AAP_OPCODE_CONTROL || data[6] != AAP_CTRL_NOISE_CONTROL)
        return AAP_PARSE_MALFORMED;

    uint8_t mode_byte = data[7];
    switch (mode_byte) {
    case 0x01:
        *mode = NOISE_CONTROL_OFF;
        break;
    case 0x02:
        *mode = NOISE_CONTROL_ANC;
        break;
    case 0x03:
        *mode = NOISE_CONTROL_TRANSPARENCY;
        break;
    case 0x04:
        *mode = NOISE_CONTROL_ADAPTIVE;
        break;
    default:
        *mode = NOISE_CONTROL_OFF;
        break;
    }

    return AAP_PARSE_OK;
}

static AapParseResult aap_parse_metadata(const uint8_t *data, size_t len, AapMetadata *metadata)
{
    /* Metadata packet: 04 00 04 00 1D 00 [5 bytes] [device_name\0] [model_number\0] [manufacturer\0] */
    if (len < 11)
        return AAP_PARSE_INCOMPLETE;

    memset(metadata, 0, sizeof(AapMetadata));

    /* Skip header (4) + opcode (1) + 00 (1) + 5 unknown bytes = position 11 */
    size_t pos = 11;

    /* Extract null-terminated strings */
    size_t i;

    /* Device name */
    i = 0;
    while (pos < len && data[pos] != '\0' && i < sizeof(metadata->device_name) - 1) {
        metadata->device_name[i++] = data[pos++];
    }
    metadata->device_name[i] = '\0';
    if (pos < len && data[pos] == '\0') pos++;  /* Skip null terminator */

    /* Model number */
    i = 0;
    while (pos < len && data[pos] != '\0' && i < sizeof(metadata->model_number) - 1) {
        metadata->model_number[i++] = data[pos++];
    }
    metadata->model_number[i] = '\0';
    if (pos < len && data[pos] == '\0') pos++;

    /* Manufacturer */
    i = 0;
    while (pos < len && data[pos] != '\0' && i < sizeof(metadata->manufacturer) - 1) {
        metadata->manufacturer[i++] = data[pos++];
    }
    metadata->manufacturer[i] = '\0';

    return AAP_PARSE_OK;
}

static AapParseResult parse_control_packet(const uint8_t *data, size_t len, AapParsedPacket *result)
{
    if (len < 8)
        return AAP_PARSE_INCOMPLETE;

    uint8_t ctrl_id = data[6];

    switch (ctrl_id) {
    case AAP_CTRL_NOISE_CONTROL:
        result->type = AAP_PKT_TYPE_NOISE_CONTROL;
        return aap_parse_noise_control(data, len, &result->data.noise_control);

    case AAP_CTRL_CONV_AWARENESS:
        result->type = AAP_PKT_TYPE_CONV_AWARENESS;
        result->data.conversational_awareness = (data[7] == 0x01);
        return AAP_PARSE_OK;

    case AAP_CTRL_LISTENING_MODES:
        result->type = AAP_PKT_TYPE_LISTENING_MODES;
        {
            uint8_t modes = data[7];
            result->data.listening_modes.raw_value = modes;
            result->data.listening_modes.off_enabled = (modes & AAP_LISTENING_MODE_OFF) != 0;
            result->data.listening_modes.transparency_enabled = (modes & AAP_LISTENING_MODE_TRANSPARENCY) != 0;
            result->data.listening_modes.anc_enabled = (modes & AAP_LISTENING_MODE_ANC) != 0;
            result->data.listening_modes.adaptive_enabled = (modes & AAP_LISTENING_MODE_ADAPTIVE) != 0;
        }
        return AAP_PARSE_OK;

    default:
        /* Other settings: keep the raw value, missing bytes read as zero */
        result->type = AAP_PKT_TYPE_CONTROL_SETTING;
        result->data.control_setting.id = ctrl_id;
        memset(result->data.control_setting.value, 0, sizeof(result->data.control_setting.value));
        for (size_t i = 0; i < 4 && 7 + i < len; i++)
            result->data.control_setting.value[i] = data[7 + i];
        return AAP_PARSE_OK;
    }
}

AapParseResult aap_parse_packet(const uint8_t *data, size_t len, AapParsedPacket *result)
{
    if (!aap_has_valid_header(data, len))
        return AAP_PARSE_INVALID_HEADER;

    if (len < 5)
        return AAP_PARSE_INCOMPLETE;

    uint8_t opcode = data[4];
    result->type = AAP_PKT_TYPE_UNKNOWN;

    switch (opcode) {
    case AAP_OPCODE_BATTERY:
        result->type = AAP_PKT_TYPE_BATTERY;
        return aap_parse_battery(data, len, &result->data.battery);

    case AAP_OPCODE_EAR_DETECTION:
        result->type = AAP_PKT_TYPE_EAR_DETECTION;
        return aap_parse_ear_detection(data, len, &result->data.ear_detection);

    case AAP_OPCODE_CONTROL:
        return parse_control_packet(data, len, result);

    case AAP_OPCODE_CA_DETECTION:
        result->type = AAP_PKT_TYPE_CA_DETECTION;
        /* Packet: 04 00 04 00 4B 00 02 00 01 [level] */
        if (len < 10)
            return AAP_PARSE_INCOMPLETE;
        result->data.ca_volume_level = data[9];
        return AAP_PARSE_OK;

    case AAP_OPCODE_METADATA:
        result->type = AAP_PKT_TYPE_METADATA;
        return aap_parse_metadata(data, len, &result->data.metadata);

    default:
        return AAP_PARSE_UNKNOWN_OPCODE;
    }
}

void aap_build_noise_control_cmd(NoiseControlMode mode, uint8_t *buffer)
{
    const uint8_t *src;
    switch (mode) {
    case NOISE_CONTROL_ANC:
        src = AAP_PKT_NC_ANC;
        break;
    case NOISE_CONTROL_TRANSPARENCY:
        src = AAP_PKT_NC_TRANSPARENCY;
        break;
    case NOISE_CONTROL_ADAPTIVE:
        src = AAP_PKT_NC_ADAPTIVE;
        break;
    default:
        src = AAP_PKT_NC_OFF;
        break;
    }
    memcpy(buffer, src, AAP_CONTROL_CMD_SIZE);
}

int aap_ca_speaking_from_level(uint8_t level)
{
    /* Seen while speaking: 01 02, then 03 0B 04 08 09 once quiet.
     * Same interpretation as LibrePods Android. */
    switch (level) {
    case 0x01:
    case 0x02:
        return 1;
    case 0x06:
    case 0x08:
    case 0x09:
        return 0;
    default:
        return -1;
    }
}

void aap_build_control_cmd(uint8_t id, const uint8_t *value, size_t value_len, uint8_t *buffer)
{
    /* 04 00 04 00 09 00 [id] [v0 v1 v2 v3] */
    memset(buffer, 0, AAP_CONTROL_CMD_SIZE);
    buffer[0] = AAP_HEADER_BYTE0;
    buffer[1] = AAP_HEADER_BYTE1;
    buffer[2] = AAP_HEADER_BYTE2;
    buffer[3] = AAP_HEADER_BYTE3;
    buffer[4] = AAP_OPCODE_CONTROL;
    buffer[6] = id;
    if (value_len > 4)
        value_len = 4;
    if (value_len > 0)
        memcpy(buffer + 7, value, value_len);
}

void aap_build_adaptive_level_cmd(int level, uint8_t *buffer)
{
    uint8_t value = (uint8_t)(level < 0 ? 0 : (level > 100 ? 100 : level));
    aap_build_control_cmd(AAP_CTRL_ADAPTIVE_LEVEL, &value, 1, buffer);
}

void aap_build_conv_awareness_cmd(bool enable, uint8_t *buffer)
{
    memcpy(buffer, enable ? AAP_PKT_CA_ENABLE : AAP_PKT_CA_DISABLE, AAP_CONTROL_CMD_SIZE);
}

void aap_build_listening_modes_cmd(uint8_t modes, uint8_t *buffer)
{
    aap_build_control_cmd(AAP_CTRL_LISTENING_MODES, &modes, 1, buffer);
}

void aap_debug_print_packet(const char *prefix, const uint8_t *data, size_t len)
{
    /* Raw packet dumps are verbose: only shown with G_MESSAGES_DEBUG set */
    GString *hex = g_string_sized_new(3 * 64 + 32);
    for (size_t i = 0; i < len && i < 64; i++) {
        g_string_append_printf(hex, "%02X ", data[i]);
    }
    if (len > 64) {
        g_string_append_printf(hex, "... (%zu more bytes)", len - 64);
    }
    g_debug("%s: %s", prefix, hex->str);
    g_string_free(hex, TRUE);
}
