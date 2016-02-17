/*
 * Freescale Management Complex (MC) bus driver
 *
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 * Author: German Rivera <German.Rivera@freescale.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "../include/mc-private.h"
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/limits.h>
#include <linux/bitops.h>
#include "../include/dpmng.h"
#include "../include/mc-sys.h"
#include "dprc-cmd.h"

/*
 * IOMMU stream ID flags
 */
#define STREAM_ID_PL_MASK   BIT(9)	    /* privilege level */
#define STREAM_ID_BMT_MASK  BIT(8)	    /* bypass memory translation */
#define STREAM_ID_VA_MASK   BIT(7)	    /* virtual address translation
					     * (two-stage translation) */
#define STREAM_ID_ICID_MASK (BIT(7) - 1)    /* isolation context ID
					     * (translation context) */

#define MAX_STREAM_ID_ICID  STREAM_ID_ICID_MASK

static struct kmem_cache *mc_dev_cache;

/**
 * fsl_mc_bus_match - device to driver matching callback
 * @dev: the MC object device structure to match against
 * @drv: the device driver to search for matching MC object device id
 * structures
 *
 * Returns 1 on success, 0 otherwise.
 */
static int fsl_mc_bus_match(struct device *dev, struct device_driver *drv)
{
	const struct fsl_mc_device_match_id *id;
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	struct fsl_mc_driver *mc_drv = to_fsl_mc_driver(drv);
	bool found = false;
	bool major_version_mismatch = false;
	bool minor_version_mismatch = false;

	if (WARN_ON(!fsl_mc_bus_type.dev_root))
		goto out;

	/* When driver_override is set, only bind to the matching driver */
	if (mc_dev->driver_override) {
		found = !strcmp(mc_dev->driver_override, mc_drv->driver.name);
		goto out;
	}

	if (!mc_drv->match_id_table)
		goto out;

	/*
	 * If the object is not 'plugged' don't match.
	 * Only exception is the root DPRC, which is a special case.
	 */
	if ((mc_dev->obj_desc.state & DPRC_OBJ_STATE_PLUGGED) == 0 &&
	    &mc_dev->dev != fsl_mc_bus_type.dev_root)
		goto out;

	/*
	 * Traverse the match_id table of the given driver, trying to find
	 * a matching for the given MC object device.
	 */
	for (id = mc_drv->match_id_table; id->vendor != 0x0; id++) {
		if (id->vendor == mc_dev->obj_desc.vendor &&
		    strcmp(id->obj_type, mc_dev->obj_desc.type) == 0) {
			if (id->ver_major == mc_dev->obj_desc.ver_major) {
				found = true;
				if (id->ver_minor != mc_dev->obj_desc.ver_minor)
					minor_version_mismatch = true;
			} else {
				major_version_mismatch = true;
			}

			break;
		}
	}

	if (major_version_mismatch) {
		dev_warn(dev,
			 "Major version mismatch: driver version %u.%u, MC object version %u.%u\n",
			 id->ver_major, id->ver_minor,
			 mc_dev->obj_desc.ver_major,
			 mc_dev->obj_desc.ver_minor);
	} else if (minor_version_mismatch) {
		dev_warn(dev,
			 "Minor version mismatch: driver version %u.%u, MC object version %u.%u\n",
			 id->ver_major, id->ver_minor,
			 mc_dev->obj_desc.ver_major,
			 mc_dev->obj_desc.ver_minor);
	}

out:
	dev_dbg(dev, "%smatched\n", found ? "" : "not ");
	return found;
}

/**
 * fsl_mc_bus_uevent - callback invoked when a device is added
 */
static int fsl_mc_bus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	pr_debug("%s invoked\n", __func__);
	return 0;
}

static ssize_t driver_override_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	const char *driver_override, *old = mc_dev->driver_override;
	char *cp;

	if (WARN_ON(dev->bus != &fsl_mc_bus_type))
		return -EINVAL;

	if (count > PATH_MAX)
		return -EINVAL;

	driver_override = kstrndup(buf, count, GFP_KERNEL);
	if (!driver_override)
		return -ENOMEM;

	cp = strchr(driver_override, '\n');
	if (cp)
		*cp = '\0';

	if (strlen(driver_override)) {
		mc_dev->driver_override = driver_override;
	} else {
		kfree(driver_override);
		mc_dev->driver_override = NULL;
	}

	kfree(old);

	return count;
}

static ssize_t driver_override_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);

	return sprintf(buf, "%s\n", mc_dev->driver_override);
}

static DEVICE_ATTR_RW(driver_override);

static struct attribute *fsl_mc_dev_attrs[] = {
	&dev_attr_driver_override.attr,
	NULL,
};

static const struct attribute_group fsl_mc_dev_group = {
	.attrs = fsl_mc_dev_attrs,
};

static const struct attribute_group *fsl_mc_dev_groups[] = {
	&fsl_mc_dev_group,
	NULL,
};

