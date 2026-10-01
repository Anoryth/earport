#!/bin/bash
#
# EarPort - Installation Script
# Builds the service from source and installs it for the current user (in
# ~/.local, no sudo, with install-daemon.sh like the prebuilt releases),
# then installs the GNOME Shell extension.
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_UUID="earport@anoryth.github.io"
EXTENSION_DIR="$HOME/.local/share/gnome-shell/extensions/$EXTENSION_UUID"

print_header() {
    echo -e "${BLUE}"
    echo "╔════════════════════════════════════════════╗"
    echo "║              EarPort Installer             ║"
    echo "╚════════════════════════════════════════════╝"
    echo -e "${NC}"
}

print_step() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

check_dependencies() {
    print_step "Checking dependencies..."

    local missing_deps=()

    # Check build tools
    if ! command -v meson &> /dev/null; then
        missing_deps+=("meson")
    fi

    if ! command -v ninja &> /dev/null; then
        missing_deps+=("ninja")
    fi

    # Check pkg-config
    if ! command -v pkg-config &> /dev/null; then
        missing_deps+=("pkg-config")
    fi

    # Check for GLib development files
    if ! pkg-config --exists glib-2.0 2>/dev/null; then
        missing_deps+=("libglib2.0-dev (Debian/Ubuntu) or glib2-devel (Fedora) or glib2 (Arch)")
    fi

    # Only the BlueZ headers are needed (no library at runtime)
    if ! pkg-config --exists bluez 2>/dev/null && [ ! -f /usr/include/bluetooth/l2cap.h ]; then
        missing_deps+=("libbluetooth-dev (Debian/Ubuntu) or bluez-libs-devel (Fedora) or bluez-libs (Arch)")
    fi

    # Check for GNOME Shell
    if ! command -v gnome-shell &> /dev/null; then
        missing_deps+=("gnome-shell")
    fi

    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing dependencies:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        echo "Install them using your package manager:"
        echo ""
        echo "  Debian/Ubuntu:"
        echo "    sudo apt install meson ninja-build pkg-config libglib2.0-dev libbluetooth-dev"
        echo ""
        echo "  Fedora:"
        echo "    sudo dnf install meson ninja-build pkg-config glib2-devel bluez-libs-devel"
        echo ""
        echo "  Arch Linux:"
        echo "    sudo pacman -S meson ninja pkg-config glib2 bluez-libs"
        echo ""
        exit 1
    fi

    print_success "All dependencies found"
}

build_daemon() {
    print_step "Building daemon..."

    cd "$SCRIPT_DIR/daemon"

    # Clean previous build if exists
    if [ -d "build" ]; then
        rm -rf build
    fi

    meson setup build --buildtype=release
    ninja -C build

    print_success "Daemon built successfully"
}

remove_legacy_install() {
    # Clean up a pre-rename LibrePods installation, if any
    if systemctl --user list-unit-files librepods-daemon.service &> /dev/null; then
        systemctl --user disable --now librepods-daemon.service 2>/dev/null || true
    fi

    if [ -f /usr/local/bin/librepods-daemon ]; then
        print_step "Removing legacy LibrePods installation (requires sudo)..."
        sudo rm -f /usr/local/bin/librepods-daemon \
            /usr/local/lib/systemd/user/librepods-daemon.service \
            /usr/local/share/dbus-1/services/org.librepods.Daemon.service
    fi

    rm -rf "$HOME/.local/share/gnome-shell/extensions/librepods@librepods.org"
}

install_daemon() {
    print_step "Installing the service for $(whoami)..."

    # Same archive and installer as the prebuilt releases
    local archive
    archive=$("$SCRIPT_DIR/tools/pack-daemon.sh" "$SCRIPT_DIR/daemon/build")
    bash "$SCRIPT_DIR/install-daemon.sh" --from-file "$SCRIPT_DIR/$archive"

    print_success "Service installed and running"
}

