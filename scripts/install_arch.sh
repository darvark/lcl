#!/usr/bin/env bash
set -euo pipefail

if ! command -v pacman >/dev/null 2>&1; then
    echo "Error: pacman not found. This script is for Arch Linux." >&2
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
    base-devel
    cmake
    pkgconf
    qt6-base
    sqlite
    openssl
    curl
    wget
)

if pacman -Si hamlib >/dev/null 2>&1; then
    PKGS+=("hamlib")
else
    echo "Info: hamlib package not available in current pacman repositories, skipping optional Hamlib support."
fi

echo "Refreshing package databases and installing packages: ${PKGS[*]}"
$SUDO pacman -Syu --noconfirm --needed "${PKGS[@]}"

echo "Done. You can now build with: cmake -S . -B build && cmake --build build"