struct bus_type fsl_mc_bus_type = {
	.name = "fsl-mc",
	.match = fsl_mc_bus_match,
	.uevent = fsl_mc_bus_uevent,
	.dev_groups = fsl_mc_dev_groups,
};
EXPORT_SYMBOL_GPL(fsl_mc_bus_type);

static int fsl_mc_driver_probe(struct device *dev)
{
	struct fsl_mc_driver *mc_drv;
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	int error;

	if (WARN_ON(!dev->driver))
		return -EINVAL;

	mc_drv = to_fsl_mc_driver(dev->driver);
	if (WARN_ON(!mc_drv->probe))
		return -EINVAL;

	error = mc_drv->probe(mc_dev);
	if (error < 0) {
		dev_err(dev, "MC object device probe callback failed: %d\n",
			error);
		return error;
	}

	return 0;
}

static int fsl_mc_driver_remove(struct device *dev)
{
	struct fsl_mc_driver *mc_drv = to_fsl_mc_driver(dev->driver);
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	int error;

	if (WARN_ON(!dev->driver))
		return -EINVAL;

	error = mc_drv->remove(mc_dev);
	if (error < 0) {
		dev_err(dev,
			"MC object device remove callback failed: %d\n",
			error);
		return error;
	}

	return 0;
}

static void fsl_mc_driver_shutdown(struct device *dev)
{
	struct fsl_mc_driver *mc_drv = to_fsl_mc_driver(dev->driver);
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);

	mc_drv->shutdown(mc_dev);
}

/**
 * __fsl_mc_driver_register - registers a child device driver with the
 * MC bus
 *
 * This function is implicitly invoked from the registration function of
 * fsl_mc device drivers, which is generated by the
 * module_fsl_mc_driver() macro.
 */
int __fsl_mc_driver_register(struct fsl_mc_driver *mc_driver,
			     struct module *owner)
{
	int error;

	mc_driver->driver.owner = owner;
	mc_driver->driver.bus = &fsl_mc_bus_type;

	if (mc_driver->probe)
		mc_driver->driver.probe = fsl_mc_driver_probe;

	if (mc_driver->remove)
		mc_driver->driver.remove = fsl_mc_driver_remove;

	if (mc_driver->shutdown)
		mc_driver->driver.shutdown = fsl_mc_driver_shutdown;

	error = driver_register(&mc_driver->driver);
	if (error < 0) {
		pr_err("driver_register() failed for %s: %d\n",
		       mc_driver->driver.name, error);
		return error;
	}

	pr_info("MC object device driver %s registered\n",
		mc_driver->driver.name);
	return 0;
}
EXPORT_SYMBOL_GPL(__fsl_mc_driver_register);

/**
 * fsl_mc_driver_unregister - unregisters a device driver from the
 * MC bus
 */
void fsl_mc_driver_unregister(struct fsl_mc_driver *mc_driver)
{
	driver_unregister(&mc_driver->driver);
}
EXPORT_SYMBOL_GPL(fsl_mc_driver_unregister);

bool fsl_mc_interrupts_supported(void)
{
	struct fsl_mc *mc = dev_get_drvdata(fsl_mc_bus_type.dev_root->parent);

	return mc->gic_supported;
}
EXPORT_SYMBOL_GPL(fsl_mc_interrupts_supported);

static int get_dprc_icid(struct fsl_mc_io *mc_io,
			 int container_id, uint16_t *icid)
{
	uint16_t dprc_handle;
	struct dprc_attributes attr;
	int error;

	error = dprc_open(mc_io, container_id, &dprc_handle);
	if (error < 0) {
		pr_err("dprc_open() failed: %d\n", error);
		return error;
	}

	memset(&attr, 0, sizeof(attr));
	error = dprc_get_attributes(mc_io, dprc_handle, &attr);
	if (error < 0) {
		pr_err("dprc_get_attributes() failed: %d\n", error);
		goto common_cleanup;
	}

	*icid = attr.icid;
	error = 0;

common_cleanup:
	(void)dprc_close(mc_io, dprc_handle);
	return error;
}

static int translate_mc_addr(enum fsl_mc_region_types mc_region_type,
			     uint64_t mc_offset, phys_addr_t *phys_addr)
{
	int i;
	struct fsl_mc *mc = dev_get_drvdata(fsl_mc_bus_type.dev_root->parent);

	if (mc->num_translation_ranges == 0) {
		/*
		 * Do identity mapping:
		 */
		*phys_addr = mc_offset;
		return 0;
	}

	for (i = 0; i < mc->num_translation_ranges; i++) {
		struct fsl_mc_addr_translation_range *range =
			&mc->translation_ranges[i];

		if (mc_region_type == range->mc_region_type &&
		    mc_offset >= range->start_mc_offset &&
		    mc_offset < range->end_mc_offset) {
			*phys_addr = range->start_phys_addr +
				     (mc_offset - range->start_mc_offset);
			return 0;
		}
	}

	return -EFAULT;
}

