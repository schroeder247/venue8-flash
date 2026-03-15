// SPDX-License-Identifier: GPL-2.0
/*
 * Intel XMM 7160 modem control driver for Dell Venue 8 3840
 *
 * Manages modem power sequencing via GPIO and PMIC. Provides a char device
 * (/dev/mdm_ctrl) for userspace modem manager (mmgr) to control modem state.
 *
 * Ported from Dell kernel 3.10 modules/drivers/modem_control/ to kernel 6.6.
 * Original: Copyright (C) 2013 Intel Corporation (GPLv2)
 *           Authors: Faouaz Tenoutit, Frederic Berat, Guillaume Ranquet
 */

#define pr_fmt(fmt) "mdm_ctrl: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>

#include <asm/intel_scu_ipc.h>

#define DRVNAME		"mdm_ctrl"
#define MDM_BOOT_DEVNAME "mdm_ctrl"

/* Modem states */
#define MDM_CTRL_STATE_UNKNOWN		0x0000
#define MDM_CTRL_STATE_OFF		0x0001
#define MDM_CTRL_STATE_COLD_BOOT	0x0002
#define MDM_CTRL_STATE_WARM_BOOT	0x0004
#define MDM_CTRL_STATE_COREDUMP		0x0008
#define MDM_CTRL_STATE_IPC_READY	0x0010
#define MDM_CTRL_STATE_FW_DOWNLOAD_READY 0x0020

/* ioctl commands */
#define MDM_CTRL_MAGIC	'm'
#define MDM_CTRL_POWER_OFF		_IO(MDM_CTRL_MAGIC, 0)
#define MDM_CTRL_POWER_ON		_IO(MDM_CTRL_MAGIC, 1)
#define MDM_CTRL_WARM_RESET		_IO(MDM_CTRL_MAGIC, 2)
#define MDM_CTRL_FLASHING_WARM_RESET	_IO(MDM_CTRL_MAGIC, 3)
#define MDM_CTRL_COLD_RESET		_IO(MDM_CTRL_MAGIC, 4)
#define MDM_CTRL_SET_STATE		_IOW(MDM_CTRL_MAGIC, 5, unsigned int)
#define MDM_CTRL_GET_STATE		_IOR(MDM_CTRL_MAGIC, 6, unsigned int)
#define MDM_CTRL_GET_HANGUP_REASONS	_IOR(MDM_CTRL_MAGIC, 7, unsigned int)
#define MDM_CTRL_CLEAR_HANGUP_REASONS	_IO(MDM_CTRL_MAGIC, 8)
#define MDM_CTRL_SET_POLLED_STATES	_IOW(MDM_CTRL_MAGIC, 9, unsigned int)

/* Hangup causes */
#define MDM_CTRL_NO_HU		0x0000
#define MDM_CTRL_HU_RESET	0x0001
#define MDM_CTRL_HU_COREDUMP	0x0002

/* Modem ready wait duration (seconds) */
#define MDM_MODEM_READY_DELAY	60

/**
 * struct mdm_ctrl - Modem control driver state
 * @dev:		Parent device
 * @scu:		SCU IPC device for PMIC access
 * @gpio_rst_bbn:	RESET_BB_N output (resets modem)
 * @gpio_pwr_on:	PWR_ON output (power on pulse)
 * @gpio_rst_out:	RESET_OUT input (modem signals ready)
 * @gpio_cdump:		CORE_DUMP input (modem crash signal)
 * @pmic_reg:		PMIC register address for modem power
 * @pmic_on:		PMIC value for power on
 * @pmic_off:		PMIC value for power off
 * @pmic_mask:		PMIC register mask (preserve other bits)
 * @modem_state:	Current modem state (atomic)
 * @polled_states:	States the userspace is waiting for
 * @polled_state_reached: Flag set when polled state is reached
 * @hangup_causes:	Bitfield of hangup reasons
 * @rst_ongoing:	Reset in progress (ignore IRQs)
 * @opened:		Device open flag (single instance)
 * @lock:		Protects open/close
 * @wait_wq:		Waitqueue for state polling
 * @hu_wq:		Hangup work queue
 * @hangup_work:	Work item for handling hangup
 * @cdev:		Char device
 * @tdev:		Device number
 * @major:		Major number
 * @class:		Device class
 * @char_dev:		Device node
 * @pre_on_delay:	Delay before PWR_ON pulse (us)
 * @on_duration:	PWR_ON pulse duration (us)
 * @pre_pwr_down_delay:	Delay before power off (us)
 * @warm_rst_duration:	Warm reset pulse duration (us)
 * @pre_cflash_delay:	Cold flash timer delay (ms)
 * @pre_wflash_delay:	Warm flash timer delay (ms)
 */
