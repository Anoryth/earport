/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * D-Bus service interface for GNOME extension communication
 */

#ifndef DBUS_SERVICE_H
#define DBUS_SERVICE_H

#include <glib.h>
#include <gio/gio.h>
#include "airpods_state.h"

/* D-Bus service constants */
#define DBUS_SERVICE_NAME       "io.github.anoryth.EarPort"
#define DBUS_OBJECT_PATH        "/io/github/anoryth/EarPort"
#define DBUS_INTERFACE_NAME     "io.github.anoryth.EarPort1"

/* Commands sent to the AirPods (controls.c): false if they could not be
 * sent (AirPods not connected), and the method then fails */
typedef struct {
    bool (*noise_control)(NoiseControlMode mode, void *user_data);
    bool (*conv_awareness)(bool enabled, void *user_data);
    bool (*adaptive_level)(int level, void *user_data);  /* 0-100, checked */
    /* At least two enabled, checked */
    bool (*listening_modes)(bool off, bool transparency, bool anc, bool adaptive, void *user_data);
    /* SetSetting: byte is the validated value to send */
    bool (*setting)(const AirPodsSettingDef *def, uint8_t byte, void *user_data);
    void (*display_name)(const char *name, void *user_data);
} DbusDeviceCallbacks;

/* Options of the service itself and actions on the link (main.c) */
typedef struct {
    void (*ear_pause_mode)(int mode, void *user_data);  /* 0-2, checked */
    void (*auto_connect)(bool enabled, void *user_data);
    void (*stem_track_controls)(bool enabled, void *user_data);
    void (*apple_handoff)(bool enabled, void *user_data);
    /* Only called when available, see dbus_service_set_hd_mic */
    void (*hd_mic)(bool enabled, void *user_data);
    /* ClaimAudio: have the AirPods play from this computer; false if not
     * connected */
    bool (*claim_audio)(void *user_data);
    /* ResearchSend: a raw packet to the AirPods, only in research mode
     * (research_log=true); false if refused or not connected */
    bool (*research_send)(const uint8_t *data, size_t len, void *user_data);
} DbusServiceCallbacks;

typedef struct DbusService DbusService;

/**
 * Create a new D-Bus service
 *
 * @param state Pointer to AirPods state (must remain valid)
 * @return New service or NULL on error
 */
DbusService *dbus_service_new(AirPodsState *state);

/**
 * Free D-Bus service
 */
void dbus_service_free(DbusService *service);

/**
 * Start the D-Bus service (acquire bus name)
 *
 * @return true on success
 */
bool dbus_service_start(DbusService *service);

/**
 * Stop the D-Bus service
 */
void dbus_service_stop(DbusService *service);

/* Set before dbus_service_start(); copied. A method whose callback is
 * missing does nothing (commands to the AirPods fail). */
void dbus_service_set_device_callbacks(DbusService *service, const DbusDeviceCallbacks *callbacks,
                                       void *user_data);
void dbus_service_set_service_callbacks(DbusService *service,
                                        const DbusServiceCallbacks *callbacks, void *user_data);

/* Current value of the service's options, announced to clients */
void dbus_service_set_auto_connect(DbusService *service, bool enabled);
void dbus_service_set_stem_track_controls(DbusService *service, bool enabled);
void dbus_service_set_apple_handoff(DbusService *service, bool enabled);

/* HD microphone option; available: built with it and the AAC-ELD decoder
 * opens, SetHdMic fails otherwise */
void dbus_service_set_hd_mic(DbusService *service, bool available, bool enabled);

/* Connecting the AirPods again after an Apple device took them: clients
 * keep quiet about the disconnection meanwhile */
void dbus_service_set_reconnecting(DbusService *service, bool reconnecting);

/**
 * Emit DeviceConnected signal
 */
void dbus_service_emit_device_connected(DbusService *service,
                                         const char *address,
                                         const char *name);

/**
 * Emit DeviceDisconnected signal
 */
void dbus_service_emit_device_disconnected(DbusService *service,
                                            const char *address,
                                            const char *name);

/**
 * Emit BatteryChanged signal
 */
void dbus_service_emit_battery_changed(DbusService *service,
                                        int left, int right, int case_battery);

/**
 * Emit NoiseControlModeChanged signal
 */
void dbus_service_emit_noise_control_changed(DbusService *service,
                                              NoiseControlMode mode);

/**
 * Emit EarDetectionChanged signal
 */
void dbus_service_emit_ear_detection_changed(DbusService *service,
                                              bool left_in_ear,
                                              bool right_in_ear);

/**
 * Emit SpeakingChanged: conversation awareness detected the user started
 * (true) or stopped (false) speaking. Clients lower/restore the volume.
 */
void dbus_service_emit_speaking_changed(DbusService *service, bool speaking);

/* Playback was paused because the user fell asleep (once per pause) */
void dbus_service_emit_paused_for_sleep(DbusService *service);

/**
 * Notify that a property has changed (emits PropertiesChanged)
 */
void dbus_service_emit_properties_changed(DbusService *service,
                                           const char *property_name);

#endif /* DBUS_SERVICE_H */
