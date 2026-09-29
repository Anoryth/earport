#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the extension zip for extensions.gnome.org and GitHub releases:
# only the files the extension needs, translations compiled from po/.
#
#   tools/pack-extension.sh      -> dist/<uuid>.shell-extension.zip

set -euo pipefail
cd "$(dirname "$0")/.."

uuid=$(sed -n 's/.*"uuid": *"\([^"]*\)".*/\1/p' extension/metadata.json)
domain=$(sed -n 's/.*"gettext-domain": *"\([^"]*\)".*/\1/p' extension/metadata.json)
out="dist/$uuid.shell-extension.zip"

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

cp -r extension/metadata.json extension/*.js extension/stylesheet.css \
      extension/icons extension/schemas "$stage/"
rm -f "$stage/schemas/gschemas.compiled"
glib-compile-schemas --strict "$stage/schemas"

for po in po/*.po; do
    lang=$(basename "$po" .po)
    mkdir -p "$stage/locale/$lang/LC_MESSAGES"
    msgfmt --check -o "$stage/locale/$lang/LC_MESSAGES/$domain.mo" "$po"
done

mkdir -p dist
rm -f "$out"
(cd "$stage" && zip -qr -X "$OLDPWD/$out" .)
echo "$out"
