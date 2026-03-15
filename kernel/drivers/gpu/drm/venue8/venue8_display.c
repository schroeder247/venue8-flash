// SPDX-License-Identifier: GPL-2.0
/*
 * Dell Venue 8 3840 — Tangier MIPI DSI display driver
 *
 * Minimal DRM driver for the Merrifield SoC's MIPI DSI output to the
 * 1200x1920 AUO/Innolux panel. Uses drm_simple_display_pipe for a
 * single-CRTC framebuffer console.
 *
 * The Tangier display controller lives in the PCI GPU BAR (PowerVR G6400,
 * PCI 00:02.0 8086:1180). The DSI registers start at offset 0xb000 within
 * the MMIO region. Pipe A drives MIPIA → DSI-1 panel.
 *
 * Register layout and timing from Dell kernel 3.10:
 *   modules/intel_media/display/tng/drv/mdfld_dsi_output.h
 *   modules/intel_media/display/tng/drv/auo_12x192_vid_8inch_lcd.c
 *
 * Copyright (C) 2026 Thomas (venue8-flash project)
 * Based on Intel Tangier display code, Copyright (C) 2012-2014 Intel Corp.
 */

#define pr_fmt(fmt) "venue8_disp: " fmt

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

/* PCI device: the DSI controller shares the GPU BAR */
#define PCI_DEVICE_ID_MRFL_GPU	0x1180

/* ------------------------------------------------------------------ */
/*  MIPI DSI Controller Registers (offset from MMIO BAR)              */
/* ------------------------------------------------------------------ */

/* MIPIA base = 0xb000, MIPIC = 0xb800 (pipe C, secondary) */
#define MIPIA_BASE			0xb000

#define MIPIA_DEVICE_READY		(MIPIA_BASE + 0x00)
#define MIPIA_INTR_STAT			(MIPIA_BASE + 0x04)
#define MIPIA_INTR_EN			(MIPIA_BASE + 0x08)
#define MIPIA_DSI_FUNC_PRG		(MIPIA_BASE + 0x0c)
#define MIPIA_HS_TX_TIMEOUT		(MIPIA_BASE + 0x10)
#define MIPIA_LP_RX_TIMEOUT		(MIPIA_BASE + 0x14)
#define MIPIA_TURN_AROUND_TIMEOUT	(MIPIA_BASE + 0x18)
#define MIPIA_DEVICE_RESET_TIMER	(MIPIA_BASE + 0x1c)
#define MIPIA_DPI_RESOLUTION		(MIPIA_BASE + 0x20)
#define MIPIA_HSYNC_COUNT		(MIPIA_BASE + 0x28)
#define MIPIA_HBP_COUNT			(MIPIA_BASE + 0x2c)
#define MIPIA_HFP_COUNT			(MIPIA_BASE + 0x30)
#define MIPIA_HACTIVE_COUNT		(MIPIA_BASE + 0x34)
#define MIPIA_VSYNC_COUNT		(MIPIA_BASE + 0x38)
#define MIPIA_VBP_COUNT			(MIPIA_BASE + 0x3c)
#define MIPIA_VFP_COUNT			(MIPIA_BASE + 0x40)
#define MIPIA_HIGH_LOW_SWITCH_COUNT	(MIPIA_BASE + 0x44)
#define MIPIA_DPI_CONTROL		(MIPIA_BASE + 0x48)
#define MIPIA_DPI_DATA			(MIPIA_BASE + 0x4c)
#define MIPIA_INIT_COUNT		(MIPIA_BASE + 0x50)
#define MIPIA_MAX_RETURN_PACK_SIZE	(MIPIA_BASE + 0x54)
#define MIPIA_VIDEO_MODE_FORMAT		(MIPIA_BASE + 0x58)
#define MIPIA_EOT_DISABLE		(MIPIA_BASE + 0x5c)
#define MIPIA_LP_BYTECLK		(MIPIA_BASE + 0x60)
#define MIPIA_LP_GEN_DATA		(MIPIA_BASE + 0x64)
#define MIPIA_HS_GEN_DATA		(MIPIA_BASE + 0x68)
#define MIPIA_LP_GEN_CTRL		(MIPIA_BASE + 0x6c)
#define MIPIA_HS_GEN_CTRL		(MIPIA_BASE + 0x70)
#define MIPIA_GEN_FIFO_STAT		(MIPIA_BASE + 0x74)
#define MIPIA_DPHY_PARAM		(MIPIA_BASE + 0x80)
#define MIPIA_CLK_LANE_SWITCH_TIME	(MIPIA_BASE + 0x88)
#define MIPIA_CONTROL			(MIPIA_BASE + 0x104)

