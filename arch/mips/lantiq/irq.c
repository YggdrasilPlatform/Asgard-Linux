// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2010 John Crispin <john@phrozen.org>
 * Copyright (C) 2010 Thomas Langer <thomas.langer@lantiq.com>
 */

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/irqdomain.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <asm/bootinfo.h>
#include <asm/irq_cpu.h>

#include <lantiq_soc.h>
#include <irq.h>

/* register definitions - internal irqs */
#define LTQ_ICU_ISR		0x0000
#define LTQ_ICU_IER		0x0008
#define LTQ_ICU_IOSR		0x0010
#define LTQ_ICU_IRSR		0x0018
#define LTQ_ICU_IMR		0x0020

#define LTQ_ICU_IM_SIZE		0x28

/* register definitions - external irqs */
#define LTQ_EIU_EXIN_C		0x0000
#define LTQ_EIU_EXIN_INIC	0x0004
#define LTQ_EIU_EXIN_INC	0x0008
#define LTQ_EIU_EXIN_INEN	0x000C

/* number of external interrupts */
#define MAX_EIU			6

/* the performance counter */
#define LTQ_PERF_IRQ		(INT_NUM_IM4_IRL0 + 31)

/*
 * irqs generated by devices attached to the EBU need to be acked in
 * a special manner
 */
#define LTQ_ICU_EBU_IRQ		22

#define ltq_icu_w32(vpe, m, x, y)	\
	ltq_w32((x), ltq_icu_membase[vpe] + m*LTQ_ICU_IM_SIZE + (y))

#define ltq_icu_r32(vpe, m, x)		\
	ltq_r32(ltq_icu_membase[vpe] + m*LTQ_ICU_IM_SIZE + (x))

#define ltq_eiu_w32(x, y)	ltq_w32((x), ltq_eiu_membase + (y))
#define ltq_eiu_r32(x)		ltq_r32(ltq_eiu_membase + (x))

/* we have a cascade of 8 irqs */
#define MIPS_CPU_IRQ_CASCADE		8

static int exin_avail;
static u32 ltq_eiu_irq[MAX_EIU];
static void __iomem *ltq_icu_membase[NR_CPUS];
static void __iomem *ltq_eiu_membase;
static struct irq_domain *ltq_domain;
static DEFINE_SPINLOCK(ltq_eiu_lock);
static DEFINE_RAW_SPINLOCK(ltq_icu_lock);
static int ltq_perfcount_irq;

int ltq_eiu_get_irq(int exin)
{
	if (exin < exin_avail)
		return ltq_eiu_irq[exin];
	return -1;
}

void ltq_disable_irq(struct irq_data *d)
{
	unsigned long offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	unsigned long im = offset / INT_NUM_IM_OFFSET;
	unsigned long flags;
	int vpe;

	offset %= INT_NUM_IM_OFFSET;

	raw_spin_lock_irqsave(&ltq_icu_lock, flags);
	for_each_present_cpu(vpe) {
		ltq_icu_w32(vpe, im,
			    ltq_icu_r32(vpe, im, LTQ_ICU_IER) & ~BIT(offset),
			    LTQ_ICU_IER);
	}
	raw_spin_unlock_irqrestore(&ltq_icu_lock, flags);
}

void ltq_mask_and_ack_irq(struct irq_data *d)
{
	unsigned long offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	unsigned long im = offset / INT_NUM_IM_OFFSET;
	unsigned long flags;
	int vpe;

	offset %= INT_NUM_IM_OFFSET;

	raw_spin_lock_irqsave(&ltq_icu_lock, flags);
	for_each_present_cpu(vpe) {
		ltq_icu_w32(vpe, im,
			    ltq_icu_r32(vpe, im, LTQ_ICU_IER) & ~BIT(offset),
			    LTQ_ICU_IER);
		ltq_icu_w32(vpe, im, BIT(offset), LTQ_ICU_ISR);
	}
	raw_spin_unlock_irqrestore(&ltq_icu_lock, flags);
}

static void ltq_ack_irq(struct irq_data *d)
{
	unsigned long offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	unsigned long im = offset / INT_NUM_IM_OFFSET;
	unsigned long flags;
	int vpe;

	offset %= INT_NUM_IM_OFFSET;

	raw_spin_lock_irqsave(&ltq_icu_lock, flags);
	for_each_present_cpu(vpe) {
		ltq_icu_w32(vpe, im, BIT(offset), LTQ_ICU_ISR);
	}
	raw_spin_unlock_irqrestore(&ltq_icu_lock, flags);
}

