#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Package a built daemon for install-daemon.sh:
#   tools/pack-daemon.sh <build-dir>  -> dist/earport-daemon-linux-<arch>.tar.gz
# The archive holds the binary and the same service templates as the
# repository, so installed service files never drift from them.

set -euo pipefail
cd "$(dirname "$0")/.."

build="${1:?usage: $0 <build-dir>}"
case "$(uname -m)" in
    x86_64) arch="x86_64" ;;
    aarch64 | arm64) arch="aarch64" ;;
    *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/earport-daemon"
install -m755 "$build/earport-daemon" "$stage/earport-daemon/"
install -m644 daemon/data/earport-daemon.service.in \
              daemon/data/io.github.anoryth.EarPort.service.in \
              LICENSE "$stage/earport-daemon/"

mkdir -p dist
out="dist/earport-daemon-linux-$arch.tar.gz"
tar -czf "$out" -C "$stage" --owner=0 --group=0 earport-daemon
echo "$out"
