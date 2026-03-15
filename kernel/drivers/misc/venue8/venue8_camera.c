// SPDX-License-Identifier: GPL-2.0
/*
 * Dell Venue 8 3840 — Camera sensor platform glue for atomisp
 *
 * Provides platform_data callbacks for the OV5693 (rear, 5MP) and OV2722
 * (front, 2MP) cameras connected via MIPI CSI-2 to the Merrifield ISP2400.
 *
 * Camera power sequencing uses:
 *   - GPIO 1 (cam_en):    Camera 1.8V power enable
 *   - GPIO 2 (cam_pwdn):  Camera power down
 *   - GPIO 3 (vga_ldo_en): Front camera LDO enable
 *   - GPIO 7 (cam_pwr_en): Camera power enable
 *   - GPIO 9 (cam_0_rst): Rear camera reset
 *   - GPIO 10 (cam_1_rst): Front camera reset
 *   - GPIO 172 (CAM_0_AF_EN): Rear camera autofocus enable
 *   - PMIC vprog1: 2.8V + 1.8V camera analog supply
 *   - SCU IPC: OSC_CLK_CAM0 (19.2MHz MCLK for rear)
 *   - SCU IPC: OSC_CLK_CAM1 (19.2MHz MCLK for front)
 *
 * CSI-2 configuration (from Dell kernel):
 *   OV5693: Primary port,   2 lanes, RAW10, BGGR bayer order
 *   OV2722: Secondary port, 1 lane,  RAW10, RGGB bayer order
 *
 * GPIO values verified from /sys/kernel/debug/gpio on running device.
 *
 * Copyright (C) 2026 Thomas (venue8-flash project)
 * Based on Dell kernel 3.10 platform_ov5693.c / platform_ov2722.c
 */

#define pr_fmt(fmt) "venue8_cam: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/property.h>

#include <asm/intel_scu_ipc.h>

#include <media/v4l2-subdev.h>

/* Include atomisp platform header */
#include <linux/atomisp_platform.h>

/* Camera GPIO numbers (verified from device) */
#define GPIO_CAM_EN		1
#define GPIO_CAM_PWDN		2
#define GPIO_VGA_LDO_EN		3
#define GPIO_CAM_PWR_EN		7
#define GPIO_CAM_0_RST		9
#define GPIO_CAM_1_RST		10
#define GPIO_CAM_0_AF_EN	172

/* SCU IPC clock IDs */
#define OSC_CLK_CAM0		0
#define OSC_CLK_CAM1		1
#define CLK_19P2MHZ		19200

/* PMIC register for vprog1 (camera analog power) */
#define VPROG1_ENABLE		1
#define VPROG1_DISABLE		0

struct venue8_camera {
	struct gpio_desc *cam_en;
	struct gpio_desc *cam_pwdn;
	struct gpio_desc *vga_ldo_en;
	struct gpio_desc *cam_pwr_en;
	struct gpio_desc *cam0_rst;
	struct gpio_desc *cam1_rst;
	struct gpio_desc *cam0_af_en;
	bool rear_power_on;
	bool front_power_on;
};

static struct venue8_camera cam_data;

/* ------------------------------------------------------------------ */
/*  OV5693 (Rear, 5MP) Callbacks                                       */
/* ------------------------------------------------------------------ */

static int ov5693_gpio_ctrl(struct v4l2_subdev *sd, int flag)
{
	if (!cam_data.cam0_rst)
		return -ENODEV;

	gpiod_set_value_cansleep(cam_data.cam0_rst, flag ? 1 : 0);
	return 0;
}

static int ov5693_flisclk_ctrl(struct v4l2_subdev *sd, int flag)
{
	return intel_scu_ipc_osc_clk(OSC_CLK_CAM0,
				     flag ? CLK_19P2MHZ : 0);
}