struct mdm_ctrl {
	struct device *dev;
	struct intel_scu_ipc_dev *scu;
	struct gpio_desc *gpio_rst_bbn;
	struct gpio_desc *gpio_pwr_on;
	struct gpio_desc *gpio_rst_out;
	struct gpio_desc *gpio_cdump;
	u16 pmic_reg;
	u8 pmic_on;
	u8 pmic_off;
	u8 pmic_mask;
	atomic_t modem_state;
	unsigned int polled_states;
	bool polled_state_reached;
	unsigned int hangup_causes;
	atomic_t rst_ongoing;
	int opened;
	struct mutex lock;
	wait_queue_head_t wait_wq;
	struct workqueue_struct *hu_wq;
	struct work_struct hangup_work;
	struct cdev cdev;
	dev_t tdev;
	int major;
	struct class *class;
	struct device *char_dev;
	u32 pre_on_delay;
	u32 on_duration;
	u32 pre_pwr_down_delay;
	u32 warm_rst_duration;
	u32 pre_cflash_delay;
	u32 pre_wflash_delay;
};

static void mdm_ctrl_set_state(struct mdm_ctrl *drv, int state)
{
	atomic_set(&drv->modem_state, state);

	if (likely(state != MDM_CTRL_STATE_UNKNOWN) &&
	    (state & drv->polled_states)) {
		drv->polled_state_reached = true;
		wake_up(&drv->wait_wq);
		pr_info("waking poll, state 0x%x\n", state);
	}
}

static int mdm_ctrl_get_state(struct mdm_ctrl *drv)
{
	return atomic_read(&drv->modem_state);
}

/*
 * PMIC modem power control via SCU IPC
 */
static int mdm_ctrl_pmic_power_on(struct mdm_ctrl *drv)
{
	u8 val;
	int ret;

	if (drv->pmic_mask) {
		ret = intel_scu_ipc_dev_ioread8(drv->scu, drv->pmic_reg,
						&val);
		if (ret) {
			dev_err(drv->dev, "PMIC read failed: %d\n", ret);
			return ret;
		}
		val = (val & drv->pmic_mask) | drv->pmic_on;
	} else {
		val = drv->pmic_on;
	}

	ret = intel_scu_ipc_dev_iowrite8(drv->scu, drv->pmic_reg, val);
	if (ret)
		dev_err(drv->dev, "PMIC power on failed: %d\n", ret);

	usleep_range(20000, 25000);
	return ret;
}

static int mdm_ctrl_pmic_power_off(struct mdm_ctrl *drv)
{
	u8 val;
	int ret;

	if (drv->pmic_mask) {
		ret = intel_scu_ipc_dev_ioread8(drv->scu, drv->pmic_reg,
						&val);
		if (ret) {
			dev_err(drv->dev, "PMIC read failed: %d\n", ret);
			return ret;
		}
		val = (val & drv->pmic_mask) | drv->pmic_off;
	} else {
		val = drv->pmic_off;
	}

	ret = intel_scu_ipc_dev_iowrite8(drv->scu, drv->pmic_reg, val);
	if (ret)
		dev_err(drv->dev, "PMIC power off failed: %d\n", ret);

	usleep_range(20000, 25000);
	return ret;
}

/*
 * Modem power sequences
 */