/* MIPI port enable and lane config */
#define MIPI_PORT_EN			BIT(31)
#define PASS_FROM_SPHY_TO_AFE		BIT(28)
#define DSI_LANE_4_0			0	/* 4 data lanes */

/* Video mode format */
#define VIDEO_MODE_NON_BURST_SYNC_PULSE	1
#define VIDEO_MODE_NON_BURST_SYNC_EVENT	2
#define VIDEO_MODE_BURST		3

/* DSI function programming: bits [4:3] = virtual channel, [2:0] = data type */
#define DSI_FUNC_RGB888_4LANE		0x204	/* 24-bit RGB, 4 lanes, VC0 */

/* DCS commands */
#define MIPI_DCS_EXIT_SLEEP_MODE	0x11
#define MIPI_DCS_SET_DISPLAY_ON		0x29
#define MIPI_DCS_SET_DISPLAY_OFF	0x28
#define MIPI_DCS_ENTER_SLEEP_MODE	0x10

/* DPI special packets */
#define DPI_SPK_TURN_ON			1
#define DPI_SPK_SHUT_DOWN		2

/* GEN FIFO status bits */
#define HS_DATA_FIFO_FULL		BIT(0)
#define HS_CTRL_FIFO_FULL		BIT(2)
#define LP_CTRL_FIFO_FULL		BIT(4)

/* ------------------------------------------------------------------ */
/*  Display Pipe / CRTC Registers                                      */
/* ------------------------------------------------------------------ */

#define HTOTAL_A		0x60000
#define HBLANK_A		0x60004
#define HSYNC_A			0x60008
#define VTOTAL_A		0x6000c
#define VBLANK_A		0x60010
#define VSYNC_A			0x60014
#define PIPEASRC		0x6001c
#define PIPEACONF		0x70008

/* Display plane A */
#define DSPACNTR		0x70180
#define DSPALINOFF		0x70184
#define DSPASTRIDE		0x70188
#define DSPASURF		0x7019c

/* Plane control bits */
#define DISPLAY_PLANE_ENABLE		BIT(31)
#define DISPPLANE_RGBX888		(0x6 << 26)
#define DISPPLANE_BGRX888		(0x7 << 26)

/* Pipe config bits */
#define PIPEACONF_ENABLE		BIT(31)
#define PIPEACONF_STATE_ON		BIT(30)

/* ------------------------------------------------------------------ */
/*  Panel Timing — AUO 1200x1920 8-inch (from Dell kernel)            */
/* ------------------------------------------------------------------ */

/* Verified against Dell kernel auo_12x192_vid_8inch_lcd.c */
#define PANEL_WIDTH		1200
#define PANEL_HEIGHT		1920
#define PANEL_HSYNC		10
#define PANEL_HBP		54
#define PANEL_HFP		64
#define PANEL_VSYNC		2
#define PANEL_VBP		3
#define PANEL_VFP		15
#define PANEL_LANES		4

/* D-PHY timing parameter from Dell kernel */
#define DPHY_PARAM_VALUE	0x311F7838
#define CLK_LANE_SWITCH_TIME	0x2B0014
#define HIGH_LOW_SWITCH_COUNT	0x35
#define INIT_COUNT_VALUE	0x7D0
#define EOT_DISABLE_VALUE	0x3
#define LP_BYTECLK_VALUE	0x6

