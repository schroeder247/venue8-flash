// SPDX-License-Identifier: GPL-2.0
/*
 * Dell Venue 8 3840 (Merrifield/Saltbay) board-level device registration
 *
 * Replaces SFI-based device enumeration removed from mainline in kernel 5.12.
 * All device data verified against the running Dell Venue 8 3840 via ADB:
 *   /sys/bus/i2c/devices/        - I2C device addresses and bus numbers
 *   /sys/kernel/debug/gpio       - GPIO pin assignments and labels
 *   /sys/class/power_supply/     - Charger and fuel gauge identification
 *   /proc/bus/input/devices      - Input device bus/address mapping
 *
 * Copyright (C) 2026 Thomas (venue8-flash project)
 * Based on Intel MID SFI code: Copyright (C) 2012-2014 Intel Corporation.
 */

#define pr_fmt(fmt) "venue8: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/gpio/machine.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/fixed.h>
#include <linux/mfd/wm8994/pdata.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/leds.h>

#include <asm/cpu_device_id.h>
#include <asm/intel-mid.h>

/*
 * PCI device IDs for Merrifield South Complex
 *
 * Verified via /sys/bus/pci/devices/0000:00:XX.Y/device on device.
 */
#define PCI_DEVICE_ID_MRFL_SCU_IPC	0x08ea
#define PCI_DEVICE_ID_MRFL_GPIO		0x1199
#define PCI_DEVICE_ID_MRFL_I2C_0	0x1195	/* 00:08.0-3 */
#define PCI_DEVICE_ID_MRFL_I2C_1	0x1196	/* 00:09.0-2 */
#define PCI_DEVICE_ID_MRFL_I2C_2	0x1197	/* 00:0a.0 */
#define PCI_DEVICE_ID_MRFL_EHCI		0x119d	/* 00:10.0 */
#define PCI_DEVICE_ID_MRFL_DWC3_OTG	0x119e
#define PCI_DEVICE_ID_MRFL_SST		0x119a
#define PCI_DEVICE_ID_MRFL_GPU		0x1180
#define PCI_DEVICE_ID_MRFL_ISP		0x1178

/*
 * I2C bus assignments for the Venue 8 3840
 *
 * Verified via:
 *   readlink /sys/bus/i2c/devices/i2c-N/device  -> PCI BDF
 *   cat /sys/bus/i2c/devices/N-XXXX/name
 *
 * PCI Designware controllers:
 *   i2c-1 = 00:08.0 (0x1195)   i2c-5 = 00:09.0 (0x1196)
 *   i2c-2 = 00:08.1 (0x1195)   i2c-6 = 00:09.1 (0x1196)
 *   i2c-3 = 00:08.2 (0x1195)   i2c-7 = 00:09.2 (0x1196)
 *   i2c-4 = 00:08.3 (0x1195)
 *
 * PMIC adapter:
 *   i2c-8 = "PMIC I2C Adapter" (Basin Cove internal, no PCI parent)
 */
#define I2C_BUS_AUDIO		1	/* WM8958 audio codec (1-001a) */
#define I2C_BUS_FUEL_GAUGE	2	/* BQ27441 fuel gauge (2-0055) */
#define I2C_BUS_CAMERA		4	/* OV5693 (4-0010), OV2722 (4-0036) */
#define I2C_BUS_SENSORS		6	/* LIS3DH, LSM303D, BH1721, L3GD20 */
#define I2C_BUS_TOUCH		7	/* eKTF2k (7-0010), Synaptics (7-0020) */
#define I2C_BUS_PMIC		8	/* BQ24261 charger (8-006b) */

/*
 * GPIO numbers for the Venue 8
 *
 * Verified from /sys/kernel/debug/gpio on running device.
 * All GPIOs are on "0000:00:0c.0" (merrifield_pinctrl), range 0-191.
 */

/* Modem control (ModemControl_* labels in debugfs) */
#define GPIO_MODEM_COREDUMP	154	/* ModemControl_CORE_DU, in */
#define GPIO_MODEM_RST_OUT	180	/* ModemControl_RST_OUT, in */
#define GPIO_MODEM_PWR_ON	181	/* ModemControl_ON, out */
#define GPIO_MODEM_RST_BBN	182	/* ModemControl_RST_BB, out */

/* HSIC USB link */
#define GPIO_HSIC_WAKEUP	70	/* hsic_wakeup, in */
#define GPIO_HSIC_AUX		173	/* hsic_aux, in */

