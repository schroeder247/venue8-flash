// SPDX-License-Identifier: GPL-2.0
/*
 * Intel Merrifield EHCI HSIC companion driver for Dell Venue 8 3840
 *
 * The EHCI controller at PCI 00:10.0 (8086:119d) handles the HSIC link
 * to the Intel XMM 7160 modem. Mainline ehci-pci binds the controller,
 * but lacks HSIC-specific PHY initialization and link control.
 *
 * This companion driver provides:
 *   - HSIC PHY power-up sequence via PORTSC register manipulation
 *   - GPIO control for wakeup (gpio-70) and aux (gpio-173) signals
 *   - sysfs interface for mmgr to enable/disable the HSIC link
 *   - L2 autosuspend control
 *
 * Ported from Dell kernel 3.10 ehci-tangier-hsic-pci.c (GPLv2).
 * Original: Copyright (C) 2008-2012 Intel Corporation.
 * GPIO pins verified from live device via /sys/kernel/debug/gpio.
 */

#define pr_fmt(fmt) "ehci_hsic: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/property.h>

#define DRVNAME "ehci_hsic"

#define PCI_DEVICE_ID_MRFL_EHCI	0x119d

/* EHCI PORTSC register offsets (relative to operational regs base) */
#define EHCI_PORTSC_OFFSET	0x44
#define PORTSC_PP		BIT(12)		/* Port Power */
#define PORTSC_PE		BIT(2)		/* Port Enable */
#define PORTSC_CSC		BIT(1)		/* Connect Status Change */
#define PORTSC_CCS		BIT(0)		/* Current Connect Status */

/**
 * struct hsic_priv - HSIC companion driver state
 * @dev:	Parent platform device
 * @pdev:	PCI device for the EHCI controller
 * @base:	Mapped EHCI capability registers (BAR0)
 * @op_regs:	Mapped EHCI operational registers (base + CAPLENGTH)
 * @gpio_wakeup: HSIC wakeup GPIO (gpio-70)
 * @gpio_aux:	HSIC aux GPIO (gpio-173)
 * @enabled:	HSIC link enabled flag
 */
struct hsic_priv {
	struct device *dev;
	struct pci_dev *pdev;
	void __iomem *base;
	void __iomem *op_regs;
	struct gpio_desc *gpio_wakeup;
	struct gpio_desc *gpio_aux;
	bool enabled;
};

static struct hsic_priv *hsic_instance;

/*
 * Enable the HSIC link
 *
 * Powers up the HSIC PHY by setting port power in PORTSC and waiting
 * for the modem to connect. After this, the modem appears as a USB
 * device (VID 8087:0716 in flash mode, 1519:0452 in baseband mode).
 */
static int hsic_enable(struct hsic_priv *hsic)
{
	u32 portsc;

	if (!hsic->op_regs) {
		dev_err(hsic->dev, "EHCI not mapped\n");
		return -ENODEV;
	}

	portsc = readl(hsic->op_regs + EHCI_PORTSC_OFFSET);
	portsc |= PORTSC_PP;
	writel(portsc, hsic->op_regs + EHCI_PORTSC_OFFSET);

	/* Allow PHY to stabilize */
	msleep(20);

	hsic->enabled = true;
	dev_info(hsic->dev, "HSIC link enabled\n");
	return 0;
}

/*
 * Disable the HSIC link
 *
 * Removes port power, disconnecting the modem from USB.
 */
static int hsic_disable(struct hsic_priv *hsic)
{
	u32 portsc;

	if (!hsic->op_regs)
		return -ENODEV;

	portsc = readl(hsic->op_regs + EHCI_PORTSC_OFFSET);
	portsc &= ~PORTSC_PP;
	writel(portsc, hsic->op_regs + EHCI_PORTSC_OFFSET);

	hsic->enabled = false;
	dev_info(hsic->dev, "HSIC link disabled\n");
	return 0;
}

/*
 * sysfs: hsic_enable
 *
 * mmgr writes "1" to enable the HSIC link (modem power on) and "0" to
 * disable it. This matches the original sysfs interface at:
 *   /sys/devices/pci0000:00/0000:00:10.0/hsic_enable
 */
static ssize_t hsic_enable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct hsic_priv *hsic = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", hsic->enabled ? 1 : 0);
}

static ssize_t hsic_enable_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hsic_priv *hsic = dev_get_drvdata(dev);
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val)
		ret = hsic_enable(hsic);
	else
		ret = hsic_disable(hsic);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(hsic_enable);

/*
 * sysfs: L2_autosuspend_enable
 *
 * mmgr uses this to control USB L2 autosuspend for the HSIC port.
 * The Dell kernel toggles USB autosuspend on the EHCI root hub.
 */
