// SPDX-License-Identifier: GPL-2.0
/*
 * Bus driver for IP cores inside a PCIe-attached FPGA
 *
 * The FPGA is a PCIe endpoint whose BARs expose a number of soft IP cores
 * that already have regular device tree aware platform drivers (Altera
 * SPI, I2C, mSGDMA, ...). Instead of patching each of those drivers to
 * know about PCI, describe the IP cores as children of the endpoint's
 * device tree node and let this driver instantiate them as platform
 * devices:
 *
 *   - the "reg" entries of the children are BAR relative, i.e.
 *     <bar offset size>, and are translated here into the CPU physical
 *     addresses the BARs were assigned at enumeration time;
 *   - the single PCIe interrupt (MSI, MSI-X or INTx) is demultiplexed by a
 *     small interrupt splitter in the FPGA, which is exposed as an
 *     interrupt controller on the endpoint's node, so children simply use
 *     "interrupts = <n IRQ_TYPE_LEVEL_HIGH>".
 *
 * So that child drivers calling of_address_to_resource() or of_iomap() on
 * their own node work as well, a "ranges" property mapping every memory
 * BAR to its PCI bus address is added to the endpoint node, and the OF
 * translation of every child "reg" entry is checked against the resources
 * computed here before the children are created.
 */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Child address <bar offset>, child size <size> */
#define FPB_ADDR_CELLS		2
#define FPB_SIZE_CELLS		1
#define FPB_REG_CELLS		(FPB_ADDR_CELLS + FPB_SIZE_CELLS)

/* phys.hi of a PCI address, see the PCI bus binding */
#define FPB_PCI_SPACE_MEM32	0x02000000
#define FPB_PCI_SPACE_MEM64	0x03000000
#define FPB_PCI_PREFETCH	0x40000000

/* Guard against a stuck source monopolising the CPU in hard IRQ context */
#define FPB_IRQ_MAX_LOOPS	32

/**
 * struct fpb_info - FPGA design specific parameters
 * @irq_bar:		BAR containing the interrupt splitter
 * @irq_offset:		offset of the splitter registers within @irq_bar
 * @irq_status:		pending flags, read only, bit n = source n
 * @irq_enable:		enable register, 1 = source is forwarded to the PCIe IRQ
 * @irq_clear:		write 1 to clear the corresponding pending flag
 * @irq_type:		1 = edge: the flag latches on the active edge of the
 *			input; 0 = level: the flag is re-latched for as long
 *			as the input is active
 * @irq_polarity:	1 = active low / falling edge, 0 = active high / rising
 * @nr_irqs:		number of sources, at most 32
 * @level_only:		the splitter has only STATUS and ENABLE: STATUS shows
 *			the current level of its inputs, which are active high
 *			level interrupts (e.g. Avalon interrupt senders) and
 *			are cleared in the IP cores themselves. @irq_clear,
 *			@irq_type and @irq_polarity are unused.
 *
 * The splitter is expected to raise the PCIe interrupt (send an MSI) on
 * every transition of (STATUS & ENABLE) from zero to non-zero.
 */
struct fpb_info {
	unsigned int irq_bar;
	u32 irq_offset;
	u32 irq_status;
	u32 irq_enable;
	u32 irq_clear;
	u32 irq_type;
	u32 irq_polarity;
	unsigned int nr_irqs;
	bool level_only;
};

static const struct fpb_info fpb_default_info = {
	.irq_bar	= 0,
	.irq_offset	= 0x0,
	.irq_status	= 0x0,
	.irq_enable	= 0x4,
	.irq_clear	= 0x8,
	.irq_type	= 0xc,
	.irq_polarity	= 0x10,
	.nr_irqs	= 32,
};

struct fpga_pci_bus {
	struct pci_dev *pdev;
	const struct fpb_info *info;

	void __iomem *irq_base;
	struct irq_domain *domain;
	/* the vector is an INTx line shared with other devices */
	bool shared;
	/* protects the cached registers below and their hardware copies */
	raw_spinlock_t lock;
	u32 irq_enabled;
	u32 irq_edge;
	u32 irq_low;
};

static void fpb_write_enable(struct fpga_pci_bus *fpb)
{
	writel(fpb->irq_enabled, fpb->irq_base + fpb->info->irq_enable);
}

static void fpb_write_config(struct fpga_pci_bus *fpb)
{
	if (!fpb->info->level_only) {
		writel(fpb->irq_edge, fpb->irq_base + fpb->info->irq_type);
		writel(fpb->irq_low, fpb->irq_base + fpb->info->irq_polarity);
	}
	fpb_write_enable(fpb);
}