void ltq_enable_irq(struct irq_data *d)
{
	unsigned long offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	unsigned long im = offset / INT_NUM_IM_OFFSET;
	unsigned long flags;
	int vpe;

	offset %= INT_NUM_IM_OFFSET;

	vpe = cpumask_first(irq_data_get_effective_affinity_mask(d));

	/* This shouldn't be even possible, maybe during CPU hotplug spam */
	if (unlikely(vpe >= nr_cpu_ids))
		vpe = smp_processor_id();

	raw_spin_lock_irqsave(&ltq_icu_lock, flags);

	ltq_icu_w32(vpe, im, ltq_icu_r32(vpe, im, LTQ_ICU_IER) | BIT(offset),
		    LTQ_ICU_IER);

	raw_spin_unlock_irqrestore(&ltq_icu_lock, flags);
}

static int ltq_eiu_settype(struct irq_data *d, unsigned int type)
{
	int i;
	unsigned long flags;

	for (i = 0; i < exin_avail; i++) {
		if (d->hwirq == ltq_eiu_irq[i]) {
			int val = 0;
			int edge = 0;

			switch (type) {
			case IRQF_TRIGGER_NONE:
				break;
			case IRQF_TRIGGER_RISING:
				val = 1;
				edge = 1;
				break;
			case IRQF_TRIGGER_FALLING:
				val = 2;
				edge = 1;
				break;
			case IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING:
				val = 3;
				edge = 1;
				break;
			case IRQF_TRIGGER_HIGH:
				val = 5;
				break;
			case IRQF_TRIGGER_LOW:
				val = 6;
				break;
			default:
				pr_err("invalid type %d for irq %ld\n",
					type, d->hwirq);
				return -EINVAL;
			}

			if (edge)
				irq_set_handler(d->hwirq, handle_edge_irq);

			spin_lock_irqsave(&ltq_eiu_lock, flags);
			ltq_eiu_w32((ltq_eiu_r32(LTQ_EIU_EXIN_C) &
				    (~(7 << (i * 4)))) | (val << (i * 4)),
				    LTQ_EIU_EXIN_C);
			spin_unlock_irqrestore(&ltq_eiu_lock, flags);
		}
	}

	return 0;
}

static unsigned int ltq_startup_eiu_irq(struct irq_data *d)
{
	int i;

	ltq_enable_irq(d);
	for (i = 0; i < exin_avail; i++) {
		if (d->hwirq == ltq_eiu_irq[i]) {
			/* by default we are low level triggered */
			ltq_eiu_settype(d, IRQF_TRIGGER_LOW);
			/* clear all pending */
			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_INC) & ~BIT(i),
				LTQ_EIU_EXIN_INC);
			/* enable */
			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_INEN) | BIT(i),
				LTQ_EIU_EXIN_INEN);
			break;
		}
	}

	return 0;
}

static void ltq_shutdown_eiu_irq(struct irq_data *d)
{
	int i;

	ltq_disable_irq(d);
	for (i = 0; i < exin_avail; i++) {
		if (d->hwirq == ltq_eiu_irq[i]) {
			/* disable */
			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_INEN) & ~BIT(i),
				LTQ_EIU_EXIN_INEN);
			break;
		}
	}
}

#if defined(CONFIG_SMP)
static int ltq_icu_irq_set_affinity(struct irq_data *d,
				    const struct cpumask *cpumask, bool force)
{
	struct cpumask tmask;

	if (!cpumask_and(&tmask, cpumask, cpu_online_mask))
		return -EINVAL;

	irq_data_update_effective_affinity(d, &tmask);

	return IRQ_SET_MASK_OK;
}
#endif

static struct irq_chip ltq_irq_type = {
	.name = "icu",
	.irq_enable = ltq_enable_irq,
	.irq_disable = ltq_disable_irq,
	.irq_unmask = ltq_enable_irq,
	.irq_ack = ltq_ack_irq,
	.irq_mask = ltq_disable_irq,
	.irq_mask_ack = ltq_mask_and_ack_irq,
#if defined(CONFIG_SMP)
	.irq_set_affinity = ltq_icu_irq_set_affinity,
#endif
};

static struct irq_chip ltq_eiu_type = {
	.name = "eiu",
	.irq_startup = ltq_startup_eiu_irq,
	.irq_shutdown = ltq_shutdown_eiu_irq,
	.irq_enable = ltq_enable_irq,
	.irq_disable = ltq_disable_irq,
	.irq_unmask = ltq_enable_irq,
	.irq_ack = ltq_ack_irq,
	.irq_mask = ltq_disable_irq,
	.irq_mask_ack = ltq_mask_and_ack_irq,
	.irq_set_type = ltq_eiu_settype,
#if defined(CONFIG_SMP)
	.irq_set_affinity = ltq_icu_irq_set_affinity,
#endif
};

