#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# /init for run-fpga-ep.sh: are the child IRQ handlers threaded, and do
# scheduling policies and priorities set on their threads take effect?
#
# Run with SMP=1 so that a SCHED_FIFO CPU hog competes with the IRQ
# threads. On non-RT kernels use APPEND=threadirqs to force threading.
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
EP=$(for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor):$(cat $d/device)" = 0x1172:0xe001 ] && basename $d; done)

# virq of the fpga-pci-bus input $1
virq() { awk -v hw="$1" '$0 ~ /fpga-pci-bus/ && $(NF-2) == hw { sub(":", "", $1); print $1 }' /proc/interrupts; }
# pid of the task named $1 (shell pattern)
pidof_comm() { for c in /proc/[0-9]*/comm; do case "$(cat $c 2>/dev/null)" in $1) basename ${c%/comm};; esac; done; }
csw() { awk '/^voluntary_ctxt_switches/ { print $2 }' /proc/$1/status; }
msi() { awk '/ITS-MSI.*'"$EP"'/ { print $2 }' /proc/interrupts; }
BAR0=$(head -1 /sys/bus/pci/devices/$EP/resource | cut -d' ' -f1)
enable() { devmem $(printf 0x%x $((BAR0 + 4))) 32; }
# 3 pings, 1 s apart: "<replies>/3 replies, max RTT <ms> ms"
pings() {
	ping -c 3 -w 10 10.0.2.2 | awk '
		/time=/ { sub(".*time=", ""); t = $1 + 0; if (t > max) max = t }
		/packets transmitted/ { r = $4 }
		END { printf "%d/3 replies, max RTT %d ms\n", r, max }'
}
hog() { rtprio hog $1 6 & HOG=$!; sleep 1; }

echo "=== I0: setup ($(uname -r), cmdline: $(cat /proc/cmdline))"
insmod /lib/modules/fpga-pci-bus.ko
ip link set eth0 up; ip addr add 10.0.2.15/24 dev eth0
stty -F /dev/ttyUL0 115200 raw -echo
cat /dev/ttyUL0 > /dev/null &
sleep 3
rtprio set $$ fifo 90 && echo "test shell: $(rtprio get $$)"
# The hog must really keep lower priorities off the CPU, otherwise
# SCHED_OTHER work still runs for 5% of every second
echo -1 > /proc/sys/kernel/sched_rt_runtime_us
echo "sched_rt_runtime_us: $(cat /proc/sys/kernel/sched_rt_runtime_us)"
# Timers expire in ksoftirqd on 5.10-rt (SCHED_OTHER by default) and ping
# needs them, so keep ksoftirqd above the hogs. NAPI raised by the eth0
# IRQ thread runs in that thread, so this does not bypass its priority.
KSD=$(pidof_comm ksoftirqd/0)
rtprio set $KSD fifo 85 && echo "ksoftirqd/0: $(rtprio get $KSD)"

ETH=$(virq 1); UL=$(virq 0)
PARENT=$(awk '/ITS-MSI.*'"$EP"'/ { sub(":", "", $1); print $1 }' /proc/interrupts)
ETHT=$(pidof_comm "irq/$ETH-*"); ULT=$(pidof_comm "irq/$UL-*"); PT=$(pidof_comm "irq/$PARENT-*")
echo "=== I1: threads"
echo "eth0     (virq $ETH): ${ETHT:-no thread} $(cat /proc/$ETHT/comm 2>/dev/null) $([ -n "$ETHT" ] && rtprio get $ETHT)"
echo "uartlite (virq $UL): ${ULT:-no thread} $(cat /proc/$ULT/comm 2>/dev/null) $([ -n "$ULT" ] && rtprio get $ULT)"
echo "demux    (virq $PARENT): ${PT:-no thread (IRQF_NO_THREAD, expected)}"
if [ -z "$ETHT" ]; then
	echo "child handlers are not threaded on this kernel"
	echo "=== DONE"; poweroff -f
fi

echo "=== I2: eth0 traffic is handled in its thread"
B=$(csw $ETHT); pings; echo "eth0 thread voluntary context switches: $B -> $(csw $ETHT)"

echo "=== I3: thread at default SCHED_FIFO 50, hog at 40"
hog 40; pings | sed 's/$/ (expect small RTT)/'; wait $HOG

echo "=== I4: thread lowered to SCHED_FIFO 30, hog at 40"
rtprio set $ETHT fifo 30 && echo "eth0 thread: $(rtprio get $ETHT)"
hog 40
M=$(msi)
ping -c 1 -w 10 10.0.2.2 > /dev/null &
sleep 1
echo "during the hog: ENABLE=$(enable) (expect bit 1 clear: masked until the thread runs), MSIs $M -> $(msi)"
pings | sed 's/$/ (expect replies held back until the hog ends)/'
wait $HOG

echo "=== I5: thread raised to SCHED_FIFO 80, hog at 70"
rtprio set $ETHT fifo 80 && echo "eth0 thread: $(rtprio get $ETHT)"
hog 70; pings | sed 's/$/ (expect small RTT)/'; wait $HOG
echo "eth0 thread after traffic: $(rtprio get $ETHT)"

echo "=== I6: SCHED_RR 75, hog at 70"
rtprio set $ETHT rr 75 && echo "eth0 thread: $(rtprio get $ETHT)"
hog 70; pings | sed 's/$/ (expect small RTT)/'; wait $HOG

echo "=== I7: uartlite thread"
rtprio set $ULT fifo 70 && echo "uartlite thread: $(rtprio get $ULT)"
echo "=== DONE"
poweroff -f
