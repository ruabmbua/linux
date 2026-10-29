# SPDX-License-Identifier: GPL-2.0
#
# Build drivers/bus/fpga-pci-bus.c as an external module.
#
#   make KDIR=<kernel build dir> [ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-]
#   make KDIR=<kernel build dir> INSTALL_MOD_PATH=<rootfs> modules_install
#
# KDIR defaults to the build tree of the running kernel. To build elsewhere,
# copy this directory with "cp -rL", which resolves the source symlink.

KDIR ?= /lib/modules/$(shell uname -r)/build

modules modules_install clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

.PHONY: modules modules_install clean
