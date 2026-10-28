# fpga-pci-bus: platform IP cores behind a PCIe-attached FPGA

This tree is Linux 5.10 with one addition: **`fpga-pci-bus`**, a PCI driver
that makes the IP cores inside a PCIe-attached FPGA appear as ordinary
device tree platform devices. The stock platform drivers of those cores
(Altera I2C/SPI, Xilinx uartlite, Cadence GEM, ...) then bind without being
patched for PCI.

For the regular kernel README see
[Documentation/admin-guide/README.rst](Documentation/admin-guide/README.rst).

| Branch | Base |
|---|---|
| `fpga-pci-bus-5.10` | v5.10.272 |
| `fpga-pci-bus-5.10-rt` | v5.10.268-rt164 (PREEMPT_RT) |

Both carry the same commits, in this order:

1. `of: irq: export of_irq_count()`
2. `of: export of_add_property()`
3. `of: address: preserve the flags cell on 1:1 translation`. Without it,
   nothing below a PCI bridge with `ranges;` (e.g. a root port) can be
   translated.

   These three are the only changes to existing kernel code. Each matches
   what mainline does: `of_irq_count()` is exported from v7.4-rc1,
   `of_add_property()` is exported, and mainline keeps the flags cell via
   `of_bus::flag_cells`.
4. `dt-bindings: bus: add binding ...`:
   [Documentation/devicetree/bindings/bus/fpga-pci-bus.yaml](Documentation/devicetree/bindings/bus/fpga-pci-bus.yaml)
5. `bus: add driver ...`: [drivers/bus/fpga-pci-bus.c](drivers/bus/fpga-pci-bus.c),
   `CONFIG_FPGA_PCI_BUS`
6. `DO NOT MERGE: fpga-pci-bus: add QEMU test device IDs`: binds the
   driver to the QEMU test devices. Leave it out of production kernels.
7. `tools/testing: add QEMU tests for fpga-pci-bus`:
   [tools/testing/fpga-pci-bus/](tools/testing/fpga-pci-bus/)
8. This README.

> The code was written with an AI assistant (Claude Code). It has no
> `Signed-off-by:`. Whoever takes it over has to review it and certify the
> DCO themselves (see Documentation/process/submitting-patches.rst).

---

## How it works

```
 CPU ── PCIe host bridge ── [root port] ── FPGA endpoint (PCI device)
                                           │
                                 BAR0 ─────┼── IRQ splitter (STATUS/ENABLE/CLEAR/TYPE/POLARITY)
                                           ├── IP core A regs       ── IRQ in 0
                                           ├── IP core B regs       ── IRQ in 1
                                 BAR2 ─────┴── IP core C regs (+DMA) ── IRQ in 2
                                           │
                    one MSI  <─────────────┘  (OR of STATUS & ENABLE)
```

At probe, `fpga-pci-bus`:

1. Takes the endpoint's DT node. The PCI core attaches it from the node
   under the host bridge or root port whose `reg` matches the device and
   function. The driver refuses to bind if there is no node or it has
   `status = "disabled"`.
2. Enables the device and bus mastering.
3. If the node has `interrupt-controller`, sets up the splitter:
   - maps and requests only the splitter registers ("fpga-pci-bus irq" in
     `/proc/iomem`);
   - masks and clears all inputs;
   - registers a linear `irq_domain` on the endpoint node;
   - allocates one vector (MSI, MSI-X or INTx) and requests a demux handler
     with `IRQF_NO_THREAD`.
