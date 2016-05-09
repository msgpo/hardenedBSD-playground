/*
 * Copyright 2003 José Fonseca.
 * Copyright 2003 Leif Delgass.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <drm/drmP.h>
#include "drm_internal.h"
#include "drm_legacy.h"

static void
drm_pci_busdma_callback(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	drm_dma_handle_t *dmah = arg;

	if (error != 0)
		return;

	KASSERT(nsegs == 1, ("drm_pci_busdma_callback: bad dma segment count"));
	dmah->busaddr = segs[0].ds_addr;
}

/**
 * drm_pci_alloc - Allocate a PCI consistent memory block, for DMA.
 * @dev: DRM device
 * @size: size of block to allocate
 * @align: alignment of block
 *
 * Return: A handle to the allocated memory block on success or NULL on
 * failure.
 */

drm_dma_handle_t *drm_pci_alloc(struct drm_device * dev, size_t size,
    size_t align)
{
	drm_dma_handle_t *dmah;
	int ret;

	/* Need power-of-two alignment, so fail the allocation if it isn't. */
	if ((align & (align - 1)) != 0) {
		DRM_ERROR("drm_pci_alloc with non-power-of-two alignment %d\n",
		    (int)align);
		return NULL;
	}

	dmah = malloc(sizeof(drm_dma_handle_t), DRM_MEM_DMA, M_ZERO | M_NOWAIT);
	if (dmah == NULL)
		return NULL;

	ret = bus_dma_tag_create(
	    bus_get_dma_tag(dev->dev->bsddev), /* parent */
	    align, 0, /* align, boundary */
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, /* lowaddr, highaddr */
	    NULL, NULL, /* filtfunc, filtfuncargs */
	    size, 1, size, /* maxsize, nsegs, maxsegsize */
	    0, NULL, NULL, /* flags, lockfunc, lockfuncargs */
	    &dmah->tag);
	if (ret != 0) {
		free(dmah, DRM_MEM_DMA);
		return NULL;
	}

	ret = bus_dmamem_alloc(dmah->tag, (void **)&dmah->vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_NOCACHE, &dmah->map);
	if (ret != 0) {
		bus_dma_tag_destroy(dmah->tag);
		free(dmah, DRM_MEM_DMA);
		return NULL;
	}

	ret = bus_dmamap_load(dmah->tag, dmah->map, dmah->vaddr, size,
	    drm_pci_busdma_callback, dmah, BUS_DMA_NOWAIT);
	if (ret != 0) {
		bus_dmamem_free(dmah->tag, dmah->vaddr, dmah->map);
		bus_dma_tag_destroy(dmah->tag);
		free(dmah, DRM_MEM_DMA);
		return NULL;
	}
	return (dmah);
}
EXPORT_SYMBOL(drm_pci_alloc);

/*
 * Free a PCI consistent memory block without freeing its descriptor.
 *
 * This function is for internal use in the Linux-specific DRM core code.
 */
void __drm_legacy_pci_free(struct drm_device * dev, drm_dma_handle_t * dmah)
{
	unsigned long addr;
	size_t sz;

	if (dmah == NULL)
		return;

	bus_dmamap_unload(dmah->tag, dmah->map);
	bus_dmamem_free(dmah->tag, dmah->vaddr, dmah->map);
	bus_dma_tag_destroy(dmah->tag);
}

/**
 * drm_pci_free - Free a PCI consistent memory block
 * @dev: DRM device
 * @dmah: handle to memory block
 */
void drm_pci_free(struct drm_device * dev, drm_dma_handle_t * dmah)
{
	__drm_legacy_pci_free(dev, dmah);
	kfree(dmah);
}

EXPORT_SYMBOL(drm_pci_free);

#ifdef CONFIG_PCI

static int drm_get_pci_domain(struct drm_device *dev)
{
#ifndef __alpha__
	/* For historical reasons, drm_get_pci_domain() is busticated
	 * on most archs and has to remain so for userspace interface
	 * < 1.4, except on alpha which was right from the beginning
	 */
	if (dev->if_version < 0x10004)
		return 0;
#endif /* __alpha__ */

	return pci_get_domain(dev->dev->bsddev);
}

