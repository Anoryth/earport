project('earport-daemon', 'c',
    version: '0.3.0',
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
bluetooth_dep = dependency('bluez', required: false)

# If bluez pkg-config not available, use libbluetooth directly
if not bluetooth_dep.found()
    cc = meson.get_compiler('c')
    bluetooth_dep = cc.find_library('bluetooth', required: true)
endif

# Source files
sources = files(
    'src/main.c',
    'src/airpods_state.c',
    'src/airpods_settings.c',
    'src/aap_protocol.c',
    'src/bluetooth.c',
    'src/bluez_monitor.c',
    'src/config.c',
    'src/dbus_service.c',
    'src/media_control.c',
)

# Build executable
executable('earport-daemon',
    sources,
    dependencies: [glib_dep, gio_dep, gio_unix_dep, bluetooth_dep],
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
