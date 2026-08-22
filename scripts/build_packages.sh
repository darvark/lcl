#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$ROOT_DIR/dist"
PKG_NAME="lcl-logger"
ARCH_NAME="$(uname -m)"
BIN_NAME="lcl-logger"
ICON_NAME="lcl.jpg"
DESKTOP_FILE_NAME="lcl-logger.desktop"

if command -v git >/dev/null 2>&1 && git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    VERSION="$(git -C "$ROOT_DIR" describe --tags --always --dirty 2>/dev/null || true)"
else
    VERSION=""
fi

if [ -z "$VERSION" ]; then
    VERSION="0.1.0"
fi

# Debian version cannot contain dashes in the upstream version component.
DEB_VERSION="${VERSION//-/.}"

detect_deb_arch() {
    if command -v dpkg >/dev/null 2>&1; then
        dpkg --print-architecture
        return
    fi

    case "$ARCH_NAME" in
        x86_64)
            echo "amd64"
            ;;
        aarch64)
            echo "arm64"
            ;;
        armv7l)
            echo "armhf"
            ;;
        *)
            echo "$ARCH_NAME"
            ;;
    esac
}

usage() {
    cat <<EOF
Usage: $0 <target>

Targets:
  debian   Build .deb package (dpkg-deb)
  fedora   Build .rpm package (rpmbuild)
  arch     Build Arch package (.pkg.tar.zst via makepkg)
  all      Build all supported package formats available on this host

Examples:
  $0 debian
  $0 fedora
  $0 arch
  $0 all
EOF
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: required command '$1' not found." >&2
        exit 1
    fi
}

build_project() {
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

    if [ ! -x "$BUILD_DIR/logger" ]; then
        echo "Error: built binary not found at $BUILD_DIR/logger" >&2
        exit 1
    fi
}

prepare_payload_dir() {
    local payload_dir="$1"

    rm -rf "$payload_dir"
    mkdir -p "$payload_dir"

    install -Dm755 "$BUILD_DIR/logger" "$payload_dir/usr/bin/$BIN_NAME"

    if [ -f "$ROOT_DIR/logger.conf" ]; then
        install -Dm644 "$ROOT_DIR/logger.conf" "$payload_dir/etc/lcl-logger/logger.conf"
    fi

    if [ -d "$ROOT_DIR/contest_defs" ]; then
        mkdir -p "$payload_dir/usr/share/lcl-logger"
        cp -a "$ROOT_DIR/contest_defs" "$payload_dir/usr/share/lcl-logger/"
    fi

    if [ -f "$ROOT_DIR/$ICON_NAME" ]; then
        install -Dm644 "$ROOT_DIR/$ICON_NAME" "$payload_dir/usr/share/lcl-logger/$ICON_NAME"
        install -Dm644 "$ROOT_DIR/$ICON_NAME" "$payload_dir/usr/share/pixmaps/$PKG_NAME.jpg"
    else
        echo "Warning: icon file '$ICON_NAME' not found in project root." >&2
    fi

    if [ -f "$ROOT_DIR/$DESKTOP_FILE_NAME" ]; then
        install -Dm644 "$ROOT_DIR/$DESKTOP_FILE_NAME" "$payload_dir/usr/share/applications/$DESKTOP_FILE_NAME"
    else
        echo "Warning: desktop file '$DESKTOP_FILE_NAME' not found in project root." >&2
    fi

    install -Dm644 "$ROOT_DIR/README.md" "$payload_dir/usr/share/doc/$PKG_NAME/README.md"
    install -Dm644 "$ROOT_DIR/LICENSE" "$payload_dir/usr/share/doc/$PKG_NAME/LICENSE"
}

build_deb() {
    require_command dpkg-deb

    local work_dir
    work_dir="$(mktemp -d)"
    local payload_dir="$work_dir/payload"
    local deb_arch
    deb_arch="$(detect_deb_arch)"

    prepare_payload_dir "$payload_dir"

    mkdir -p "$payload_dir/DEBIAN"
    cat > "$payload_dir/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $DEB_VERSION
Section: hamradio
Priority: optional
Architecture: $deb_arch
Maintainer: LCL Logger Team <noreply@example.com>
Description: LCL Logger - contest logging application
 LCL Logger is a Qt-based contest logger for amateur radio operators.
EOF

    mkdir -p "$DIST_DIR"
    local out_file="$DIST_DIR/${PKG_NAME}_${DEB_VERSION}_${deb_arch}.deb"
    dpkg-deb --build "$payload_dir" "$out_file" >/dev/null
    echo "Built package: $out_file"

    rm -rf "$work_dir"
}