static int fsl_mc_device_get_mmio_regions(struct fsl_mc_device *mc_dev,
					  struct fsl_mc_device *mc_bus_dev)
{
	int i;
	int error;
	struct resource *regions;
	struct dprc_obj_desc *obj_desc = &mc_dev->obj_desc;
	struct device *parent_dev = mc_dev->dev.parent;
	enum fsl_mc_region_types mc_region_type;

	if (strcmp(obj_desc->type, "dprc") == 0 ||
	    strcmp(obj_desc->type, "dpmcp") == 0) {
		mc_region_type = FSL_MC_PORTAL;
	} else if (strcmp(obj_desc->type, "dpio") == 0) {
		mc_region_type = FSL_QBMAN_PORTAL;
	} else {
		/*
		 * This function should not have been called for this MC object
		 * type, as this object type is not supposed to have MMIO
		 * regions
		 */
		WARN_ON(true);
		return -EINVAL;
	}

	regions = kmalloc_array(obj_desc->region_count,
				sizeof(regions[0]), GFP_KERNEL);
	if (!regions)
		return -ENOMEM;

	for (i = 0; i < obj_desc->region_count; i++) {
		struct dprc_region_desc region_desc;

		error = dprc_get_obj_region(mc_bus_dev->mc_io,
					    mc_bus_dev->mc_handle,
					    obj_desc->type,
					    obj_desc->id, i, &region_desc);
		if (error < 0) {
			dev_err(parent_dev,
				"dprc_get_obj_region() failed: %d\n", error);
			goto error_cleanup_regions;
		}

		WARN_ON(region_desc.size == 0);
		error = translate_mc_addr(mc_region_type,
					  region_desc.base_offset,
					  &regions[i].start);
		if (error < 0) {
			dev_err(parent_dev,
				"Invalid MC offset: %#llx (for %s.%d\'s region %d)\n",
				region_desc.base_offset,
				obj_desc->type, obj_desc->id, i);
			goto error_cleanup_regions;
		}

		regions[i].end = regions[i].start + region_desc.size - 1;
		regions[i].name = "fsl-mc object MMIO region";
		regions[i].flags = IORESOURCE_IO;
	}

	mc_dev->regions = regions;
	return 0;

error_cleanup_regions:
	kfree(regions);
	return error;
}

/**
 * Add a newly discovered MC object device to be visible in Linux
 */
int fsl_mc_device_add(struct dprc_obj_desc *obj_desc,
		      struct fsl_mc_io *mc_io,
		      struct device *parent_dev,
		      const char *driver_override,
		      struct fsl_mc_device **new_mc_dev)
{
	int error;
	struct fsl_mc_device *mc_dev = NULL;
	struct fsl_mc_bus *mc_bus = NULL;
	struct fsl_mc_device *parent_mc_dev;

	if (parent_dev->bus == &fsl_mc_bus_type)
		parent_mc_dev = to_fsl_mc_device(parent_dev);
	else
		parent_mc_dev = NULL;

	if (strcmp(obj_desc->type, "dprc") == 0) {
		/*
		 * Allocate an MC bus device object:
		 */
		mc_bus = devm_kzalloc(parent_dev, sizeof(*mc_bus), GFP_KERNEL);
		if (!mc_bus)
			return -ENOMEM;

		mc_dev = &mc_bus->mc_dev;
	} else {
		/*
		 * Allocate a regular fsl_mc_device object:
		 */
		mc_dev = kmem_cache_zalloc(mc_dev_cache, GFP_KERNEL);
		if (!mc_dev)
			return -ENOMEM;
	}

	mc_dev->obj_desc = *obj_desc;
	mc_dev->mc_io = mc_io;
	if (driver_override) {
		/*
		 * We trust driver_override, so we don't need to use
		 * kstrndup() here
		 */
		mc_dev->driver_override = kstrdup(driver_override, GFP_KERNEL);
		if (!mc_dev->driver_override) {
			error = -ENOMEM;
			goto error_cleanup_dev;
		}
	}

	device_initialize(&mc_dev->dev);
	INIT_LIST_HEAD(&mc_dev->dev.msi_list);
	mc_dev->dev.parent = parent_dev;
	mc_dev->dev.bus = &fsl_mc_bus_type;
	dev_set_name(&mc_dev->dev, "%s.%d", obj_desc->type, obj_desc->id);