/* Size of the splitter register block */
static u32 fpb_irq_regs_size(const struct fpb_info *info)
{
	if (info->level_only)
		return max(info->irq_status, info->irq_enable) + 4;

	return max(max3(info->irq_status, info->irq_enable, info->irq_clear),
		   max(info->irq_type, info->irq_polarity)) + 4;
}

static void fpb_irq_mask(struct irq_data *d)
{
	struct fpga_pci_bus *fpb = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&fpb->lock, flags);
	fpb->irq_enabled &= ~BIT(irqd_to_hwirq(d));
	fpb_write_enable(fpb);
	raw_spin_unlock_irqrestore(&fpb->lock, flags);
}

static void fpb_irq_unmask(struct irq_data *d)
{
	struct fpga_pci_bus *fpb = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&fpb->lock, flags);
	fpb->irq_enabled |= BIT(irqd_to_hwirq(d));
	fpb_write_enable(fpb);
	raw_spin_unlock_irqrestore(&fpb->lock, flags);
}

static void fpb_irq_ack(struct irq_data *d)
{
	struct fpga_pci_bus *fpb = irq_data_get_irq_chip_data(d);

	writel(BIT(irqd_to_hwirq(d)), fpb->irq_base + fpb->info->irq_clear);
}

/*
 * Program how the splitter latches the input and pick the matching flow
 * handler: level sources are kept masked while their handler runs, edge
 * sources are only acknowledged.
 */
static int fpb_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct fpga_pci_bus *fpb = irq_data_get_irq_chip_data(d);
	u32 bit = BIT(irqd_to_hwirq(d));
	unsigned long flags;
	bool edge, low;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
		edge = false;
		low = false;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		edge = false;
		low = true;
		break;
	case IRQ_TYPE_EDGE_RISING:
		edge = true;
		low = false;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		edge = true;
		low = true;
		break;
	default:
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&fpb->lock, flags);
	if (edge)
		fpb->irq_edge |= bit;
	else
		fpb->irq_edge &= ~bit;
	if (low)
		fpb->irq_low |= bit;
	else
		fpb->irq_low &= ~bit;
	fpb_write_config(fpb);
	/* Drop what was latched under the previous configuration */
	writel(bit, fpb->irq_base + fpb->info->irq_clear);
	raw_spin_unlock_irqrestore(&fpb->lock, flags);

	irq_set_handler_locked(d, edge ? handle_edge_irq : handle_level_irq);

	return 0;
}

static struct irq_chip fpb_irq_chip = {
	.name		= "fpga-pci-bus",
	.irq_ack	= fpb_irq_ack,
	.irq_mask	= fpb_irq_mask,
	.irq_unmask	= fpb_irq_unmask,
	.irq_set_type	= fpb_irq_set_type,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};

/*
 * Level only splitter: nothing to configure and nothing to acknowledge,
 * the input drops once the IP core's driver has cleared the cause.
 */
static int fpb_level_irq_set_type(struct irq_data *d, unsigned int type)
{
	return (type & IRQ_TYPE_SENSE_MASK) == IRQ_TYPE_LEVEL_HIGH ? 0 : -EINVAL;
}

static struct irq_chip fpb_level_irq_chip = {
	.name		= "fpga-pci-bus",
	.irq_mask	= fpb_irq_mask,
	.irq_unmask	= fpb_irq_unmask,
	.irq_set_type	= fpb_level_irq_set_type,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};

static int fpb_irq_domain_map(struct irq_domain *d, unsigned int virq,
			      irq_hw_number_t hwirq)
{
	struct fpga_pci_bus *fpb = d->host_data;

	irq_set_chip_data(virq, fpb);
	irq_set_chip_and_handler(virq, fpb->info->level_only ?
				 &fpb_level_irq_chip : &fpb_irq_chip,
				 handle_level_irq);
	irq_set_noprobe(virq);

	return 0;
}

static const struct irq_domain_ops fpb_irq_domain_ops = {
	.map	= fpb_irq_domain_map,
	.xlate	= irq_domain_xlate_twocell,
};