/* ------------------------------------------------------------------ */
/*  Driver State                                                       */
/* ------------------------------------------------------------------ */

struct venue8_display {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct pci_dev *pdev;
	void __iomem *mmio;
	struct gpio_desc *gpio_lcd_reset;
	struct gpio_desc *gpio_lcd_power;
	bool panel_on;
};

#define to_venue8(x) container_of(x, struct venue8_display, drm)

/* ------------------------------------------------------------------ */
/*  Register I/O helpers                                               */
/* ------------------------------------------------------------------ */

static inline u32 disp_read(struct venue8_display *vd, u32 reg)
{
	return ioread32(vd->mmio + reg);
}

static inline void disp_write(struct venue8_display *vd, u32 reg, u32 val)
{
	iowrite32(val, vd->mmio + reg);
}

/* ------------------------------------------------------------------ */
/*  DSI command helpers                                                */
/* ------------------------------------------------------------------ */

static void venue8_dsi_wait_fifo(struct venue8_display *vd)
{
	int i;

	for (i = 0; i < 1000; i++) {
		u32 stat = disp_read(vd, MIPIA_GEN_FIFO_STAT);
		if (!(stat & (HS_DATA_FIFO_FULL | HS_CTRL_FIFO_FULL)))
			return;
		udelay(10);
	}
	pr_warn("DSI FIFO timeout\n");
}

static void venue8_dsi_send_dcs(struct venue8_display *vd, u8 cmd)
{
	u32 ctrl;

	venue8_dsi_wait_fifo(vd);

	/* MCS short write, no parameter: type 0x05, 1 byte */
	ctrl = (0x05 << 0) | ((u32)cmd << 8);
	disp_write(vd, MIPIA_HS_GEN_CTRL, ctrl);
}

static void venue8_dsi_send_spk(struct venue8_display *vd, u8 spk)
{
	disp_write(vd, MIPIA_DPI_CONTROL, spk);
}

/* ------------------------------------------------------------------ */
/*  Panel power sequencing                                             */
/* ------------------------------------------------------------------ */

