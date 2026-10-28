#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# /init for run-fpga-ep.sh
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
EP=$(for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor):$(cat $d/device)" = 0x1172:0xe001 ] && basename $d; done)
echo 8 > /proc/sys/kernel/printk
irqs() { grep "fpga-pci-bus" /proc/interrupts; grep "$EP" /proc/interrupts; }

echo "=== F1: load module"
insmod /lib/modules/fpga-pci-bus.ko
sleep 1
dmesg | grep -i "fpga\|uartlite\|ttyUL\|macb\|eth0" | tail -12
echo "--- endpoint $EP, generated ranges:$(hexdump -v -e '4/1 "%02x" " "' /sys/bus/pci/devices/$EP/of_node/ranges 2>&1)"
echo "--- iomem"; grep -A7 "$EP" /proc/iomem
irqs

BAR0=$(head -1 /sys/bus/pci/devices/$EP/resource | cut -d' ' -f1)
reg() { devmem $(printf 0x%x $((BAR0 + $1))) 32; }
echo "splitter (after probe: macb requested its IRQ): type=$(reg 0xc) polarity=$(reg 0x10) enable=$(reg 0x4)"

echo "=== F2: uartlite TX"
stty -F /dev/ttyUL0 115200 raw -echo
echo "hello-from-guest" > /dev/ttyUL0
echo "=== F3: uartlite RX"
# Open the port before telling the host to send: opening clears the FIFO
exec 3< /dev/ttyUL0
cat <&3 > /rx &
CAT=$!
sleep 1
echo "splitter (ttyUL0 open, edge): type=$(reg 0xc) enable=$(reg 0x4) (expect 0x1 0x3)"
echo "UL-READY"
sleep 3
kill $CAT; exec 3<&-
echo "guest received: $(cat /rx)"
irqs

echo "=== F3b: 16550 (8250_of, of_address_to_resource) RX and TX"
dmesg | grep "ttyS0"
stty -F /dev/ttyS0 115200 raw -echo
echo "hello-16550-guest" > /dev/ttyS0
exec 3< /dev/ttyS0
cat <&3 > /rx2 &
CAT=$!
sleep 1
echo "S-READY"
sleep 3
kill $CAT; exec 3<&-
echo "guest received on ttyS0: $(cat /rx2)"
irqs

echo "=== F4: macb over BAR2, DMA through the endpoint"
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
sleep 3
cat /sys/class/net/eth0/operstate
ping -c 3 -W 2 10.0.2.2
wget -q -O /payload http://10.0.2.100:8080/payload && echo "wget ok"
echo "payload md5: $(md5sum /payload | cut -d' ' -f1)"
echo "splitter (ttyUL0 closed again): type=$(reg 0xc) enable=$(reg 0x4) (expect 0x1 0x2)"
irqs

echo "=== F5: unload and reload"
# 5.10 macb_remove() frees the MDIO bus before unregister_netdev(), which
# is a use-after-free when the interface is up (fixed in later kernels)
ip link set eth0 down
rmmod fpga-pci-bus && echo "rmmod ok"
ls /sys/class/net; ls /dev/ttyUL0 2>&1
insmod /lib/modules/fpga-pci-bus.ko && echo "reload ok"
sleep 1
ip link set eth0 up; ip addr add 10.0.2.15/24 dev eth0; sleep 3
wget -q -O /payload2 http://10.0.2.100:8080/payload && echo "payload md5 after reload: $(md5sum /payload2 | cut -d' ' -f1)"
echo "=== DONE"
poweroff -f
