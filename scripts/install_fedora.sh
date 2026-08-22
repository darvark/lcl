#!/usr/bin/env bash
set -euo pipefail

if command -v dnf >/dev/null 2>&1; then
    PKG_MGR="dnf"
elif command -v yum >/dev/null 2>&1; then
    PKG_MGR="yum"
else
    echo "Error: dnf/yum not found. This script is for Fedora." >&2
    exit 1
fi

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "Error: run as root or install sudo." >&2
        exit 1
    fi
else
    SUDO=""
fi

PKGS=(
    gcc
    gcc-c++
    make
    cmake
    pkgconf-pkg-config
    qt6-qtbase-devel
    sqlite-devel
    openssl-devel
    curl
    wget
)

if $PKG_MGR list --available hamlib-devel >/dev/null 2>&1; then
    PKGS+=("hamlib-devel")
else
    echo "Info: hamlib-devel not available in enabled repositories, skipping optional Hamlib support."
fi

echo "Installing packages: ${PKGS[*]}"
$SUDO $PKG_MGR install -y "${PKGS[@]}"

echo "Done. You can now build with: cmake -S . -B build && cmake --build build"
