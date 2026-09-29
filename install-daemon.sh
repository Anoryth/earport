#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install the EarPort service (earport-daemon) for the current user from the
# prebuilt binaries of a GitHub release: no compiler, no sudo. Run it again
# to update.
#
#   curl -fsSL https://github.com/Anoryth/earport/releases/latest/download/install-daemon.sh | bash
#
# Options (after "bash -s --" when piped):
#   --version X.Y.Z   install this release instead of the latest one
#   --uninstall       remove the service (settings in ~/.config/earport stay)
#   --from-file FILE  install from a downloaded archive (offline, testing)

set -euo pipefail

REPO="Anoryth/earport"
BIN_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
DBUS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/dbus-1/services"
UNIT="earport-daemon.service"
DBUS_SERVICE="io.github.anoryth.EarPort.service"

info() { echo "==> $*"; }
warn() { echo "!   $*" >&2; }
die() { echo "✗   $*" >&2; exit 1; }

version=""
archive=""
action="install"
start=1
while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="${2:?--version needs a value}"; shift 2 ;;
        --from-file) archive="${2:?--from-file needs a file}"; shift 2 ;;
        --uninstall) action="uninstall"; shift ;;
        --no-start) start=0; shift ;;  # For tests: install files only
        *) die "Unknown option: $1" ;;
    esac
done

# The service lives in the user's session: root has no business here
[ "$(id -u)" -ne 0 ] || die "Run this as your regular user, without sudo."

uninstall() {
    systemctl --user disable --now "$UNIT" 2>/dev/null || true
    rm -f "$BIN_DIR/earport-daemon" "$UNIT_DIR/$UNIT" "$DBUS_DIR/$DBUS_SERVICE"
    systemctl --user daemon-reload 2>/dev/null || true
    info "EarPort service removed (settings kept in ~/.config/earport)"
}

if [ "$action" = "uninstall" ]; then
    uninstall
    exit 0
fi

case "$(uname -m)" in
    x86_64) arch="x86_64" ;;
    aarch64 | arm64) arch="aarch64" ;;
    *) die "No prebuilt service for $(uname -m): build it from source (see the README)." ;;
esac
asset="earport-daemon-linux-$arch.tar.gz"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ -z "$archive" ]; then
    if [ -n "$version" ]; then
        base="https://github.com/$REPO/releases/download/v${version#v}"
    else
        base="https://github.com/$REPO/releases/latest/download"
    fi

    command -v curl >/dev/null || die "curl is needed to download the service."
    info "Downloading $asset"
    curl -fsSL -o "$work/$asset" "$base/$asset" || die "Download failed: $base/$asset"
    curl -fsSL -o "$work/SHA256SUMS" "$base/SHA256SUMS" || die "Download failed: $base/SHA256SUMS"
    (cd "$work" && grep " $asset\$" SHA256SUMS | sha256sum --check --quiet) ||
        die "Checksum mismatch: the download is corrupted, try again."
    archive="$work/$asset"
fi

tar -xzf "$archive" -C "$work"
dir="$work/earport-daemon"
[ -x "$dir/earport-daemon" ] || die "Unexpected archive content."

# Replace the binary with a new file: a running daemon keeps the old one
install -Dm755 "$dir/earport-daemon" "$BIN_DIR/earport-daemon"
mkdir -p "$UNIT_DIR" "$DBUS_DIR"
sed "s|@bindir@|$BIN_DIR|g" "$dir/$UNIT.in" > "$UNIT_DIR/$UNIT"
sed "s|@bindir@|$BIN_DIR|g" "$dir/$DBUS_SERVICE.in" > "$DBUS_DIR/$DBUS_SERVICE"
info "Installed $("$BIN_DIR/earport-daemon" --version 2>/dev/null || echo earport-daemon) in $BIN_DIR"

for other in /usr/bin/earport-daemon /usr/local/bin/earport-daemon; do
    [ -e "$other" ] && warn "Another EarPort service is installed in $other; this one takes precedence."
done

systemctl is-active --quiet bluetooth 2>/dev/null ||
    warn "The Bluetooth service is not running: start it to use your AirPods."

if [ "$start" -eq 1 ]; then
    systemctl --user daemon-reload
    systemctl --user enable "$UNIT" >/dev/null 2>&1
    systemctl --user restart "$UNIT"
    info "EarPort service running. Install or enable the GNOME Shell extension to use it."
fi