/* Bluetooth (bcm_bt_lpm labels) */
#define GPIO_BT_RESET		71	/* bcm_bt_lpm, out */
#define GPIO_BT_WAKE_DEV	184	/* bcm_bt_lpm, out (device-wakeup) */
#define GPIO_BT_WAKE_HOST	185	/* bcm_bt_lpm, in (host-wakeup) */

/* WiFi */
#define GPIO_WLAN_EN		96	/* vwlan, out hi */

/* Touchscreen */
#define GPIO_TOUCH_INT		183	/* unnamed, in hi */
#define GPIO_TOUCH_RST		175	/* unnamed, out hi */

/* Sensors */
#define GPIO_ALS_INT		44	/* als_int, out hi */
#define GPIO_ACCEL_INT1		46	/* INTERRUPT_PIN1_LSM30, in */
#define GPIO_ACCEL_INT2		47	/* INTERRUPT_PIN2_LSM30, in */
#define GPIO_FUEL_GAUGE_ALERT	52

/* Camera */
#define GPIO_CAM_EN		1	/* cam_en */
#define GPIO_CAM_PWDN		2	/* cam_pwdn */
#define GPIO_VGA_LDO_EN		3	/* vga_ldo_en */
#define GPIO_CAM_PWR_EN		7	/* cam_pwr_en */
#define GPIO_CAM_0_RST		9	/* cam_0_rst */
#define GPIO_CAM_1_RST		10	/* cam_1_rst */
#define GPIO_CAM_0_AF_EN	172	/* CAM_0_AF_EN */

/* Audio codec (IRQ derived: /proc/interrupts shows IRQ 270 = GPIO 14+256) */
#define GPIO_AUDIOCODEC_INT	14

/* Volume buttons */
#define GPIO_VOLUME_DOWN	61	/* volume_down, in hi */
#define GPIO_VOLUME_UP		62	/* volume_up, in hi */

/* LEDs */
#define GPIO_LED_YELLOW		8	/* yellow, out lo */
#define GPIO_LED_WHITE		55	/* white, out lo */

/* SD card detect */
#define GPIO_SD_CD		77	/* sd_cd, in hi */

/* GPS */
#define GPIO_GPS_ENABLE		165	/* intel_mid_gps_enable, out lo */

/* HDMI */
#define GPIO_HDMI_HPD		16	/* hdmi_hpd, in lo */
#define GPIO_HDMI_LS_EN		177	/* HDMI_LS_EN, in lo */

/* Display */
#define GPIO_LCD_3P3		189	/* LCD_3P3 */
#define GPIO_LCD_RESET		190	/* LCD_RESET */

/* ------------------------------------------------------------------ */
/*  WM8958 audio codec regulators                                      */
/* ------------------------------------------------------------------ */

/*
 * The WM8958 needs external supplies for digital and speaker domains.
 * On the Venue 8 these are always-on rails from the Basin Cove PMIC.
 * We model them as fixed regulators so the WM8994 MFD driver can
 * request them through the regulator framework.
 *
 * Supply string format: "<bus>-<addr>" where bus=1, addr=001a.
 */
static struct regulator_consumer_supply venue8_vcc18_consumers[] = {
	REGULATOR_SUPPLY("DBVDD", "1-001a"),
	REGULATOR_SUPPLY("DBVDD1", "1-001a"),
	REGULATOR_SUPPLY("DBVDD2", "1-001a"),
	REGULATOR_SUPPLY("DBVDD3", "1-001a"),
	REGULATOR_SUPPLY("AVDD2", "1-001a"),
	REGULATOR_SUPPLY("CPVDD", "1-001a"),
};

static struct regulator_init_data venue8_vcc18_data = {
	.constraints = {
		.always_on = 1,
	},
	.num_consumer_supplies = ARRAY_SIZE(venue8_vcc18_consumers),
	.consumer_supplies = venue8_vcc18_consumers,
};

static struct fixed_voltage_config venue8_vcc18_config = {
	.supply_name	= "VCC_1.8V_PDA",
	.microvolts	= 1800000,
	.init_data	= &venue8_vcc18_data,
};

static struct platform_device venue8_vcc18_device = {
	.name = "reg-fixed-voltage",
	.id = 0,
	.dev = {
		.platform_data = &venue8_vcc18_config,
	},
};