	if (strcmp(obj_desc->type, "dprc") == 0) {
		struct fsl_mc_io *mc_io2;

		mc_dev->flags |= FSL_MC_IS_DPRC;

		/*
		 * To get the DPRC's ICID, we need to open the DPRC
		 * in get_dprc_icid(). For child DPRCs, we do so using the
		 * parent DPRC's MC portal instead of the child DPRC's MC
		 * portal, in case the child DPRC is already opened with
		 * its own portal (e.g., the DPRC used by AIOP).
		 *
		 * NOTE: There cannot be more than one active open for a
		 * given MC object, using the same MC portal.
		 */
		if (parent_mc_dev) {
			/*
			 * device being added is a child DPRC device
			 */
			mc_io2 = parent_mc_dev->mc_io;
		} else {
			/*
			 * device being added is the root DPRC device
			 */
			if (WARN_ON(!mc_io)) {
				error = -EINVAL;
				goto error_cleanup_dev;
			}

			mc_io2 = mc_io;

			if (!fsl_mc_bus_type.dev_root)
				fsl_mc_bus_type.dev_root = &mc_dev->dev;
		}

		error = get_dprc_icid(mc_io2, obj_desc->id, &mc_dev->icid);
		if (error < 0)
			goto error_cleanup_dev;
	} else {
		/*
		 * A non-DPRC MC object device has to be a child of another
		 * MC object (specifically a DPRC object)
		 */
		mc_dev->icid = parent_mc_dev->icid;
		mc_dev->dma_mask = FSL_MC_DEFAULT_DMA_MASK;
		mc_dev->dev.dma_mask = &mc_dev->dma_mask;
	}

	/*
	 * Get MMIO regions for the device from the MC:
	 *
	 * NOTE: the root DPRC is a special case as its MMIO region is
	 * obtained from the device tree
	 */
	if (parent_mc_dev && obj_desc->region_count != 0) {
		error = fsl_mc_device_get_mmio_regions(mc_dev,
						       parent_mc_dev);
		if (error < 0)
			goto error_cleanup_dev;
	}

	/*
	 * The device-specific probe callback will get invoked by device_add()
	 */
	error = device_add(&mc_dev->dev);
	if (error < 0) {
		dev_err(parent_dev,
			"device_add() failed for device %s: %d\n",
			dev_name(&mc_dev->dev), error);
		goto error_cleanup_dev;
	}

	(void)get_device(&mc_dev->dev);
	dev_dbg(parent_dev, "Added MC object device %s\n",
		dev_name(&mc_dev->dev));

	*new_mc_dev = mc_dev;
	return 0;

error_cleanup_dev:
	kfree(mc_dev->regions);
	if (mc_bus)
		devm_kfree(parent_dev, mc_bus);
	else
		kmem_cache_free(mc_dev_cache, mc_dev);

	return error;
}
EXPORT_SYMBOL_GPL(fsl_mc_device_add);

/**
 * fsl_mc_device_remove - Remove a MC object device from being visible to
 * Linux
 *
 * @mc_dev: Pointer to a MC object device object
 */
void fsl_mc_device_remove(struct fsl_mc_device *mc_dev)
{
	struct fsl_mc_bus *mc_bus = NULL;

	kfree(mc_dev->regions);

	/*
	 * The device-specific remove callback will get invoked by device_del()
	 */
	device_del(&mc_dev->dev);
	put_device(&mc_dev->dev);

	if (strcmp(mc_dev->obj_desc.type, "dprc") == 0) {
		mc_bus = to_fsl_mc_bus(mc_dev);
		if (mc_dev->mc_io) {
			fsl_destroy_mc_io(mc_dev->mc_io);
			mc_dev->mc_io = NULL;
		}

		if (&mc_dev->dev == fsl_mc_bus_type.dev_root)
			fsl_mc_bus_type.dev_root = NULL;
	} else if (strcmp(mc_dev->obj_desc.type, "dpmcp") == 0) {
		if (mc_dev->mc_io) {
			fsl_destroy_mc_io(mc_dev->mc_io);
			mc_dev->mc_io = NULL;
		}
	}

	kfree(mc_dev->driver_override);
	mc_dev->driver_override = NULL;
	if (mc_bus)
		devm_kfree(mc_dev->dev.parent, mc_bus);
	else
		kmem_cache_free(mc_dev_cache, mc_dev);
}
EXPORT_SYMBOL_GPL(fsl_mc_device_remove);

static int mc_bus_msi_prepare(struct irq_domain *domain, struct device *dev,
			      int nvec, msi_alloc_info_t *info)
{
	int error;
	u32 its_dev_id;
	struct dprc_attributes dprc_attr;
	struct fsl_mc_device *mc_bus_dev = to_fsl_mc_device(dev);

	if (WARN_ON(!(mc_bus_dev->flags & FSL_MC_IS_DPRC)))
		return -EINVAL;

	error = dprc_get_attributes(mc_bus_dev->mc_io,
				    mc_bus_dev->mc_handle, &dprc_attr);
	if (error < 0) {
		dev_err(&mc_bus_dev->dev,
			"dprc_get_attributes() failed: %d\n",
			error);
		return error;
	}

	/*
	 * Build the device Id to be passed to the GIC-ITS:
	 *
	 * NOTE: This device id corresponds to the IOMMU stream ID
	 * associated with the DPRC object.
	 */
	its_dev_id = mc_bus_dev->icid;
	if (its_dev_id > STREAM_ID_ICID_MASK) {
		dev_err(&mc_bus_dev->dev,
			"Invalid ICID: %#x\n", its_dev_id);
		return -ERANGE;
	}

	if (dprc_attr.options & DPRC_CFG_OPT_IOMMU_BYPASS)
		its_dev_id |= STREAM_ID_PL_MASK | STREAM_ID_BMT_MASK;

	return __its_msi_prepare(domain->parent, its_dev_id, dev, nvec, info);
}

