/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * LE discovery and advert filtering against a fake org.bluez (fake_bluez.c)
 */

#include <gio/gio.h>

#include "ble_proximity.h"
#include "ble_scanner.h"
#include "fake_bluez.h"

#define OURS "00:00:00:00:00:1A"
#define OTHER_PHONE "00:00:00:00:00:1B"
#define KEYBOARD "00:00:00:00:00:1C"
#define SILENT "00:00:00:00:00:1D"

typedef struct {
    int filtered;
    int adverts;
    guint8 last_byte;
} Seen;

/* Only OURS is ours */
static bool filter(const char *address, const uint8_t *data, size_t len, void *user_data)
{
    (void)data;
    (void)len;
    ((Seen *)user_data)->filtered++;
    return g_str_equal(address, OURS);
}

static void on_advert(const char *address, const uint8_t *data, size_t len, void *user_data)
{
    Seen *seen = user_data;
    g_assert_cmpstr(address, ==, OURS);
    g_assert_cmpuint(len, ==, 2);
    seen->adverts++;
    seen->last_byte = data[1];
}

static GVariant *manufacturer_data(guint16 company, guint8 value)
{
    const guint8 bytes[] = { 0x07, value };
    GVariantBuilder data;
    g_variant_builder_init(&data, G_VARIANT_TYPE("a{qv}"));
    g_variant_builder_add(&data, "{qv}", company,
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes, sizeof(bytes), 1));
    return g_variant_builder_end(&data);
}

static void set_rssi(const char *path, gint16 rssi)
{
    fake_bluez_set(path, "RSSI", g_variant_new_int16(rssi), true);
}

static void wait_adverts(Seen *seen, int count)
{
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (seen->adverts < count && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    fake_bluez_run(30);
    g_assert_cmpint(seen->adverts, ==, count);
}

static BleScanner *scanner_new(Seen *seen)
{
    fake_bluez_reset();
    *seen = (Seen){ 0 };
    BleScanner *scanner = ble_scanner_new(FAKE_ADAPTER_PATH, filter, on_advert, seen);
    g_assert_nonnull(scanner);
    fake_bluez_sync();
    return scanner;
}

/* LE only, every advert, then stopped once */
static void test_discovery(void)
{
    Seen seen;
    BleScanner *scanner = scanner_new(&seen);

    ble_scanner_start(scanner);
    ble_scanner_start(scanner);
    g_assert_cmpint(fake_bluez_wait_calls("StartDiscovery", 1), ==, 1);
    g_autoptr(GVariant) params = fake_bluez_last_call("SetDiscoveryFilter");
    g_autoptr(GVariant) filter_dict = g_variant_get_child_value(params, 0);
    const char *transport = NULL;
    g_assert_true(g_variant_lookup(filter_dict, "Transport", "&s", &transport));
    g_assert_cmpstr(transport, ==, "le");
    gboolean duplicates = FALSE;
    g_assert_true(g_variant_lookup(filter_dict, "DuplicateData", "b", &duplicates));
    g_assert_true(duplicates);

    ble_scanner_stop(scanner);
    ble_scanner_stop(scanner);
    g_assert_cmpint(fake_bluez_wait_calls("StopDiscovery", 1), ==, 1);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("StartDiscovery"), ==, 1);
    g_assert_cmpint(fake_bluez_calls("StopDiscovery"), ==, 1);
    ble_scanner_free(scanner);
}

/* Each address decided once; RSSI-only changes fetch the data again for
 * ours, and for nobody else */