int drm_pci_set_busid(struct drm_device *dev, struct drm_master *master)
{
	master->unique = kasprintf(GFP_KERNEL, "pci:%04x:%02x:%02x.%d",
					drm_get_pci_domain(dev),
				   pci_get_bus(dev->dev->bsddev),
				   pci_get_slot(dev->dev->bsddev),
					PCI_FUNC(dev->pdev->devfn));
	if (!master->unique)
		return -ENOMEM;

	master->unique_len = strlen(master->unique);
	return 0;
}
EXPORT_SYMBOL(drm_pci_set_busid);

int drm_pci_set_unique(struct drm_device *dev,
		       struct drm_master *master,
		       struct drm_unique *u)
{
	int domain, bus, slot, func, ret;

	master->unique_len = u->unique_len;
	master->unique = kmalloc(master->unique_len + 1, GFP_KERNEL);
	if (!master->unique) {
		ret = -ENOMEM;
		goto err;
	}

	if (copy_from_user(master->unique, u->unique, master->unique_len)) {
		ret = -EFAULT;
		goto err;
	}

	master->unique[master->unique_len] = '\0';

	/* Return error if the busid submitted doesn't match the device's actual
	 * busid.
	 */
	ret = sscanf(master->unique, "PCI:%d:%d:%d", &bus, &slot, &func);
	if (ret != 3) {
		ret = -EINVAL;
		goto err;
	}

	domain = bus >> 8;
	bus &= 0xff;

	if ((domain != drm_get_pci_domain(dev)) ||
	    (bus != dev->pdev->bus->number) ||
	    (slot != PCI_SLOT(dev->pdev->devfn)) ||
	    (func != PCI_FUNC(dev->pdev->devfn))) {
		ret = -EINVAL;
		goto err;
	}
	return 0;
err:
	return ret;
}

static int drm_pci_irq_by_busid(struct drm_device *dev, struct drm_irq_busid *p)
{
	if ((p->busnum >> 8) != drm_get_pci_domain(dev) ||
	    (p->busnum & 0xff) != dev->pdev->bus->number ||
	    p->devnum != PCI_SLOT(dev->pdev->devfn) || p->funcnum != PCI_FUNC(dev->pdev->devfn))
		return -EINVAL;

	p->irq = dev->pdev->irq;

	DRM_DEBUG("%d:%d:%d => IRQ %d\n", p->busnum, p->devnum, p->funcnum,
		  p->irq);
	return 0;
}

/**
 * drm_irq_by_busid - Get interrupt from bus ID
 * @dev: DRM device
 * @data: IOCTL parameter pointing to a drm_irq_busid structure
 * @file_priv: DRM file private.
 *
 * Finds the PCI device with the specified bus id and gets its IRQ number.
 * This IOCTL is deprecated, and will now return EINVAL for any busid not equal
 * to that of the device that this DRM instance attached to.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_irq_by_busid(struct drm_device *dev, void *data,
		     struct drm_file *file_priv)
{
	struct drm_irq_busid *p = data;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	/* UMS was only ever support on PCI devices. */
	if (WARN_ON(!dev->pdev))
		return -EINVAL;

	if (!drm_core_check_feature(dev, DRIVER_HAVE_IRQ))
		return -EINVAL;

	return drm_pci_irq_by_busid(dev, p);
}

static void drm_pci_agp_init(struct drm_device *dev)
{
	if (drm_core_check_feature(dev, DRIVER_USE_AGP)) {
		if (drm_pci_device_is_agp(dev))
			dev->agp = drm_agp_init(dev);
		if (dev->agp) {
			dev->agp->agp_mtrr = arch_phys_wc_add(
				dev->agp->agp_info.ai_aperture_base,
				dev->agp->agp_info.ai_aperture_size *
				1024 * 1024);
		}
	}
}