static int ov5693_power_ctrl(struct v4l2_subdev *sd, int flag)
{
	int ret = 0;

	if (flag) {
		if (cam_data.cam_pwdn)
			gpiod_set_value_cansleep(cam_data.cam_pwdn, 1);
		if (cam_data.cam_en)
			gpiod_set_value_cansleep(cam_data.cam_en, 1);

		ret = intel_scu_ipc_msic_vprog1(VPROG1_ENABLE);
		usleep_range(10000, 11000);
	} else {
		if (cam_data.cam_pwdn)
			gpiod_set_value_cansleep(cam_data.cam_pwdn, 0);
		if (cam_data.cam_en)
			gpiod_set_value_cansleep(cam_data.cam_en, 0);

		ret = intel_scu_ipc_msic_vprog1(VPROG1_DISABLE);
	}

	cam_data.rear_power_on = flag;
	return ret;
}

static int ov5693_csi_configure(struct v4l2_subdev *sd, int flag)
{
	return camera_sensor_csi(sd, ATOMISP_CAMERA_PORT_PRIMARY, 2,
				 ATOMISP_INPUT_FORMAT_RAW_10,
				 atomisp_bayer_order_bggr, flag);
}

static struct camera_sensor_platform_data ov5693_platform_data = {
	.gpio_ctrl	= ov5693_gpio_ctrl,
	.flisclk_ctrl	= ov5693_flisclk_ctrl,
	.power_ctrl	= ov5693_power_ctrl,
	.csi_cfg	= ov5693_csi_configure,
};

/* ------------------------------------------------------------------ */
/*  OV2722 (Front, 2MP) Callbacks                                      */
/* ------------------------------------------------------------------ */

static int ov2722_gpio_ctrl(struct v4l2_subdev *sd, int flag)
{
	if (!cam_data.cam1_rst)
		return -ENODEV;

	if (flag) {
		gpiod_set_value_cansleep(cam_data.cam1_rst, 0);
		msleep(20);
		gpiod_set_value_cansleep(cam_data.cam1_rst, 1);
		if (cam_data.cam_pwr_en)
			gpiod_set_value_cansleep(cam_data.cam_pwr_en, 0);
		if (cam_data.vga_ldo_en)
			gpiod_set_value_cansleep(cam_data.vga_ldo_en, 1);
		usleep_range(10000, 11000);
	} else {
		gpiod_set_value_cansleep(cam_data.cam1_rst, 0);
		if (cam_data.cam_pwr_en)
			gpiod_set_value_cansleep(cam_data.cam_pwr_en, 0);
		if (cam_data.vga_ldo_en)
			gpiod_set_value_cansleep(cam_data.vga_ldo_en, 0);
	}
	return 0;
}

static int ov2722_flisclk_ctrl(struct v4l2_subdev *sd, int flag)
{
	return intel_scu_ipc_osc_clk(OSC_CLK_CAM1,
				     flag ? CLK_19P2MHZ : 0);
}

static int ov2722_power_ctrl(struct v4l2_subdev *sd, int flag)
{
	int ret = 0;

	if (flag) {
		if (!cam_data.front_power_on) {
			ret = intel_scu_ipc_msic_vprog1(VPROG1_ENABLE);
			if (!ret)
				cam_data.front_power_on = true;
		}
	} else {
		if (cam_data.front_power_on) {
			ret = intel_scu_ipc_msic_vprog1(VPROG1_DISABLE);
			if (!ret)
				cam_data.front_power_on = false;
		}
	}
	return ret;
}

static int ov2722_csi_configure(struct v4l2_subdev *sd, int flag)
{
	return camera_sensor_csi(sd, ATOMISP_CAMERA_PORT_SECONDARY, 1,
				 ATOMISP_INPUT_FORMAT_RAW_10,
				 atomisp_bayer_order_rggb, flag);
}

