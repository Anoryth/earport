project('librepods-daemon', 'c',
    version: '0.2.0',
    license: 'GPL-3.0-or-later',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ]
)

# Expose the project version to the sources (single source of truth)
add_project_arguments(
    '-DLIBREPODS_VERSION="@0@"'.format(meson.project_version()),
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
    'src/aap_protocol.c',
    'src/bluetooth.c',
    'src/bluez_monitor.c',
    'src/config.c',
    'src/dbus_service.c',
    'src/media_control.c',
)

# Build executable
executable('librepods-daemon',
    sources,
    dependencies: [glib_dep, gio_dep, gio_unix_dep, bluetooth_dep],
    install: true,
    install_dir: get_option('bindir'),
)

# Install systemd user service
install_data('data/librepods-daemon.service',
    install_dir: join_paths(get_option('prefix'), 'lib', 'systemd', 'user')
)

# Install D-Bus service file
install_data('data/org.librepods.Daemon.service',
    install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services')
)