static void venue8_panel_on(struct venue8_display *vd)
{
	if (vd->panel_on)
		return;

	/* LCD power rail (GPIO 189) */
	if (vd->gpio_lcd_power)
		gpiod_set_value_cansleep(vd->gpio_lcd_power, 1);
	usleep_range(10000, 12000);

	/* LCD reset (GPIO 190) — active-low pulse */
	if (vd->gpio_lcd_reset) {
		gpiod_set_value_cansleep(vd->gpio_lcd_reset, 0);
		usleep_range(2000, 3000);
		gpiod_set_value_cansleep(vd->gpio_lcd_reset, 1);
		usleep_range(10000, 12000);
	}

	/* DCS: exit sleep mode */
	venue8_dsi_send_dcs(vd, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(5);

	/* DCS: set display on */
	venue8_dsi_send_dcs(vd, MIPI_DCS_SET_DISPLAY_ON);
	msleep(20);

	/* DPI turn-on packet */
	venue8_dsi_send_spk(vd, DPI_SPK_TURN_ON);

	vd->panel_on = true;
	pr_info("panel enabled\n");
}

static void venue8_panel_off(struct venue8_display *vd)
{
	if (!vd->panel_on)
		return;

	/* DPI shutdown */
	venue8_dsi_send_spk(vd, DPI_SPK_SHUT_DOWN);
	msleep(2);

	/* DCS: display off + enter sleep */
	venue8_dsi_send_dcs(vd, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(30);
	venue8_dsi_send_dcs(vd, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(50);

	/* LCD reset low, power off */
	if (vd->gpio_lcd_reset)
		gpiod_set_value_cansleep(vd->gpio_lcd_reset, 0);
	if (vd->gpio_lcd_power)
		gpiod_set_value_cansleep(vd->gpio_lcd_power, 0);

	vd->panel_on = false;
	pr_info("panel disabled\n");
}

/* ------------------------------------------------------------------ */
/*  DSI controller initialization                                      */
/* ------------------------------------------------------------------ */

static void venue8_dsi_init(struct venue8_display *vd)
{
	u32 htotal, vtotal;

	/* Device not ready during programming */
	disp_write(vd, MIPIA_DEVICE_READY, 0);

	/* DSI function programming: 24-bit RGB888, 4 lanes, VC0 */
	disp_write(vd, MIPIA_DSI_FUNC_PRG, DSI_FUNC_RGB888_4LANE);

	/* Timing parameters from Dell kernel */
	disp_write(vd, MIPIA_HS_TX_TIMEOUT, 0xffffff);
	disp_write(vd, MIPIA_LP_RX_TIMEOUT, 0xffff);
	disp_write(vd, MIPIA_TURN_AROUND_TIMEOUT, 0x18);
	disp_write(vd, MIPIA_DEVICE_RESET_TIMER, 0xffff);

	/* Horizontal timing (DSI byte clock units) */
	disp_write(vd, MIPIA_HSYNC_COUNT, PANEL_HSYNC);
	disp_write(vd, MIPIA_HBP_COUNT, PANEL_HBP);
	disp_write(vd, MIPIA_HFP_COUNT, PANEL_HFP);
	disp_write(vd, MIPIA_HACTIVE_COUNT, PANEL_WIDTH);

	/* Vertical timing (line units) */
	disp_write(vd, MIPIA_VSYNC_COUNT, PANEL_VSYNC);
	disp_write(vd, MIPIA_VBP_COUNT, PANEL_VBP);
	disp_write(vd, MIPIA_VFP_COUNT, PANEL_VFP);

	/* DPI resolution register: (height << 16) | width */
	disp_write(vd, MIPIA_DPI_RESOLUTION,
		   ((PANEL_HEIGHT - 1) << 16) | (PANEL_WIDTH - 1));

	/* Video mode: burst mode */
	disp_write(vd, MIPIA_VIDEO_MODE_FORMAT, 0xF);

	/* D-PHY timing parameters */
	disp_write(vd, MIPIA_DPHY_PARAM, DPHY_PARAM_VALUE);
	disp_write(vd, MIPIA_CLK_LANE_SWITCH_TIME, CLK_LANE_SWITCH_TIME);
	disp_write(vd, MIPIA_HIGH_LOW_SWITCH_COUNT, HIGH_LOW_SWITCH_COUNT);

	/* Misc */
	disp_write(vd, MIPIA_INIT_COUNT, INIT_COUNT_VALUE);
	disp_write(vd, MIPIA_EOT_DISABLE, EOT_DISABLE_VALUE);
	disp_write(vd, MIPIA_LP_BYTECLK, LP_BYTECLK_VALUE);

	/* Interrupts: enable all */
	disp_write(vd, MIPIA_INTR_EN, 0xffffffff);

	/* Enable MIPI port: port enable + pass-through + 4 lanes */
	disp_write(vd, MIPIA_CONTROL,
		   MIPI_PORT_EN | PASS_FROM_SPHY_TO_AFE | DSI_LANE_4_0);

	/* Set up CRTC pipe A timing */
	htotal = PANEL_WIDTH + PANEL_HBP + PANEL_HFP + PANEL_HSYNC - 1;
	vtotal = PANEL_HEIGHT + PANEL_VBP + PANEL_VFP + PANEL_VSYNC - 1;

	disp_write(vd, HTOTAL_A, (htotal << 16) | (PANEL_WIDTH - 1));
	disp_write(vd, HBLANK_A, (htotal << 16) | (PANEL_WIDTH - 1));
	disp_write(vd, HSYNC_A,
		   ((PANEL_WIDTH + PANEL_HFP + PANEL_HSYNC - 1) << 16) |
		   (PANEL_WIDTH + PANEL_HFP - 1));
	disp_write(vd, VTOTAL_A, (vtotal << 16) | (PANEL_HEIGHT - 1));
	disp_write(vd, VBLANK_A, (vtotal << 16) | (PANEL_HEIGHT - 1));
	disp_write(vd, VSYNC_A,
		   ((PANEL_HEIGHT + PANEL_VFP + PANEL_VSYNC - 1) << 16) |
		   (PANEL_HEIGHT + PANEL_VFP - 1));

	/* Pipe source: (width - 1) << 16 | (height - 1) */
	disp_write(vd, PIPEASRC,
		   ((PANEL_WIDTH - 1) << 16) | (PANEL_HEIGHT - 1));

	/* Device ready */
	disp_write(vd, MIPIA_DEVICE_READY, 1);

	pr_info("DSI controller initialized: %dx%d, 4 lanes, burst mode\n",
		PANEL_WIDTH, PANEL_HEIGHT);
}

/* ------------------------------------------------------------------ */
/*  DRM simple display pipe callbacks                                  */
/* ------------------------------------------------------------------ */

static const u32 venue8_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static void venue8_pipe_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			       struct drm_plane_state *plane_state)
{
	struct venue8_display *vd = to_venue8(pipe->crtc.dev);

	venue8_dsi_init(vd);
	venue8_panel_on(vd);

	/* Enable pipe A */
	disp_write(vd, PIPEACONF, PIPEACONF_ENABLE);

	/* Enable display plane A: BGRX8888 format */
	if (plane_state->fb) {
		struct drm_gem_shmem_object *shmem;
		dma_addr_t addr;

		shmem = drm_fb_shmem_get_obj(plane_state->fb, 0);
		if (shmem && shmem->sgt) {
			addr = sg_dma_address(shmem->sgt->sgl);
			disp_write(vd, DSPASURF, addr);
		}

		disp_write(vd, DSPASTRIDE, plane_state->fb->pitches[0]);
		disp_write(vd, DSPALINOFF, 0);
		disp_write(vd, DSPACNTR,
			   DISPLAY_PLANE_ENABLE | DISPPLANE_BGRX888);
	}
}

static void venue8_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct venue8_display *vd = to_venue8(pipe->crtc.dev);

	/* Disable plane */
	disp_write(vd, DSPACNTR, 0);
	/* Disable pipe */
	disp_write(vd, PIPEACONF, 0);

	venue8_panel_off(vd);
}

static void venue8_pipe_update(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *old_state)
{
	struct venue8_display *vd = to_venue8(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;

	if (state->fb) {
		struct drm_gem_shmem_object *shmem;

		shmem = drm_fb_shmem_get_obj(state->fb, 0);
		if (shmem && shmem->sgt) {
			dma_addr_t addr = sg_dma_address(shmem->sgt->sgl);
			disp_write(vd, DSPASURF, addr);
		}
		disp_write(vd, DSPASTRIDE, state->fb->pitches[0]);
	}
}

static const struct drm_simple_display_pipe_funcs venue8_pipe_funcs = {
	.enable		= venue8_pipe_enable,
	.disable	= venue8_pipe_disable,
	.update		= venue8_pipe_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/*  Connector                                                          */
/* ------------------------------------------------------------------ */

static int venue8_conn_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	mode->clock = (PANEL_WIDTH + PANEL_HBP + PANEL_HFP + PANEL_HSYNC) *
		      (PANEL_HEIGHT + PANEL_VBP + PANEL_VFP + PANEL_VSYNC) *
		      60 / 1000;
	mode->hdisplay = PANEL_WIDTH;
	mode->hsync_start = PANEL_WIDTH + PANEL_HFP;
	mode->hsync_end = PANEL_WIDTH + PANEL_HFP + PANEL_HSYNC;
	mode->htotal = PANEL_WIDTH + PANEL_HFP + PANEL_HSYNC + PANEL_HBP;
	mode->vdisplay = PANEL_HEIGHT;
	mode->vsync_start = PANEL_HEIGHT + PANEL_VFP;
	mode->vsync_end = PANEL_HEIGHT + PANEL_VFP + PANEL_VSYNC;
	mode->vtotal = PANEL_HEIGHT + PANEL_VFP + PANEL_VSYNC + PANEL_VBP;
	mode->width_mm = 107;
	mode->height_mm = 172;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs venue8_conn_helper_funcs = {
	.get_modes = venue8_conn_get_modes,
};

static const struct drm_connector_funcs venue8_conn_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------------ */
/*  DRM driver                                                         */
/* ------------------------------------------------------------------ */

DEFINE_DRM_GEM_FOPS(venue8_fops);

static const struct drm_driver venue8_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name			= "venue8-display",
	.desc			= "Dell Venue 8 3840 Tangier DSI Display",
	.date			= "20260315",
	.major			= 1,
	.minor			= 0,
	.fops			= &venue8_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
};

/* ------------------------------------------------------------------ */
/*  PCI probe                                                          */
/* ------------------------------------------------------------------ */

static int venue8_display_probe(struct pci_dev *pdev,
				const struct pci_device_id *ent)
{
	struct venue8_display *vd;
	struct drm_device *drm;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	vd = devm_drm_dev_alloc(&pdev->dev, &venue8_drm_driver,
				struct venue8_display, drm);
	if (IS_ERR(vd))
		return PTR_ERR(vd);

	drm = &vd->drm;
	vd->pdev = pdev;
	pci_set_drvdata(pdev, vd);

	/* Map MMIO BAR0 — GPU + display registers */
	vd->mmio = pcim_iomap(pdev, 0, 0);
	if (!vd->mmio) {
		dev_err(&pdev->dev, "failed to map BAR0\n");
		return -ENOMEM;
	}

	/* Get LCD GPIOs (optional — may not be available if board file
	 * hasn't registered them yet, e.g. during early bringup) */
	vd->gpio_lcd_power = devm_gpiod_get_optional(&pdev->dev,
						      "lcd-power",
						      GPIOD_OUT_LOW);
	vd->gpio_lcd_reset = devm_gpiod_get_optional(&pdev->dev,
						      "lcd-reset",
						      GPIOD_OUT_HIGH);

	/* Mode config */
	drm->mode_config.min_width = PANEL_WIDTH;
	drm->mode_config.max_width = PANEL_WIDTH;
	drm->mode_config.min_height = PANEL_HEIGHT;
	drm->mode_config.max_height = PANEL_HEIGHT;
	drm->mode_config.preferred_depth = 32;

	/* Connector */
	ret = drm_connector_init(drm, &vd->connector, &venue8_conn_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;
	drm_connector_helper_add(&vd->connector, &venue8_conn_helper_funcs);

	/* Simple display pipe */
	ret = drm_simple_display_pipe_init(drm, &vd->pipe,
					   &venue8_pipe_funcs,
					   venue8_formats,
					   ARRAY_SIZE(venue8_formats),
					   NULL, &vd->connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(drm, 32);

	dev_info(&pdev->dev, "Venue 8 display: %dx%d DSI\n",
		 PANEL_WIDTH, PANEL_HEIGHT);
	return 0;
}

static void venue8_display_remove(struct pci_dev *pdev)
{
	struct venue8_display *vd = pci_get_drvdata(pdev);

	venue8_panel_off(vd);
	drm_dev_unplug(&vd->drm);
}

static const struct pci_device_id venue8_display_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_MRFL_GPU) },
	{ }
};
MODULE_DEVICE_TABLE(pci, venue8_display_pci_ids);

static struct pci_driver venue8_display_driver = {
	.name		= "venue8-display",
	.id_table	= venue8_display_pci_ids,
	.probe		= venue8_display_probe,
	.remove		= venue8_display_remove,
};
module_pci_driver(venue8_display_driver);

MODULE_DESCRIPTION("Dell Venue 8 3840 Tangier MIPI DSI display driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomas <venue8-flash project>");