void drm_pci_agp_destroy(struct drm_device *dev)
{
	if (dev->agp) {
		arch_phys_wc_del(dev->agp->agp_mtrr);
		drm_agp_clear(dev);
		kfree(dev->agp);
		dev->agp = NULL;
	}
}

/**
 * drm_get_pci_dev - Register a PCI device with the DRM subsystem
 * @pdev: PCI device
 * @ent: entry from the PCI ID table that matches @pdev
 * @driver: DRM device driver
 *
 * Attempt to gets inter module "drm" information. If we are first
 * then register the character device and inter module information.
 * Try and register, if we fail to register, backout previous work.
 *
 * NOTE: This function is deprecated, please use drm_dev_alloc() and
 * drm_dev_register() instead and remove your ->load() callback.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_get_pci_dev(struct pci_dev *pdev, const struct pci_device_id *ent,
		    struct drm_driver *driver)
{
	struct drm_device *dev;
	int ret;

	DRM_DEBUG("\n");

	dev = drm_dev_alloc(driver, &pdev->dev);
	if (!dev)
		return -ENOMEM;

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;

	dev->pdev = pdev;
#ifdef __alpha__
	dev->hose = pdev->sysdata;
#endif

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		pci_set_drvdata(pdev, dev);

	drm_pci_agp_init(dev);

	ret = drm_dev_register(dev, ent->driver_data);
	if (ret)
		goto err_agp;

	DRM_INFO("Initialized %s %d.%d.%d %s for %s on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 driver->date, pci_name(pdev), dev->primary->index);

	/* No locking needed since shadow-attach is single-threaded since it may
	 * only be called from the per-driver module init hook. */
	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		list_add_tail(&dev->legacy_dev_list, &driver->legacy_dev_list);

	return 0;

err_agp:
	drm_pci_agp_destroy(dev);
	pci_disable_device(pdev);
err_free:
	drm_dev_unref(dev);
	return ret;
}
EXPORT_SYMBOL(drm_get_pci_dev);

/**
 * drm_pci_init - Register matching PCI devices with the DRM subsystem
 * @driver: DRM device driver
 * @pdriver: PCI device driver
 *
 * Initializes a drm_device structures, registering the stubs and initializing
 * the AGP device.
 *
 * NOTE: This function is deprecated. Modern modesetting drm drivers should use
 * pci_register_driver() directly, this function only provides shadow-binding
 * support for old legacy drivers on top of that core pci function.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_pci_init(struct drm_driver *driver, struct pci_driver *pdriver)
{
	struct pci_dev *pdev = NULL;
	const struct pci_device_id *pid;
	int i;

	DRM_DEBUG("\n");

	/* FreeBSD specific hackery */
	pdriver->busname = "vgapci";
	pdriver->bsdclass = &drm_devclass;
	pdriver->name = "drmn";

	if (driver->driver_features & DRIVER_MODESET)
		return pci_register_driver(pdriver);

	DRM_ERROR("FreeBSD needs DRIVER_MODESET");
	return (-ENOTSUP);
#ifdef __linux__	
	/* If not using KMS, fall back to stealth mode manual scanning. */
	INIT_LIST_HEAD(&driver->legacy_dev_list);
	for (i = 0; pdriver->id_table[i].vendor != 0; i++) {
		pid = &pdriver->id_table[i];

		/* Loop around setting up a DRM device for each PCI device
		 * matching our ID and device class.  If we had the internal
		 * function that pci_get_subsys and pci_get_class used, we'd
		 * be able to just pass pid in instead of doing a two-stage
		 * thing.
		 */
		pdev = NULL;
		while ((pdev =
			pci_get_subsys(pid->vendor, pid->device, pid->subvendor,
				       pid->subdevice, pdev)) != NULL) {
			if ((pdev->class & pid->class_mask) != pid->class)
				continue;

			/* stealth mode requires a manual probe */
			pci_dev_get(pdev);
			drm_get_pci_dev(pdev, pid, driver);
		}
	}
	return 0;