static void mc_bus_mask_msi_irq(struct irq_data *d)
{
	/* Bus specefic Mask */
	irq_chip_mask_parent(d);
}

static void mc_bus_unmask_msi_irq(struct irq_data *d)
{
	/* Bus specefic unmask */
	irq_chip_unmask_parent(d);
}

/*
 * This function is invoked from devm_request_irq(),
 * devm_request_threaded_irq(), dev_free_irq()
 */
static void mc_bus_msi_domain_write_msg(struct irq_data *irq_data,
					struct msi_msg *msg)
{
	struct msi_desc *msi_entry = irq_data->msi_desc;
	struct fsl_mc_device *mc_bus_dev = to_fsl_mc_device(msi_entry->dev);
	struct fsl_mc_bus *mc_bus = to_fsl_mc_bus(mc_bus_dev);
	struct fsl_mc_device_irq *irq_res =
		&mc_bus->irq_resources[msi_entry->msi_attrib.entry_nr];

	/*
	 * NOTE: This function is invoked with interrupts disabled
	 */

	if (irq_res->irq_number == irq_data->irq) {
		irq_res->msi_paddr =
			((u64)msg->address_hi << 32) | msg->address_lo;

		irq_res->msi_value = msg->data;

		/*
		 * NOTE: We cannot do the actual programming of the MSI
		 * in the MC, as this function is invoked in atomic context
		 * (interrupts disabled) and we cannot reliably send MC commands
		 * in atomic context.
		 */
	}
}

static struct irq_chip mc_bus_msi_irq_chip = {
	.name			= "fsl-mc-bus-msi",
	.irq_unmask		= mc_bus_unmask_msi_irq,
	.irq_mask		= mc_bus_mask_msi_irq,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_write_msi_msg	= mc_bus_msi_domain_write_msg,
};

static struct msi_domain_ops mc_bus_msi_ops = {
	.msi_prepare	= mc_bus_msi_prepare,
};

static struct msi_domain_info mc_bus_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		   MSI_FLAG_MULTI_PCI_MSI | MSI_FLAG_PCI_MSIX),
	.ops	= &mc_bus_msi_ops,
	.chip	= &mc_bus_msi_irq_chip,
};

static int create_mc_irq_domain(struct platform_device *mc_pdev,
				struct irq_domain **new_irq_domain)
{
	int error;
	struct device_node *its_of_node;
	struct irq_domain *its_domain;
	struct irq_domain *irq_domain;
	struct device_node *mc_of_node = mc_pdev->dev.of_node;

	its_of_node = of_parse_phandle(mc_of_node, "lpi-parent", 0);
	if (!its_of_node) {
		dev_err(&mc_pdev->dev,
			"lpi-parent phandle missing for %s\n",
			mc_of_node->full_name);
		return -ENOENT;
	}

	/*
	 * Extract MSI parent node:
	 */
	its_domain = irq_find_host(its_of_node);
	if (!its_domain) {
		dev_err(&mc_pdev->dev, "Unable to find parent domain\n");
		error = -ENOENT;
		goto cleanup_its_of_node;
	}

	irq_domain = msi_create_irq_domain(mc_of_node, &mc_bus_msi_domain_info,
					   its_domain->parent);
	if (!irq_domain) {
		dev_err(&mc_pdev->dev, "Failed to allocate msi_domain\n");
		error = -ENOMEM;
		goto cleanup_its_of_node;
	}

	dev_dbg(&mc_pdev->dev, "Allocated MSI domain\n");
	*new_irq_domain = irq_domain;
	return 0;

cleanup_its_of_node:
	of_node_put(its_of_node);
	return error;
}

/*
 * Initialize the interrupt pool associated with a MC bus.
 * It allocates a block of IRQs from the GIC-ITS
 */
