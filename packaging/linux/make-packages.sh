#!/bin/sh
# Build the portable tarball, Debian package, and RPM from a Release build.
# Run from anywhere; output goes to dist/ in the repository by default.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-$(cat "$ROOT/VERSION")}
RELEASE=${RELEASE:-1}
ARCH_DEB=${ARCH_DEB:-amd64}
ARCH_RPM=${ARCH_RPM:-x86_64}
OUT=${OUT:-$ROOT/dist}
BUILD=${BUILD:-1}
BIN=${BIN:-$ROOT/bin/Release}

if [ "$BUILD" = 1 ]; then
    cmake -S "$ROOT" -B "$ROOT/_release" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT/_release" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi
for f in quake3 libui.so libui_new.so libcgame.so libqagame.so; do
    test -x "$BIN/$f" || { echo "missing $BIN/$f (set BIN=... or BUILD=1)" >&2; exit 1; }
done

TMPBASE=${TMPDIR:-$ROOT/build}
mkdir -p "$TMPBASE"
TMP=$(mktemp -d "$TMPBASE/vkq3ng-pkg.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
ROOTDIR="$TMP/root"
install -d \
    "$ROOTDIR/usr/lib/vkq3ng/bin" \
    "$ROOTDIR/usr/share/vkq3ng/baseq3" \
    "$ROOTDIR/usr/share/doc/vkq3ng/LICENSES" \
    "$ROOTDIR/usr/bin" \
    "$OUT"

install -m 0755 "$BIN/quake3" "$ROOTDIR/usr/lib/vkq3ng/bin/quake3"
for f in libui.so libui_new.so libcgame.so libqagame.so; do
    install -m 0755 "$BIN/$f" "$ROOTDIR/usr/lib/vkq3ng/bin/$f"
done
cp -a "$ROOT/blue_noise_textures" "$ROOTDIR/usr/share/vkq3ng/baseq3/"
install -m 0644 "$ROOT/README.md" "$ROOT/packaging/linux/PACKAGE.txt" \
    "$ROOTDIR/usr/share/doc/vkq3ng/"
cp -a "$ROOT/blue_noise_textures/LICENSE.txt" \
    "$ROOT/blue_noise_textures/COPYING.txt" \
    "$ROOT/packaging/linux/LICENSES/GPL-2.0.txt" \
    "$ROOTDIR/usr/share/doc/vkq3ng/LICENSES/"
install -m 0755 "$ROOT/packaging/linux/vkq3ng" "$ROOTDIR/usr/bin/vkq3ng"
chmod -R go-w "$ROOTDIR"

strip --strip-unneeded \
    "$ROOTDIR/usr/lib/vkq3ng/bin/quake3" \
    "$ROOTDIR/usr/lib/vkq3ng/bin/"*.so 2>/dev/null || true

# Debian package
DEBROOT="$TMP/deb"
install -d "$DEBROOT/DEBIAN"
INSTALLED_SIZE=$(du -sk "$ROOTDIR" | awk '{print $1}')
sed -e "s/@VERSION@/$VERSION/g" \
    -e "s/@RELEASE@/$RELEASE/g" \
    -e "s/@ARCH@/$ARCH_DEB/g" \
    -e "s/@INSTALLED_SIZE@/$INSTALLED_SIZE/g" \
    "$ROOT/packaging/linux/debian/control.in" > "$DEBROOT/DEBIAN/control"
cp -a "$ROOTDIR/." "$DEBROOT/"
dpkg-deb --root-owner-group --build "$DEBROOT" "$OUT/vkq3ng_${VERSION}-${RELEASE}_${ARCH_DEB}.deb" >/dev/null

# RPM package
TOP="$TMP/rpmbuild"
install -d "$TOP/BUILD" "$TOP/BUILDROOT" "$TOP/RPMS" "$TOP/SOURCES" "$TOP/SPECS" "$TOP/SRPMS" "$TOP/rpmdb"
sed -e "s/@VERSION@/$VERSION/g" \
    -e "s/@RELEASE@/$RELEASE/g" \
    -e "s/@ARCH@/$ARCH_RPM/g" \
    -e "s|@STAGE_ROOT@|$ROOTDIR|g" \
    "$ROOT/packaging/linux/vkq3ng.spec.in" > "$TOP/SPECS/vkq3ng.spec"
rpmbuild -bb \
    --define "_topdir $TOP" \
    --define "_dbpath $TOP/rpmdb" \
    --define "_build_name_fmt %{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}.rpm" \
    "$TOP/SPECS/vkq3ng.spec" >/dev/null
cp "$TOP/RPMS/"*.rpm "$OUT/"

# Portable tarball, matching the system layout (bin/ prefix, not /usr).
PORT="$TMP/portable/vkq3ng-${VERSION}-linux-${ARCH_RPM}"
install -d "$PORT/baseq3"
cp -a "$ROOTDIR/usr/lib/vkq3ng/bin" "$PORT/bin"
cp -a "$ROOTDIR/usr/share/vkq3ng/baseq3/blue_noise_textures" "$PORT/baseq3/"
install -m 0644 "$ROOT/README.md" "$ROOT/packaging/linux/PACKAGE.txt" "$PORT/"
cp -a "$ROOTDIR/usr/share/doc/vkq3ng/LICENSES" "$PORT/"
install -m 0755 "$ROOT/packaging/linux/run-portable.sh" "$PORT/run.sh"
tar -C "$TMP/portable" -czf "$OUT/vkq3ng-${VERSION}-linux-${ARCH_RPM}.tar.gz" "vkq3ng-${VERSION}-linux-${ARCH_RPM}"

cd "$OUT"
sha256sum "vkq3ng_${VERSION}-${RELEASE}_${ARCH_DEB}.deb" \
    "vkq3ng-${VERSION}-${RELEASE}.${ARCH_RPM}.rpm" \
    "vkq3ng-${VERSION}-linux-${ARCH_RPM}.tar.gz" > SHA256SUMS
printf 'Built in %s:\n' "$OUT"
ls -lh "vkq3ng_${VERSION}-${RELEASE}_${ARCH_DEB}.deb" \
    "vkq3ng-${VERSION}-${RELEASE}.${ARCH_RPM}.rpm" \
    "vkq3ng-${VERSION}-linux-${ARCH_RPM}.tar.gz" SHA256SUMS