build_rpm() {
    require_command rpmbuild
    require_command tar

    local work_dir
    work_dir="$(mktemp -d)"
    local src_root="$work_dir/${PKG_NAME}-${VERSION}"

    prepare_payload_dir "$src_root"

    local topdir="$work_dir/rpmbuild"
    mkdir -p "$topdir"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

    tar -C "$work_dir" -czf "$topdir/SOURCES/${PKG_NAME}-${VERSION}.tar.gz" "${PKG_NAME}-${VERSION}"

    cat > "$topdir/SPECS/${PKG_NAME}.spec" <<EOF
Name:           $PKG_NAME
Version:        ${VERSION//-/_}
Release:        1%{?dist}
Summary:        LCL Logger contest logging app
License:        GPL-3.0-or-later
URL:            https://example.invalid/$PKG_NAME
Source0:        %{name}-$VERSION.tar.gz
BuildArch:      $ARCH_NAME

%description
LCL Logger is a Qt-based contest logger for amateur radio operators.

%prep
%setup -q

%build
# Prebuilt binary from cmake build directory is packaged as-is.

%install
mkdir -p %{buildroot}
cp -a * %{buildroot}/

%files
/usr/bin/$BIN_NAME
/etc/lcl-logger/logger.conf
/usr/share/lcl-logger/contest_defs
/usr/share/lcl-logger/$ICON_NAME
/usr/share/pixmaps/$PKG_NAME.jpg
/usr/share/applications/$DESKTOP_FILE_NAME
/usr/share/doc/%{name}/README.md
/usr/share/doc/%{name}/LICENSE

%changelog
* $(date '+%a %b %d %Y') LCL Logger Team <noreply@example.com> - ${VERSION//-/_}-1
- Automated package build
EOF

    rpmbuild --define "_topdir $topdir" -bb "$topdir/SPECS/${PKG_NAME}.spec" >/dev/null

    mkdir -p "$DIST_DIR"
    find "$topdir/RPMS" -type f -name "*.rpm" -exec cp -f {} "$DIST_DIR/" \;
    echo "Built package(s):"
    find "$DIST_DIR" -maxdepth 1 -type f -name "${PKG_NAME}-*.rpm" -print

    rm -rf "$work_dir"
}

build_arch_pkg() {
    require_command makepkg
    require_command tar
    require_command sha256sum

    if [ "${EUID:-$(id -u)}" -eq 0 ]; then
        echo "Error: makepkg must not be run as root." >&2
        exit 1
    fi

    local work_dir
    work_dir="$(mktemp -d)"
    local src_root="$work_dir/${PKG_NAME}-${VERSION}"
    local pkgbuild_dir="$work_dir/pkgbuild"

    prepare_payload_dir "$src_root"

    mkdir -p "$pkgbuild_dir"
    tar -C "$work_dir" -czf "$pkgbuild_dir/${PKG_NAME}-${VERSION}.tar.gz" "${PKG_NAME}-${VERSION}"

    cat > "$pkgbuild_dir/PKGBUILD" <<EOF
pkgname=$PKG_NAME
pkgver=${VERSION//-/.}
pkgrel=1
pkgdesc="LCL Logger - contest logging application"
arch=('$ARCH_NAME')
url="https://example.invalid/$PKG_NAME"
license=('GPL3')
depends=('qt6-base' 'sqlite' 'openssl')
source=("${PKG_NAME}-${VERSION}.tar.gz")
sha256sums=('SKIP')

package() {
    cd "\$srcdir/${PKG_NAME}-${VERSION}"
    cp -a * "\$pkgdir/"
}
EOF

    (
        cd "$pkgbuild_dir"
        makepkg -f --nodeps --noconfirm >/dev/null
    )

    mkdir -p "$DIST_DIR"
    find "$pkgbuild_dir" -maxdepth 1 -type f -name "${PKG_NAME}-*.pkg.tar.zst" -exec cp -f {} "$DIST_DIR/" \;
    echo "Built package(s):"
    find "$DIST_DIR" -maxdepth 1 -type f -name "${PKG_NAME}-*.pkg.tar.zst" -print

    rm -rf "$work_dir"
}

main() {
    if [ "${1:-}" = "" ]; then
        usage
        exit 1
    fi

    local target="$1"

    case "$target" in
        -h|--help|help)
            usage
            return 0
            ;;
    esac

    build_project

    case "$target" in
        debian)
            build_deb
            ;;
        fedora)
            build_rpm
            ;;
        arch)
            build_arch_pkg
            ;;
        all)
            if command -v dpkg-deb >/dev/null 2>&1; then
                build_deb
            else
                echo "Skipping debian: dpkg-deb not found"
            fi
            if command -v rpmbuild >/dev/null 2>&1; then
                build_rpm
            else
                echo "Skipping fedora: rpmbuild not found"
            fi
            if command -v makepkg >/dev/null 2>&1; then
                build_arch_pkg
            else
                echo "Skipping arch: makepkg not found"
            fi
            ;;
        *)
            echo "Error: unknown target '$target'"
            usage
            exit 1
            ;;
    esac
}

main "$@"