int __must_check fsl_mc_populate_irq_pool(struct fsl_mc_bus *mc_bus,
					  unsigned int irq_count)
{
	unsigned int i;
	struct msi_desc *msi_entry;
	struct msi_desc *next_msi_entry;
	struct fsl_mc_device_irq *irq_resources;
	struct fsl_mc_device_irq *irq_res;
	int error;
	struct fsl_mc_device *mc_bus_dev = &mc_bus->mc_dev;
	struct fsl_mc *mc = dev_get_drvdata(fsl_mc_bus_type.dev_root->parent);
	struct fsl_mc_resource_pool *res_pool =
			&mc_bus->resource_pools[FSL_MC_POOL_IRQ];

	/*
	 * Detect duplicate invocations of this function:
	 */
	if (WARN_ON(!list_empty(&mc_bus_dev->dev.msi_list)))
		return -EINVAL;

	if (WARN_ON(irq_count == 0 ||
		    irq_count > FSL_MC_IRQ_POOL_MAX_TOTAL_IRQS))
		return -EINVAL;

	irq_resources =
		devm_kzalloc(&mc_bus_dev->dev,
			     sizeof(*irq_resources) * irq_count,
			     GFP_KERNEL);
	if (!irq_resources)
		return -ENOMEM;

	for (i = 0; i < irq_count; i++) {
		irq_res = &irq_resources[i];
		msi_entry = alloc_msi_entry(&mc_bus_dev->dev);
		if (!msi_entry) {
			dev_err(&mc_bus_dev->dev, "Failed to allocate msi entry\n");
			error = -ENOMEM;
			goto cleanup_msi_entries;
		}

		msi_entry->msi_attrib.is_msix = 1;
		msi_entry->msi_attrib.is_64 = 1;
		msi_entry->msi_attrib.entry_nr = i;
		msi_entry->nvec_used = 1;
		list_add_tail(&msi_entry->list, &mc_bus_dev->dev.msi_list);

		/*
		 * NOTE: irq_res->msi_paddr will be set by the
		 * mc_bus_msi_domain_write_msg() callback
		 */
		irq_res->resource.type = res_pool->type;
		irq_res->resource.data = irq_res;
		irq_res->resource.parent_pool = res_pool;
		INIT_LIST_HEAD(&irq_res->resource.node);
		list_add_tail(&irq_res->resource.node, &res_pool->free_list);
	}

	/*
	 * NOTE: Calling this function will trigger the invocation of the
	 * mc_bus_msi_prepare() callback
	 */
	error = msi_domain_alloc_irqs(mc->irq_domain,
				      &mc_bus_dev->dev, irq_count);

	if (error) {
		dev_err(&mc_bus_dev->dev, "Failed to allocate IRQs\n");
		goto cleanup_msi_entries;
	}

	for_each_msi_entry(msi_entry, &mc_bus_dev->dev) {
		u32 irq_num = msi_entry->irq;

		irq_res = &irq_resources[msi_entry->msi_attrib.entry_nr];
		irq_res->irq_number = irq_num;
		irq_res->resource.id = irq_num;
	}

	res_pool->max_count = irq_count;
	res_pool->free_count = irq_count;
	mc_bus->irq_resources = irq_resources;
	return 0;

cleanup_msi_entries:
	list_for_each_entry_safe(msi_entry, next_msi_entry,
				 &mc_bus_dev->dev.msi_list, list)
		kfree(msi_entry);

	devm_kfree(&mc_bus_dev->dev, irq_resources);
	return error;
}
EXPORT_SYMBOL_GPL(fsl_mc_populate_irq_pool);

/**
 * Teardown the interrupt pool associated with an MC bus.
 * It frees the IRQs that were allocated to the pool, back to the GIC-ITS.
 */
void fsl_mc_cleanup_irq_pool(struct fsl_mc_bus *mc_bus)
{
	struct msi_desc *msi_entry;
	struct msi_desc *next_msi_entry;
	struct fsl_mc *mc = dev_get_drvdata(fsl_mc_bus_type.dev_root->parent);
	struct fsl_mc_resource_pool *res_pool =
			&mc_bus->resource_pools[FSL_MC_POOL_IRQ];

	if (WARN_ON(!mc_bus->irq_resources))
		return;

	if (WARN_ON(res_pool->max_count == 0))
		return;

	if (WARN_ON(res_pool->free_count != res_pool->max_count))
		return;

	msi_domain_free_irqs(mc->irq_domain, &mc_bus->mc_dev.dev);
	list_for_each_entry_safe(msi_entry, next_msi_entry,
				 &mc_bus->mc_dev.dev.msi_list, list)
		kfree(msi_entry);

	devm_kfree(&mc_bus->mc_dev.dev, mc_bus->irq_resources);
	res_pool->max_count = 0;
	res_pool->free_count = 0;
	mc_bus->irq_resources = NULL;
}
EXPORT_SYMBOL_GPL(fsl_mc_cleanup_irq_pool);