static irqreturn_t fpb_irq_demux(int irq, void *data)
{
	struct fpga_pci_bus *fpb = data;
	irqreturn_t ret = IRQ_NONE;
	unsigned long pending, flags;
	unsigned int loops, bit;
	u32 status;

	for (loops = 0; loops < FPB_IRQ_MAX_LOOPS; loops++) {
		status = readl(fpb->irq_base + fpb->info->irq_status);
		/* All ones: the device is gone (surprise removal, link down) */
		if (status == U32_MAX)
			goto out;

		pending = status & READ_ONCE(fpb->irq_enabled);
		if (!pending)
			goto out;

		for_each_set_bit(bit, &pending, fpb->info->nr_irqs)
			generic_handle_irq(irq_find_mapping(fpb->domain, bit));
		ret = IRQ_HANDLED;
	}

	dev_warn_ratelimited(&fpb->pdev->dev,
			     "interrupts still pending: %#lx\n", pending);

	/*
	 * (STATUS & ENABLE) stays non-zero, so the splitter will not send
	 * another MSI on its own: let it see a new zero to non-zero edge.
	 */
	raw_spin_lock_irqsave(&fpb->lock, flags);
	writel(0, fpb->irq_base + fpb->info->irq_enable);
	fpb_write_enable(fpb);
	raw_spin_unlock_irqrestore(&fpb->lock, flags);

out:
	/*
	 * An MSI that was sent while the loop above was still running finds
	 * nothing left to do. That is expected and must not be counted as
	 * spurious, or the kernel eventually disables the vector. Only a
	 * shared INTx line needs an honest answer.
	 */
	return fpb->shared ? ret : IRQ_HANDLED;
}

static void fpb_irq_domain_remove(void *data)
{
	struct fpga_pci_bus *fpb = data;
	irq_hw_number_t hwirq;
	unsigned int virq;

	for (hwirq = 0; hwirq < fpb->info->nr_irqs; hwirq++) {
		virq = irq_find_mapping(fpb->domain, hwirq);
		if (virq)
			irq_dispose_mapping(virq);
	}
	irq_domain_remove(fpb->domain);

	fpb->irq_enabled = 0;
	fpb_write_enable(fpb);
}

static void fpb_free_irq_vectors(void *data)
{
	pci_free_irq_vectors(data);
}

static int fpb_irq_init(struct fpga_pci_bus *fpb)
{
	const struct fpb_info *info = fpb->info;
	struct pci_dev *pdev = fpb->pdev;
	struct device *dev = &pdev->dev;
	resource_size_t start;
	unsigned long irqflags;
	int ret;

	if (!info->nr_irqs || info->nr_irqs > 32 ||
	    info->irq_bar >= PCI_STD_NUM_BARS)
		return -EINVAL;

	start = pci_resource_start(pdev, info->irq_bar) + info->irq_offset;
	if (!(pci_resource_flags(pdev, info->irq_bar) & IORESOURCE_MEM) ||
	    info->irq_offset + fpb_irq_regs_size(info) >
	    pci_resource_len(pdev, info->irq_bar)) {
		dev_err(dev, "interrupt splitter is outside of BAR%u\n",
			info->irq_bar);
		return -ENXIO;
	}

	/*
	 * Only request the splitter registers: the rest of the BARs is
	 * requested by the child drivers.
	 */
	if (!devm_request_mem_region(dev, start, fpb_irq_regs_size(info),
				     "fpga-pci-bus irq"))
		return -EBUSY;

	fpb->irq_base = devm_ioremap(dev, start, fpb_irq_regs_size(info));
	if (!fpb->irq_base)
		return -ENOMEM;

	/* Everything masked, level high until irq_set_type() says otherwise */
	raw_spin_lock_init(&fpb->lock);
	fpb->irq_enabled = 0;
	fpb->irq_edge = 0;
	fpb->irq_low = 0;
	fpb_write_config(fpb);
	if (!info->level_only)
		writel(GENMASK(info->nr_irqs - 1, 0),
		       fpb->irq_base + info->irq_clear);

	fpb->domain = irq_domain_add_linear(dev->of_node, info->nr_irqs,
					    &fpb_irq_domain_ops, fpb);
	if (!fpb->domain)
		return -ENOMEM;

	ret = devm_add_action_or_reset(dev, fpb_irq_domain_remove, fpb);
	if (ret)
		return ret;

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(dev, fpb_free_irq_vectors, pdev);
	if (ret)
		return ret;

	/* The demux handler must run in hard IRQ context */
	irqflags = IRQF_NO_THREAD;
	fpb->shared = !pdev->msi_enabled && !pdev->msix_enabled;
	if (fpb->shared)
		irqflags |= IRQF_SHARED;

	return devm_request_irq(dev, pci_irq_vector(pdev, 0), fpb_irq_demux,
				irqflags, dev_name(dev), fpb);
}

