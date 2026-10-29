#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot test for drivers/bus/fpga-pci-bus.c on QEMU arm64 virt.
#
# QEMU's "edu" device stands in for an FPGA with an interrupt splitter,
# "ivshmem-plain" for one whose BAR2 hosts a stock gpio-mmio IP core.
# Needs the TEST ONLY PCI IDs in the driver.
#
# Usage: run.sh <kbuild output dir> <static arm64 busybox>
#   make O=$OUT ARCH=arm64 allnoconfig \
#        KCONFIG_ALLCONFIG=tools/testing/fpga-pci-bus/qemu.config
#   make O=$OUT ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules
#
# Environment:
#   MODULE=<path>/fpga-pci-bus.ko  test this module instead of the one in
#                                  the kbuild output dir, e.g. one built
#                                  with out-of-tree/Makefile
set -e

OUT=$(realpath "$1")
BUSYBOX=$(realpath "$2")
HERE=$(dirname "$(realpath "$0")")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

QEMU_ARGS=(-M virt,gic-version=3 -cpu cortex-a57 -smp 2 -m 1G
	   -nographic -no-reboot
	   -object memory-backend-file,id=shm,mem-path=$WORK/shm,size=1M,share=on
	   -device edu,addr=2.0
	   -device ivshmem-plain,memdev=shm,addr=3.0)

# Device tree: QEMU's own, plus the endpoint nodes
qemu-system-aarch64 "${QEMU_ARGS[@]}" -machine dumpdtb="$WORK/virt.dtb" >/dev/null
dtc -q -I dtb -O dts "$WORK/virt.dtb" -o "$WORK/virt.dts"
python3 - "$WORK/virt.dts" "$HERE/endpoints.dtsi" "$WORK/test.dts" <<'PY'
import sys
s = open(sys.argv[1]).read()
nodes = open(sys.argv[2]).read()
anchor = 'compatible = "pci-host-ecam-generic";\n'
i = s.index(anchor, s.index('pcie@10000000 {')) + len(anchor)
s = s[:i] + nodes + s[i:]
s = s.replace('chosen {', 'chosen {\n\t\tbootargs = "console=ttyAMA0 '
	      'uio_pdrv_genirq.of_id=generic-uio";', 1)
open(sys.argv[3], 'w').write(s)
PY
dtc -q -I dts -O dtb "$WORK/test.dts" -o "$WORK/test.dtb"

# initramfs
R=$WORK/rootfs
mkdir -p "$R"/{bin,proc,sys,dev,lib/modules}
cp "$BUSYBOX" "$R/bin/busybox"
for a in sh mount insmod rmmod cat grep echo dmesg devmem dd hexdump ls \
	 sleep poweroff readlink printf head sed cut basename; do
	ln -s busybox "$R/bin/$a"
done
cp "$HERE/init.sh" "$R/init"
cp "${MODULE:-$OUT/drivers/bus/fpga-pci-bus.ko}" "$R/lib/modules/fpga-pci-bus.ko"
(cd "$R" && find . | cpio -o -H newc --quiet) > "$WORK/initramfs.cpio"

qemu-system-aarch64 "${QEMU_ARGS[@]}" -kernel "$OUT/arch/arm64/boot/Image" \
	-initrd "$WORK/initramfs.cpio" -dtb "$WORK/test.dtb" | tee "$WORK/log"

echo "--- host view of ivshmem BAR2 (dirout, dat), expect 0x21 0x20:"
od -A x -t x4 -N 8 "$WORK/shm"
! grep -E "BUG:|WARNING:|possible recursive|inconsistent lock" "$WORK/log"
