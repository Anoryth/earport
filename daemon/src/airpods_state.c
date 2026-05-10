/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 */

#include "airpods_state.h"
#include <string.h>

void airpods_state_init(AirPodsState *state)
{
    memset(state, 0, sizeof(AirPodsState));
    g_mutex_init(&state->lock);

    state->connected = false;
    state->device_name = NULL;
    state->device_address = NULL;
    state->display_name = NULL;
    state->model = AIRPODS_MODEL_UNKNOWN;

    state->battery.left.level = -1;
    state->battery.left.available = false;
    state->battery.right.level = -1;
    state->battery.right.available = false;
    state->battery.case_battery.level = -1;
    state->battery.case_battery.available = false;

    state->noise_control_mode = NOISE_CONTROL_OFF;
    state->conversational_awareness = false;
    state->adaptive_noise_level = 50;
    state->one_bud_anc_enabled = false;

    /* Default: Transparency and ANC enabled for long press */
    state->listening_modes.off_enabled = false;
    state->listening_modes.transparency_enabled = true;
    state->listening_modes.anc_enabled = true;
    state->listening_modes.adaptive_enabled = true;

    state->ear_detection.left_in_ear = false;
    state->ear_detection.right_in_ear = false;
    state->ear_detection.primary_left = true;
}

void airpods_state_cleanup(AirPodsState *state)
{
    g_mutex_lock(&state->lock);
    g_free(state->device_name);
    g_free(state->device_address);
    g_free(state->display_name);
    state->device_name = NULL;
    state->device_address = NULL;
    state->display_name = NULL;
    g_mutex_unlock(&state->lock);
    g_mutex_clear(&state->lock);
}

void airpods_state_reset(AirPodsState *state)
{
    g_mutex_lock(&state->lock);

    state->connected = false;
    g_free(state->device_name);
    g_free(state->device_address);
    g_free(state->display_name);
    state->device_name = NULL;
    state->device_address = NULL;
    state->display_name = NULL;
    state->model = AIRPODS_MODEL_UNKNOWN;

    state->battery.left.level = -1;
    state->battery.left.status = BATTERY_STATUS_UNKNOWN;
    state->battery.left.available = false;
    state->battery.right.level = -1;
    state->battery.right.status = BATTERY_STATUS_UNKNOWN;
    state->battery.right.available = false;
    state->battery.case_battery.level = -1;
    state->battery.case_battery.status = BATTERY_STATUS_UNKNOWN;
    state->battery.case_battery.available = false;

    state->noise_control_mode = NOISE_CONTROL_OFF;
    state->conversational_awareness = false;
    state->adaptive_noise_level = 50;

    state->ear_detection.left_in_ear = false;
    state->ear_detection.right_in_ear = false;

    g_mutex_unlock(&state->lock);
}

void airpods_state_set_device(AirPodsState *state,
                               const char *name,
                               const char *address,
                               AirPodsModel model)
{
    g_mutex_lock(&state->lock);
    g_free(state->device_name);
    g_free(state->device_address);
    state->device_name = g_strdup(name);
    state->device_address = g_strdup(address);
    state->model = model;
    state->connected = true;
    g_mutex_unlock(&state->lock);
}

void airpods_state_set_display_name(AirPodsState *state, const char *display_name)
{
    g_mutex_lock(&state->lock);
    g_free(state->display_name);
    /* Empty string means no custom name (use model) */
    if (display_name && display_name[0] != '\0') {
        state->display_name = g_strdup(display_name);
    } else {
        state->display_name = NULL;
    }
    g_mutex_unlock(&state->lock);
}

const char *airpods_state_get_display_name(AirPodsState *state)
{
    /* Note: caller must hold lock or accept potential race */
    if (state->display_name && state->display_name[0] != '\0') {
        return state->display_name;
    }
    /* Fall back to BlueZ device name while the model is not yet detected
     * (metadata packet arrives shortly after connection). */
    if (state->model == AIRPODS_MODEL_UNKNOWN &&
        state->device_name && state->device_name[0] != '\0') {
        return state->device_name;
    }
    return airpods_model_to_string(state->model);
}