static struct camera_sensor_platform_data ov2722_platform_data = {
	.gpio_ctrl	= ov2722_gpio_ctrl,
	.flisclk_ctrl	= ov2722_flisclk_ctrl,
	.power_ctrl	= ov2722_power_ctrl,
	.csi_cfg	= ov2722_csi_configure,
};

/* ------------------------------------------------------------------ */
/*  GPIO Lookup Table                                                  */
/* ------------------------------------------------------------------ */

static struct gpiod_lookup_table venue8_camera_gpios = {
	.dev_id = "venue8_camera",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_CAM_EN,
			    "cam-en", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_CAM_PWDN,
			    "cam-pwdn", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_VGA_LDO_EN,
			    "vga-ldo-en", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_CAM_PWR_EN,
			    "cam-pwr-en", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_CAM_0_RST,
			    "cam0-rst", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_CAM_1_RST,
			    "cam1-rst", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_CAM_0_AF_EN,
			    "cam0-af-en", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/* ------------------------------------------------------------------ */
/*  Platform driver                                                    */
/* ------------------------------------------------------------------ */

static int venue8_camera_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	/* Request GPIOs */
	cam_data.cam_en = devm_gpiod_get_optional(dev, "cam-en",
						   GPIOD_OUT_LOW);
	cam_data.cam_pwdn = devm_gpiod_get_optional(dev, "cam-pwdn",
						      GPIOD_OUT_LOW);
	cam_data.vga_ldo_en = devm_gpiod_get_optional(dev, "vga-ldo-en",
						        GPIOD_OUT_LOW);
	cam_data.cam_pwr_en = devm_gpiod_get_optional(dev, "cam-pwr-en",
						        GPIOD_OUT_LOW);
	cam_data.cam0_rst = devm_gpiod_get_optional(dev, "cam0-rst",
						      GPIOD_OUT_LOW);
	cam_data.cam1_rst = devm_gpiod_get_optional(dev, "cam1-rst",
						      GPIOD_OUT_HIGH);
	cam_data.cam0_af_en = devm_gpiod_get_optional(dev, "cam0-af-en",
						        GPIOD_OUT_LOW);

	dev_info(dev, "Venue 8 camera platform ready (OV5693 + OV2722)\n");
	return 0;
}

static struct platform_driver venue8_camera_driver = {
	.probe = venue8_camera_probe,
	.driver = {
		.name = "venue8_camera",
	},
};

/* ------------------------------------------------------------------ */
/*  Module init — register GPIO table and export platform data         */
/* ------------------------------------------------------------------ */

/*
 * These symbols are referenced by the atomisp staging driver's sensor
 * subdevice code to configure power and CSI.
 */
void *venue8_ov5693_platform_data(void *info)
{
	return &ov5693_platform_data;
}
EXPORT_SYMBOL_GPL(venue8_ov5693_platform_data);

void *venue8_ov2722_platform_data(void *info)
{
	return &ov2722_platform_data;
}
EXPORT_SYMBOL_GPL(venue8_ov2722_platform_data);

static struct platform_device venue8_camera_device = {
	.name = "venue8_camera",
	.id = -1,
};

static int __init venue8_camera_init(void)
{
	int ret;

	gpiod_add_lookup_table(&venue8_camera_gpios);

	ret = platform_driver_register(&venue8_camera_driver);
	if (ret)
		return ret;

	ret = platform_device_register(&venue8_camera_device);
	if (ret) {
		platform_driver_unregister(&venue8_camera_driver);
		return ret;
	}

	pr_info("camera platform registered\n");
	return 0;
}

static void __exit venue8_camera_exit(void)
{
	platform_device_unregister(&venue8_camera_device);
	platform_driver_unregister(&venue8_camera_driver);
	gpiod_remove_lookup_table(&venue8_camera_gpios);
}

module_init(venue8_camera_init);
module_exit(venue8_camera_exit);

MODULE_DESCRIPTION("Dell Venue 8 3840 camera sensor platform glue for atomisp");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomas <venue8-flash project>");
