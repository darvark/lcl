#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "Error: apt-get not found. This script is for Ubuntu/Debian." >&2
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

COMMON_PKGS=(
    build-essential
    cmake
    make
    pkg-config
    libsqlite3-dev
    libssl-dev
    curl
    wget
)

if apt-cache show qt6-base-dev >/dev/null 2>&1; then
    QT_PKG="qt6-base-dev"
else
    QT_PKG="qtbase5-dev"
fi

PKGS=("${COMMON_PKGS[@]}" "$QT_PKG")

if apt-cache show libhamlib-dev >/dev/null 2>&1; then
    PKGS+=("libhamlib-dev")
else
    echo "Info: libhamlib-dev not available in current APT sources, skipping optional Hamlib support."
fi

echo "Updating APT package index..."
$SUDO apt-get update

echo "Installing packages: ${PKGS[*]}"
$SUDO apt-get install -y "${PKGS[@]}"

echo "Done. You can now build with: cmake -S . -B build && cmake --build build"
