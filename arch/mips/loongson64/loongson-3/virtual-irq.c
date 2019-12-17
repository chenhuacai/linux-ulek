// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2020, Huacai Chen <chenhc@lemote.com>
 *  Copyright (C) 2020, Jiaxun Yang <jiaxun.yang@flygoat.com>
 *  Loongson Local IO Interrupt Controller support
 */

#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/module.h>
#include <irq.h>
#include <loongson.h>
#include <loongson-pch.h>

#define VIRT_LIOINTC_BASE	NR_IRQS_LEGACY
#define LOONGSON_INT_STATUS	LOONGSON3_REG32(LOONGSON3_REG_BASE, LOONGSON_INT_ROUTER_OFFSET + 0x20)

static DEFINE_RAW_SPINLOCK(pch_irq_lock);

static void mask_pch_irq(struct irq_data *d)
{
	int irq_nr;

	raw_spin_lock(&pch_irq_lock);
	irq_nr = d->irq - VIRT_LIOINTC_BASE;
	LOONGSON_INT_ROUTER_INTENCLR = (1 << irq_nr);
	raw_spin_unlock(&pch_irq_lock);
}

static void unmask_pch_irq(struct irq_data *d)
{
	int irq_nr;

	raw_spin_lock(&pch_irq_lock);
	irq_nr = d->irq - VIRT_LIOINTC_BASE;
	LOONGSON_INT_ROUTER_INTENSET = (1 << irq_nr);
	raw_spin_unlock(&pch_irq_lock);
}

static struct irq_chip pch_irq_chip = {
	.name			= "Loongson",
	.irq_mask		= mask_pch_irq,
	.irq_unmask		= unmask_pch_irq,
	.irq_set_affinity	= plat_set_irq_affinity,
	.flags			= IRQCHIP_MASK_ON_SUSPEND,
};

void virtio_irq_dispatch(void)
{
	u32 pending;

	pending = LOONGSON_INT_STATUS;

	if (!pending)
		spurious_interrupt();

	while (pending) {
		int irq = __ffs(pending);

		do_IRQ(VIRT_LIOINTC_BASE + irq);
		pending &= ~BIT(irq);
	}
}

static struct irqaction cascade_irqaction = {
	.handler = no_action,
	.flags = IRQF_NO_SUSPEND,
	.name = "cascade",
};

void virtio_init_irq(void)
{
	int i;

	LOONGSON_INT_ROUTER_INTENCLR = 0xffffffff;

	/* Route UART int to cpu Core0 INT0 */
	LOONGSON_INT_ROUTER_ENTRY(0) =
				LOONGSON_INT_COREx_INTy(loongson_sysconf.boot_cpu_id, 0);

	/* Route others to Core0 INT1 (IP3) */
	for (i = 1; i < 32; i++)
		LOONGSON_INT_ROUTER_ENTRY(i) =
				LOONGSON_INT_COREx_INTy(loongson_sysconf.boot_cpu_id, 1);

	setup_irq(VIRT_LIOINTC_BASE, &cascade_irqaction);
}

int __init liointc_of_init(struct device_node *node, struct device_node *parent)
{
	int i = 0;

	irq_alloc_descs(-1, VIRT_LIOINTC_BASE, 32, 0);
	irq_domain_add_legacy(node, 32, VIRT_LIOINTC_BASE,
			VIRT_LIOINTC_BASE, &irq_domain_simple_ops, NULL);

	for (i = VIRT_LIOINTC_BASE; i < VIRT_LIOINTC_BASE + 32; i++) {
		irq_set_noprobe(i);
		irq_set_chip_and_handler(i, &pch_irq_chip, handle_level_irq);
	}

	return 0;
}

IRQCHIP_DECLARE(loongson_liointc_1_0, "loongson,liointc-1.0", liointc_of_init);