static int mdm_ctrl_cold_boot(struct mdm_ctrl *drv)
{
	dev_warn(drv->dev, "cold boot requested\n");

	mdm_ctrl_set_state(drv, MDM_CTRL_STATE_COLD_BOOT);
	atomic_set(&drv->rst_ongoing, 1);

	mdm_ctrl_pmic_power_on(drv);

	/* Toggle RESET_BB_N */
	gpiod_set_value_cansleep(drv->gpio_rst_bbn, 1);

	/* Wait, then pulse PWR_ON */
	usleep_range(drv->pre_on_delay, drv->pre_on_delay + 1000);
	gpiod_set_value_cansleep(drv->gpio_pwr_on, 1);
	usleep_range(drv->on_duration, drv->on_duration + 1000);
	gpiod_set_value_cansleep(drv->gpio_pwr_on, 0);

	/* If no IPC ready signal, set ready immediately */
	if (!drv->gpio_rst_out) {
		atomic_set(&drv->rst_ongoing, 0);
		mdm_ctrl_set_state(drv, MDM_CTRL_STATE_IPC_READY);
	}

	return 0;
}

static int mdm_ctrl_warm_reset(struct mdm_ctrl *drv)
{
	dev_info(drv->dev, "warm reset requested\n");

	atomic_set(&drv->rst_ongoing, 1);
	mdm_ctrl_set_state(drv, MDM_CTRL_STATE_WARM_BOOT);

	gpiod_set_value_cansleep(drv->gpio_rst_bbn, 0);
	usleep_range(drv->warm_rst_duration, drv->warm_rst_duration + 1000);
	gpiod_set_value_cansleep(drv->gpio_rst_bbn, 1);

	return 0;
}

static int mdm_ctrl_power_off(struct mdm_ctrl *drv)
{
	dev_info(drv->dev, "power off requested\n");

	atomic_set(&drv->rst_ongoing, 1);
	mdm_ctrl_set_state(drv, MDM_CTRL_STATE_OFF);

	gpiod_set_value_cansleep(drv->gpio_rst_bbn, 0);
	usleep_range(drv->pre_pwr_down_delay,
		     drv->pre_pwr_down_delay + 1000);

	mdm_ctrl_pmic_power_off(drv);

	return 0;
}

static int mdm_ctrl_cold_reset(struct mdm_ctrl *drv)
{
	dev_warn(drv->dev, "cold reset requested\n");

	mdm_ctrl_power_off(drv);
	mdm_ctrl_cold_boot(drv);

	return 0;
}

/*
 * IRQ handlers for modem reset/coredump signals
 */
static void mdm_ctrl_handle_hangup(struct work_struct *work)
{
	struct mdm_ctrl *drv = container_of(work, struct mdm_ctrl,
					    hangup_work);

	if (drv->hangup_causes & MDM_CTRL_HU_RESET)
		mdm_ctrl_set_state(drv, MDM_CTRL_STATE_WARM_BOOT);

	if (drv->hangup_causes & MDM_CTRL_HU_COREDUMP)
		mdm_ctrl_set_state(drv, MDM_CTRL_STATE_COREDUMP);

	dev_info(drv->dev, "hangup (reasons: 0x%x)\n", drv->hangup_causes);
}

static irqreturn_t mdm_ctrl_reset_irq(int irq, void *data)
{
	struct mdm_ctrl *drv = data;
	int value;

	value = gpiod_get_value(drv->gpio_rst_out);

	if (mdm_ctrl_get_state(drv) == MDM_CTRL_STATE_OFF)
		return IRQ_HANDLED;

	if (atomic_read(&drv->rst_ongoing)) {
		/* Rising edge = IPC ready */
		if (value) {
			atomic_set(&drv->rst_ongoing, 0);
			dev_info(drv->dev, "IPC ready\n");
			mdm_ctrl_set_state(drv, MDM_CTRL_STATE_IPC_READY);
		}
		return IRQ_HANDLED;
	}

	/* Unexpected reset */
	dev_err(drv->dev, "unexpected RESET_OUT 0x%x\n", value);
	atomic_set(&drv->rst_ongoing, 1);
	drv->hangup_causes |= MDM_CTRL_HU_RESET;
	queue_work(drv->hu_wq, &drv->hangup_work);

	return IRQ_HANDLED;
}

