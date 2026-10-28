#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# /init for run.sh
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
echo 8 > /proc/sys/kernel/printk
EDU=/sys/bus/pci/devices/0000:00:02.0
SHM=/sys/bus/pci/devices/0000:00:03.0
irqcount() { grep "fpga-pci-bus *$1 " /proc/interrupts; }
ev() { dd if=$1 bs=4 count=1 2>/dev/null | hexdump -e '1/4 "%u\n"'; }

echo "=== T1: load module"
insmod /lib/modules/fpga-pci-bus.ko
dmesg | grep -i "fpga\|uio\|gpio" | tail -20
echo "--- platform devices"; ls /sys/bus/platform/devices | grep -i "uio\|gpio@"
for d in /sys/bus/platform/devices/*; do
  case "$(readlink $d/driver)" in *uio*|*gpio*) echo "$(basename $d) -> $(basename $(readlink $d/driver)) parent=$(basename $(readlink -f $d/..))";; esac
done
echo "--- iomem"; grep -B1 -A4 "0000:00:0[23].0" /proc/iomem
echo "--- PCI BARs (edu bar0, shm bar2)"; head -1 $EDU/resource; sed -n 3p $SHM/resource
echo "--- edu irq mode: msi_irqs=$(ls $EDU/msi_irqs 2>/dev/null)"

echo "=== T2: address translation via UIO map"
for u in /sys/class/uio/uio*; do echo "$(basename $u): name=$(cat $u/name) addr=$(cat $u/maps/map0/addr) size=$(cat $u/maps/map0/size)"; done
BAR0=$(head -1 $EDU/resource | cut -d' ' -f1)
U0=$(cat /sys/class/uio/uio0/maps/map0/addr)
echo "edu ID register via uio0 map addr: $(devmem $U0 32) (expect 0x010000ED)"

echo "=== T3: level IRQ (bit 3) -> uio0"
irqcount 3
devmem $(printf 0x%x $((BAR0 + 0x60))) 32 0x8
sleep 1
irqcount 3
echo "edu status after (expect 0, acked by flow handler): $(devmem $(printf 0x%x $((BAR0 + 0x24))) 32)"

echo "=== T4: edge IRQ (bit 5) -> uio1, twice with re-enable"
irqcount 5
devmem $(printf 0x%x $((BAR0 + 0x60))) 32 0x20; sleep 1
printf '\001\000\000\000' > /dev/uio1
devmem $(printf 0x%x $((BAR0 + 0x60))) 32 0x20; sleep 1
irqcount 5
echo "(expect uio1 count 2)"

echo "=== T4b: uio0 re-enable delivers a second level IRQ"
printf '\001\000\000\000' > /dev/uio0
devmem $(printf 0x%x $((BAR0 + 0x60))) 32 0x8; sleep 1
irqcount 3
echo "=== T5: masked source (bit 7, no consumer) is ignored"
devmem $(printf 0x%x $((BAR0 + 0x60))) 32 0x80; sleep 1
grep fpga-pci-bus /proc/interrupts; grep -i "0000:00:02.0" /proc/interrupts

echo "=== T6: gpio-mmio in ivshmem BAR2"
for c in /sys/class/gpio/gpiochip*; do echo "$(basename $c) label=$(cat $c/label) ngpio=$(cat $c/ngpio)"; done
B=$(for c in /sys/class/gpio/gpiochip*; do case "$(cat $c/label)" in *gpio*) cat $c/base;; esac; done | head -1)
echo $((B+0)) > /sys/class/gpio/export; echo $((B+5)) > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio$((B+0))/direction; echo 1 > /sys/class/gpio/gpio$((B+0))/value
echo high > /sys/class/gpio/gpio$((B+5))/direction
SHM2=$(sed -n 3p $SHM/resource | cut -d' ' -f1)
echo "dirout=$(devmem $SHM2 32) dat=$(devmem $(printf 0x%x $((SHM2+4))) 32) (expect 0x21 0x21)"
echo 0 > /sys/class/gpio/gpio$((B+0))/value
echo "dat after clearing gpio0=$(devmem $(printf 0x%x $((SHM2+4))) 32) (expect 0x20)"
echo $((B+0)) > /sys/class/gpio/unexport; echo $((B+5)) > /sys/class/gpio/unexport

echo "=== T7: unload and reload"
rmmod fpga-pci-bus && echo "rmmod ok"
ls /sys/bus/platform/devices | grep -ci "uio\|gpio@" ; grep -c fpga-pci-bus /proc/interrupts
insmod /lib/modules/fpga-pci-bus.ko && echo "reload ok"
ls /sys/class/uio; devmem $(cat /sys/class/uio/uio0/maps/map0/addr) 32
devmem $(printf 0x%x $((BAR0 + 0x60))) 32 0x8; sleep 1; irqcount 3
dmesg | grep -i "warn\|bug\|oops\|leak\|refcount" | head
echo "=== DONE"
poweroff -f