/* Speaker supply from battery rail */
static struct regulator_consumer_supply venue8_vbat_consumers[] = {
	REGULATOR_SUPPLY("SPKVDD1", "1-001a"),
	REGULATOR_SUPPLY("SPKVDD2", "1-001a"),
};

static struct regulator_init_data venue8_vbat_data = {
	.constraints = {
		.always_on = 1,
	},
	.num_consumer_supplies = ARRAY_SIZE(venue8_vbat_consumers),
	.consumer_supplies = venue8_vbat_consumers,
};

static struct fixed_voltage_config venue8_vbat_config = {
	.supply_name	= "V_BAT",
	.microvolts	= 3700000,
	.init_data	= &venue8_vbat_data,
};

static struct platform_device venue8_vbat_device = {
	.name = "reg-fixed-voltage",
	.id = 1,
	.dev = {
		.platform_data = &venue8_vbat_config,
	},
};

/* ------------------------------------------------------------------ */
/*  WM8958 audio codec platform data                                   */
/* ------------------------------------------------------------------ */

/*
 * GPIO defaults configure the WM8958 internal GPIOs for:
 *   GPIO1:       Logic level input/output
 *   GPIO3-5,7:   AIF2 voice call interface
 *   GPIO8:       Codec interrupt (for jack detect)
 *   GPIO9-11:    AIF3 Bluetooth SCO interface
 *
 * Values from Dell kernel platform_wm8994.c, verified by audio jack input
 * device presence at /devices/platform/mrfld_wm8958/sound/card1/input7.
 */
static struct wm8994_pdata venue8_wm8994_pdata = {
	.gpio_defaults = {
		[0]  = 0x0003,	/* GPIO1:  logic level I/O */
		[2]  = 0x8100,	/* GPIO3:  AIF2 ADCDAT */
		[3]  = 0x8100,	/* GPIO4:  AIF2 ADCLRCLK */
		[4]  = 0x8100,	/* GPIO5:  AIF2 DACDAT */
		[6]  = 0x0100,	/* GPIO7:  AIF2 DACDAT */
		[7]  = 0x0003,	/* GPIO8:  codec interrupt */
		[8]  = 0x0105,	/* GPIO9:  AIF3 ADCDAT (BT) */
		[9]  = 0x0100,	/* GPIO10: AIF3 DACDAT (BT) */
		[10] = 0x0100,	/* GPIO11: AIF3 LRCLK (BT) */
	},
	.irq_flags = IRQF_TRIGGER_RISING | IRQF_ONESHOT,

	/* Jack detection timing (ms) */
	.mic_id_delay = 300,
	.micdet_delay = 500,
	.micb2_delay = 5000,

	/* LDO pins always driven (connected to PMIC, won't float) */
	.ldo_ena_always_driven = true,

	/* IRQ GPIO for edge-triggered interrupt (no level IRQ on this host) */
	.irq_gpio = GPIO_AUDIOCODEC_INT,
};

/* ------------------------------------------------------------------ */
/*  I2C device declarations                                            */
/* ------------------------------------------------------------------ */

/*
 * Each bus gets its own board_info array, registered via
 * i2c_register_board_info() at initcall time.  This mirrors what
 * SFI's sfi_parse_devs() did in the Dell 3.10 kernel.
 *
 * All bus numbers and addresses verified from /sys/bus/i2c/devices/.
 */

/* Bus 1 (PCI 00:08.0): WM8958 audio codec */
static struct i2c_board_info venue8_i2c1_devs[] __initdata = {
	{
		I2C_BOARD_INFO("wm8994", 0x1a),
		.platform_data = &venue8_wm8994_pdata,
	},
};

/* Bus 2 (PCI 00:08.1): BQ27441 fuel gauge */
static struct i2c_board_info venue8_i2c2_devs[] __initdata = {
	{
		I2C_BOARD_INFO("bq27441", 0x55),
	},
};

/*
 * Bus 4 (PCI 00:08.3): Camera sensors
 *
 * These are registered here for I2C probing, but full camera support
 * also requires atomisp platform data for CSI lanes, power sequencing,
 * and PMIC camera rail control.
 */
static struct i2c_board_info venue8_i2c4_devs[] __initdata = {
	{
		/* Sony/Omnivision OV5693 5MP rear camera */
		I2C_BOARD_INFO("ov5693", 0x10),
	},
	{
		/* Omnivision OV2722 2MP front camera */
		I2C_BOARD_INFO("ov2722", 0x36),
	},
};

