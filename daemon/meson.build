project('earport-daemon', 'c',
    version: '0.3.5',
    license: 'GPL-3.0-or-later',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ]
)

# Expose the project version to the sources (single source of truth)
add_project_arguments(
    '-DEARPORT_VERSION="@0@"'.format(meson.project_version()),
    language: 'c'
)

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.56')
gio_dep = dependency('gio-2.0', version: '>= 2.56')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.56')
# Only the BlueZ headers are needed (socket structures): not linking to
# libbluetooth keeps prebuilt binaries portable
bluetooth_dep = dependency('bluez', required: false)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

# Without the bluez pkg-config file, just check the headers are there
if bluetooth_dep.found()
    bluetooth_dep = bluetooth_dep.partial_dependency(compile_args: true, includes: true)
else
    cc.check_header('bluetooth/l2cap.h', required: true)
    bluetooth_dep = declare_dependency()
endif

# Source files
sources = files(
    'src/main.c',
    'src/airpods_state.c',
    'src/airpods_settings.c',
    'src/aap_link.c',
    'src/aap_protocol.c',
    'src/aes128.c',
    'src/autoconnect.c',
    'src/battery_estimator.c',
    'src/ble_proximity.c',
    'src/ble_scanner.c',
    'src/bluetooth.c',
    'src/bluez_monitor.c',
    'src/config.c',
    'src/controls.c',
    'src/device.c',
    'src/dbus_service.c',
    'src/media_control.c',
    'src/research.c',
)

# Build executable
executable('earport-daemon',
    sources,
    dependencies: [glib_dep, gio_dep, gio_unix_dep, bluetooth_dep, m_dep],
    install: true,
    install_dir: get_option('bindir'),
)

# Tests
test_aap_protocol = executable('test-aap-protocol',
    files('tests/test_aap_protocol.c', 'src/aap_protocol.c', 'src/airpods_settings.c', 'src/airpods_state.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('aap-protocol', test_aap_protocol)

test_airpods_settings = executable('test-airpods-settings',
    files('tests/test_airpods_settings.c', 'src/airpods_settings.c', 'src/airpods_state.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('airpods-settings', test_airpods_settings)

test_battery_estimator = executable('test-battery-estimator',
    files('tests/test_battery_estimator.c', 'src/battery_estimator.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep, m_dep],
)
test('battery-estimator', test_battery_estimator)

test_aes128 = executable('test-aes128',
    files('tests/test_aes128.c', 'src/aes128.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('aes128', test_aes128)

test_ble_proximity = executable('test-ble-proximity',
    files('tests/test_ble_proximity.c', 'src/ble_proximity.c', 'src/aes128.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep],
)
test('ble-proximity', test_ble_proximity)

test_device = executable('test-device',
    files('tests/test_device.c', 'src/device.c', 'src/aap_protocol.c', 'src/airpods_settings.c',
          'src/airpods_state.c', 'src/battery_estimator.c', 'src/config.c', 'src/dbus_service.c',
          'src/media_control.c'),
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep, m_dep],
)
test('device', test_device)

# The link over a fake transport, with delays shortened (keep in sync with
# the test)
test_aap_link = executable('test-aap-link',
    files('tests/test_aap_link.c', 'tests/fake_bluetooth.c', 'src/aap_link.c',
          'src/aap_protocol.c', 'src/research.c', 'src/airpods_settings.c', 'src/airpods_state.c'),
    include_directories: include_directories('src'),
    c_args: ['-DSEND_GAP_MS=2', '-DHANDSHAKE_DELAY_MS=4', '-DNOTIF_RETRY_INTERVAL_MS=20',
             '-DRECONNECT_BASE_DELAY_MS=10', '-DSILENT_RECONNECT_DELAY_MS=30',
             '-DPROXIMITY_KEYS_DELAY_MS=30'],
    dependencies: [glib_dep],
)
test('aap-link', test_aap_link)

# D-Bus service, controls, device and link together, on a private bus
test_dbus = executable('test-dbus',
    files('tests/test_dbus.c', 'tests/fake_bluetooth.c', 'src/aap_link.c', 'src/controls.c',
          'src/dbus_service.c', 'src/device.c', 'src/aap_protocol.c', 'src/research.c',
          'src/airpods_settings.c', 'src/airpods_state.c', 'src/battery_estimator.c',
          'src/config.c', 'src/media_control.c'),
    include_directories: include_directories('src'),
    c_args: ['-DSEND_GAP_MS=2', '-DHANDSHAKE_DELAY_MS=4'],
    dependencies: [glib_dep, gio_dep, m_dep],
)
test('dbus', test_dbus)

# Service files point to the installed daemon (/usr/local/bin by default,
# /usr/bin for distribution packages)
service_conf = configuration_data()
service_conf.set('bindir', get_option('prefix') / get_option('bindir'))

# Install systemd user service
configure_file(
    input: 'data/earport-daemon.service.in',
    output: 'earport-daemon.service',
    configuration: service_conf,
    install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)

# Install D-Bus service file
configure_file(
    input: 'data/io.github.anoryth.EarPort.service.in',
    output: 'io.github.anoryth.EarPort.service',
    configuration: service_conf,
    install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

# Protocol probe for reverse-engineering (not installed):
#   ninja -C build aap-probe
executable('aap-probe',
    files('tools/aap_probe.c'),
    # The probe still uses libbluetooth's str2ba()
    dependencies: [bluetooth_dep, cc.find_library('bluetooth', required: false)],
    build_by_default: false,
)
