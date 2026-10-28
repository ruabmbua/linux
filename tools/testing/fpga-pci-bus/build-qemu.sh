#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Build qemu-system-aarch64 with the fpga-ep test device of
# qemu-fpga-ep.diff, for run-fpga-ep.sh.
#
# Usage: build-qemu.sh <QEMU 8.2.x source tree>
#   e.g. curl -LO https://download.qemu.org/qemu-8.2.2.tar.xz
#        tar xf qemu-8.2.2.tar.xz
#        build-qemu.sh qemu-8.2.2
# Needs QEMU's usual build dependencies (ninja, glib, pixman, libslirp).
# The result is <source>/build-fpga-ep/qemu-system-aarch64.
# Debian/Ubuntu "+ds" source tarballs lack the EDK2 images: set
# "if unpack_edk2_blobs" in pc-bios/meson.build to "if false" first.
set -e

SRC=$(realpath "$1")
HERE=$(dirname "$(realpath "$0")")

cd "$SRC"
if [ ! -e hw/misc/fpga_ep.c ]; then
	patch -p1 < "$HERE/qemu-fpga-ep.diff"
fi

mkdir -p build-fpga-ep
cd build-fpga-ep
../configure --target-list=aarch64-softmmu --enable-slirp \
	--disable-docs --disable-werror --disable-tools --disable-guest-agent \
	--disable-install-blobs
ninja qemu-system-aarch64
echo "Built $PWD/qemu-system-aarch64"