static irqreturn_t mdm_ctrl_coredump_irq(int irq, void *data)
{
	struct mdm_ctrl *drv = data;

	if (mdm_ctrl_get_state(drv) == MDM_CTRL_STATE_OFF)
		return IRQ_HANDLED;

	if (atomic_read(&drv->rst_ongoing))
		return IRQ_HANDLED;

	dev_err(drv->dev, "CORE DUMP detected\n");
	drv->hangup_causes |= MDM_CTRL_HU_COREDUMP;
	queue_work(drv->hu_wq, &drv->hangup_work);

	return IRQ_HANDLED;
}

/*
 * Char device file operations
 */
static int mdm_ctrl_dev_open(struct inode *inode, struct file *filep)
{
	struct mdm_ctrl *drv = container_of(inode->i_cdev, struct mdm_ctrl,
					    cdev);

	mutex_lock(&drv->lock);
	if (drv->opened) {
		mutex_unlock(&drv->lock);
		return -EBUSY;
	}

	filep->private_data = drv;
	drv->opened = 1;
	mutex_unlock(&drv->lock);
	return 0;
}

static int mdm_ctrl_dev_close(struct inode *inode, struct file *filep)
{
	struct mdm_ctrl *drv = filep->private_data;

	mutex_lock(&drv->lock);
	drv->opened = 0;
	mutex_unlock(&drv->lock);
	return 0;
}

static long mdm_ctrl_dev_ioctl(struct file *filep, unsigned int cmd,
			       unsigned long arg)
{
	struct mdm_ctrl *drv = filep->private_data;
	unsigned int state, param;
	int ret = 0;

	state = mdm_ctrl_get_state(drv);

	switch (cmd) {
	case MDM_CTRL_POWER_OFF:
		mdm_ctrl_power_off(drv);
		break;

	case MDM_CTRL_POWER_ON:
		if (state == MDM_CTRL_STATE_OFF ||
		    state == MDM_CTRL_STATE_UNKNOWN)
			mdm_ctrl_cold_boot(drv);
		else
			dev_info(drv->dev, "power on: already on\n");
		break;

	case MDM_CTRL_WARM_RESET:
		if (state != MDM_CTRL_STATE_OFF)
			mdm_ctrl_warm_reset(drv);
		else
			dev_err(drv->dev, "warm reset: modem is off\n");
		break;

	case MDM_CTRL_FLASHING_WARM_RESET:
		if (state != MDM_CTRL_STATE_OFF) {
			mdm_ctrl_set_state(drv, MDM_CTRL_STATE_WARM_BOOT);
			atomic_set(&drv->rst_ongoing, 1);
			gpiod_set_value_cansleep(drv->gpio_rst_bbn, 0);
			usleep_range(drv->warm_rst_duration,
				     drv->warm_rst_duration + 1000);
			gpiod_set_value_cansleep(drv->gpio_rst_bbn, 1);
		}
		break;

	case MDM_CTRL_COLD_RESET:
		if (state != MDM_CTRL_STATE_OFF)
			mdm_ctrl_cold_reset(drv);
		break;

	case MDM_CTRL_SET_STATE:
		if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
			return -EFAULT;
		param &= (MDM_CTRL_STATE_OFF | MDM_CTRL_STATE_COLD_BOOT |
			  MDM_CTRL_STATE_WARM_BOOT | MDM_CTRL_STATE_COREDUMP |
			  MDM_CTRL_STATE_IPC_READY |
			  MDM_CTRL_STATE_FW_DOWNLOAD_READY);
		mdm_ctrl_set_state(drv, param);
		break;

	case MDM_CTRL_GET_STATE:
		param = state;
		if (copy_to_user((void __user *)arg, &param, sizeof(param)))
			return -EFAULT;
		break;

	case MDM_CTRL_GET_HANGUP_REASONS:
		param = drv->hangup_causes;
		if (copy_to_user((void __user *)arg, &param, sizeof(param)))
			return -EFAULT;
		break;

	case MDM_CTRL_CLEAR_HANGUP_REASONS:
		drv->hangup_causes = MDM_CTRL_NO_HU;
		break;

	case MDM_CTRL_SET_POLLED_STATES:
		if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
			return -EFAULT;
		drv->polled_states = param;
		/* Safe: wake_up below handles the race with poll() */
		if (waitqueue_active(&drv->wait_wq)) {
			state = mdm_ctrl_get_state(drv);
			if (state)
				drv->polled_state_reached =
					((state & param) == state);
			wake_up(&drv->wait_wq);
		} else {
			drv->polled_state_reached = false;
		}
		break;

	default:
		ret = -ENOIOCTLCMD;
	}

	return ret;
}