static void ltq_hw_irq_handler(struct irq_desc *desc)
{
	unsigned int module = irq_desc_get_irq(desc) - 2;
	u32 irq;
	irq_hw_number_t hwirq;
	int vpe = smp_processor_id();

	irq = ltq_icu_r32(vpe, module, LTQ_ICU_IOSR);
	if (irq == 0)
		return;

	/*
	 * silicon bug causes only the msb set to 1 to be valid. all
	 * other bits might be bogus
	 */
	irq = __fls(irq);
	hwirq = irq + MIPS_CPU_IRQ_CASCADE + (INT_NUM_IM_OFFSET * module);
	generic_handle_irq(irq_linear_revmap(ltq_domain, hwirq));

	/* if this is a EBU irq, we need to ack it or get a deadlock */
	if (irq == LTQ_ICU_EBU_IRQ && !module && LTQ_EBU_PCC_ISTAT != 0)
		ltq_ebu_w32(ltq_ebu_r32(LTQ_EBU_PCC_ISTAT) | 0x10,
			LTQ_EBU_PCC_ISTAT);
}

static int icu_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	struct irq_chip *chip = &ltq_irq_type;
	struct irq_data *data;
	int i;

	if (hw < MIPS_CPU_IRQ_CASCADE)
		return 0;

	for (i = 0; i < exin_avail; i++)
		if (hw == ltq_eiu_irq[i])
			chip = &ltq_eiu_type;

	data = irq_get_irq_data(irq);

	irq_data_update_effective_affinity(data, cpumask_of(0));

	irq_set_chip_and_handler(irq, chip, handle_level_irq);

	return 0;
}

static const struct irq_domain_ops irq_domain_ops = {
	.xlate = irq_domain_xlate_onetwocell,
	.map = icu_map,
};

int __init icu_of_init(struct device_node *node, struct device_node *parent)
{
	struct device_node *eiu_node;
	struct resource res;
	int i, ret, vpe;

	/* load register regions of available ICUs */
	for_each_possible_cpu(vpe) {
		if (of_address_to_resource(node, vpe, &res))
			panic("Failed to get icu%i memory range", vpe);

		if (!request_mem_region(res.start, resource_size(&res),
					res.name))
			pr_err("Failed to request icu%i memory\n", vpe);

		ltq_icu_membase[vpe] = ioremap(res.start,
					resource_size(&res));

		if (!ltq_icu_membase[vpe])
			panic("Failed to remap icu%i memory", vpe);
	}

	/* turn off all irqs by default */
	for_each_possible_cpu(vpe) {
		for (i = 0; i < MAX_IM; i++) {
			/* make sure all irqs are turned off by default */
			ltq_icu_w32(vpe, i, 0, LTQ_ICU_IER);

			/* clear all possibly pending interrupts */
			ltq_icu_w32(vpe, i, ~0, LTQ_ICU_ISR);
			ltq_icu_w32(vpe, i, ~0, LTQ_ICU_IMR);

			/* clear resend */
			ltq_icu_w32(vpe, i, 0, LTQ_ICU_IRSR);
		}
	}

	mips_cpu_irq_init();

	for (i = 0; i < MAX_IM; i++)
		irq_set_chained_handler(i + 2, ltq_hw_irq_handler);

	ltq_domain = irq_domain_add_linear(node,
		(MAX_IM * INT_NUM_IM_OFFSET) + MIPS_CPU_IRQ_CASCADE,
		&irq_domain_ops, 0);

	/* tell oprofile which irq to use */
	ltq_perfcount_irq = irq_create_mapping(ltq_domain, LTQ_PERF_IRQ);

	/* the external interrupts are optional and xway only */
	eiu_node = of_find_compatible_node(NULL, NULL, "lantiq,eiu-xway");
	if (eiu_node && !of_address_to_resource(eiu_node, 0, &res)) {
		/* find out how many external irq sources we have */
		exin_avail = of_property_count_u32_elems(eiu_node,
							 "lantiq,eiu-irqs");

		if (exin_avail > MAX_EIU)
			exin_avail = MAX_EIU;

		ret = of_property_read_u32_array(eiu_node, "lantiq,eiu-irqs",
						ltq_eiu_irq, exin_avail);
		if (ret)
			panic("failed to load external irq resources");

		if (!request_mem_region(res.start, resource_size(&res),
							res.name))
			pr_err("Failed to request eiu memory");

		ltq_eiu_membase = ioremap(res.start,
							resource_size(&res));
		if (!ltq_eiu_membase)
			panic("Failed to remap eiu memory");
	}

	return 0;
}

int get_c0_perfcount_int(void)
{
	return ltq_perfcount_irq;
}
EXPORT_SYMBOL_GPL(get_c0_perfcount_int);

unsigned int get_c0_compare_int(void)
{
	return CP0_LEGACY_COMPARE_IRQ;
}

static const struct of_device_id of_irq_ids[] __initconst = {
	{ .compatible = "lantiq,icu", .data = icu_of_init },
	{},
};

void __init arch_init_irq(void)
{
	of_irq_init(of_irq_ids);
}
