/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Apple AirPods Protocol (AAP) implementation
 * Based on reverse-engineered protocol from LibrePods project
 */

#ifndef AAP_PROTOCOL_H
#define AAP_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "airpods_state.h"

/* AAP packet header */
#define AAP_HEADER_SIZE 4
#define AAP_HEADER_BYTE0 0x04
#define AAP_HEADER_BYTE1 0x00
#define AAP_HEADER_BYTE2 0x04
#define AAP_HEADER_BYTE3 0x00

/* Handshake header (different from standard) */
#define AAP_HANDSHAKE_HEADER_BYTE0 0x00
#define AAP_HANDSHAKE_HEADER_BYTE1 0x00

/* Opcodes */
#define AAP_OPCODE_BATTERY       0x04
#define AAP_OPCODE_EAR_DETECTION 0x06
#define AAP_OPCODE_CONTROL       0x09
#define AAP_OPCODE_AUDIO_SOURCE  0x0E
#define AAP_OPCODE_HOSTS         0x2E
#define AAP_OPCODE_NOTIFICATIONS 0x0F
#define AAP_OPCODE_HEAD_TRACKING 0x17
#define AAP_OPCODE_METADATA      0x1D
#define AAP_OPCODE_CA_DETECTION  0x4B
#define AAP_OPCODE_SET_FEATURES  0x4D
#define AAP_OPCODE_PROXIMITY_KEYS_REQ 0x30
#define AAP_OPCODE_PROXIMITY_KEYS_RSP 0x31
#define AAP_OPCODE_SLEEP_DETECTION 0x57

/* Proximity key types (opcode 0x31) */
#define AAP_PROXIMITY_KEY_IRK 0x01  /* Resolves the BLE random address */
#define AAP_PROXIMITY_KEY_ENC 0x04  /* Decrypts BLE battery data */
#define AAP_PROXIMITY_KEY_SIZE 16

/* Control command identifiers (byte after opcode 0x09) */
#define AAP_CTRL_NOISE_CONTROL       0x0D
#define AAP_CTRL_LISTENING_MODES     0x1A
#define AAP_CTRL_ONE_BUD_ANC         0x1B
#define AAP_CTRL_CONV_AWARENESS      0x28
#define AAP_CTRL_ADAPTIVE_LEVEL      0x2E
#define AAP_CTRL_ALLOW_OFF_OPTION    0x34

/* Listening modes bitmask values (for 0x1A ListeningModeConfigs) */
#define AAP_LISTENING_MODE_OFF          0x01
#define AAP_LISTENING_MODE_ANC          0x02
#define AAP_LISTENING_MODE_TRANSPARENCY 0x04
#define AAP_LISTENING_MODE_ADAPTIVE     0x08

/* Battery component types */
#define AAP_BATTERY_SINGLE 0x01  /* AirPods Max (headphones) */
#define AAP_BATTERY_RIGHT  0x02
#define AAP_BATTERY_LEFT   0x04
#define AAP_BATTERY_CASE   0x08

/* Ear detection status */
#define AAP_EAR_IN_EAR   0x00
#define AAP_EAR_OUT      0x01
#define AAP_EAR_IN_CASE  0x02

/* Packet sizes */
#define AAP_HANDSHAKE_SIZE       16
#define AAP_REQUEST_NOTIF_SIZE   10
#define AAP_SET_FEATURES_SIZE    14
#define AAP_CONTROL_CMD_SIZE     11
#define AAP_MIN_BATTERY_SIZE     12

/* Pre-built packets */
extern const uint8_t AAP_PKT_HANDSHAKE[AAP_HANDSHAKE_SIZE];
extern const uint8_t AAP_PKT_REQUEST_NOTIFICATIONS[AAP_REQUEST_NOTIF_SIZE];
extern const uint8_t AAP_PKT_SET_FEATURES[AAP_SET_FEATURES_SIZE];