/*
 * Bus 6 (PCI 00:09.1): Sensors
 *
 * IRQ mapping: Merrifield GPIO-to-IRQ = gpio_number + 256.
 * Verified: GPIO 46 → IRQ 302, GPIO 47 → IRQ 303.
 */
static struct i2c_board_info venue8_i2c6_devs[] __initdata = {
	{
		/* STMicro LIS3DH accelerometer */
		I2C_BOARD_INFO("lis3dh", 0x18),
	},
	{
		/* STMicro LSM303D accelerometer + magnetometer */
		I2C_BOARD_INFO("lsm303d", 0x1e),
		.irq = GPIO_ACCEL_INT1 + 256,
	},
	{
		/* ROHM BH1721 ambient light sensor */
		I2C_BOARD_INFO("bh1721", 0x23),
		.irq = GPIO_ALS_INT + 256,
	},
	{
		/* STMicro L3GD20 gyroscope */
		I2C_BOARD_INFO("l3gd20_gyr", 0x6b),
	},
};

/*
 * Bus 7 (PCI 00:09.2): Touchscreen controllers
 *
 * The Dell Venue 8 has BOTH an ELAN eKTF2k and a Synaptics DSX
 * touch controller on the same bus.  The Synaptics is the primary
 * touch input (verified as input4 in /proc/bus/input/devices with
 * active touch_input and active_pen phys).
 *
 * For mainline: Synaptics → rmi4_i2c (drivers/input/rmi4/)
 *               ELAN → elants_i2c (drivers/input/touchscreen/)
 */
static struct i2c_board_info venue8_i2c7_devs[] __initdata = {
	{
		/* ELAN eKTF2k at 0x10 */
		I2C_BOARD_INFO("ekth3500", 0x10),
		.irq = GPIO_TOUCH_INT + 256,
	},
	{
		/* Synaptics DSX at 0x20 (primary touch + pen) */
		I2C_BOARD_INFO("rmi4_i2c", 0x20),
		.irq = GPIO_TOUCH_INT + 256,
	},
};

/*
 * Bus 8 (PMIC I2C Adapter): BQ24261 charger
 *
 * The charger sits on the Basin Cove PMIC's internal I2C bus, NOT on a
 * PCI Designware controller.  The PMIC MFD driver (intel_soc_pmic_mrfld)
 * must create this adapter for the charger to probe.
 *
 * BQ24261 has no mainline driver.  The BQ24257 driver is the closest
 * register-compatible match (same TI BQ2425x/2426x family).
 */
static struct i2c_board_info venue8_i2c8_devs[] __initdata = {
	{
		I2C_BOARD_INFO("bq24257", 0x6b),
	},
};

/* ------------------------------------------------------------------ */
/*  GPIO lookup tables                                                 */
/* ------------------------------------------------------------------ */

/*
 * All pin numbers verified from /sys/kernel/debug/gpio on running device.
 * GPIO controller: "0000:00:0c.0" = merrifield_pinctrl, pins 0-191.
 */

/* Modem control GPIOs */
static struct gpiod_lookup_table venue8_modem_gpios = {
	.dev_id = "modem_control",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_MODEM_RST_OUT,
			    "rst-out", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_MODEM_PWR_ON,
			    "pwr-on", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_MODEM_RST_BBN,
			    "rst-bbn", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_MODEM_COREDUMP,
			    "coredump", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/* HSIC USB link GPIOs */
static struct gpiod_lookup_table venue8_hsic_gpios = {
	.dev_id = "ehci_hsic",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_HSIC_WAKEUP,
			    "wakeup", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_HSIC_AUX,
			    "aux", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/* Fuel gauge alert GPIO */
static struct gpiod_lookup_table venue8_fuel_gauge_gpios = {
	.dev_id = "i2c-bq27441",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_FUEL_GAUGE_ALERT,
			    "alert", GPIO_ACTIVE_LOW),
		{ },
	},
};

/* Touchscreen GPIOs (shared between ELAN and Synaptics) */
static struct gpiod_lookup_table venue8_touch_gpios = {
	.dev_id = "i2c-rmi4_i2c",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_TOUCH_INT,
			    "attn", GPIO_ACTIVE_LOW),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_TOUCH_RST,
			    "reset", GPIO_ACTIVE_LOW),
		{ },
	},
};

