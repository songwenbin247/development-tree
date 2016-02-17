/*
 * Freescale Power Management driver
 *
 * Copyright 2015 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define pr_fmt(fmt) "PM: " fmt

#include <asm/psci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/suspend.h>
#include <linux/of_address.h>

#define FSL_SLEEP		(1 << 1)
#define CCSR_RCPM_IPPDEXPCR	0x140

static unsigned int sleep_modes;
static void __iomem *fsl_rcpm_base;
static u32 ippdexpcr0;

static void fsl_pm_enable_wakeup(void)
{
	iowrite32be(ippdexpcr0, fsl_rcpm_base + CCSR_RCPM_IPPDEXPCR);
}

static int fsl_pm_suspend(suspend_state_t state)
{
	int ret = 0;

	fsl_pm_enable_wakeup();

	switch (state) {
	case PM_SUSPEND_STANDBY:
		ret = psci_sys_suspend(0);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int fsl_pm_valid(suspend_state_t state)
{
	if (state == PM_SUSPEND_STANDBY && (sleep_modes & FSL_SLEEP))
		return 1;
	else
		return 0;
}

static void fsl_pm_set_wakeup_device(struct device *dev, int enable)
{
	u32 value[4];
	int ret;

	ret = of_property_read_u32_array(dev->of_node, "sleep", value, 3);
	if (ret) {
		pr_debug("%s(): can not find the \"sleep\" property.\n",
				__func__);
		return;
	}

	if (enable)
		ippdexpcr0 |= value[1];
	else
		ippdexpcr0 &= ~value[1];
}

static void fsl_pm_config_device(struct device *dev, void *enable)
{

	if (device_may_wakeup(dev))
		fsl_pm_set_wakeup_device(dev, *((int *)enable));
}

static int fsl_pm_begin(suspend_state_t state)
{
	int enable = 1;

	ippdexpcr0 = 0;
	dpm_for_each_dev(&enable, fsl_pm_config_device);

	return 0;
}

static const struct platform_suspend_ops fsl_pm_suspend_ops = {
		.valid = fsl_pm_valid,
		.enter = fsl_pm_suspend,
		.begin = fsl_pm_begin,
	};

static const struct of_device_id rcpm_matches[] = {
	{
		.compatible = "fsl,qoriq-rcpm-2.1",
	},
	{}
};

static int __init fsl_pm_init(void)
{
	const struct of_device_id *match;
	struct device_node *np;

	sleep_modes = 0;
	np = of_find_matching_node_and_match(NULL, rcpm_matches, &match);
	if (!np) {
		pr_debug("%s: can't find the rcpm node.\n", __func__);
		return -EINVAL;
	}

	fsl_rcpm_base = of_iomap(np, 0);
	of_node_put(np);
	if (!fsl_rcpm_base)
		return -ENOMEM;

	sleep_modes |= FSL_SLEEP;
	suspend_set_ops(&fsl_pm_suspend_ops);

	pr_info("Power Management driver initialized.\n");

	return 0;
}
device_initcall(fsl_pm_init);
