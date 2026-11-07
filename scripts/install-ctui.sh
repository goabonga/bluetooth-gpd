#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>
#
# Installs the ctui library for the jobs that compile the program. Without a
# tag it takes the version published on the AUR; with one (the argument,
# usually the repository variable CTUI_TAG) it installs exactly that tag.
# When the version wanted is the one the AUR publishes and makepkg is there,
# the package of the AUR repository is built and installed; otherwise the
# tag of the upstream repository is installed under /usr/local.
#
# Usage: install-ctui.sh [TAG]

set -euo pipefail

aur=https://aur.archlinux.org
tag=${1:-}

# The AUR version is [epoch:]pkgver-pkgrel; the tag is v<pkgver>.
published=$(curl -fsSg "$aur/rpc/v5/info?arg%5B%5D=ctui" |
    sed -n 's/.*"Version":"\([^"]*\)".*/\1/p' || true)
if [ -n "$published" ]; then
    published=${published#*:}
    published=v${published%-*}
fi
if [ -z "$tag" ]; then
    if [ -z "$published" ]; then
        echo "install-ctui.sh: ctui is not published on the AUR" >&2
        exit 1
    fi
    tag=$published
fi

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT

if [ "$tag" = "$published" ] && command -v makepkg >/dev/null; then
    echo "Installing the AUR package of ctui $tag"
    build="git clone --quiet $aur/ctui.git $src/ctui &&
        cd $src/ctui && makepkg --force --nodeps --nocheck"
    if [ "$(id -u)" -eq 0 ]; then
        # makepkg refuses to run as root.
        id builder >/dev/null 2>&1 || useradd -m builder
        chown builder "$src"
        su builder -c "$build"
        pacman -U --noconfirm "$src"/ctui/ctui-*.pkg.tar.*
    else
        bash -c "$build"
        sudo pacman -U --noconfirm "$src"/ctui/ctui-*.pkg.tar.*
    fi
else
    echo "Installing ctui $tag from the upstream sources"
    git clone --quiet --depth 1 --branch "$tag" \
        https://github.com/goabonga/ctui.git "$src/ctui"
    make -C "$src/ctui" install PREFIX=/usr/local
fi