static int parse_mc_ranges(struct device *dev,
			   int *paddr_cells,
			   int *mc_addr_cells,
			   int *mc_size_cells,
			   const __be32 **ranges_start,
			   uint8_t *num_ranges)
{
	const __be32 *prop;
	int range_tuple_cell_count;
	int ranges_len;
	int tuple_len;
	struct device_node *mc_node = dev->of_node;

	*ranges_start = of_get_property(mc_node, "ranges", &ranges_len);
	if (!(*ranges_start) || !ranges_len) {
		dev_warn(dev,
			 "missing or empty ranges property for device tree node '%s'\n",
			 mc_node->name);

		*num_ranges = 0;
		return 0;
	}

	*paddr_cells = of_n_addr_cells(mc_node);

	prop = of_get_property(mc_node, "#address-cells", NULL);
	if (prop)
		*mc_addr_cells = be32_to_cpup(prop);
	else
		*mc_addr_cells = *paddr_cells;

	prop = of_get_property(mc_node, "#size-cells", NULL);
	if (prop)
		*mc_size_cells = be32_to_cpup(prop);
	else
		*mc_size_cells = of_n_size_cells(mc_node);

	range_tuple_cell_count = *paddr_cells + *mc_addr_cells +
				 *mc_size_cells;

	tuple_len = range_tuple_cell_count * sizeof(__be32);
	if (ranges_len % tuple_len != 0) {
		dev_err(dev, "malformed ranges property '%s'\n", mc_node->name);
		return -EINVAL;
	}

	*num_ranges = ranges_len / tuple_len;
	return 0;
}

static int get_mc_addr_translation_ranges(struct device *dev,
					  struct fsl_mc_addr_translation_range
						**ranges,
					  uint8_t *num_ranges)
{
	int error;
	int paddr_cells;
	int mc_addr_cells;
	int mc_size_cells;
	int i;
	const __be32 *ranges_start;
	const __be32 *cell;

	error = parse_mc_ranges(dev,
				&paddr_cells,
				&mc_addr_cells,
				&mc_size_cells,
				&ranges_start,
				num_ranges);
	if (error < 0)
		return error;

	if (!(*num_ranges)) {
		/*
		 * Missing or empty ranges property ("ranges;") for the
		 * 'fsl,qoriq-mc' node. In this case, identity mapping
		 * will be used.
		 */
		*ranges = NULL;
		return 0;
	}

	*ranges = devm_kcalloc(dev, *num_ranges,
			       sizeof(struct fsl_mc_addr_translation_range),
			       GFP_KERNEL);
	if (!(*ranges))
		return -ENOMEM;

	cell = ranges_start;
	for (i = 0; i < *num_ranges; ++i) {
		struct fsl_mc_addr_translation_range *range = &(*ranges)[i];

		range->mc_region_type = of_read_number(cell, 1);
		range->start_mc_offset = of_read_number(cell + 1,
							mc_addr_cells - 1);
		cell += mc_addr_cells;
		range->start_phys_addr = of_read_number(cell, paddr_cells);
		cell += paddr_cells;
		range->end_mc_offset = range->start_mc_offset +
				       of_read_number(cell, mc_size_cells);

		cell += mc_size_cells;
	}

	return 0;
}

/**
 * fsl_mc_bus_probe - callback invoked when the root MC bus is being
 * added
 */