/* Number of entries in the "reg" property of @np, or a negative error */
static int fpb_reg_count(struct fpga_pci_bus *fpb, struct device_node *np)
{
	int len;

	if (!of_get_property(np, "reg", &len))
		return 0;

	if (!len || len % (FPB_REG_CELLS * sizeof(__be32))) {
		dev_err(&fpb->pdev->dev, "%pOF: malformed reg property\n", np);
		return -EINVAL;
	}

	return len / (FPB_REG_CELLS * sizeof(__be32));
}

/* Translate the BAR relative "reg" entry @i of @np into a CPU address */
static int fpb_reg_to_resource(struct fpga_pci_bus *fpb, struct device_node *np,
			       int i, struct resource *res, bool report)
{
	struct pci_dev *pdev = fpb->pdev;
	const __be32 *reg;
	u32 bar, offset, size;

	reg = of_get_property(np, "reg", NULL);
	reg += i * FPB_REG_CELLS;
	bar = be32_to_cpup(reg);
	offset = be32_to_cpup(reg + 1);
	size = be32_to_cpup(reg + 2);

	if (bar >= PCI_STD_NUM_BARS ||
	    !(pci_resource_flags(pdev, bar) & IORESOURCE_MEM)) {
		if (report)
			dev_err(&pdev->dev, "%pOF: reg %d: BAR%u is not a memory BAR\n",
				np, i, bar);
		return -EINVAL;
	}

	if (!size || offset >= pci_resource_len(pdev, bar) ||
	    size > pci_resource_len(pdev, bar) - offset) {
		if (report)
			dev_err(&pdev->dev, "%pOF: reg %d: exceeds BAR%u\n",
				np, i, bar);
		return -EINVAL;
	}

	res->start = pci_resource_start(pdev, bar) + offset;
	res->end = res->start + size - 1;
	res->flags = IORESOURCE_MEM;
	/* Nest the child resource below the BAR in /proc/iomem */
	res->parent = &pdev->resource[bar];
	if (of_property_read_string_index(np, "reg-names", i, &res->name))
		res->name = np->full_name;

	return 0;
}

static void fpb_remove_ranges(void *data)
{
	struct device_node *np = data;
	struct property *prop = of_find_property(np, "ranges", NULL);

	/* The property stays allocated on the node's list of dead properties */
	if (prop)
		of_remove_property(np, prop);
}

/*
 * Add a "ranges" property mapping <bar 0> to the PCI bus address of each
 * memory BAR, so that the OF core can translate the children's addresses
 * through the PCI bridges to CPU addresses.
 */
static int fpb_add_ranges(struct fpga_pci_bus *fpb)
{
	struct pci_dev *pdev = fpb->pdev;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct property *prop;
	unsigned long flags;
	__be32 *ranges, *r;
	pci_bus_addr_t addr;
	unsigned int bar;
	int ret;

	if (of_find_property(np, "ranges", NULL)) {
		dev_err(dev, "%pOF: must not have a ranges property, it is generated\n",
			np);
		return -EINVAL;
	}

	/* Allocated once: removed properties are never freed */
	prop = kzalloc(sizeof(*prop) + PCI_STD_NUM_BARS * 6 * sizeof(__be32),
		       GFP_KERNEL);
	if (!prop)
		return -ENOMEM;

	ranges = r = (__be32 *)(prop + 1);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		flags = pci_resource_flags(pdev, bar);
		if (!(flags & IORESOURCE_MEM) || !pci_resource_len(pdev, bar))
			continue;

		if (pci_resource_len(pdev, bar) > U32_MAX) {
			dev_err(dev, "BAR%u is too large for #size-cells = <1>\n", bar);
			kfree(prop);
			return -E2BIG;
		}

		addr = pci_bus_address(pdev, bar);
		/* child address: <bar 0> */
		*r++ = cpu_to_be32(bar);
		*r++ = 0;
		/* parent address: PCI memory space */
		*r++ = cpu_to_be32((flags & IORESOURCE_MEM_64 ? FPB_PCI_SPACE_MEM64 :
				    FPB_PCI_SPACE_MEM32) |
				   (flags & IORESOURCE_PREFETCH ? FPB_PCI_PREFETCH : 0));
		*r++ = cpu_to_be32(upper_32_bits(addr));
		*r++ = cpu_to_be32(lower_32_bits(addr));
		/* size */
		*r++ = cpu_to_be32(pci_resource_len(pdev, bar));
	}

	prop->name = "ranges";
	prop->value = ranges;
	prop->length = (r - ranges) * sizeof(__be32);

	ret = of_add_property(np, prop);
	if (ret) {
		kfree(prop);
		return ret;
	}

	return devm_add_action_or_reset(dev, fpb_remove_ranges, np);
}