/*
 * WiFi GPIO (BCM4335 via brcmfmac on SDIO)
 *
 * gpio-96 "vwlan" controls WLAN chip power.  brcmfmac SDIO expects the
 * SDIO controller's platform data or firmware to handle power, but the
 * GPIO is available for manual control if needed.
 */
static struct gpiod_lookup_table venue8_wifi_gpios = {
	.dev_id = "brcmfmac",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_WLAN_EN,
			    "wlan-enable", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/*
 * Bluetooth GPIOs (BCM4335 via hci_bcm on UART)
 *
 * Three GPIOs verified from debugfs with "bcm_bt_lpm" label:
 *   gpio-71:  BT reset / regulator enable (out)
 *   gpio-184: Device wakeup, host→BT (out)
 *   gpio-185: Host wakeup, BT→host (in)
 */
static struct gpiod_lookup_table venue8_bt_gpios = {
	.dev_id = "bcm_bt_lpm",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_BT_RESET,
			    "shutdown", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_BT_WAKE_DEV,
			    "device-wakeup", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_BT_WAKE_HOST,
			    "host-wakeup", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/*
 * Audio machine driver GPIOs
 *
 * Codec IRQ: GPIO 14 (derived from /proc/interrupts: IRQ 270 = 14 + 256).
 * Codec reset: unknown (not in debugfs, may be handled by WM8994 LDO).
 */
static struct gpiod_lookup_table venue8_audio_gpios = {
	.dev_id = "mrfld_wm8958",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_AUDIOCODEC_INT,
			    "codec-int", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/* ------------------------------------------------------------------ */
/*  Modem control platform device                                      */
/* ------------------------------------------------------------------ */

/*
 * The XMM 7160 modem is controlled via GPIO lines and PMIC register 0x31.
 * This platform_device provides the hardware configuration to the
 * modem_control driver (ported from Dell kernel modules/drivers/modem_control/).
 */
static const struct property_entry venue8_modem_props[] = {
	PROPERTY_ENTRY_U16("pmic-reg", 0x31),
	PROPERTY_ENTRY_U8("pmic-on", 0x02),
	PROPERTY_ENTRY_U8("pmic-off", 0x00),
	PROPERTY_ENTRY_U8("pmic-mask", 0xfc),
	PROPERTY_ENTRY_STRING("modem-type", "xmm7160"),
	PROPERTY_ENTRY_U32("pre-on-delay-us", 200000),
	PROPERTY_ENTRY_U32("on-duration-us", 60000),
	PROPERTY_ENTRY_U32("pre-pwr-down-delay-us", 650000),
	PROPERTY_ENTRY_U32("warm-rst-duration-us", 60000),
	PROPERTY_ENTRY_U32("pre-cflash-delay-ms", 15000),
	PROPERTY_ENTRY_U32("pre-wflash-delay-ms", 30000),
	{ },
};

static const struct software_node venue8_modem_node = {
	.name = "modem_control",
	.properties = venue8_modem_props,
};

static struct platform_device venue8_modem_device = {
	.name = "modem_control",
	.id = -1,
};

/* ------------------------------------------------------------------ */
/*  HSIC USB companion device                                          */
/* ------------------------------------------------------------------ */

/*
 * The EHCI controller at PCI 00:10.0 (8086:119d) needs HSIC-specific PHY
 * initialization.  Verified via ADB: hsic_enable=1, L2_autosuspend_enable=1.
 */
static const struct property_entry venue8_hsic_props[] = {
	PROPERTY_ENTRY_U32("pci-device-id", PCI_DEVICE_ID_MRFL_EHCI),
	{ },
};

static const struct software_node venue8_hsic_node = {
	.name = "ehci_hsic",
	.properties = venue8_hsic_props,
};

static struct platform_device venue8_hsic_device = {
	.name = "ehci_hsic",
	.id = -1,
};

/* ------------------------------------------------------------------ */
/*  Audio machine driver platform device                               */
/* ------------------------------------------------------------------ */

/*
 * The ASoC machine driver "mrfld_wm8958" binds the Intel SST (Atom)
 * platform to the WM8958 codec.  Verified: the Dell kernel creates
 * /devices/platform/mrfld_wm8958/sound/card1 with jack detect on input7.
 */
static struct platform_device venue8_audio_device = {
	.name = "mrfld_wm8958",
	.id = -1,
};

/* ------------------------------------------------------------------ */
/*  GPIO keys (volume buttons)                                         */
/* ------------------------------------------------------------------ */

/*
 * Volume up/down buttons verified from /sys/kernel/debug/gpio:
 *   gpio-61 (volume_down) in hi
 *   gpio-62 (volume_up)   in hi
 * Both are active-low (normally high, pulled low when pressed).
 * Dell kernel shows these as input5: gpio-keys.
 */
static struct gpio_keys_button venue8_gpio_buttons[] = {
	{
		.code		= KEY_VOLUMEDOWN,
		.gpio		= GPIO_VOLUME_DOWN,
		.active_low	= 1,
		.desc		= "volume_down",
		.type		= EV_KEY,
		.wakeup		= 0,
		.debounce_interval = 10,
	},
	{
		.code		= KEY_VOLUMEUP,
		.gpio		= GPIO_VOLUME_UP,
		.active_low	= 1,
		.desc		= "volume_up",
		.type		= EV_KEY,
		.wakeup		= 0,
		.debounce_interval = 10,
	},
};

static struct gpio_keys_platform_data venue8_gpio_keys_pdata = {
	.buttons	= venue8_gpio_buttons,
	.nbuttons	= ARRAY_SIZE(venue8_gpio_buttons),
	.rep		= 0,
};

static struct platform_device venue8_gpio_keys_device = {
	.name	= "gpio-keys",
	.id	= -1,
	.dev	= {
		.platform_data = &venue8_gpio_keys_pdata,
	},
};

/* ------------------------------------------------------------------ */
/*  LEDs                                                               */
/* ------------------------------------------------------------------ */

/*
 * Two indicator LEDs verified from /sys/kernel/debug/gpio:
 *   gpio-8  "yellow" out lo
 *   gpio-55 "white"  out lo
 */
static struct gpio_led venue8_leds[] = {
	{
		.name			= "venue8::yellow",
		.gpio			= GPIO_LED_YELLOW,
		.active_low		= 0,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
		.default_trigger	= "none",
	},
	{
		.name			= "venue8::white",
		.gpio			= GPIO_LED_WHITE,
		.active_low		= 0,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
		.default_trigger	= "none",
	},
};

static struct gpio_led_platform_data venue8_leds_pdata = {
	.num_leds	= ARRAY_SIZE(venue8_leds),
	.leds		= venue8_leds,
};

static struct platform_device venue8_leds_device = {
	.name	= "leds-gpio",
	.id	= -1,
	.dev	= {
		.platform_data = &venue8_leds_pdata,
	},
};

/* ------------------------------------------------------------------ */
/*  Sensor interrupt GPIO lookup table                                 */
/* ------------------------------------------------------------------ */

/*
 * LSM303D INT1/INT2 (gpio-46, gpio-47) for data-ready interrupts.
 * ALS INT (gpio-44) for ambient light threshold events.
 * The ST IIO drivers use devm_gpiod_get_optional() or i2c_board_info.irq.
 */
static struct gpiod_lookup_table venue8_sensor_gpios = {
	.dev_id = "i2c-lsm303d",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_ACCEL_INT1,
			    "int1", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_ACCEL_INT2,
			    "int2", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/* ------------------------------------------------------------------ */
/*  SD card detect GPIO lookup table                                   */
/* ------------------------------------------------------------------ */

/*
 * SD card detect on gpio-77 (sd_cd, active low — card inserted = low).
 * The SDHCI PCI driver on Merrifield may pick this up via platform data.
 */
static struct gpiod_lookup_table venue8_sdcard_gpios = {
	.dev_id = "0000:00:01.3",
	.table = {
		GPIO_LOOKUP("merrifield_pinctrl", GPIO_SD_CD,
			    "cd", GPIO_ACTIVE_LOW),
		{ },
	},
};

/* ------------------------------------------------------------------ */
/*  Platform checks and registration                                   */
/* ------------------------------------------------------------------ */

static bool __init venue8_check_platform(void)
{
	if (boot_cpu_data.x86 != 6)
		return false;

	/*
	 * Model 0x4a = Merrifield (Tangier)
	 * Model 0x5a = Moorefield (Anniedale)
	 */
	if (boot_cpu_data.x86_model != 0x4a)
		return false;

	pr_info("Merrifield (Tangier) SoC detected, model 0x%x stepping %d\n",
		boot_cpu_data.x86_model, boot_cpu_data.x86_stepping);
	return true;
}

static void __init venue8_register_regulators(void)
{
	int ret;

	ret = platform_device_register(&venue8_vcc18_device);
	if (ret)
		pr_err("failed to register VCC_1.8V regulator: %d\n", ret);

	ret = platform_device_register(&venue8_vbat_device);
	if (ret)
		pr_err("failed to register V_BAT regulator: %d\n", ret);
}

static void __init venue8_register_i2c_devices(void)
{
	i2c_register_board_info(I2C_BUS_AUDIO,
				venue8_i2c1_devs,
				ARRAY_SIZE(venue8_i2c1_devs));
	i2c_register_board_info(I2C_BUS_FUEL_GAUGE,
				venue8_i2c2_devs,
				ARRAY_SIZE(venue8_i2c2_devs));
	i2c_register_board_info(I2C_BUS_CAMERA,
				venue8_i2c4_devs,
				ARRAY_SIZE(venue8_i2c4_devs));
	i2c_register_board_info(I2C_BUS_SENSORS,
				venue8_i2c6_devs,
				ARRAY_SIZE(venue8_i2c6_devs));
	i2c_register_board_info(I2C_BUS_TOUCH,
				venue8_i2c7_devs,
				ARRAY_SIZE(venue8_i2c7_devs));
	i2c_register_board_info(I2C_BUS_PMIC,
				venue8_i2c8_devs,
				ARRAY_SIZE(venue8_i2c8_devs));

	pr_info("registered I2C devices on buses 1, 2, 4, 6, 7, 8\n");
}

static void __init venue8_register_gpio_tables(void)
{
	gpiod_add_lookup_table(&venue8_modem_gpios);
	gpiod_add_lookup_table(&venue8_hsic_gpios);
	gpiod_add_lookup_table(&venue8_fuel_gauge_gpios);
	gpiod_add_lookup_table(&venue8_touch_gpios);
	gpiod_add_lookup_table(&venue8_wifi_gpios);
	gpiod_add_lookup_table(&venue8_bt_gpios);
	gpiod_add_lookup_table(&venue8_audio_gpios);
	gpiod_add_lookup_table(&venue8_sensor_gpios);
	gpiod_add_lookup_table(&venue8_sdcard_gpios);
}

static int __init venue8_register_platform_devices(void)
{
	int ret;

	ret = device_add_software_node(&venue8_modem_device.dev,
				       &venue8_modem_node);
	if (ret) {
		pr_err("failed to add modem software node: %d\n", ret);
		return ret;
	}

	ret = platform_device_register(&venue8_modem_device);
	if (ret) {
		pr_err("failed to register modem device: %d\n", ret);
		goto err_remove_modem_node;
	}

	ret = device_add_software_node(&venue8_hsic_device.dev,
				       &venue8_hsic_node);
	if (ret) {
		pr_err("failed to add HSIC software node: %d\n", ret);
		goto err_unreg_modem;
	}

	ret = platform_device_register(&venue8_hsic_device);
	if (ret) {
		pr_err("failed to register HSIC device: %d\n", ret);
		goto err_remove_hsic_node;
	}

	ret = platform_device_register(&venue8_audio_device);
	if (ret) {
		pr_err("failed to register audio device: %d\n", ret);
		goto err_unreg_hsic;
	}

	ret = platform_device_register(&venue8_gpio_keys_device);
	if (ret)
		pr_err("failed to register gpio-keys: %d\n", ret);

	ret = platform_device_register(&venue8_leds_device);
	if (ret)
		pr_err("failed to register LEDs: %d\n", ret);

	return 0;

err_unreg_hsic:
	platform_device_unregister(&venue8_hsic_device);
err_remove_hsic_node:
	device_remove_software_node(&venue8_hsic_device.dev);
err_unreg_modem:
	platform_device_unregister(&venue8_modem_device);
err_remove_modem_node:
	device_remove_software_node(&venue8_modem_device.dev);
	return ret;
}

static int __init venue8_board_init(void)
{
	if (!venue8_check_platform())
		return -ENODEV;

	pr_info("Dell Venue 8 3840 board init\n");

	venue8_register_regulators();
	venue8_register_gpio_tables();
	venue8_register_i2c_devices();

	return venue8_register_platform_devices();
}
arch_initcall(venue8_board_init);

MODULE_DESCRIPTION("Dell Venue 8 3840 board-level device registration");
MODULE_LICENSE("GPL");