void airpods_state_set_battery(AirPodsState *state,
                                int8_t left, BatteryStatus left_status,
                                int8_t right, BatteryStatus right_status,
                                int8_t case_level, BatteryStatus case_status)
{
    g_mutex_lock(&state->lock);

    state->battery.left.level = left;
    state->battery.left.status = left_status;
    state->battery.left.available = (left >= 0);

    state->battery.right.level = right;
    state->battery.right.status = right_status;
    state->battery.right.available = (right >= 0);

    state->battery.case_battery.level = case_level;
    state->battery.case_battery.status = case_status;
    state->battery.case_battery.available = (case_level >= 0);

    g_mutex_unlock(&state->lock);
}

void airpods_state_set_noise_control(AirPodsState *state, NoiseControlMode mode)
{
    g_mutex_lock(&state->lock);
    state->noise_control_mode = mode;
    g_mutex_unlock(&state->lock);
}

void airpods_state_set_ear_detection(AirPodsState *state,
                                      bool left_in_ear,
                                      bool right_in_ear,
                                      bool primary_left)
{
    g_mutex_lock(&state->lock);
    state->ear_detection.left_in_ear = left_in_ear;
    state->ear_detection.right_in_ear = right_in_ear;
    state->ear_detection.primary_left = primary_left;
    g_mutex_unlock(&state->lock);
}

void airpods_state_set_conversational_awareness(AirPodsState *state, bool enabled)
{
    g_mutex_lock(&state->lock);
    state->conversational_awareness = enabled;
    g_mutex_unlock(&state->lock);
}

void airpods_state_set_adaptive_noise_level(AirPodsState *state, int level)
{
    g_mutex_lock(&state->lock);
    state->adaptive_noise_level = CLAMP(level, 0, 100);
    g_mutex_unlock(&state->lock);
}

void airpods_state_set_listening_modes(AirPodsState *state,
                                        bool off_enabled,
                                        bool transparency_enabled,
                                        bool anc_enabled,
                                        bool adaptive_enabled)
{
    g_mutex_lock(&state->lock);
    state->listening_modes.off_enabled = off_enabled;
    state->listening_modes.transparency_enabled = transparency_enabled;
    state->listening_modes.anc_enabled = anc_enabled;
    state->listening_modes.adaptive_enabled = adaptive_enabled;
    g_mutex_unlock(&state->lock);
}

const char *airpods_model_to_string(AirPodsModel model)
{
    switch (model) {
    case AIRPODS_MODEL_1:
        return "AirPods 1st Gen";
    case AIRPODS_MODEL_2:
        return "AirPods 2nd Gen";
    case AIRPODS_MODEL_3:
        return "AirPods 3rd Gen";
    case AIRPODS_MODEL_4:
        return "AirPods 4th Gen";
    case AIRPODS_MODEL_4_ANC:
        return "AirPods 4th Gen (ANC)";
    case AIRPODS_MODEL_PRO:
        return "AirPods Pro";
    case AIRPODS_MODEL_PRO_2:
        return "AirPods Pro 2";
    case AIRPODS_MODEL_PRO_2_USBC:
        return "AirPods Pro 2 (USB-C)";
    case AIRPODS_MODEL_PRO_3:
        return "AirPods Pro 3";
    case AIRPODS_MODEL_MAX:
        return "AirPods Max";
    case AIRPODS_MODEL_MAX_USBC:
        return "AirPods Max (USB-C)";
    default:
        return "Unknown AirPods";
    }
}

const char *noise_control_mode_to_string(NoiseControlMode mode)
{
    switch (mode) {
    case NOISE_CONTROL_OFF:
        return "off";
    case NOISE_CONTROL_ANC:
        return "anc";
    case NOISE_CONTROL_TRANSPARENCY:
        return "transparency";
    case NOISE_CONTROL_ADAPTIVE:
        return "adaptive";
    default:
        return "off";
    }
}