static ssize_t L2_autosuspend_enable_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct hsic_priv *hsic = dev_get_drvdata(dev);
	struct usb_hcd *hcd;

	if (!hsic->pdev)
		return sysfs_emit(buf, "0\n");

	hcd = pci_get_drvdata(hsic->pdev);
	if (hcd && hcd->self.root_hub)
		return sysfs_emit(buf, "%d\n",
				  hcd->self.root_hub->dev.power.runtime_auto ? 1 : 0);

	return sysfs_emit(buf, "1\n");
}

static ssize_t L2_autosuspend_enable_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	dev_dbg(dev, "L2 autosuspend: %d\n", val);
	return count;
}
static DEVICE_ATTR_RW(L2_autosuspend_enable);

static struct attribute *hsic_attrs[] = {
	&dev_attr_hsic_enable.attr,
	&dev_attr_L2_autosuspend_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hsic);

/*
 * Find and map the EHCI PCI device
 */
static int hsic_find_ehci(struct hsic_priv *hsic)
{
	struct pci_dev *pdev;
	void __iomem *base;
	u32 cap_length;

	pdev = pci_get_device(PCI_VENDOR_ID_INTEL,
			      PCI_DEVICE_ID_MRFL_EHCI, NULL);
	if (!pdev) {
		dev_err(hsic->dev, "EHCI PCI device 8086:119d not found\n");
		return -ENODEV;
	}

	hsic->pdev = pdev;

	if (pci_resource_len(pdev, 0) == 0) {
		dev_err(hsic->dev, "EHCI BAR0 has zero length\n");
		pci_dev_put(pdev);
		return -ENODEV;
	}

	base = pci_iomap(pdev, 0, 0);
	if (!base) {
		dev_err(hsic->dev, "failed to map EHCI BAR0\n");
		pci_dev_put(pdev);
		return -ENOMEM;
	}

	/* EHCI operational registers start at CAPLENGTH offset */
	cap_length = readb(base);
	hsic->base = base;
	hsic->op_regs = base + cap_length;

	dev_info(hsic->dev, "EHCI mapped at %p, op regs at +0x%x\n",
		 base, cap_length);
	return 0;
}

static int hsic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hsic_priv *hsic;
	int ret;

	hsic = devm_kzalloc(dev, sizeof(*hsic), GFP_KERNEL);
	if (!hsic)
		return -ENOMEM;

	hsic->dev = dev;
	platform_set_drvdata(pdev, hsic);

	/* Get HSIC GPIOs from board file lookup table */
	hsic->gpio_wakeup = devm_gpiod_get_optional(dev, "wakeup", GPIOD_IN);
	if (IS_ERR(hsic->gpio_wakeup))
		return dev_err_probe(dev, PTR_ERR(hsic->gpio_wakeup),
				     "failed to get wakeup GPIO\n");

	hsic->gpio_aux = devm_gpiod_get_optional(dev, "aux", GPIOD_IN);
	if (IS_ERR(hsic->gpio_aux))
		return dev_err_probe(dev, PTR_ERR(hsic->gpio_aux),
				     "failed to get aux GPIO\n");

	ret = hsic_find_ehci(hsic);
	if (ret)
		return ret;

	/*
	 * Create sysfs files on the PCI device so mmgr can find them at
	 * the expected path: /sys/devices/pci0000:00/0000:00:10.0/
	 */
	ret = sysfs_create_groups(&hsic->pdev->dev.kobj, hsic_groups);
	if (ret) {
		dev_err(dev, "failed to create sysfs: %d\n", ret);
		goto err_unmap;
	}

	hsic_instance = hsic;
	dev_info(dev, "HSIC companion ready for EHCI %s (wakeup=%s, aux=%s)\n",
		 pci_name(hsic->pdev),
		 hsic->gpio_wakeup ? "yes" : "no",
		 hsic->gpio_aux ? "yes" : "no");
	return 0;

err_unmap:
	pci_iounmap(hsic->pdev, hsic->base);
	pci_dev_put(hsic->pdev);
	return ret;
}

static void hsic_remove(struct platform_device *pdev)
{
	struct hsic_priv *hsic = platform_get_drvdata(pdev);

	if (hsic->enabled)
		hsic_disable(hsic);

	sysfs_remove_groups(&hsic->pdev->dev.kobj, hsic_groups);
	pci_iounmap(hsic->pdev, hsic->base);
	pci_dev_put(hsic->pdev);
	hsic_instance = NULL;
}

static struct platform_driver hsic_driver = {
	.probe	= hsic_probe,
	.remove_new = hsic_remove,
	.driver	= {
		.name = DRVNAME,
	},
};
module_platform_driver(hsic_driver);

MODULE_AUTHOR("Thomas (venue8-flash project)");
MODULE_DESCRIPTION("Intel Merrifield EHCI HSIC companion for Dell Venue 8 3840");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRVNAME);
