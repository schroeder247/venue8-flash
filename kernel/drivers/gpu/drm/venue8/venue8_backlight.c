// SPDX-License-Identifier: GPL-2.0
/*
 * Dell Venue 8 3840 — Backlight driver
 *
 * Controls the panel backlight via the Tangier display controller's
 * BLC (Backlight Control) PWM register, accessible through the GPU's
 * MMIO BAR.
 *
 * Based on Dell kernel 3.10 psb_bl.c:
 *   - Brightness range: 0-100 (maps to PWM duty cycle)
 *   - PWM inverted: duty = 0xFF - (0xFF * level / max_level)
 *   - Two panel variants: AUO (max 125) and Innolux (max 109)
 *     We use 100 as the user-facing max and scale internally.
 *
 * Copyright (C) 2026 Thomas (venue8-flash project)
 */

#define pr_fmt(fmt) "venue8_bl: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>
#include <linux/pci.h>
#include <linux/io.h>

/* BLC (Backlight Control) PWM registers in GPU MMIO space */
#define BLC_PWM_CTL		0x61254
#define BLC_PWM_CTL2		0x61250

/* PWM frequency and duty cycle fields */
#define BACKLIGHT_DUTY_MASK	0x0000ffff
#define BACKLIGHT_FREQ_MASK	0xffff0000
#define BACKLIGHT_FREQ_SHIFT	16

/* PWM hardware max from Dell kernel */
#define HW_MAX_LEVEL		125
#define USER_MAX_LEVEL		100
#define USER_DEFAULT_LEVEL	50

struct venue8_backlight {
	void __iomem *mmio;	/* GPU MMIO base */
	struct backlight_device *bd;
};

static int venue8_bl_update_status(struct backlight_device *bd)
{
	struct venue8_backlight *bl = bl_get_data(bd);
	int brightness = bd->props.brightness;
	u32 duty_val;
	u32 pwm;

	if (bd->props.power != FB_BLANK_UNBLANK ||
	    bd->props.state & BL_CORE_SUSPENDED)
		brightness = 0;

	/* Scale user 0-100 to hardware 0-HW_MAX_LEVEL */
	duty_val = (0xFF * brightness * HW_MAX_LEVEL) /
		   (USER_MAX_LEVEL * HW_MAX_LEVEL);
	/* PWM is inverted: 0xFF = off, 0x00 = full brightness */
	duty_val = 0xFF - duty_val;

	pwm = ioread32(bl->mmio + BLC_PWM_CTL);
	pwm = (pwm & BACKLIGHT_FREQ_MASK) | (duty_val & BACKLIGHT_DUTY_MASK);
	iowrite32(pwm, bl->mmio + BLC_PWM_CTL);

	return 0;
}

static int venue8_bl_get_brightness(struct backlight_device *bd)
{
	struct venue8_backlight *bl = bl_get_data(bd);
	u32 pwm;
	u32 duty_val;
	int brightness;

	pwm = ioread32(bl->mmio + BLC_PWM_CTL);
	duty_val = 0xFF - (pwm & BACKLIGHT_DUTY_MASK);
	brightness = (duty_val * USER_MAX_LEVEL) / 0xFF;

	return clamp(brightness, 0, USER_MAX_LEVEL);
}

static const struct backlight_ops venue8_bl_ops = {
	.update_status	= venue8_bl_update_status,
	.get_brightness	= venue8_bl_get_brightness,
};

static int venue8_bl_probe(struct platform_device *pdev)
{
	struct venue8_backlight *bl;
	struct backlight_properties props;
	struct pci_dev *gpu_pci;

	/* Find the GPU PCI device to get MMIO BAR */
	gpu_pci = pci_get_device(PCI_VENDOR_ID_INTEL, 0x1180, NULL);
	if (!gpu_pci) {
		dev_err(&pdev->dev, "GPU PCI device not found\n");
		return -ENODEV;
	}

	bl = devm_kzalloc(&pdev->dev, sizeof(*bl), GFP_KERNEL);
	if (!bl) {
		pci_dev_put(gpu_pci);
		return -ENOMEM;
	}

	bl->mmio = pci_iomap(gpu_pci, 0, 0);
	pci_dev_put(gpu_pci);
	if (!bl->mmio) {
		dev_err(&pdev->dev, "failed to map GPU BAR0\n");
		return -ENOMEM;
	}

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = USER_MAX_LEVEL;
	props.brightness = USER_DEFAULT_LEVEL;
	props.power = FB_BLANK_UNBLANK;

	bl->bd = devm_backlight_device_register(&pdev->dev, "psb-bl",
						&pdev->dev, bl,
						&venue8_bl_ops, &props);
	if (IS_ERR(bl->bd)) {
		pci_iounmap(gpu_pci, bl->mmio);
		return PTR_ERR(bl->bd);
	}

	platform_set_drvdata(pdev, bl);

	/* Set initial brightness */
	backlight_update_status(bl->bd);

	dev_info(&pdev->dev, "backlight registered (max=%d, default=%d)\n",
		 USER_MAX_LEVEL, USER_DEFAULT_LEVEL);
	return 0;
}

static struct platform_driver venue8_bl_driver = {
	.probe = venue8_bl_probe,
	.driver = {
		.name = "venue8-backlight",
	},
};
module_platform_driver(venue8_bl_driver);

MODULE_DESCRIPTION("Dell Venue 8 3840 backlight driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:venue8-backlight");