# The former install.sh installed the service system-wide: the new one
# takes precedence, remove the old files (the only step needing sudo)
remove_system_install() {
    local files=(/usr/local/bin/earport-daemon
                 /usr/local/lib/systemd/user/earport-daemon.service
                 /usr/local/share/dbus-1/services/io.github.anoryth.EarPort.service)
    local found=()
    for f in "${files[@]}"; do
        [ -e "$f" ] && found+=("$f")
    done
    [ ${#found[@]} -eq 0 ] && return 0

    print_step "An older system-wide EarPort service is installed in /usr/local"
    if [ -t 0 ]; then
        read -r -p "    Remove it now (asks for sudo)? [Y/n] " answer
        if [ -z "$answer" ] || [[ "$answer" =~ ^[YyOo] ]]; then
            sudo rm -f "${found[@]}"
            print_success "Older service removed"
            return 0
        fi
    fi
    print_warning "Kept. To remove it later: sudo rm -f ${found[*]}"
}

install_extension() {
    print_step "Installing GNOME Shell extension..."

    # Create extensions directory if it doesn't exist
    mkdir -p "$(dirname "$EXTENSION_DIR")"

    # Remove old extension if exists
    if [ -d "$EXTENSION_DIR" ]; then
        rm -rf "$EXTENSION_DIR"
    fi

    # Copy extension files
    cp -r "$SCRIPT_DIR/extension" "$EXTENSION_DIR"

    # Compile GSettings schemas (required by getSettings())
    if command -v glib-compile-schemas &> /dev/null; then
        glib-compile-schemas "$EXTENSION_DIR/schemas"
    else
        print_warning "glib-compile-schemas not found (glib2 tools): the extension settings won't load"
    fi

    print_success "Extension installed to $EXTENSION_DIR"
}

enable_extension() {
    print_step "Enabling extension..."

    # Try to enable the extension
    if command -v gnome-extensions &> /dev/null; then
        gnome-extensions enable "$EXTENSION_UUID" 2>/dev/null || true
        print_success "Extension enabled"
    else
        print_warning "Could not enable extension automatically"
        echo "    Please enable it manually via GNOME Extensions app"
    fi
}

print_completion() {
    echo ""
    echo -e "${GREEN}════════════════════════════════════════════${NC}"
    echo -e "${GREEN}    Installation completed successfully!    ${NC}"
    echo -e "${GREEN}════════════════════════════════════════════${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Restart GNOME Shell:"
    echo "     - Wayland: Log out and log back in"
    echo "     - X11: Press Alt+F2, type 'r', press Enter"
    echo ""
    echo "  2. Pair your AirPods via Bluetooth settings"
    echo ""
    echo "  3. The EarPort indicator will appear in Quick Settings"
    echo ""
    echo "To check daemon status:"
    echo "  systemctl --user status earport-daemon.service"
    echo ""
    echo "To view daemon logs:"
    echo "  journalctl --user -u earport-daemon.service -f"
    echo ""
}

uninstall() {
    print_header
    print_step "Uninstalling EarPort..."

    bash "$SCRIPT_DIR/install-daemon.sh" --uninstall
    remove_system_install

    # Remove extension
    print_step "Removing extension..."
    if [ -d "$EXTENSION_DIR" ]; then
        rm -rf "$EXTENSION_DIR"
    fi
    print_success "Extension removed"

    echo ""
    echo -e "${GREEN}Uninstallation completed!${NC}"
    echo "Please restart GNOME Shell to complete the removal."
}

show_help() {
    echo "EarPort - Installation Script"
    echo ""
    echo "Usage: $0 [OPTION]"
    echo ""
    echo "Options:"
    echo "  --install     Build and install the service and the extension (default)"
    echo "  --uninstall   Remove the service and the extension"
    echo "  --daemon      Install only the daemon"
    echo "  --extension   Install only the extension"
    echo "  --help        Show this help message"
    echo ""
}

# Main
main() {
    # Everything goes to the user's session and home
    if [ "$(id -u)" -eq 0 ]; then
        print_error "Run this script as your regular user, without sudo."
        exit 1
    fi

    case "${1:-}" in
        --uninstall)
            uninstall
            ;;
        --daemon)
            print_header
            check_dependencies
            remove_legacy_install
            build_daemon
            install_daemon
            remove_system_install
            echo ""
            print_success "Daemon installation completed!"
            ;;
        --extension)
            print_header
            remove_legacy_install
            install_extension
            enable_extension
            echo ""
            print_success "Extension installation completed!"
            echo "Please restart GNOME Shell to load the extension."
            ;;
        --help|-h)
            show_help
            ;;
        --install|"")
            print_header
            check_dependencies
            remove_legacy_install
            build_daemon
            install_daemon
            remove_system_install
            install_extension
            enable_extension
            print_completion
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
}

main "$@"