static void test_adverts(void)
{
    Seen seen;
    BleScanner *scanner = scanner_new(&seen);
    ble_scanner_start(scanner);

    /* Found with its data */
    const char *ours = fake_bluez_add_device(OURS, "", false, false, false);
    fake_bluez_set(ours, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 1), false);
    fake_bluez_announce(ours);
    wait_adverts(&seen, 1);
    g_assert_cmpint(seen.filtered, ==, 1);

    /* New data, then the same advert again */
    fake_bluez_set(ours, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 2), true);
    wait_adverts(&seen, 2);
    g_assert_cmpuint(seen.last_byte, ==, 2);
    set_rssi(ours, -50);
    g_assert_cmpint(fake_bluez_wait_calls("Get", 1), ==, 1);
    wait_adverts(&seen, 3);
    g_assert_cmpuint(seen.last_byte, ==, 2);
    g_assert_cmpint(seen.filtered, ==, 1);

    /* Someone else's iPhone: decided once, then ignored */
    const char *phone = fake_bluez_add_device(OTHER_PHONE, "", false, false, false);
    fake_bluez_set(phone, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 3), true);
    fake_bluez_run(50);
    g_assert_cmpint(seen.filtered, ==, 2);
    fake_bluez_set(phone, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 4), true);
    set_rssi(phone, -60);

    /* A keyboard with other data, a device with none: one Get at most */
    const char *keyboard = fake_bluez_add_device(KEYBOARD, "", false, false, false);
    fake_bluez_set(keyboard, "ManufacturerData", manufacturer_data(0x0006, 5), true);
    const char *silent = fake_bluez_add_device(SILENT, "", false, false, false);
    for (int i = 0; i < 3; i++) {
        set_rssi(keyboard, -70 - i);
        set_rssi(silent, -70 - i);
        /* Adverts come ~100 ms apart: the signals arrive, the first Get
         * has its answer */
        fake_bluez_run(5);
        fake_bluez_wait_calls("Get", 2);
        fake_bluez_settle();
    }
    g_assert_cmpint(fake_bluez_calls("Get"), ==, 2);
    g_assert_cmpint(seen.filtered, ==, 2);
    g_assert_cmpint(seen.adverts, ==, 3);

    /* Apple data showing up later is still decided */
    fake_bluez_set(keyboard, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 6), true);
    fake_bluez_run(50);
    g_assert_cmpint(seen.filtered, ==, 3);
    ble_scanner_free(scanner);
}

/* Not scanning: adverts from other scans are not ours to report */
static void test_stopped(void)
{
    Seen seen;
    BleScanner *scanner = scanner_new(&seen);

    const char *ours = fake_bluez_add_device(OURS, "", false, false, false);
    fake_bluez_set(ours, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 1), true);
    set_rssi(ours, -50);
    fake_bluez_run(50);
    g_assert_cmpint(seen.adverts, ==, 0);
    g_assert_cmpint(fake_bluez_calls("Get"), ==, 0);

    /* Decided when scanning starts, with the next RSSI change */
    ble_scanner_start(scanner);
    set_rssi(ours, -51);
    wait_adverts(&seen, 1);
    ble_scanner_free(scanner);
}

/* New keys: every address is decided again */
static void test_reset_filter(void)
{
    Seen seen;
    BleScanner *scanner = scanner_new(&seen);
    ble_scanner_start(scanner);

    const char *phone = fake_bluez_add_device(OTHER_PHONE, "", false, false, false);
    fake_bluez_set(phone, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 1), true);
    fake_bluez_run(50);
    g_assert_cmpint(seen.filtered, ==, 1);
    ble_scanner_reset_filter(scanner);
    fake_bluez_set(phone, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 2), true);
    fake_bluez_run(50);
    g_assert_cmpint(seen.filtered, ==, 2);
    ble_scanner_free(scanner);
}

/* Freed while a Get is in flight: its answer is dropped */
static void test_free_pending(void)
{
    Seen seen;
    BleScanner *scanner = scanner_new(&seen);
    ble_scanner_start(scanner);

    const char *ours = fake_bluez_add_device(OURS, "", false, false, false);
    fake_bluez_set(ours, "ManufacturerData", manufacturer_data(APPLE_COMPANY_ID, 1), true);
    wait_adverts(&seen, 1);
    fake_bluez_delay_get(100);
    set_rssi(ours, -50);
    /* The signal arrives, the Get leaves, the scanner goes */
    g_assert_cmpint(fake_bluez_wait_calls("Get", 1), ==, 1);
    ble_scanner_free(scanner);
    fake_bluez_run(200);
    g_assert_cmpint(seen.adverts, ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    fake_bluez_start();

    g_test_add_func("/ble-scanner/discovery", test_discovery);
    g_test_add_func("/ble-scanner/adverts", test_adverts);
    g_test_add_func("/ble-scanner/stopped", test_stopped);
    g_test_add_func("/ble-scanner/reset-filter", test_reset_filter);
    g_test_add_func("/ble-scanner/free-pending", test_free_pending);
    int ret = g_test_run();

    fake_bluez_stop();
    return ret;
}