/*
 * Check that the OF core translates every valid child "reg" entry to the
 * address computed by fpb_reg_to_resource(). A mismatch means that the
 * device tree between the endpoint and the host bridge does not translate
 * PCI addresses correctly, so child drivers using of_address_to_resource()
 * would map the wrong registers.
 */
static int fpb_check_translation(struct fpga_pci_bus *fpb)
{
	struct device_node *np = fpb->pdev->dev.of_node;
	struct device *dev = &fpb->pdev->dev;
	struct resource expected, res;
	struct device_node *child;
	int i, nreg;

	for_each_available_child_of_node(np, child) {
		nreg = fpb_reg_count(fpb, child);

		for (i = 0; i < nreg; i++) {
			/* Broken entries are reported when creating the child */
			if (fpb_reg_to_resource(fpb, child, i, &expected, false))
				continue;

			if (of_address_to_resource(child, i, &res)) {
				dev_err(dev, "%pOF: reg %d: the OF core cannot translate it\n",
					child, i);
				goto fail;
			}

			if (res.start != expected.start || res.end != expected.end) {
				dev_err(dev, "%pOF: reg %d: the OF core translates it to %pR instead of %pR\n",
					child, i, &res, &expected);
				goto fail;
			}
		}
	}

	return 0;

fail:
	dev_err(dev, "check the ranges of the bridges between %pOF and the host bridge\n",
		np);
	of_node_put(child);
	return -EINVAL;
}

static void fpb_unregister_child(void *data)
{
	struct platform_device *child = data;
	struct device_node *np = child->dev.of_node;

	of_node_clear_flag(np, OF_POPULATED);
	platform_device_unregister(child);
}

static int fpb_create_child(struct fpga_pci_bus *fpb, struct device_node *np)
{
	struct device *dev = &fpb->pdev->dev;
	struct platform_device *child;
	struct resource *res;
	int i, nreg, nirq, ret;
	char *name;

	nreg = fpb_reg_count(fpb, np);
	if (nreg < 0)
		return nreg;
	nirq = of_irq_count(np);

	res = kcalloc(nreg + nirq, sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	for (i = 0; i < nreg; i++) {
		ret = fpb_reg_to_resource(fpb, np, i, &res[i], true);
		if (ret)
			goto out_free_res;
	}

	/* Like of_irq_to_resource_table(), stops at the first failing entry */
	nirq = of_irq_to_resource_table(np, res + nreg, nirq);

	/* Same naming scheme as of_platform_populate() */
	if (nreg)
		name = kasprintf(GFP_KERNEL, "%llx.%pOFn",
				 (unsigned long long)res[0].start, np);
	else
		name = kasprintf(GFP_KERNEL, "%s:%pOFn", dev_name(dev), np);
	if (!name) {
		ret = -ENOMEM;
		goto out_free_res;
	}

	child = platform_device_alloc(name, PLATFORM_DEVID_NONE);
	kfree(name);
	if (!child) {
		ret = -ENOMEM;
		goto out_free_res;
	}

	child->dev.parent = dev;
	child->dev.of_node = of_node_get(np);
	child->dev.fwnode = &np->fwnode;

	ret = platform_device_add_resources(child, res, nreg + nirq);
	if (ret)
		goto out_put_child;

	ret = platform_device_add(child);
	if (ret)
		goto out_put_child;

	kfree(res);
	return devm_add_action_or_reset(dev, fpb_unregister_child, child);

out_put_child:
	platform_device_put(child);
out_free_res:
	kfree(res);
	return ret;
}

static void fpb_create_children(struct fpga_pci_bus *fpb)
{
	struct device_node *np = fpb->pdev->dev.of_node;
	struct device_node *child;
	int ret;

	/* Honours status = "disabled" on the children */
	for_each_available_child_of_node(np, child) {
		if (!of_get_property(child, "compatible", NULL))
			continue;

		/* Skip nodes already claimed, e.g. by of_clk_init() */
		if (of_node_test_and_set_flag(child, OF_POPULATED))
			continue;

		/*
		 * A broken child must not take its siblings down with it:
		 * report and carry on.
		 */
		ret = fpb_create_child(fpb, child);
		if (ret) {
			of_node_clear_flag(child, OF_POPULATED);
			dev_err(&fpb->pdev->dev, "%pOF: failed to create device: %d\n",
				child, ret);
		}
	}
}

static int fpb_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct fpga_pci_bus *fpb;
	u32 cells[2];
	int ret;

	if (!np) {
		dev_err(dev, "no device tree node for this device\n");
		return -ENODEV;
	}

	/* The PCI core attaches the node regardless of its status */
	if (!of_device_is_available(np))
		return -ENODEV;

	fpb = devm_kzalloc(dev, sizeof(*fpb), GFP_KERNEL);
	if (!fpb)
		return -ENOMEM;

	fpb->pdev = pdev;
	fpb->info = (const struct fpb_info *)id->driver_data;
	/* IDs added through sysfs new_id have no driver data */
	if (!fpb->info)
		fpb->info = &fpb_default_info;
	pci_set_drvdata(pdev, fpb);

	if (of_property_read_u32(np, "#address-cells", &cells[0]) ||
	    of_property_read_u32(np, "#size-cells", &cells[1]) ||
	    cells[0] != FPB_ADDR_CELLS || cells[1] != FPB_SIZE_CELLS) {
		dev_err(dev, "%pOF: needs #address-cells = <2> and #size-cells = <1>\n",
			np);
		return -EINVAL;
	}

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	/* Needed for MSI and for bus mastering IP cores */
	pci_set_master(pdev);

	if (of_property_read_bool(np, "interrupt-controller")) {
		ret = fpb_irq_init(fpb);
		if (ret) {
			dev_err(dev, "failed to set up interrupt splitter: %d\n", ret);
			return ret;
		}
	}

	ret = fpb_add_ranges(fpb);
	if (ret) {
		dev_err(dev, "failed to add ranges: %d\n", ret);
		return ret;
	}

	ret = fpb_check_translation(fpb);
	if (ret)
		return ret;

	fpb_create_children(fpb);

	return 0;
}

