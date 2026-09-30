/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "research.h"
#include "airpods_settings.h"

#include <glib.h>

/* AAP packets start with a 4-byte header, then the opcode */
#define AAP_HEADER_SIZE 4

static bool enabled;

void research_set_enabled(bool value)
{
    enabled = value;
    if (enabled)
        g_message("Research log enabled");
}

static void log_packet(const char *what, const uint8_t *data, size_t len)
{
    GString *hex = g_string_new(NULL);
    for (size_t i = AAP_HEADER_SIZE; i < len; i++)
        g_string_append_printf(hex, "%s%02X", i > AAP_HEADER_SIZE ? " " : "", data[i]);
    g_message("Research: %s: %s", what, hex->str);
    g_string_free(hex, TRUE);
}

void research_observe(const uint8_t *data, size_t len, AapParseResult result,
                      const AapParsedPacket *packet)
{
    if (!enabled || !aap_has_valid_header(data, len) || len <= AAP_HEADER_SIZE)
        return;

    if (result == AAP_PARSE_UNKNOWN_OPCODE) {
        log_packet("unknown packet", data, len);
    } else if (result == AAP_PARSE_OK && packet->type == AAP_PKT_TYPE_CONTROL_SETTING &&
               airpods_setting_by_id(packet->data.control_setting.id) == NULL) {
        log_packet("unknown setting", data, len);
    }
}