NoiseControlMode noise_control_mode_from_string(const char *str)
{
    if (str == NULL)
        return NOISE_CONTROL_OFF;

    if (g_ascii_strcasecmp(str, "anc") == 0 ||
        g_ascii_strcasecmp(str, "noise_cancellation") == 0 ||
        g_ascii_strcasecmp(str, "cancellation") == 0)
        return NOISE_CONTROL_ANC;

    if (g_ascii_strcasecmp(str, "transparency") == 0 ||
        g_ascii_strcasecmp(str, "transparent") == 0)
        return NOISE_CONTROL_TRANSPARENCY;

    if (g_ascii_strcasecmp(str, "adaptive") == 0)
        return NOISE_CONTROL_ADAPTIVE;

    return NOISE_CONTROL_OFF;
}

bool airpods_model_supports_anc(AirPodsModel model)
{
    switch (model) {
    case AIRPODS_MODEL_PRO:
    case AIRPODS_MODEL_PRO_2:
    case AIRPODS_MODEL_PRO_2_USBC:
    case AIRPODS_MODEL_PRO_3:
    case AIRPODS_MODEL_MAX:
    case AIRPODS_MODEL_MAX_USBC:
    case AIRPODS_MODEL_4_ANC:
        return true;
    default:
        return false;
    }
}

bool airpods_model_supports_adaptive(AirPodsModel model)
{
    switch (model) {
    case AIRPODS_MODEL_PRO_2:
    case AIRPODS_MODEL_PRO_2_USBC:
    case AIRPODS_MODEL_PRO_3:
    case AIRPODS_MODEL_4_ANC:
        return true;
    default:
        return false;
    }
}

bool airpods_model_is_headphones(AirPodsModel model)
{
    switch (model) {
    case AIRPODS_MODEL_MAX:
    case AIRPODS_MODEL_MAX_USBC:
        return true;
    default:
        return false;
    }
}

AirPodsModel airpods_model_from_number(const char *model_number)
{
    if (model_number == NULL || model_number[0] == '\0')
        return AIRPODS_MODEL_UNKNOWN;

    /* Model numbers from https://support.apple.com/en-us/109525 */
    static const struct {
        const char *number;
        AirPodsModel model;
    } model_map[] = {
        /* AirPods 1st Gen */
        {"A1523", AIRPODS_MODEL_1},
        {"A1722", AIRPODS_MODEL_1},
        /* AirPods 2nd Gen */
        {"A2032", AIRPODS_MODEL_2},
        {"A2031", AIRPODS_MODEL_2},
        /* AirPods 3rd Gen */
        {"A2565", AIRPODS_MODEL_3},
        {"A2564", AIRPODS_MODEL_3},
        /* AirPods 4th Gen */
        {"A3053", AIRPODS_MODEL_4},
        {"A3050", AIRPODS_MODEL_4},
        {"A3054", AIRPODS_MODEL_4},
        /* AirPods 4th Gen (ANC) */
        {"A3056", AIRPODS_MODEL_4_ANC},
        {"A3055", AIRPODS_MODEL_4_ANC},
        {"A3057", AIRPODS_MODEL_4_ANC},
        /* AirPods Pro */
        {"A2084", AIRPODS_MODEL_PRO},
        {"A2083", AIRPODS_MODEL_PRO},
        /* AirPods Pro 2 (Lightning) */
        {"A2931", AIRPODS_MODEL_PRO_2},
        {"A2699", AIRPODS_MODEL_PRO_2},
        {"A2698", AIRPODS_MODEL_PRO_2},
        /* AirPods Pro 2 (USB-C) */
        {"A3047", AIRPODS_MODEL_PRO_2_USBC},
        {"A3048", AIRPODS_MODEL_PRO_2_USBC},
        {"A3049", AIRPODS_MODEL_PRO_2_USBC},
        /* AirPods Pro 3 */
        {"A3064", AIRPODS_MODEL_PRO_3},
        {"A3065", AIRPODS_MODEL_PRO_3},
        {"A3063", AIRPODS_MODEL_PRO_3},
        /* AirPods Max (Lightning) */
        {"A2096", AIRPODS_MODEL_MAX},
        /* AirPods Max (USB-C) */
        {"A3184", AIRPODS_MODEL_MAX_USBC},
        {NULL, AIRPODS_MODEL_UNKNOWN}
    };

    for (int i = 0; model_map[i].number != NULL; i++) {
        if (g_strcmp0(model_number, model_map[i].number) == 0) {
            return model_map[i].model;
        }
    }

    return AIRPODS_MODEL_UNKNOWN;
}