#endif	
}

int drm_pcie_get_speed_cap_mask(struct drm_device *dev, u32 *mask)
{
	struct pci_dev *root;
	u32 lnkcap, lnkcap2;

	*mask = 0;
	if (!dev->pdev)
		return -EINVAL;

	/* XXX need to initialize more of bus in linuxkpi - note to self */
	root = dev->pdev->bus->self;

	/* we've been informed via and serverworks don't make the cut */
	if (root->vendor == PCI_VENDOR_ID_VIA ||
	    root->vendor == PCI_VENDOR_ID_SERVERWORKS)
		return -EINVAL;

	pcie_capability_read_dword(root, PCI_EXP_LNKCAP, &lnkcap);
	pcie_capability_read_dword(root, PCI_EXP_LNKCAP2, &lnkcap2);

	if (lnkcap2) {	/* PCIe r3.0-compliant */
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_2_5GB)
			*mask |= DRM_PCIE_SPEED_25;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_5_0GB)
			*mask |= DRM_PCIE_SPEED_50;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_8_0GB)
			*mask |= DRM_PCIE_SPEED_80;
	} else {	/* pre-r3.0 */
		if (lnkcap & PCI_EXP_LNKCAP_SLS_2_5GB)
			*mask |= DRM_PCIE_SPEED_25;
		if (lnkcap & PCI_EXP_LNKCAP_SLS_5_0GB)
			*mask |= (DRM_PCIE_SPEED_25 | DRM_PCIE_SPEED_50);
	}

	DRM_INFO("probing gen 2 caps for device %x:%x = %x/%x\n", root->vendor, root->device, lnkcap, lnkcap2);
	return 0;
}
EXPORT_SYMBOL(drm_pcie_get_speed_cap_mask);

int drm_pcie_get_max_link_width(struct drm_device *dev, u32 *mlw)
{
	struct pci_dev *root;
	u32 lnkcap;

	*mlw = 0;
	if (!dev->pdev)
		return -EINVAL;

	root = dev->pdev->bus->self;

	pcie_capability_read_dword(root, PCI_EXP_LNKCAP, &lnkcap);

	*mlw = (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4;

	DRM_INFO("probing mlw for device %x:%x = %x\n", root->vendor, root->device, lnkcap);
	return 0;
}
EXPORT_SYMBOL(drm_pcie_get_max_link_width);

#else

int drm_pci_init(struct drm_driver *driver, struct pci_driver *pdriver)
{
	return -1;
}

void drm_pci_agp_destroy(struct drm_device *dev) {}

int drm_irq_by_busid(struct drm_device *dev, void *data,
		     struct drm_file *file_priv)
{
	return -EINVAL;
}

int drm_pci_set_unique(struct drm_device *dev,
		       struct drm_master *master,
		       struct drm_unique *u)
{
	return -EINVAL;
}
#endif

EXPORT_SYMBOL(drm_pci_init);

/**
 * drm_pci_exit - Unregister matching PCI devices from the DRM subsystem
 * @driver: DRM device driver
 * @pdriver: PCI device driver
 *
 * Unregisters one or more devices matched by a PCI driver from the DRM
 * subsystem.
 *
 * NOTE: This function is deprecated. Modern modesetting drm drivers should use
 * pci_unregister_driver() directly, this function only provides shadow-binding
 * support for old legacy drivers on top of that core pci function.
 */
void drm_pci_exit(struct drm_driver *driver, struct pci_driver *pdriver)
{
	struct drm_device *dev, *tmp;
	DRM_DEBUG("\n");

	if (driver->driver_features & DRIVER_MODESET) {
		pci_unregister_driver(pdriver);
	} else {
		list_for_each_entry_safe(dev, tmp, &driver->legacy_dev_list,
					 legacy_dev_list) {
			list_del(&dev->legacy_dev_list);
			drm_put_dev(dev);
		}
	}
	DRM_INFO("Module unloaded\n");
}
EXPORT_SYMBOL(drm_pci_exit);
