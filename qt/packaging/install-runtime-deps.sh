#!/usr/bin/env bash
# Install the host libraries OBSBOT4Linux-x86_64.AppImage needs at runtime.
#
# The AppImage bundles Qt 6, QML, FFmpeg and the OBSBOT SDK, but deliberately
# does NOT ship the display-server client stack (X11/xcb/xkbcommon), the GPU
# stack (GL/EGL) or the font stack: those must match the host's X server and
# GPU driver. This script installs them, plus fuse3 (the AppImage runtime is
# static and only needs `fusermount` to mount itself), and
# adds the current user to the `video` group so the camera is reachable.
#
# Usage:  ./install-runtime-deps.sh        (asks for sudo when needed)
# Supported: Debian/Ubuntu (apt), Fedora (dnf), Arch/CachyOS/Manjaro (pacman).
set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || { echo "error: run as root or install sudo" >&2; exit 1; }
    SUDO="sudo"
fi

if command -v apt-get >/dev/null 2>&1; then
    PKGS=(
        fuse3
        libgl1 libegl1 libopengl0 libglx0 libdrm2
        libx11-6 libx11-xcb1 libxcb1 libsm6 libice6
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0
        libxcb-render0 libxcb-render-util0 libxcb-shape0 libxcb-shm0 libxcb-sync1
        libxcb-xfixes0 libxcb-xinerama0 libxcb-xinput0 libxcb-xkb1 libxcb-glx0 libxcb-util1
        libxkbcommon0 libxkbcommon-x11-0
        libfontconfig1 libfreetype6
    )
    $SUDO apt-get update
    $SUDO apt-get install -y --no-install-recommends "${PKGS[@]}"
elif command -v dnf >/dev/null 2>&1; then
    PKGS=(
        fuse3
        libglvnd-glx libglvnd-egl libglvnd-opengl libdrm
        libX11 libX11-xcb libxcb libSM libICE
        xcb-util-cursor xcb-util-wm xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util
        libxkbcommon libxkbcommon-x11
        fontconfig freetype
    )
    $SUDO dnf install -y "${PKGS[@]}"
elif command -v pacman >/dev/null 2>&1; then
    PKGS=(
        fuse3
        libglvnd libdrm
        libx11 libxcb libsm libice
        xcb-util-cursor xcb-util-wm xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util
        libxkbcommon libxkbcommon-x11
        fontconfig freetype2
    )
    $SUDO pacman -S --needed --noconfirm "${PKGS[@]}"
else
    echo "error: unsupported distro (need apt, dnf or pacman)." >&2
    exit 1
fi

# Camera access: USB/V4L2 devices are owned by the `video` group.
USER_NAME="${SUDO_USER:-$(id -un)}"
if id -nG "$USER_NAME" | tr ' ' '\n' | grep -qx video; then
    echo ">> $USER_NAME is already in the video group"
else
    $SUDO usermod -aG video "$USER_NAME"
    echo ">> added $USER_NAME to the video group: log out and back in to apply"
fi

echo ">> runtime dependencies installed. Run: chmod +x OBSBOT4Linux-x86_64.AppImage && ./OBSBOT4Linux-x86_64.AppImage"