static int __maybe_unused fpb_resume(struct device *dev)
{
	struct fpga_pci_bus *fpb = dev_get_drvdata(dev);
	unsigned long flags;

	if (!fpb->irq_base)
		return 0;

	/* The FPGA may have lost its state while powered down */
	raw_spin_lock_irqsave(&fpb->lock, flags);
	fpb_write_config(fpb);
	raw_spin_unlock_irqrestore(&fpb->lock, flags);

	return 0;
}

static SIMPLE_DEV_PM_OPS(fpb_pm_ops, NULL, fpb_resume);

/*
 * TEST ONLY: QEMU "edu" device. Its IRQ status (0x24) and ack (0x64)
 * registers behave like the splitter; it has no enable, type or polarity
 * registers, so offsets that edu ignores are used for them.
 */
static const struct fpb_info fpb_qemu_edu_info = {
	.irq_bar	= 0,
	.irq_offset	= 0x20,
	.irq_status	= 0x04,
	.irq_enable	= 0x08,
	.irq_clear	= 0x44,
	.irq_type	= 0x0c,		/* ignored by edu */
	.irq_polarity	= 0x10,		/* ignored by edu */
	.nr_irqs	= 32,
};

static const struct pci_device_id fpb_pci_ids[] = {
	/* TEST ONLY: QEMU edu and ivshmem-plain */
	{ PCI_DEVICE(0x1234, 0x11e8),
	  .driver_data = (kernel_ulong_t)&fpb_qemu_edu_info },
	{ PCI_DEVICE(0x1af4, 0x1110),
	  .driver_data = (kernel_ulong_t)&fpb_default_info },
	/* Replace with the IDs of the actual FPGA design */
	{ PCI_DEVICE(PCI_VENDOR_ID_ALTERA, 0xe001),
	  .driver_data = (kernel_ulong_t)&fpb_default_info },
	{ }
};
MODULE_DEVICE_TABLE(pci, fpb_pci_ids);

static struct pci_driver fpb_driver = {
	.name		= "fpga-pci-bus",
	.id_table	= fpb_pci_ids,
	.probe		= fpb_probe,
	.driver.pm	= &fpb_pm_ops,
};
module_pci_driver(fpb_driver);

MODULE_DESCRIPTION("Bus driver for IP cores inside a PCIe-attached FPGA");
MODULE_LICENSE("GPL v2");