extern const uint8_t AAP_PKT_NC_OFF[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_NC_ANC[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_NC_TRANSPARENCY[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_NC_ADAPTIVE[AAP_CONTROL_CMD_SIZE];

extern const uint8_t AAP_PKT_CA_ENABLE[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_CA_DISABLE[AAP_CONTROL_CMD_SIZE];

/* Parsed packet result */
typedef enum {
    AAP_PARSE_OK,
    AAP_PARSE_INCOMPLETE,
    AAP_PARSE_INVALID_HEADER,
    AAP_PARSE_UNKNOWN_OPCODE,
    AAP_PARSE_MALFORMED,
} AapParseResult;

/* Packet type after parsing */
typedef enum {
    AAP_PKT_TYPE_UNKNOWN,
    AAP_PKT_TYPE_BATTERY,
    AAP_PKT_TYPE_EAR_DETECTION,
    AAP_PKT_TYPE_NOISE_CONTROL,
    AAP_PKT_TYPE_CONV_AWARENESS,
    AAP_PKT_TYPE_CA_DETECTION,
    AAP_PKT_TYPE_METADATA,
    AAP_PKT_TYPE_LISTENING_MODES,
    AAP_PKT_TYPE_ADAPTIVE_LEVEL,
    AAP_PKT_TYPE_CONTROL_SETTING,
    AAP_PKT_TYPE_PROXIMITY_KEYS,
    AAP_PKT_TYPE_SLEEP_DETECTION,
    AAP_PKT_TYPE_AUDIO_SOURCE,
    AAP_PKT_TYPE_HOSTS,
} AapPacketType;

/* Parsed battery data */
typedef struct {
    int8_t left_level;      /* 0-100 or -1 */
    int8_t right_level;
    int8_t case_level;
    BatteryStatus left_status;
    BatteryStatus right_status;
    BatteryStatus case_status;
    bool primary_known;     /* A pod is listed: the first one is the primary */
    bool primary_left;
} AapBatteryData;

/* Parsed ear detection data */
/* Parsed ear detection data: by role, see AapBatteryData for the sides */
typedef struct {
    bool primary_in_ear;
    bool secondary_in_ear;
} AapEarDetectionData;

/* Parsed metadata */
typedef struct {
    char device_name[64];
    char model_number[16];
    char manufacturer[32];
} AapMetadata;

/* Listening modes configuration (bitmask) */
typedef struct {
    bool off_enabled;
    bool transparency_enabled;
    bool anc_enabled;
    bool adaptive_enabled;
    uint8_t raw_value;  /* Raw bitmask for debugging */
} AapListeningModes;

/* Any other control command: identifier and its (up to 4) value bytes */
typedef struct {
    uint8_t id;
    uint8_t value[4];
} AapControlSetting;

/* Keys used to recognize the AirPods' BLE advertisements */
typedef struct {
    bool has_irk;
    bool has_enc;
    uint8_t irk[AAP_PROXIMITY_KEY_SIZE];  /* As sent: little-endian */
    uint8_t enc[AAP_PROXIMITY_KEY_SIZE];
} AapProximityKeys;

/* Sleep detection update ("Pause Media When Falling Asleep"): the AirPods
 * detect it, the host decides and pauses */
#define AAP_SLEEP_MSG_STATUS 0x02      /* Received: sleep status */
#define AAP_SLEEP_MSG_THRESHOLD 0x03   /* Sent: confidence threshold */
#define AAP_SLEEP_MSG_RESET 0x04       /* Sent: detection reset, with a reason */
#define AAP_SLEEP_STATUS_ASLEEP 0x01
#define AAP_SLEEP_RESET_USER_ACTIVE 0x01

typedef struct {
    uint8_t msg_type;          /* AAP_SLEEP_MSG_STATUS for the status */
    uint8_t status;            /* AAP_SLEEP_STATUS_ASLEEP when asleep */
    int rewind_seconds;        /* Playback to replay after the pause */
    uint8_t confidence;        /* Compared to the threshold */
} AapSleepDetection;

#define AAP_SLEEP_MSG_SIZE 10   /* Header, opcode, length, type, value */

/* Device the AirPods currently play from, among the hosts they are
 * connected to: several only when they take this computer for an Apple
 * device */
typedef enum {
    AAP_AUDIO_SOURCE_NONE = 0x00,
    AAP_AUDIO_SOURCE_CALL = 0x01,
    AAP_AUDIO_SOURCE_MEDIA = 0x02,
} AapAudioSourceType;

typedef struct {
    char address[18];          /* "AA:BB:CC:DD:EE:FF", empty when none */
    AapAudioSourceType type;
} AapAudioSource;

/* Control setting asking the AirPods to play from this host (1) or to let
 * the others have them (0), when they switch between hosts */
#define AAP_CTRL_OWNS_CONNECTION 0x06

/* The hosts the AirPods are connected to, with flags (0x02: the one they
 * play from); Apple calls it the TiPi table */
#define AAP_MAX_HOSTS 4

typedef struct {
    char address[18];
    uint8_t status;
    uint8_t flags;
} AapHost;

typedef struct {
    uint8_t state;
    uint8_t count;
    AapHost hosts[AAP_MAX_HOSTS];
} AapHosts;

/* Parse result union */
typedef struct {
    AapPacketType type;
    union {
        AapBatteryData battery;
        AapEarDetectionData ear_detection;
        NoiseControlMode noise_control;
        bool conversational_awareness;
        int ca_volume_level;
        AapMetadata metadata;
        AapListeningModes listening_modes;
        int adaptive_level;           /* 0-100 */
        AapControlSetting control_setting;
        AapProximityKeys proximity_keys;
        AapSleepDetection sleep_detection;
        AapAudioSource audio_source;
        AapHosts hosts;
    } data;
} AapParsedPacket;

/**
 * Check if buffer starts with valid AAP header
 */
bool aap_has_valid_header(const uint8_t *data, size_t len);

/**
 * Get opcode from packet (assumes valid header)
 */
uint8_t aap_get_opcode(const uint8_t *data, size_t len);

/**
 * Parse incoming AAP packet
 *
 * @param data Raw packet data
 * @param len Data length
 * @param result Output parsed packet
 * @return Parse result code
 */
AapParseResult aap_parse_packet(const uint8_t *data, size_t len, AapParsedPacket *result);

/**
 * Parse battery packet
 */
AapParseResult aap_parse_battery(const uint8_t *data, size_t len, AapBatteryData *battery);

/**
 * Parse ear detection packet
 */
AapParseResult aap_parse_ear_detection(const uint8_t *data, size_t len, AapEarDetectionData *ear);

/**
 * Parse noise control response
 */
AapParseResult aap_parse_noise_control(const uint8_t *data, size_t len, NoiseControlMode *mode);

/* Proximity keys request: 04 00 04 00 30 00 05 00 */
#define AAP_PROXIMITY_KEYS_REQ_SIZE 8
extern const uint8_t AAP_PKT_REQUEST_PROXIMITY_KEYS[AAP_PROXIMITY_KEYS_REQ_SIZE];

/**
 * Interpret a conversation awareness event level (opcode 0x4B)
 *
 * @return 1 when the user starts speaking (lower the volume), 0 when the
 *         conversation is over (restore it), -1 for intermediate steps
 */
int aap_ca_speaking_from_level(uint8_t level);

/**
 * Build a control command packet: 04 00 04 00 09 00 [id] [value...] padded
 * with zeros
 *
 * @param id Control command identifier
 * @param value Value bytes (at most 4 are used)
 * @param value_len Number of value bytes
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_control_cmd(uint8_t id, const uint8_t *value, size_t value_len, uint8_t *buffer);

/**
 * Build noise control command packet
 *
 * @param mode Desired noise control mode
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_noise_control_cmd(NoiseControlMode mode, uint8_t *buffer);

/**
 * Build adaptive noise level command
 *
 * @param level Level 0-100
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_adaptive_level_cmd(int level, uint8_t *buffer);

/**
 * Build a sleep detection message for the AirPods (threshold or reset)
 * @param buffer Must be at least AAP_SLEEP_MSG_SIZE bytes
 */
void aap_build_sleep_detection_msg(uint8_t msg_type, uint8_t value, uint8_t *buffer);

/**
 * Build conversational awareness command
 *
 * @param enable Enable or disable
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_conv_awareness_cmd(bool enable, uint8_t *buffer);

/**
 * Build listening modes configuration command
 *
 * @param modes Bitmask of enabled modes (use AAP_LISTENING_MODE_* constants)
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_listening_modes_cmd(uint8_t modes, uint8_t *buffer);

/**
 * Debug: print packet as hex string
 */
void aap_debug_print_packet(const char *prefix, const uint8_t *data, size_t len);

#endif /* AAP_PROTOCOL_H */