static __poll_t mdm_ctrl_dev_poll(struct file *filep,
				  struct poll_table_struct *pt)
{
	struct mdm_ctrl *drv = filep->private_data;
	__poll_t ret = 0;

	poll_wait(filep, &drv->wait_wq, pt);

	if (drv->polled_state_reached ||
	    (mdm_ctrl_get_state(drv) & drv->polled_states)) {
		drv->polled_state_reached = false;
		ret |= EPOLLHUP | EPOLLRDNORM;
	}

	return ret;
}

static const struct file_operations mdm_ctrl_fops = {
	.owner		= THIS_MODULE,
	.open		= mdm_ctrl_dev_open,
	.release	= mdm_ctrl_dev_close,
	.unlocked_ioctl	= mdm_ctrl_dev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.poll		= mdm_ctrl_dev_poll,
};

/*
 * Platform driver probe/remove
 */
static int mdm_ctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mdm_ctrl *drv;
	int ret;
	int irq;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = dev;
	platform_set_drvdata(pdev, drv);

	/* Read modem config from device properties (set by board file) */
	device_property_read_u16(dev, "pmic-reg", &drv->pmic_reg);
	device_property_read_u8(dev, "pmic-on", &drv->pmic_on);
	device_property_read_u8(dev, "pmic-off", &drv->pmic_off);
	device_property_read_u8(dev, "pmic-mask", &drv->pmic_mask);
	device_property_read_u32(dev, "pre-on-delay-us", &drv->pre_on_delay);
	device_property_read_u32(dev, "on-duration-us", &drv->on_duration);
	device_property_read_u32(dev, "pre-pwr-down-delay-us",
				 &drv->pre_pwr_down_delay);
	device_property_read_u32(dev, "warm-rst-duration-us",
				 &drv->warm_rst_duration);
	device_property_read_u32(dev, "pre-cflash-delay-ms",
				 &drv->pre_cflash_delay);
	device_property_read_u32(dev, "pre-wflash-delay-ms",
				 &drv->pre_wflash_delay);

	dev_info(dev, "PMIC reg=0x%02x on=0x%02x off=0x%02x mask=0x%02x\n",
		 drv->pmic_reg, drv->pmic_on, drv->pmic_off, drv->pmic_mask);

	/* Acquire SCU IPC for PMIC access */
	drv->scu = devm_intel_scu_ipc_dev_get(dev);
	if (IS_ERR(drv->scu)) {
		ret = PTR_ERR(drv->scu);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get SCU IPC: %d\n", ret);
		return ret;
	}

	/* Request GPIOs */
	drv->gpio_rst_bbn = devm_gpiod_get(dev, "rst-bbn", GPIOD_OUT_LOW);
	if (IS_ERR(drv->gpio_rst_bbn)) {
		ret = PTR_ERR(drv->gpio_rst_bbn);
		dev_err(dev, "failed to get rst-bbn GPIO: %d\n", ret);
		return ret;
	}

	drv->gpio_pwr_on = devm_gpiod_get(dev, "pwr-on", GPIOD_OUT_LOW);
	if (IS_ERR(drv->gpio_pwr_on)) {
		ret = PTR_ERR(drv->gpio_pwr_on);
		dev_err(dev, "failed to get pwr-on GPIO: %d\n", ret);
		return ret;
	}

	drv->gpio_rst_out = devm_gpiod_get_optional(dev, "rst-out",
						     GPIOD_IN);
	drv->gpio_cdump = devm_gpiod_get_optional(dev, "coredump", GPIOD_IN);

	/* Initialization */
	mutex_init(&drv->lock);
	init_waitqueue_head(&drv->wait_wq);
	INIT_WORK(&drv->hangup_work, mdm_ctrl_handle_hangup);

	drv->hu_wq = alloc_ordered_workqueue(DRVNAME "-hu", 0);
	if (!drv->hu_wq) {
		dev_err(dev, "failed to create hangup workqueue\n");
		return -ENOMEM;
	}

	/* Register char device */
	ret = alloc_chrdev_region(&drv->tdev, 0, 1, MDM_BOOT_DEVNAME);
	if (ret) {
		dev_err(dev, "alloc_chrdev_region failed: %d\n", ret);
		goto err_destroy_wq;
	}

	drv->major = MAJOR(drv->tdev);
	cdev_init(&drv->cdev, &mdm_ctrl_fops);
	drv->cdev.owner = THIS_MODULE;

	ret = cdev_add(&drv->cdev, drv->tdev, 1);
	if (ret) {
		dev_err(dev, "cdev_add failed: %d\n", ret);
		goto err_unreg_chrdev;
	}

	drv->class = class_create(MDM_BOOT_DEVNAME);
	if (IS_ERR(drv->class)) {
		ret = PTR_ERR(drv->class);
		dev_err(dev, "class_create failed: %d\n", ret);
		goto err_del_cdev;
	}

	drv->char_dev = device_create(drv->class, NULL, drv->tdev, NULL,
				      MDM_BOOT_DEVNAME);
	if (IS_ERR(drv->char_dev)) {
		ret = PTR_ERR(drv->char_dev);
		dev_err(dev, "device_create failed: %d\n", ret);
		goto err_destroy_class;
	}

	/* Request IRQs for modem status GPIOs */
	if (drv->gpio_rst_out) {
		irq = gpiod_to_irq(drv->gpio_rst_out);
		if (irq > 0) {
			ret = devm_request_irq(dev, irq, mdm_ctrl_reset_irq,
					       IRQF_TRIGGER_RISING |
					       IRQF_TRIGGER_FALLING |
					       IRQF_NO_SUSPEND,
					       DRVNAME "-rst", drv);
			if (ret)
				dev_warn(dev, "rst-out IRQ request failed: %d\n",
					 ret);
		}
	}

	if (drv->gpio_cdump) {
		irq = gpiod_to_irq(drv->gpio_cdump);
		if (irq > 0) {
			ret = devm_request_irq(dev, irq, mdm_ctrl_coredump_irq,
					       IRQF_TRIGGER_RISING |
					       IRQF_NO_SUSPEND,
					       DRVNAME "-cdump", drv);
			if (ret)
				dev_warn(dev, "coredump IRQ request failed: %d\n",
					 ret);
		}
	}

	mdm_ctrl_set_state(drv, MDM_CTRL_STATE_OFF);

	/* Power off modem at boot (mmgr will power it on) */
	mdm_ctrl_power_off(drv);

	dev_info(dev, "modem control ready (XMM 7160)\n");
	return 0;

err_destroy_class:
	class_destroy(drv->class);
err_del_cdev:
	cdev_del(&drv->cdev);
err_unreg_chrdev:
	unregister_chrdev_region(drv->tdev, 1);
err_destroy_wq:
	destroy_workqueue(drv->hu_wq);
	return ret;
}

static void mdm_ctrl_remove(struct platform_device *pdev)
{
	struct mdm_ctrl *drv = platform_get_drvdata(pdev);

	device_destroy(drv->class, drv->tdev);
	class_destroy(drv->class);
	cdev_del(&drv->cdev);
	unregister_chrdev_region(drv->tdev, 1);
	destroy_workqueue(drv->hu_wq);
}

static struct platform_driver mdm_ctrl_driver = {
	.probe	= mdm_ctrl_probe,
	.remove_new = mdm_ctrl_remove,
	.driver	= {
		.name = DRVNAME,
	},
};
module_platform_driver(mdm_ctrl_driver);

MODULE_AUTHOR("Thomas (venue8-flash project)");
MODULE_DESCRIPTION("Intel XMM 7160 modem control driver for Dell Venue 8 3840");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRVNAME);
