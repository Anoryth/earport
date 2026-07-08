/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#ifndef AIRPODS_STATE_H
#define AIRPODS_STATE_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

/* AirPods model identifiers (from BLE advertisement) */
typedef enum {
    AIRPODS_MODEL_UNKNOWN = 0,
    AIRPODS_MODEL_1 = 0x0220,
    AIRPODS_MODEL_2 = 0x0F20,
    AIRPODS_MODEL_3 = 0x1320,
    AIRPODS_MODEL_4 = 0x1920,
    AIRPODS_MODEL_4_ANC = 0x1B20,
    AIRPODS_MODEL_PRO = 0x0E20,
    AIRPODS_MODEL_PRO_2 = 0x1420,
    AIRPODS_MODEL_PRO_2_USBC = 0x2420,
    AIRPODS_MODEL_PRO_3 = 0x2027,
    AIRPODS_MODEL_MAX = 0x0A20,
    AIRPODS_MODEL_MAX_USBC = 0x1F20,
} AirPodsModel;

/* Noise control modes */
typedef enum {
    NOISE_CONTROL_OFF = 1,
    NOISE_CONTROL_ANC = 2,
    NOISE_CONTROL_TRANSPARENCY = 3,
    NOISE_CONTROL_ADAPTIVE = 4,
} NoiseControlMode;

/* Battery charging status */
typedef enum {
    BATTERY_STATUS_UNKNOWN = 0,
    BATTERY_STATUS_CHARGING = 1,
    BATTERY_STATUS_DISCHARGING = 2,
    BATTERY_STATUS_DISCONNECTED = 4,
} BatteryStatus;

/* Battery information for a single component */
typedef struct {
    int8_t level;           /* 0-100, -1 if unavailable */
    BatteryStatus status;
    bool available;
} BatteryInfo;

/* Complete battery state */
typedef struct {
    BatteryInfo left;
    BatteryInfo right;
    BatteryInfo case_battery;
} BatteryState;

/* Ear detection state */
typedef struct {
    bool left_in_ear;
    bool right_in_ear;
    bool primary_left;      /* Which pod is primary (for mic) */
} EarDetectionState;

/* Long-press listening modes configuration */
typedef struct {
    bool off_enabled;           /* Off mode available on long press */
    bool transparency_enabled;  /* Transparency mode available */
    bool anc_enabled;           /* ANC mode available */
    bool adaptive_enabled;      /* Adaptive mode available */
} ListeningModesConfig;

/* Complete AirPods state */
typedef struct {
    /* Connection info */
    bool connected;
    char *device_name;
    char *device_address;
    char *display_name;     /* Custom display name (NULL = use model name) */
    AirPodsModel model;

    /* Battery */
    BatteryState battery;

    /* Features */
    NoiseControlMode noise_control_mode;
    bool conversational_awareness;
    int adaptive_noise_level;   /* 0-100 */
    bool one_bud_anc_enabled;
    ListeningModesConfig listening_modes;  /* Long-press modes config */

    /* Ear detection */
    EarDetectionState ear_detection;

    /* Extension settings (stored in daemon) */
    int ear_pause_mode;   /* 0=disabled, 1=one_out, 2=both_out */

    /* Internal state */
    GMutex lock;
} AirPodsState;

/* Initialize state structure */
void airpods_state_init(AirPodsState *state);

/* Cleanup state structure */
void airpods_state_cleanup(AirPodsState *state);

/* Reset state to disconnected */
void airpods_state_reset(AirPodsState *state);

/* Set device info */
void airpods_state_set_device(AirPodsState *state,
                               const char *name,
                               const char *address,
                               AirPodsModel model);

/* Set custom display name */
void airpods_state_set_display_name(AirPodsState *state, const char *display_name);

/* Get display name (returns custom name if set, otherwise model name) */
const char *airpods_state_get_display_name(AirPodsState *state);

/* Update battery state */
void airpods_state_set_battery(AirPodsState *state,
                                int8_t left, BatteryStatus left_status,
                                int8_t right, BatteryStatus right_status,
                                int8_t case_level, BatteryStatus case_status);

/* Update noise control mode */
void airpods_state_set_noise_control(AirPodsState *state, NoiseControlMode mode);

/* Update ear detection */
void airpods_state_set_ear_detection(AirPodsState *state,
                                      bool left_in_ear,
                                      bool right_in_ear,
                                      bool primary_left);

/* Update conversational awareness */
void airpods_state_set_conversational_awareness(AirPodsState *state, bool enabled);

/* Update adaptive noise level */
void airpods_state_set_adaptive_noise_level(AirPodsState *state, int level);

/* Update listening modes configuration */
void airpods_state_set_listening_modes(AirPodsState *state,
                                        bool off_enabled,
                                        bool transparency_enabled,
                                        bool anc_enabled,
                                        bool adaptive_enabled);

/* Get model name as string */
const char *airpods_model_to_string(AirPodsModel model);

/* Get noise control mode as string */
const char *noise_control_mode_to_string(NoiseControlMode mode);

/* Parse noise control mode from string */
NoiseControlMode noise_control_mode_from_string(const char *str);

/* Check if model supports ANC */
bool airpods_model_supports_anc(AirPodsModel model);

/* Check if model supports adaptive transparency */
bool airpods_model_supports_adaptive(AirPodsModel model);

/* Check if model is headphones (AirPods Max) vs earbuds */
bool airpods_model_is_headphones(AirPodsModel model);

/* Get model enum from model number string (e.g., "A2699" -> AIRPODS_MODEL_PRO_2) */
AirPodsModel airpods_model_from_number(const char *model_number);

#endif /* AIRPODS_STATE_H */
