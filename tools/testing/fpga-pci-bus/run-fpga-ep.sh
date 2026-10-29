#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot test for drivers/bus/fpga-pci-bus.c with real IP core drivers.
#
# Needs a QEMU 8.2 built with qemu-fpga-ep.diff (patch -p1, then
# --target-list=aarch64-softmmu): a PCI device 1172:e001 with
# the interrupt splitter and a Xilinx uartlite in BAR0, and a Cadence GEM
# in BAR2 that bus masters through the endpoint. Linux binds uartlite and
# macb to them, unmodified.
#
# Usage: run-fpga-ep.sh <qemu-system-aarch64> <kbuild output dir> <busybox>
# (kernel built as described in run.sh)
#
# Environment:
#   INIT=init-irqthreads.sh  run the IRQ thread priority test instead
#   SMP=1                    number of CPUs (default 2)
#   APPEND=threadirqs        extra kernel command line
#   ROOTPORT=1               put the FPGA behind a PCIe root port
#   MODULE=<path>/fpga-pci-bus.ko  test an out-of-tree build, see run.sh
set -e

QEMU=$(realpath "$1")
OUT=$(realpath "$2")
BUSYBOX=$(realpath "$3")
HERE=$(dirname "$(realpath "$0")")
WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$WORK"' EXIT

mkfifo "$WORK/ul.in" "$WORK/ul.out" "$WORK/ser.in" "$WORK/ser.out"
if [ -n "$ROOTPORT" ]; then
	EP_DEV=(-device pcie-root-port,id=rp0,bus=pcie.0,addr=5.0,chassis=1
		-device fpga-ep,bus=rp0,addr=0.0,chardev=ul,serial=ser,netdev=n0)
else
	EP_DEV=(-device fpga-ep,addr=4.0,chardev=ul,serial=ser,netdev=n0)
fi
INIT=${INIT:-init-fpga-ep.sh}
QEMU_ARGS=(-M virt,gic-version=3 -cpu cortex-a57 -smp ${SMP:-2} -m 1G
	   -nographic -no-reboot
	   -chardev pipe,id=ul,path=$WORK/ul
	   -chardev pipe,id=ser,path=$WORK/ser
	   -netdev user,id=n0,guestfwd=tcp:10.0.2.100:8080-cmd:"cat $WORK/http"
	   "${EP_DEV[@]}")

# HTTP response served to the guest through slirp
head -c 200000 /dev/urandom > "$WORK/payload"
{ printf 'HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n' 200000
  cat "$WORK/payload"; } > "$WORK/http"

"$QEMU" "${QEMU_ARGS[@]}" -machine dumpdtb="$WORK/virt.dtb" >/dev/null
dtc -q -I dtb -O dts "$WORK/virt.dtb" -o "$WORK/virt.dts"
python3 - "$WORK/virt.dts" "$HERE/fpga-ep.dtsi" "$WORK/test.dts" \
	"${APPEND:-}" "${ROOTPORT:-}" <<'PY'
import sys
s = open(sys.argv[1]).read()
nodes = open(sys.argv[2]).read()
if sys.argv[5]:
    # behind a root port at 00:05.0: the endpoint is 01:00.0
    nodes = nodes.replace('fpga@4,0 {', 'fpga@0,0 {', 1)
    nodes = nodes.replace('reg = <0x2000 0 0 0 0>;', 'reg = <0x10000 0 0 0 0>;', 1)
    nodes = ('\t\tpcie@5,0 {\n\t\t\tdevice_type = "pci";\n'
             '\t\t\treg = <0x2800 0 0 0 0>;\n\t\t\tbus-range = <1 1>;\n'
             '\t\t\t#address-cells = <3>;\n\t\t\t#size-cells = <2>;\n'
             '\t\t\tranges;\n\n' + nodes + '\t\t};\n')
anchor = 'compatible = "pci-host-ecam-generic";\n'
i = s.index(anchor, s.index('pcie@10000000 {')) + len(anchor)
s = s[:i] + nodes + s[i:]
clk = ('\n\tfpga_clk: fpga-clk {\n\t\tcompatible = "fixed-clock";\n'
       '\t\t#clock-cells = <0>;\n\t\tclock-frequency = <125000000>;\n\t};\n')
i = s.rindex('};')
s = s[:i] + clk + s[i:]
s = s.replace('chosen {', 'chosen {\n\t\tbootargs = "console=ttyAMA0 %s";'
	      % sys.argv[4], 1)
open(sys.argv[3], 'w').write(s)
PY
dtc -q -I dts -O dtb "$WORK/test.dts" -o "$WORK/test.dtb"

R=$WORK/rootfs
mkdir -p "$R"/{bin,proc,sys,dev,lib/modules}
cp "$BUSYBOX" "$R/bin/busybox"
for a in sh mount insmod rmmod cat grep echo dmesg sleep poweroff ls stty \
	 kill ip ping wget md5sum cut tail head printf devmem awk basename \
	 uname od tr hexdump; do
	ln -s busybox "$R/bin/$a"
done
cp "$HERE/$INIT" "$R/init"
"${CROSS_COMPILE:-aarch64-linux-gnu-}gcc" -static -O2 -o "$R/bin/rtprio" \
	"$HERE/rtprio.c"
cp "${MODULE:-$OUT/drivers/bus/fpga-pci-bus.ko}" "$R/lib/modules/fpga-pci-bus.ko"
(cd "$R" && find . | cpio -o -H newc --quiet) > "$WORK/initramfs.cpio"

cat "$WORK/ul.out" > "$WORK/ul.txt" &
cat "$WORK/ser.out" > "$WORK/ser.txt" &
"$QEMU" "${QEMU_ARGS[@]}" -kernel "$OUT/arch/arm64/boot/Image" \
	-initrd "$WORK/initramfs.cpio" -dtb "$WORK/test.dtb" 2>&1 |
	while IFS= read -r line; do
		printf '%s\n' "$line"
		case "$line" in
		UL-READY*) echo "hello-from-host" > "$WORK/ul.in";;
		S-READY*) echo "hello-16550-host" > "$WORK/ser.in";;
		esac
	done | tee "$WORK/log"

echo "--- host received from ttyUL0: $(tr -d '\r' < "$WORK/ul.txt")"
echo "--- host received from ttyS0: $(tr -d '\r' < "$WORK/ser.txt")"
echo "--- payload md5 on host: $(md5sum < "$WORK/payload" | cut -d' ' -f1)"
fail=0
if [ "$INIT" = init-fpga-ep.sh ]; then
	# The data must have made it through every IP core
	check() { grep -q "$2" "$1" || { echo "FAIL: $3"; fail=1; }; }
	check "$WORK/ul.txt" hello-from-guest "uartlite TX"
	check "$WORK/ser.txt" hello-16550-guest "16550 TX"
	check "$WORK/log" "guest received: hello-from-host" "uartlite RX"
	check "$WORK/log" "guest received on ttyS0: hello-16550-host" "16550 RX"
	md5=$(md5sum < "$WORK/payload" | cut -d' ' -f1)
	check "$WORK/log" "payload md5: $md5" "macb download"
	check "$WORK/log" "payload md5 after reload: $md5" "macb download after reload"
	[ $fail = 0 ] && echo "PASS: data through all IP cores"
fi
if grep -E "BUG:|WARNING:|possible recursive|inconsistent lock" "$WORK/log"; then
	echo "FAIL: kernel warnings"
	fail=1
fi
exit $fail