4. Adds a `ranges` property to the endpoint node. It maps the child
   address `<bar 0>` to the PCI bus address of every memory BAR (with the
   BAR's 32/64-bit and prefetchable flags), so the OF core can translate
   child addresses through the PCI bridges to CPU addresses. Child drivers
   can then use `of_address_to_resource()`, `of_iomap()` and friends on
   their own node (see "Child drivers").
5. Checks that the OF core translates every valid child `reg` entry to the
   same address the driver computes itself. On a mismatch the probe fails,
   with an error naming the child and pointing at the bridge `ranges`. A
   mismatch means a bridge between the endpoint and the host bridge does
   not pass PCI addresses through, and child drivers would map the wrong
   registers.
6. For every available child node with a `compatible`:
   - converts each `reg` entry `<bar offset size>` into
     `pci_resource_start(bar) + offset`, checking it against the BAR;
   - keeps `reg-names`, so `platform_get_resource_byname()` works;
   - resolves `interrupts`/`interrupt-names` through the splitter domain;
   - registers a platform device named `<cpu-address>.<node-name>`, like
     `of_platform_populate()` does, with the child's `of_node`. OF matching,
     `of:` modalias autoloading and `platform_get_irq()` all work as usual.

   Each child resource nests below its BAR in `/proc/iomem`. Nodes with
   `status = "disabled"`, nodes without `compatible` and nodes already
   claimed (e.g. `fixed-clock` by `of_clk_init()`) are skipped. A child with
   a bad `reg` is reported and skipped, and its siblings are still created.

On remove, everything is undone in reverse: children unregistered,
`ranges` removed, IRQ freed, mappings disposed, splitter masked.

The probe fails with a logged error, instead of carrying on with a
half-working setup, when:

- there is no DT node;
- the node is not `#address-cells = <2>` / `#size-cells = <1>`;
- the node already has a `ranges`;
- a BAR is 4 GiB or larger (its size doesn't fit one size cell);
- the splitter cannot be set up;
- the OF translation check fails.

A child with a broken `reg` (missing BAR, out of range) is only reported
and skipped.

### Interrupt demultiplexing

The demux handler reads `STATUS & ENABLE` and calls `generic_handle_irq()`
for every set bit. It loops until nothing is pending, at most 32 times.
Reading `0xffffffff` is treated as "device gone" (surprise removal, link
down) and ends the loop.

`irq_set_type()` programs the splitter input to match what the consumer
asks for, from the DT `interrupts` cell or e.g. `IRQF_TRIGGER_RISING` in
the child driver. It also picks the flow handler and clears anything
latched under the old setting:

| Trigger type | TYPE bit | POLARITY bit | Flow handler |
|---|---|---|---|
| `IRQ_TYPE_LEVEL_HIGH` (default) | 0 | 0 | `handle_level_irq`: mask, ack, handle, unmask |
| `IRQ_TYPE_LEVEL_LOW` | 0 | 1 | `handle_level_irq` |
| `IRQ_TYPE_EDGE_RISING` | 1 | 0 | `handle_edge_irq`: ack, handle |
| `IRQ_TYPE_EDGE_FALLING` | 1 | 1 | `handle_edge_irq` |

Mask, unmask and type updates hold a raw spinlock and work from cached
copies of ENABLE, TYPE and POLARITY. Those registers are therefore never
read back, and are restored on resume.

---

## Device tree

The endpoint node goes where PCI device nodes go: below the host bridge, or
below the root port when there is one. Its `reg` encodes the device and
function: `<(dev << 11 | fn << 8) 0 0 0 0>`. The bus number is ignored for
matching. Full schema:
[fpga-pci-bus.yaml](Documentation/devicetree/bindings/bus/fpga-pci-bus.yaml).

```dts
#include <dt-bindings/interrupt-controller/irq.h>

fpga_clk: fpga-clk {                      /* put fixed clocks outside the endpoint */
	compatible = "fixed-clock";
	#clock-cells = <0>;
	clock-frequency = <125000000>;
};

&pcie {                                   /* your host bridge */
	pcie@0,0 {                            /* root port, if present */
		device_type = "pci";
		reg = <0x0000 0 0 0 0>;
		#address-cells = <3>;
		#size-cells = <2>;
		ranges;

		fpga@0,0 {
			compatible = "pci1172,e001";   /* your vendor,device */
			reg = <0x10000 0 0 0 0>;       /* bus 1, dev 0, fn 0 */
			#address-cells = <2>;          /* <bar offset> */
			#size-cells = <1>;
			interrupt-controller;          /* omit if there is no splitter */
			#interrupt-cells = <2>;        /* <input type> */

			i2c@0,100 {
				compatible = "altr,softip-i2c-v1.0";
				reg = <0 0x100 0x40>;      /* BAR0 + 0x100 */
				interrupts = <0 IRQ_TYPE_LEVEL_HIGH>;
				clocks = <&fpga_clk>;
				clock-frequency = <100000>;
				fifo-size = <4>;
				#address-cells = <1>;
				#size-cells = <0>;
			};

			ethernet@2,0 {
				compatible = "cdns,gem";
				reg = <2 0x0 0x800>;       /* BAR2 + 0 */
				interrupts = <1 IRQ_TYPE_LEVEL_HIGH>;
				clocks = <&fpga_clk>, <&fpga_clk>;
				clock-names = "pclk", "hclk";
				phy-mode = "rgmii-id";
			};

			spi@1,0 {
				compatible = "altr,spi-1.0";
				reg = <1 0x0 0x20>;        /* BAR1 */
				interrupts = <2 IRQ_TYPE_LEVEL_HIGH>;
				status = "disabled";       /* skipped */
			};
		};
	};
};
```

- **Cells:** the endpoint must have `#address-cells = <2>` and
  `#size-cells = <1>`. The 5.10 OF core reads three address cells into one
  u64, which loses the BAR index, so `<bar offset-hi offset-lo>` cannot be
  translated and is rejected.
- **No `ranges` in the DT:** the driver generates the endpoint's `ranges`
  once the BARs are assigned. Bridges between the endpoint and the host
  bridge need a translating `ranges`; for a root port, an empty `ranges;`
  works (with the flags-cell backport above).
- **Choosing BARs:** each `reg` entry picks its own BAR, so one IP core can
  have registers in one BAR and buffers in another.
- **Interrupt parent:** children need no `interrupt-parent`, because the
  endpoint node is their nearest interrupt controller.
- **Grandchildren** (e.g. `mdio` below an ethernet node) are left to the
  child's own driver, as with `of_platform_populate()`.

---

## Integrating with real hardware

### 1. PCI IDs

Put the IDs of your design in `fpb_pci_ids[]` in `drivers/bus/fpga-pci-bus.c`.
The placeholder is Altera `1172:e001`. Use the same pair in the DT
`compatible = "pciVVVV,DDDD"`. With several FPGA designs, add one entry each,
with its own `struct fpb_info` as `driver_data`.

### 2. The interrupt splitter

The driver expects a splitter with this layout. All registers are 32-bit,
little endian, and only accessed as 32-bit words. Bit *n* is input *n*.

| Offset | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | STATUS | RO | latched pending flags; reading must not clear them |
| 0x04 | ENABLE | RW | 1 = input is forwarded to the PCIe interrupt (0 = masked) |
| 0x08 | CLEAR | WO | write 1 to clear the flag (W1C) |
| 0x0C | TYPE | RW | 1 = edge: flag latches on the active edge; 0 = level: flag is re-latched for as long as the input is active |
| 0x10 | POLARITY | RW | 1 = active low / falling edge; 0 = active high / rising edge |

The layout is the `struct fpb_info` passed as `driver_data`:

```c
static const struct fpb_info fpb_default_info = {
	.irq_bar	= 0,       /* BAR holding the splitter */
	.irq_offset	= 0x0,     /* splitter base within that BAR */
	.irq_status	= 0x0,     /* register offsets relative to irq_offset */
	.irq_enable	= 0x4,
	.irq_clear	= 0x8,
	.irq_type	= 0xc,
	.irq_polarity	= 0x10,
	.nr_irqs	= 32,      /* number of inputs, <= 32 */
};
```

Requirements for the HDL:

- **MSI generation:** send an MSI on every transition of
  `(STATUS & ENABLE) != 0` from false to true. The driver relies on this.
  - It loops until nothing is pending, so a flag that latches meanwhile is
    picked up.
  - A level source unmasked while still pending produces a new 0→1
    transition and thus a new MSI.
  - A splitter that only sends an MSI per input edge, or a single MSI that
    is never re-armed, will lose interrupts.
  - With INTx instead, assert the line while `(STATUS & ENABLE) != 0`.
- **Level inputs** must re-latch after CLEAR while still active. That is
  what makes `handle_level_irq` work: mask, clear, run the handler (which
  quiets the source), unmask.
- **Edge inputs** latch once per active edge. Use edge capture for IP cores
  that pulse their interrupt (e.g. Xilinx uartlite). If a pulse input is
  level-latched, its flag can never be cleared.
- **Polarity:** inversion is optional in hardware; it can also be fixed in
  the HDL. If you implement it, make the flag latch according to
  `input ^ POLARITY`.
- **Bit 31:** keep at least one STATUS bit reserved as zero, e.g. use at
  most 31 inputs and set `nr_irqs` accordingly. A STATUS of `0xffffffff` is
  read as "device gone", so 32 inputs all pending at once would be dropped.
- **Placement:** keep the splitter registers out of every child's `reg`
  range. The driver requests them, so an overlapping child fails to probe
  with `-EBUSY`.

If your splitter differs, adapt these places in `fpga-pci-bus.c`:

| Your hardware | Change |
|---|---|
| Different offsets or BAR, fewer inputs | only the `fpb_info` |
| MASK register (1 = masked) instead of ENABLE | `fpb_write_enable()`: write `~fpb->irq_enabled` |
| No TYPE/POLARITY registers (fixed in HDL) | make `fpb_write_config()` skip them, and have `fpb_irq_set_type()` only accept the type the HDL implements |
| STATUS is read-to-clear | `fpb_irq_ack()` becomes a no-op; keep the value read in `fpb_irq_demux()` |
| Separate SET/CLEAR enable registers | `fpb_irq_mask()` / `fpb_irq_unmask()` write the single bit; the cache can stay |
| One MSI-X vector per input | not needed here; the splitter design is the point of this driver |

### 3. Child drivers

Most platform drivers work unchanged. Check these points:

- **Resources:** both ways work.
  - The platform device API: `platform_get_resource*()`,
    `devm_platform_ioremap_resource*()`, `platform_get_irq*()`.
  - The OF API on the driver's own node: `of_address_to_resource()`,
    `of_iomap()` (e.g. `gpio-altera` via `of_mm_gpiochip_add_data()`),
    `of_irq_get()` / `irq_of_parse_and_map()`. This works thanks to the
    generated `ranges`.

  `8250_of` (`ns16550a`), which uses `of_address_to_resource()`, is tested
  this way.
- **OF match table:** the driver needs one. 5.10's `altera-msgdma` has none
  (added in later kernels) and needs that small backport.
- **Clocks:** IP cores need clocks like on an SoC; use `fixed-clock` nodes.
- **DMA by IP cores** (bus masters in the FPGA) goes through the PCIe
  endpoint:
  - Children inherit `dma-coherent` from the host bridge node through the
    normal OF DMA setup, and get the default 32-bit mask unless their
    driver sets a wider one.
  - `dma-ranges` is only followed while every level has one. If the host
    bridge's `dma-ranges` has an offset, also add an empty `dma-ranges;`
    to the endpoint node and the root port node.
  - This was tested with the Cadence GEM, with no IOMMU.
  - Behind an **IOMMU in translating mode**, child devices are not in the
    endpoint's IOMMU group, so their DMA will fault. Boot with
    `iommu.passthrough=1` or keep the endpoint in passthrough; this is not
    handled yet.
- **Affinity:** child IRQs follow the single MSI and cannot be moved
  individually (no `irq_set_affinity`). Move the MSI's affinity instead.

### 4. Bring-up checklist

```sh
lspci -nn                            # IDs match fpb_pci_ids[]
ls /sys/bus/pci/devices/0000:01:00.0/of_node   # DT node attached
hexdump -v -e '4/1 "%02x" " "' /sys/bus/pci/devices/0000:01:00.0/of_node/ranges
                                     # generated: <bar 0> <pci-addr (3 cells)> <size>, per BAR
dmesg | grep fpga-pci-bus            # errors name the offending child node
grep -A10 '0000:01:00.0' /proc/iomem # children nested under their BARs
grep fpga-pci-bus /proc/interrupts   # one line per child IRQ (input, type)
cat /sys/kernel/debug/irq/domains/*fpga*  # with CONFIG_GENERIC_IRQ_DEBUGFS
```

The child devices are under `/sys/bus/pci/devices/<bdf>/`, named
`<address>.<node>`.

### 5. PREEMPT_RT

The `-rt` branch carries the same code; no changes were needed:

- The demux handler is `IRQF_NO_THREAD`, so it stays in hard IRQ context
  and only does MMIO and `generic_handle_irq()`.
- The chip callbacks use a raw spinlock.
- Child handlers are force-threaded with `IRQF_ONESHOT`. Level inputs stay
  masked until their thread finishes; edge inputs are only acked.

One caveat: in the INTx fallback, the `IRQF_SHARED | IRQF_NO_THREAD` handler
cannot share its line with a force-threaded handler. MSI is not affected.

### 6. Threaded child handlers and their priorities

Each child interrupt is a separate Linux IRQ, so it gets its own IRQ
thread like any other interrupt. Nothing in the child driver or the DT has
to change.

- **When there are threads:**
  - On **PREEMPT_RT**, every handler requested with plain `request_irq()`
    is force-threaded.
  - On **non-RT** kernels this happens with the `threadirqs` boot option.
  - A driver using `request_threaded_irq()` gets a thread in either case.
  - On non-RT kernels without `threadirqs`, the child handlers run in hard
    IRQ context, called from the demux handler.
- **What you see:**
  - Threads are named `irq/<virq>-<name>` and start as SCHED_FIFO priority
    50, the kernel default for IRQ threads.
  - Find the virq in `/proc/interrupts`, on the `fpga-pci-bus <input>`
    line.
  - Some drivers request their IRQ before they know their final name; macb
    shows up as `irq/52-eth%d`.
  - The demux handler itself has no thread.
- **Priorities:** set them with the usual tools, e.g.
  `chrt -f -p 80 $(pgrep 'irq/52-')` or `chrt -r -p ...`. They persist.
- **CPU affinity:** pin a thread with `taskset -p`; that persists too.
  `/proc/irq/<child>/smp_affinity` cannot be written (EIO), because child
  IRQs follow the PCIe MSI. Move the MSI's affinity instead.
- **Masking while a thread is pending:**
  - Forced threading implies `IRQF_ONESHOT`. A level input therefore stays
    masked in the splitter (its ENABLE bit clear) from the hard interrupt
    until its thread has finished. A thread that cannot run causes no
    interrupt storm, and the other inputs keep working.
  - Edge inputs are not masked; a new edge re-runs the thread.

Verified in QEMU (test 3 below) on 5.10.268-rt164 and on 5.10.272 with
`threadirqs`. Setup: one CPU, a SCHED_FIFO busy loop as competing load, RT
throttling disabled. Each line is the maximum RTT of 3 pings through the
macb behind the splitter:

| eth0 IRQ thread | competing load | RT kernel | non-RT, `threadirqs` |
|---|---|---|---|
| FIFO 50 (default) | FIFO 40 | 4 ms | 17 ms |
| FIFO 30 | FIFO 40 | replies held ~3.9 s until the load ends | ~3.7 s |
| FIFO 80 | FIFO 70 | 4 ms | 9 ms |
| RR 75 | FIFO 70 | 4 ms | 9 ms |

On 5.10-rt, macb's NAPI receive processing runs inside its IRQ thread, so
the thread's priority covers the whole receive path. In the starved case,
the eth0 input was masked and only one MSI arrived.

Two things to keep in mind when tuning a real RT system:

- **Timers on 5.10-rt** expire in `ksoftirqd/N`, which is SCHED_OTHER by
  default. RT load above it delays every timer (timeouts, `alarm()`,
  driver watchdogs), so give it a suitable RT priority. Later RT kernels
  moved timers to separate `ktimers` threads.
- **RT throttling:** by default RT tasks get 95% of the CPU
  (`/proc/sys/kernel/sched_rt_runtime_us`). The remaining 5% lets
  SCHED_OTHER work run; it can hide or cause latency effects.

---

## Testing

Everything is in [tools/testing/fpga-pci-bus/](tools/testing/fpga-pci-bus/)
and runs QEMU arm64 `virt` with a minimal kernel (`allnoconfig` plus
[qemu.config](tools/testing/fpga-pci-bus/qemu.config), which includes KASAN,
lockdep and sleep-in-atomic checks). It needs the "DO NOT MERGE" commit, a
`aarch64-linux-gnu-` toolchain, `dtc`, `cpio`, `python3`, and a static arm64
busybox (e.g. Debian/Ubuntu `busybox-static:arm64`, `/usr/bin/busybox-static`).

```sh
OUT=$PWD/../build-qemu
make O=$OUT ARCH=arm64 allnoconfig \
     KCONFIG_ALLCONFIG=tools/testing/fpga-pci-bus/qemu.config     # qemu-rt.config on -rt
make O=$OUT ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image modules
```

### Test 1: stock QEMU (`run.sh`)

```sh
tools/testing/fpga-pci-bus/run.sh $OUT /path/to/busybox-static
```

The FPGA is played by QEMU's `edu` device, whose IRQ status/ack registers
act as the splitter. A second endpoint, `ivshmem-plain`, has host shared
memory in a 64-bit BAR2 that hosts the stock `gpio-mmio` driver. The test
checks:

- BAR relative translation: the edu ID register reads `0x010000ED` through
  the child's mapping. `ivshmem`'s BAR2 is 64-bit prefetchable above 4 GiB,
  so the generated `ranges` and the translation check cover that kind of
  BAR too.
- Level IRQ through the domain (`uio_pdrv_genirq`), acked by the flow
  handler, re-delivered after re-enable.
- Edge IRQ delivered twice.
- A pending input nobody enabled is ignored.
- `gpio-mmio` writes appear in the host's shared memory file (`0x21`, then
  `0x20`).
- `status = "disabled"` children are not created, and a child pointing at a
  missing BAR is rejected without affecting the others.
- rmmod/insmod is clean.

### Test 2: real IP-core drivers (`run-fpga-ep.sh`)

[qemu-fpga-ep.diff](tools/testing/fpga-pci-bus/qemu-fpga-ep.diff) adds a
QEMU 8.2 device `fpga-ep` (`1172:e001`, the driver's default ID and
splitter layout) that models such an FPGA:

- BAR0 holds the splitter at 0x0, a Xilinx uartlite at 0x1000 (input 0)
  and a 16550 at 0x2000 (input 2).
- BAR2 holds a Cadence GEM (input 1). Its DMA goes through the endpoint's
  bus-master address space, like an FPGA DMA master.

The diff also makes QEMU's uartlite pulse its interrupt like the real IP;
the stock model holds it high forever.

```sh
tools/testing/fpga-pci-bus/build-qemu.sh /path/to/qemu-8.2.2
tools/testing/fpga-pci-bus/run-fpga-ep.sh /path/to/qemu-8.2.2/build-fpga-ep/qemu-system-aarch64 \
        $OUT /path/to/busybox-static
ROOTPORT=1 tools/testing/fpga-pci-bus/run-fpga-ep.sh ...   # endpoint behind a PCIe root port
```

Stock `uartlite`, `8250_of` and `macb` bind to the children. `8250_of`
finds its registers with `of_address_to_resource()`. The test checks:

- TTY data in both directions on the uartlite and on the 16550.
- Ping, and a 200 KB HTTP download whose MD5 must match the host's, before
  and after a module reload.
- The splitter TYPE/ENABLE registers as programmed by the driver. uartlite
  asks for `IRQF_TRIGGER_RISING`, so its input becomes edge.
- The generated `ranges` (dumped from sysfs) and the `/proc/iomem` nesting.

It prints `PASS: data through all IP cores` when every transfer arrived
intact. With `ROOTPORT=1`, the endpoint sits behind a `pcie-root-port`
whose DT node has `ranges;`. Without the flags-cell backport, the driver
refuses to probe there with "the OF core cannot translate it". With it,
everything passes.

### Test 3: IRQ threads and priorities (`run-fpga-ep.sh` with `init-irqthreads.sh`)

```sh
INIT=init-irqthreads.sh SMP=1 tools/testing/fpga-pci-bus/run-fpga-ep.sh \
        <qemu> $OUT /path/to/busybox-static              # PREEMPT_RT kernel
INIT=init-irqthreads.sh SMP=1 APPEND=threadirqs tools/testing/fpga-pci-bus/run-fpga-ep.sh \
        <qemu> $OUT /path/to/busybox-static              # non-RT kernel
```

It uses `rtprio`, a small chrt replacement built from
[rtprio.c](tools/testing/fpga-pci-bus/rtprio.c), because busybox has no
`chrt`. The test:

1. lists the child IRQ threads and their default policy;
2. shows that eth0 traffic runs in its thread (the thread's context
   switches increase);
3. runs pings against a SCHED_FIFO busy loop with the eth0 thread above,
   below, and again above it (SCHED_FIFO and SCHED_RR);
4. reads the splitter ENABLE register while the thread is starved.

The results are in the table in "Threaded child handlers" above. On a
non-RT kernel without `threadirqs` it reports that the handlers are not
threaded.

All scripts exit non-zero on any `BUG:`/`WARNING:`/lockdep report.

### Results

| | v5.10.272 (KASAN, lockdep) | v5.10.268-rt164 (lockdep) |
|---|---|---|
| `run.sh` | pass | pass |
| `run-fpga-ep.sh` | pass | data checks pass; fails on a macb lockdep report, see below |
| `ROOTPORT=1 run-fpga-ep.sh` | pass | data checks pass; same macb lockdep report |
| `init-irqthreads.sh` | pass with `threadirqs`; without it the handlers are not threaded, as expected | functional checks pass; same macb lockdep report |

The bridge driver itself showed no lockdep, KASAN or atomic-sleep reports
on either kernel. Findings in other code:

- **macb (5.10) use-after-free on unbind while the interface is up.**
  `macb_remove()` frees the MDIO bus before `unregister_netdev()`. A plain
  `echo ... > /sys/bus/platform/drivers/macb/unbind` reproduces it without
  this driver; later kernels fixed the order. The test takes `eth0` down
  before rmmod.
- **macb on PREEMPT_RT:** lockdep reports a circular dependency
  `bp->lock → softirq_ctrl.lock → _xmit_ETHER`. It comes from
  `macb_mac_link_up()` allocating with BHs disabled under `bp->lock`; no
  fpga-pci-bus frames are involved.
- **KASAN on 5.10-rt** reports sleeping in atomic context in `call_rcu()`
  (fixed in 5.15+), so `qemu-rt.config` leaves KASAN out.

### Provenance of the RT base

kernel.org was not reachable from the build machine. `v5.10.268-rt164`
(commit `6119c34b6412`, base `v5.10.268`) came from the stable-rt tags
mirrored in https://gitlab.com/cip-project/cip-kernel/linux-cip.git and is
applied as a single commit. The tag signature (Luis Claudio R. Goncalves,
key `9354064999728D31D464D140F394A423F8E67C26`) was **not** verified. Check
it, or re-apply the RT patch from kernel.org, before relying on this branch.

---

## Limitations and possible follow-ups

- Only memory BARs are supported (no I/O BARs), and each must be smaller than
  4 GiB (one size cell).
- No per-child IOMMU handling (see DMA above).
- Children are created at probe only. Reloading a different FPGA bitstream
  means unbinding and rebinding the PCI device, or rescanning it.
- With mainline kernels this could build on `CONFIG_PCI_DYNAMIC_OF_NODES`
  and a DT overlay applied to the endpoint, as `drivers/misc/lan966x_pci.c`
  does. The `<bar offset>` child layout used here is compatible with that.
