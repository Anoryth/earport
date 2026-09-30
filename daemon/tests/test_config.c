/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Configuration tests, each in its own temporary configuration directory
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"

#define ADDRESS "00:11:22:33:44:55"
#define OTHER_ADDRESS "66:77:88:99:AA:BB"

static char *config_file(const char *name)
{
    return g_build_filename(g_get_user_config_dir(), "earport", name, NULL);
}

static char *read_config_file(const char *name)
{
    g_autofree char *path = config_file(name);
    char *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    return contents;
}

static void write_config_file(const char *dir, const char *name, const char *contents)
{
    g_autofree char *dir_path = g_build_filename(g_get_user_config_dir(), dir, NULL);
    g_autofree char *path = g_build_filename(dir_path, name, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir_path, 0755), ==, 0);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

/* ============================================================================
 * Daemon settings
 * ========================================================================== */

/* Checked once per process: must run first */
static void test_migration(void)
{
    EarPortConfig config;

    write_config_file("librepods", "daemon.conf", "[Settings]\near_pause_mode=2\n");
    write_config_file("librepods", "devices.conf", "[00_11_22_33_44_55]\nconversational_awareness=true\n");

    g_assert_true(config_load(&config));
    g_assert_cmpint(config.ear_pause_mode, ==, 2);

    g_autofree char *old_dir = g_build_filename(g_get_user_config_dir(), "librepods", NULL);
    g_autofree char *devices = config_file("devices.conf");
    g_assert_false(g_file_test(old_dir, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(devices, G_FILE_TEST_EXISTS));
}

static void test_defaults(void)
{
    EarPortConfig config;

    g_assert_true(config_load(&config));
    g_assert_cmpint(config.ear_pause_mode, ==, 1);
    g_assert_false(config.auto_connect);
    g_assert_false(config.research_log);

    /* Written on first start, without the hidden option */
    g_autofree char *contents = read_config_file("daemon.conf");
    g_assert_nonnull(strstr(contents, "auto_connect=false"));
    g_assert_null(strstr(contents, "research_log"));
}

static void test_save_load(void)
{
    EarPortConfig config = { .ear_pause_mode = 2, .auto_connect = true };
    EarPortConfig loaded;

    g_assert_true(config_save(&config));
    g_assert_true(config_load(&loaded));
    g_assert_cmpint(loaded.ear_pause_mode, ==, 2);
    g_assert_true(loaded.auto_connect);
}

/* Set by hand, it must survive the preferences saving the file */
static void test_research_log_kept(void)
{
    EarPortConfig config;

    write_config_file("earport", "daemon.conf", "[Settings]\near_pause_mode=1\nresearch_log=true\n");
    g_assert_true(config_load(&config));
    g_assert_true(config.research_log);

    config.auto_connect = true;
    g_assert_true(config_save(&config));
    g_autofree char *contents = read_config_file("daemon.conf");
    g_assert_nonnull(strstr(contents, "research_log=true"));
}

/* ============================================================================
 * Device profiles
 * ========================================================================== */

static void test_profile(void)
{
    DeviceProfile profile, loaded;

    /* Unknown device: defaults, nothing to send back */
    g_assert_false(config_load_device_profile(ADDRESS, &loaded));
    g_assert_false(loaded.has_saved_settings);
    g_assert_true(loaded.listening_modes.anc_enabled);

    config_get_default_profile(&profile);
    g_strlcpy(profile.display_name, "Mes AirPods", sizeof(profile.display_name));
    profile.listening_modes.off_enabled = true;
    profile.conversational_awareness = true;
    profile.adaptive_noise_level = 30;
    profile.listening_modes_set = true;
    g_assert_true(config_save_device_profile(ADDRESS, &profile));

    g_assert_true(config_load_device_profile(ADDRESS, &loaded));
    g_assert_true(loaded.has_saved_settings);
    g_assert_cmpstr(loaded.display_name, ==, "Mes AirPods");
    g_assert_true(loaded.listening_modes.off_enabled);
    g_assert_true(loaded.conversational_awareness);
    g_assert_cmpint(loaded.adaptive_noise_level, ==, 30);
    g_assert_true(loaded.listening_modes_set);

    /* Other AirPods keep their own profile */
    g_assert_false(config_load_device_profile(OTHER_ADDRESS, &loaded));
    g_assert_false(config_load_device_profile("", &loaded));
}

static void test_profile_level_clamped(void)
{
    DeviceProfile loaded;

    write_config_file("earport", "devices.conf",
                      "[00_11_22_33_44_55]\nadaptive_noise_level=250\nhas_saved_settings=true\n");
    g_assert_true(config_load_device_profile(ADDRESS, &loaded));
    g_assert_cmpint(loaded.adaptive_noise_level, ==, 100);
}

/* The learned discharge rate must not make the profile look saved: saved
 * settings are pushed to the AirPods on every connection */
static void test_drain_rate(void)
{
    DeviceProfile profile;

    g_assert_cmpfloat(config_load_drain_rate(ADDRESS), ==, 0);
    g_assert_true(config_save_drain_rate(ADDRESS, 19.8));
    g_assert_cmpfloat_with_epsilon(config_load_drain_rate(ADDRESS), 19.8, 1e-9);

    config_load_device_profile(ADDRESS, &profile);
    g_assert_false(profile.has_saved_settings);

    /* And saving the profile keeps the rate */
    config_get_default_profile(&profile);
    g_assert_true(config_save_device_profile(ADDRESS, &profile));
    g_assert_cmpfloat_with_epsilon(config_load_drain_rate(ADDRESS), 19.8, 1e-9);
}

/* ============================================================================
 * Proximity keys
 * ========================================================================== */

static void test_irk(void)
{
    uint8_t irk[16], other[16], loaded[16];
    char *address = NULL;
    struct stat st;

    for (int i = 0; i < 16; i++) {
        irk[i] = (uint8_t)(i * 17);
        other[i] = (uint8_t)(255 - i);
    }

    g_assert_false(config_load_proximity_irk(&address, loaded));

    g_assert_true(config_save_proximity_irk(ADDRESS, irk));
    g_assert_true(config_load_proximity_irk(&address, loaded));
    g_assert_cmpstr(address, ==, ADDRESS);
    g_assert_cmpmem(loaded, 16, irk, 16);
    g_clear_pointer(&address, g_free);

    /* Keys are secrets: readable by the user only */
    g_autofree char *path = config_file("keys.conf");
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    /* The AirPods used last are the ones to recognize */
    g_assert_true(config_save_proximity_irk(OTHER_ADDRESS, other));
    g_assert_true(config_load_proximity_irk(&address, loaded));
    g_assert_cmpstr(address, ==, OTHER_ADDRESS);
    g_assert_cmpmem(loaded, 16, other, 16);
    g_free(address);
}

static void test_irk_corrupt(void)
{
    uint8_t loaded[16];
    char *address = NULL;

    write_config_file("earport", "keys.conf",
                      "[General]\nlast_device=00:11:22:33:44:55\n"
                      "[00_11_22_33_44_55]\nirk=00112233zz\n");
    g_assert_false(config_load_proximity_irk(&address, loaded));
    g_assert_null(address);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    g_test_add_func("/config/migration", test_migration);
    g_test_add_func("/config/defaults", test_defaults);
    g_test_add_func("/config/save-load", test_save_load);
    g_test_add_func("/config/research-log-kept", test_research_log_kept);
    g_test_add_func("/config/profile", test_profile);
    g_test_add_func("/config/profile-level-clamped", test_profile_level_clamped);
    g_test_add_func("/config/drain-rate", test_drain_rate);
    g_test_add_func("/config/irk", test_irk);
    g_test_add_func("/config/irk-corrupt", test_irk_corrupt);

    return g_test_run();
}
