/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#ifndef AIRPODS_STATE_H
#define AIRPODS_STATE_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include "airpods_settings.h"

/* AirPods model identifiers, as the two bytes appear in BLE adverts: Apple's
 * Bluetooth product ID, little-endian (product 0x2024 -> 0x2420) */
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
    AIRPODS_MODEL_PRO_3 = 0x2720,
    AIRPODS_MODEL_MAX = 0x0A20,
    AIRPODS_MODEL_MAX_USBC = 0x1F20,
    AIRPODS_MODEL_5_WIRELESS_CHARGING = 0x3020,
    AIRPODS_MODEL_5 = 0x3620,
    AIRPODS_MODEL_MAX_2 = 0x2D20,
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
    bool primary_in_ear;    /* As the AirPods report it */
    bool secondary_in_ear;
    bool primary_left;      /* Which pod is primary: listed first in battery reports */
} EarDetectionState;

/* Long-press listening modes configuration */
typedef struct {
    bool off_enabled;           /* Off mode available on long press */
    bool transparency_enabled;  /* Transparency mode available */
    bool anc_enabled;           /* ANC mode available */
    bool adaptive_enabled;      /* Adaptive mode available */
} ListeningModesConfig;

/* Defaults, shared by the state and the device profiles: Apple's
 * long-press modes (Noise Cancellation and Transparency) and the middle
 * of the adaptive scale */
#define AIRPODS_DEFAULT_LISTENING_MODES ((ListeningModesConfig) { \
    .off_enabled = false, .transparency_enabled = true, \
    .anc_enabled = true, .adaptive_enabled = false })
#define AIRPODS_DEFAULT_ADAPTIVE_LEVEL 50

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
    int listening_minutes_left;   /* Estimated listening time, -1 if unknown */

    /* Features */
    NoiseControlMode noise_control_mode;
    bool conversational_awareness;
    int adaptive_noise_level;   /* 0-100 */
    ListeningModesConfig listening_modes;  /* Long-press modes config */

    /* Generic settings, indexed like the airpods_settings table */
    uint8_t setting_values[AIRPODS_SETTING_COUNT];
    bool setting_announced[AIRPODS_SETTING_COUNT];  /* Sent by the AirPods */

    /* Ear detection */
    EarDetectionState ear_detection;

    /* Battery of the AirPods while not connected here (BLE adverts) */
    bool nearby_valid;
    int nearby_left, nearby_right, nearby_case;   /* -1 if unknown */
    bool nearby_left_charging, nearby_right_charging, nearby_case_charging;
    const char *nearby_host;     /* "none", "idle", "music", "call" */
    bool nearby_headphones;      /* AirPods Max: one battery */
    char *nearby_name;           /* Custom or Bluetooth name */

    /* Device the AirPods play from: "" unknown, "none", "computer" (this
     * one), "other" (an Apple device, when they take this computer for one
     * too) */
    const char *audio_source;
    char host_address[18];       /* This computer's Bluetooth address */

    /* Battery of the charging case from its own adverts, connected here
     * or not */
    int case_advert_level;       /* -1 if unknown */
    bool case_advert_charging;

    /* Extension settings (stored in daemon) */
    int ear_pause_mode;   /* 0=disabled, 1=one_out, 2=both_out */
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

/* Case battery as the AirPods report it or, when they don't know it (pods
 * out of the case), from the case's adverts; -1 if unknown */
int airpods_state_case_level(const AirPodsState *state, bool *charging);

/* Same for AirPods seen nearby but not connected here */
int airpods_state_nearby_case_level(const AirPodsState *state, bool *charging);

/* Update the estimated listening time; returns true if it changed */
bool airpods_state_set_listening_minutes(AirPodsState *state, int minutes);

/* Update noise control mode */
void airpods_state_set_noise_control(AirPodsState *state, NoiseControlMode mode);

/* Update ear detection, as reported for the primary and secondary pods */
void airpods_state_set_ear_detection(AirPodsState *state,
                                      bool primary_in_ear,
                                      bool secondary_in_ear);

/* Which pod is primary (it changes, e.g. when it goes in the case); true
 * if left and right changed with it */
bool airpods_state_set_primary_left(AirPodsState *state, bool primary_left);

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

/* Store a setting value received from (or sent to) the AirPods.
 * Returns true if the stored value changed; unknown IDs are ignored. */
bool airpods_state_set_setting(AirPodsState *state, uint8_t id, uint8_t value);

/* Get a setting value, false if the AirPods have not announced it */
bool airpods_state_get_setting(AirPodsState *state, uint8_t id, uint8_t *value);

/* Get model name as string */
const char *airpods_model_to_string(AirPodsModel model);

/* Get noise control mode as string */
const char *noise_control_mode_to_string(NoiseControlMode mode);

/* Parse a noise control mode name; false if unknown */
bool noise_control_mode_from_string(const char *str, NoiseControlMode *mode);

/* Check if model supports ANC */
bool airpods_model_supports_anc(AirPodsModel model);

/* Check if model supports adaptive transparency */
bool airpods_model_supports_adaptive(AirPodsModel model);

/* Check if model is headphones (AirPods Max) vs earbuds */
bool airpods_model_is_headphones(AirPodsModel model);

/* Whether the charging case advertises its own battery */
bool airpods_model_has_ble_case(AirPodsModel model);

/* Get model enum from model number string (e.g., "A2699" -> AIRPODS_MODEL_PRO_2) */
AirPodsModel airpods_model_from_number(const char *model_number);

#endif /* AIRPODS_STATE_H */