static int fsl_mc_bus_probe(struct platform_device *pdev)
{
	struct dprc_obj_desc obj_desc;
	int error;
	struct fsl_mc *mc;
	struct fsl_mc_device *mc_bus_dev = NULL;
	struct fsl_mc_io *mc_io = NULL;
	int container_id;
	phys_addr_t mc_portal_phys_addr;
	uint32_t mc_portal_size;
	struct mc_version mc_version;
	struct resource res;

	dev_info(&pdev->dev, "Root MC bus device probed");

	mc = devm_kzalloc(&pdev->dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;

	platform_set_drvdata(pdev, mc);
	error = create_mc_irq_domain(pdev, &mc->irq_domain);
	if (error < 0) {
		dev_warn(&pdev->dev,
			 "WARNING: MC bus driver will run without interrupt support\n");
	} else {
		mc->gic_supported = true;
	}

	/*
	 * Get physical address of MC portal for the root DPRC:
	 */
	error = of_address_to_resource(pdev->dev.of_node, 0, &res);
	if (error < 0) {
		dev_err(&pdev->dev,
			"of_address_to_resource() failed for %s\n",
			pdev->dev.of_node->full_name);
		goto error_cleanup_irq_domain;
	}

	mc_portal_phys_addr = res.start;
	mc_portal_size = resource_size(&res);
	error = fsl_create_mc_io(&pdev->dev, mc_portal_phys_addr,
				 mc_portal_size, NULL, 0, &mc_io);
	if (error < 0)
		goto error_cleanup_irq_domain;

	error = mc_get_version(mc_io, &mc_version);
	if (error != 0) {
		dev_err(&pdev->dev,
			"mc_get_version() failed with error %d\n", error);
		goto error_cleanup_mc_io;
	}

	dev_info(&pdev->dev,
		 "Freescale Management Complex Firmware version: %u.%u.%u\n",
		 mc_version.major, mc_version.minor, mc_version.revision);

	if (mc_version.major < MC_VER_MAJOR) {
		dev_err(&pdev->dev,
			"ERROR: MC firmware version not supported by driver (driver version: %u.%u)\n",
			MC_VER_MAJOR, MC_VER_MINOR);
		error = -ENOTSUPP;
		goto error_cleanup_mc_io;
	}

	if (mc_version.major > MC_VER_MAJOR) {
		dev_warn(&pdev->dev,
			 "WARNING: driver may not support newer MC firmware features (driver version: %u.%u)\n",
			 MC_VER_MAJOR, MC_VER_MINOR);
	}

	error = get_mc_addr_translation_ranges(&pdev->dev,
					       &mc->translation_ranges,
					       &mc->num_translation_ranges);
	if (error < 0)
		goto error_cleanup_mc_io;

	error = dpmng_get_container_id(mc_io, &container_id);
	if (error < 0) {
		dev_err(&pdev->dev,
			"dpmng_get_container_id() failed: %d\n", error);
		goto error_cleanup_mc_io;
	}

	obj_desc.vendor = FSL_MC_VENDOR_FREESCALE;
	strcpy(obj_desc.type, "dprc");
	obj_desc.id = container_id;
	obj_desc.ver_major = DPRC_VER_MAJOR;
	obj_desc.ver_minor = DPRC_VER_MINOR;
	obj_desc.irq_count = 1;
	obj_desc.region_count = 0;

	error = fsl_mc_device_add(&obj_desc, mc_io, &pdev->dev, NULL,
				  &mc_bus_dev);
	if (error < 0)
		goto error_cleanup_mc_io;

	mc->root_mc_bus_dev = mc_bus_dev;
	return 0;

error_cleanup_mc_io:
	fsl_destroy_mc_io(mc_io);

error_cleanup_irq_domain:
	if (mc->gic_supported)
		irq_domain_remove(mc->irq_domain);

	return error;
}

/**
 * fsl_mc_bus_remove - callback invoked when the root MC bus is being
 * removed
 */
static int fsl_mc_bus_remove(struct platform_device *pdev)
{
	struct fsl_mc *mc = platform_get_drvdata(pdev);

	if (WARN_ON(&mc->root_mc_bus_dev->dev != fsl_mc_bus_type.dev_root))
		return -EINVAL;

	if (mc->gic_supported)
		irq_domain_remove(mc->irq_domain);

	fsl_mc_device_remove(mc->root_mc_bus_dev);
	dev_info(&pdev->dev, "Root MC bus device removed");
	return 0;
}

static const struct of_device_id fsl_mc_bus_match_table[] = {
	{.compatible = "fsl,qoriq-mc",},
	{},
};

MODULE_DEVICE_TABLE(of, fsl_mc_bus_match_table);

static struct platform_driver fsl_mc_bus_driver = {
	.driver = {
		   .name = "fsl_mc_bus",
		   .owner = THIS_MODULE,
		   .pm = NULL,
		   .of_match_table = fsl_mc_bus_match_table,
		   },
	.probe = fsl_mc_bus_probe,
	.remove = fsl_mc_bus_remove,
};

static int __init fsl_mc_bus_driver_init(void)
{
	int error;

	mc_dev_cache = kmem_cache_create("fsl_mc_device",
					 sizeof(struct fsl_mc_device), 0, 0,
					 NULL);
	if (!mc_dev_cache) {
		pr_err("Could not create fsl_mc_device cache\n");
		return -ENOMEM;
	}

	error = bus_register(&fsl_mc_bus_type);
	if (error < 0) {
		pr_err("fsl-mc bus type registration failed: %d\n", error);
		goto error_cleanup_cache;
	}

	pr_info("fsl-mc bus type registered\n");

	error = platform_driver_register(&fsl_mc_bus_driver);
	if (error < 0) {
		pr_err("platform_driver_register() failed: %d\n", error);
		goto error_cleanup_bus;
	}

	error = dprc_driver_init();
	if (error < 0)
		goto error_cleanup_driver;

	error = fsl_mc_allocator_driver_init();
	if (error < 0)
		goto error_cleanup_dprc_driver;

	return 0;

error_cleanup_dprc_driver:
	dprc_driver_exit();

error_cleanup_driver:
	platform_driver_unregister(&fsl_mc_bus_driver);

error_cleanup_bus:
	bus_unregister(&fsl_mc_bus_type);

error_cleanup_cache:
	kmem_cache_destroy(mc_dev_cache);
	return error;
}

postcore_initcall(fsl_mc_bus_driver_init);

static void __exit fsl_mc_bus_driver_exit(void)
{
	if (WARN_ON(!mc_dev_cache))
		return;

	fsl_mc_allocator_driver_exit();
	dprc_driver_exit();
	platform_driver_unregister(&fsl_mc_bus_driver);
	bus_unregister(&fsl_mc_bus_type);
	kmem_cache_destroy(mc_dev_cache);
	pr_info("MC bus unregistered\n");
}

module_exit(fsl_mc_bus_driver_exit);

MODULE_AUTHOR("Freescale Semiconductor Inc.");
MODULE_DESCRIPTION("Freescale Management Complex (MC) bus driver");
MODULE_LICENSE("GPL");
