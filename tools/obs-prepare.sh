#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Files for the openSUSE Build Service package (Fedora, Debian, Ubuntu):
#
#   tools/obs-prepare.sh v0.6.1  -> dist/obs/: that release
#   tools/obs-prepare.sh main    -> dist/obs/: a snapshot, to try the recipes
#
# Then, in the osc checkout of the package: remove the old files, copy
# dist/obs/* in, `osc addremove` and `osc commit`. OBS turns debian.tar.gz
# and the .dsc into a Debian source package itself (debtransform).

set -euo pipefail
cd "$(dirname "$0")/.."

ref=${1:?a release tag (v0.6.1) or a branch (main)}
if ! git cat-file -e "$ref:packaging/debian/control" 2>/dev/null; then
    echo "$ref has no packaging/ (it came after 0.6.0)" >&2
    exit 1
fi
release=$(git show "$ref:packaging/fedora/earport.spec" | sed -n 's/^Version: *//p')
if [[ $ref == v[0-9]* ]]; then
    version=${ref#v}
    if [ "$version" != "$release" ]; then
        echo "packaging/ is at $release, not $version: update the spec and debian/changelog" >&2
        exit 1
    fi
else
    version="$release+git$(date +%Y%m%d).$(git rev-parse --short "$ref")"
fi

out=dist/obs
rm -rf "$out"
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

# Sources, named as the spec's Source0 ends
git archive --prefix="earport-$version/" -o "$out/earport-$version.tar.gz" "$ref"

# Fedora
git show "$ref:packaging/fedora/earport.spec" |
    sed "s/^Version: .*/Version:        $version/" > "$out/earport.spec"

# Debian and Ubuntu: debian/ from that ref, a changelog entry for a snapshot
git archive "$ref" packaging/debian | tar -x -C "$stage"
debian="$stage/packaging/debian"
if [ "$version" != "$release" ]; then
    { printf 'earport (%s-1) unstable; urgency=medium\n\n  * Snapshot of %s.\n\n -- %s  %s\n\n' \
        "$version" "$ref" "$(sed -n 's/^Maintainer: *//p' "$debian/control")" "$(date -R)"
      cat "$debian/changelog"; } > "$stage/changelog"
    mv "$stage/changelog" "$debian/changelog"
fi
tar -czf "$out/debian.tar.gz" -C "$debian/.." debian

build_depends=$(awk '/^Build-Depends:/ { on = 1; sub(/^Build-Depends: */, "") }
                     on && /^[A-Z]/ && !/^Build-Depends:/ { on = 0 }
                     on { gsub(/^ +| +$/, ""); printf "%s ", $0 }' "$debian/control")
cat > "$out/earport.dsc" <<EOF
Format: 3.0 (quilt)
Source: earport
Binary: earport
Architecture: linux-any
Version: $version-1
Maintainer: $(sed -n 's/^Maintainer: *//p' "$debian/control")
Homepage: https://github.com/Anoryth/earport
Build-Depends: ${build_depends% }
EOF

ls -1 "$out"
