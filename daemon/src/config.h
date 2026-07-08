/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Configuration file management
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>
#include <stdbool.h>
#include "airpods_state.h"

/* Configuration data structure */
typedef struct {
    int ear_pause_mode;   /* 0=disabled, 1=one_out, 2=both_out */
} EarPortConfig;

/**
 * Load configuration from file
 * Creates default config if file doesn't exist
 *
 * @param config Pointer to config structure to fill
 * @return true on success
 */
bool config_load(EarPortConfig *config);

/**
 * Save configuration to file
 *
 * @param config Pointer to config structure to save
 * @return true on success
 */
bool config_save(const EarPortConfig *config);

/**
 * Get default configuration values
 *
 * @param config Pointer to config structure to fill with defaults
 */
void config_get_defaults(EarPortConfig *config);

/* Complete device profile (per-device settings) */
typedef struct {
    char display_name[64];              /* Custom display name (empty = use model) */
    ListeningModesConfig listening_modes;
    bool conversational_awareness;      /* CA enabled */
    int adaptive_noise_level;           /* 0-100 */
    char preferred_nc_mode[16];         /* "off", "anc", "transparency", "adaptive" */
    bool has_saved_settings;            /* Whether profile has saved settings */
} DeviceProfile;

/**
 * Load complete device profile
 *
 * @param device_address Bluetooth MAC address of the device
 * @param profile Pointer to structure to fill with profile data
 * @return true if found and loaded, false if not found (defaults used)
 */
bool config_load_device_profile(const char *device_address, DeviceProfile *profile);

/**
 * Save complete device profile
 *
 * @param device_address Bluetooth MAC address of the device
 * @param profile Pointer to profile to save
 * @return true on success
 */
bool config_save_device_profile(const char *device_address, const DeviceProfile *profile);

/**
 * Get default device profile
 *
 * @param profile Pointer to structure to fill with defaults
 */
void config_get_default_profile(DeviceProfile *profile);

/**
 * Load listening modes for a specific device
 *
 * @param device_address Bluetooth MAC address of the device
 * @param modes Pointer to structure to fill with listening modes
 * @return true if found and loaded, false if not found (defaults used)
 */
bool config_load_device_listening_modes(const char *device_address, ListeningModesConfig *modes);

/**
 * Save listening modes for a specific device
 *
 * @param device_address Bluetooth MAC address of the device
 * @param modes Pointer to listening modes to save
 * @return true on success
 */
bool config_save_device_listening_modes(const char *device_address, const ListeningModesConfig *modes);

/**
 * Get default listening modes
 *
 * @param modes Pointer to structure to fill with defaults
 */
void config_get_default_listening_modes(ListeningModesConfig *modes);

#endif /* CONFIG_H */
