#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential ca-certificates ccache clang-format cmake curl \
    dbus-x11 extra-cmake-modules gdb gettext git \
    kirigami-addons-dev kio6 kpeople-vcard \
    libdbus-1-dev libevdev-dev libfakekey-dev \
    libkf6colorscheme-dev libkf6configwidgets-dev libkf6crash-dev \
    libkf6dbusaddons-dev libkf6guiaddons-dev libkf6i18n-dev \
    libkf6iconthemes-dev libkf6itemmodels-dev libkf6kcmutils-dev \
    libkf6kio-dev libkf6modemmanagerqt-dev libkf6notifications-dev \
    libkf6package-dev libkf6people-dev libkf6pulseaudioqt-dev \
    libkf6qqc2desktopstyle-dev libkf6service-dev libkf6solid-dev \
    libkf6statusnotifieritem-dev libkf6widgetsaddons-dev libkf6windowsystem-dev \
    libkirigami-dev libssl-dev libwayland-bin libwayland-dev \
    libx11-dev libxkbcommon-dev libxtst-dev meson ninja-build pkg-config \
    plasma-desktoptheme plasma-wayland-protocols \
    python3 python3-jinja2 python3-yaml shellcheck sshfs \
    qml6-module-org-kde-config qml6-module-org-kde-kitemmodels \
    qml6-module-org-kde-kquickcontrolsaddons qml6-module-org-kde-people \
    qml6-module-qtcore qml6-module-qtmultimedia qml6-module-qtqml \
    qml6-module-qtqml-models qml6-module-qtqml-workerscript \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
    qml6-module-qtquick-layouts qml6-module-qtquick-particles \
    qml6-module-qtquick-templates qml6-module-qtquick-window \
    qt6-base-dev qt6-base-dev-tools qt6-base-private-dev \
    qt6-connectivity-dev qt6-declarative-dev qt6-declarative-private-dev \
    qt6-multimedia-dev qt6-tools-dev-tools qt6-wayland qt6-wayland-dev \
    wayland-protocols xauth xvfb

# Ubuntu 26.04 ships libei 1.5; the mousepad plugin requires 1.6.
if ! pkg-config --atleast-version=1.6 libei-1.0; then
    libei_source=$(mktemp -d)
    trap 'rm -rf "$libei_source"' EXIT
    git clone --depth 1 --branch 1.6.0 \
        https://gitlab.freedesktop.org/libinput/libei.git "$libei_source"
    test "$(git -C "$libei_source" rev-parse HEAD)" = \
        8a46bf3d4b6af7f25a61be53386cf618218114ef
    meson setup "$libei_source/build" "$libei_source" \
        --prefix=/usr/local --libdir=lib --buildtype=release \
        -Dtests=disabled -Ddocumentation=[] -Dliboeffis=disabled -Dlibeis=disabled
    meson compile -C "$libei_source/build"
    meson install -C "$libei_source/build"
    ldconfig
fi

pkg-config --atleast-version=1.6 libei-1.0
rm -rf /var/lib/apt/lists/*
