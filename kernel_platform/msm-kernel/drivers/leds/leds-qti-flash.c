// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */
#define pr_fmt(fmt)	"qti-flash: %s: " fmt, __func__

#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/led-class-flash.h>
#include <linux/leds-qti-flash.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/soc/qcom/battery_charger.h>

#include "leds.h"

#define FLASH_LED_REVISION1			0x00

#define FLASH_LED_PERIPH_SUBTYPE		0x05

#define FLASH_LED_STATUS1			0x06

#define FLASH_LED_STATUS2			0x07
#define  FLASH_LED_OTST1_STATUS			BIT(5)
#define  FLASH_LED_OTST2_STATUS			BIT(4)
#define  FLASH_LED_VPH_PWR_LOW			BIT(0)

#define FLASH_INT_RT_STS			0x10
#define  FLASH_LED_FAULT_RT_STS			BIT(0)
#define  FLASH_LED_ALL_RAMP_DN_DONE_RT_STS	BIT(3)
#define  FLASH_LED_ALL_RAMP_UP_DONE_RT_STS	BIT(4)

#define FLASH_LED_SAFETY_TIMER(id)		(0x3E + id)
#define  FLASH_LED_SAFETY_TIMER_EN_MASK		BIT(7)
#define  FLASH_LED_SAFETY_TIMER_EN		BIT(7)
#define  SAFETY_TIMER_MAX_TIMEOUT_MS		1280
#define  SAFETY_TIMER_MIN_TIMEOUT_MS		10
#define  SAFETY_TIMER_STEP_SIZE			10
#define  SAFETY_TIMER_DEFAULT_TIMEOUT_MS	200

#define FLASH_LED_ITARGET(id)			(0x42 + id)
#define  FLASH_LED_ITARGET_MASK			GENMASK(6, 0)

#define FLASH_ENABLE_CONTROL			0x46
#define  FLASH_MODULE_ENABLE			BIT(7)
#define  FLASH_MODULE_DISABLE			0x0

#define FLASH_LED_IRESOLUTION			0x49
#define  FLASH_LED_IRESOLUTION_MASK(id)		BIT(id)

#define FLASH_LED_STROBE_CTRL(id)		(0x4A + id)
#define  FLASH_LED_STROBE_CFG_MASK		GENMASK(6, 4)
#define  FLASH_LED_STROBE_CFG_SHIFT		4
#define  FLASH_LED_HW_SW_STROBE_SEL		BIT(2)
#define  FLASH_LED_STROBE_SEL_SHIFT		2

#define FLASH_EN_LED_CTRL			0x4E

#define FLASH_LED_IBATT_OCP_THRESH_DEFAULT_UA	2500000
#define FLASH_LED_RPARA_DEFAULT_UOHM		80000
#define VPH_DROOP_THRESH_VAL_UV			3400000

#define  FLASH_LED_ENABLE(id)			BIT(id)
#define  FLASH_LED_DISABLE			0

#define FLASH_LED_MITIGATION_SW			0x65
#define  FLASH_LED_LMH_MITIGATION_SW_EN		BIT(0)

#define FLASH_LED_THERMAL_OTST2_CFG1		0x78
#define FLASH_LED_THERMAL_OTST1_CFG1		0x7A
#define  FLASH_LED_THERMAL_THRSH_MASK		GENMASK(2, 0)
#define  FLASH_LED_V1_OTST1_THRSH_MIN		0x13
#define  FLASH_LED_V2_OTST1_THRSH_MIN		0x10
#define  FLASH_LED_OTST2_THRSH_MIN		0x30

#define MAX_IRES_LEVELS				2
#define IRES_12P5_MAX_CURR_MA			1500
#define IRES_5P0_MAX_CURR_MA			640
#define TORCH_MAX_CURR_MA			500
#define INDICATOR_MAX_CURR_MA			100
#define IRES_12P5_UA				12500
#define IRES_5P0_UA				5000
#define IRES_DEFAULT_UA				IRES_12P5_UA
#define MAX_FLASH_CURRENT_MA			2000
#define IBATT_OCP_THRESH_DEFAULT_UA		4500000
#define OTST1_CURR_LIM_MA			200
#define OTST2_CURR_LIM_MA			500
#define VLED_MAX_DEFAULT_UV			3500000

#define LED_MASK_ALL(led)		GENMASK(led->data->max_channels - 1, 0)

#define FORCE_TORCH_MODE		0x68
#define FORCE_TORCH			BIT(0)

enum flash_led_type {
	FLASH_LED_TYPE_UNKNOWN,
	FLASH_LED_TYPE_FLASH,
	FLASH_LED_TYPE_TORCH,
	FLASH_LED_TYPE_INDICATOR,
};

enum flash_led_revision {
	FLASH_LED_REVISION_2P0 = 1,
};

enum flash_led_subtype {
	FLASH_LED_4_CHAN = 0x7,
};

enum strobe_type {
	SW_STROBE = 0,
	HW_STROBE,
};

enum thermal_levels {
	OTST1_IDX,
	OTST2_IDX,
	OTST_MAX,
};

enum pmic_type {
	PM8350C,
	PM2250,
};

struct flash_node_data {
	struct qti_flash_led		*led;
	struct led_classdev_flash	fdev;
	u32				ires_ua;
	u32				default_ires_ua;
	u32				user_current_ma;
	u32				current_ma;
	u32				max_current;
	u8				duration;
	u8				id;
	u8				updated_ires_idx;
	u8				ires_idx;
	u8				strobe_config;
	u8				strobe_sel;
	enum flash_led_type		type;
	bool				configured;
	bool				enabled;
};

struct flash_switch_data {
	struct qti_flash_led		*led;
	struct led_classdev		cdev;
	struct hrtimer			on_timer;
	struct hrtimer			off_timer;
	u64				on_time_ms;
	u64				off_time_ms;
	u32				led_mask;
	bool				enabled;
	bool				symmetry_en;
};

struct pmic_data {
	u8	max_channels;
	int	pmic_type;
};

enum qti_flash_iio_props {
	RBATT,
	OCV,
	IBAT,
	F_TRIGGER,
	F_ACTIVE,
};

static char *qti_flash_iio_prop_names[] = {
	[RBATT] = "rbatt",
	[OCV] = "voltage_ocv",
	[IBAT] = "current_now",
	[F_TRIGGER] = "flash_trigger",
	[F_ACTIVE] = "flash_active",
};

/**
 * struct qti_flash_led: Main Flash LED data structure
 * @pdev:			Pointer for platform device
 * @regmap:			Pointer for regmap structure
 * @fnode:			Pointer for array of child LED devices
 * @snode:			Pointer for array of child switch devices
 * @batt_psy:			Pointer for battery power supply
 * @lock:			Spinlock to be used for critical section
 * @num_fnodes:			Number of flash/torch nodes defined in device
 *				tree
 * @num_snodes:			Number of switch nodes defined in device tree
 * @hw_strobe_gpio:		Pointer for array of GPIOs for HW strobing
 * @all_ramp_up_done_irq:	IRQ number for all ramp up interrupt
 * @all_ramp_down_done_irq:	IRQ number for all ramp down interrupt
 * @led_fault_irq:		IRQ number for LED fault interrupt
 * @max_current:		Maximum current available for flash
 * @thermal_derate_current:	Thermal derating current limits
 * @base:			Base address of the flash LED module
 * @revision:			Revision of the flash LED module
 * @subtype:			Peripheral subtype of the flash LED module
 * @chan_en_map:		Bit map of individual channel enable
 * @module_en:			Flag used to enable/disable flash LED module
 * @trigger_lmh:		Flag to enable lmh mitigation
 * @non_all_mask_switch_present: Used in handling symmetry for all_mask switch
 * @secure_vm:			Flag indicating whether flash LED is used by
 *				secure VM
 * @debug_board_present:	Flag to indicate debug board present
 */
struct qti_flash_led {
	struct platform_device		*pdev;
	struct regmap			*regmap;
	struct flash_node_data		*fnode;
	struct flash_switch_data	*snode;
	struct power_supply		*batt_psy;
	struct power_supply		*usb_psy;
	struct iio_channel		**iio_channels;
	struct pmic_data		*data;
	spinlock_t			lock;
	u32				num_fnodes;
	u32				num_snodes;
	int				*hw_strobe_gpio;
	int				all_ramp_up_done_irq;
	int				all_ramp_down_done_irq;
	int				led_fault_irq;
	int				ibatt_ocp_threshold_ua;
	int				max_current;
	int				thermal_derate_current[OTST_MAX];
	u16				base;
	u8				revision;
	u8				subtype;
	u8				chan_en_map;
	bool				module_en;
	bool				trigger_lmh;
	bool				non_all_mask_switch_present;
	bool				secure_vm;
	bool				debug_board_present;
};

struct flash_current_headroom {
	u16 current_ma;
	u16 headroom_mv;
};

static const struct flash_current_headroom pm8350c_map[4] = {
	{750, 200}, {1000, 250}, {1250, 300}, {1500, 400},
};

static const u32 flash_led_max_ires_values[MAX_IRES_LEVELS] = {
	IRES_5P0_MAX_CURR_MA, IRES_12P5_MAX_CURR_MA
};

static int timeout_to_code(u32 timeout)
{
	if (!timeout || timeout > SAFETY_TIMER_MAX_TIMEOUT_MS)
		return -EINVAL;

	return DIV_ROUND_CLOSEST(timeout, SAFETY_TIMER_STEP_SIZE) - 1;
}

static int get_ires_idx(u32 ires_ua)
{
	if (ires_ua == IRES_5P0_UA)
		return 0;
	else if (ires_ua == IRES_12P5_UA)
		return 1;
	else
		return -EINVAL;
}

static int current_to_code(u32 target_curr_ma, u32 ires_ua)
{
	if (!ires_ua || !target_curr_ma ||
		(target_curr_ma < DIV_ROUND_CLOSEST(ires_ua, 1000)))
		return 0;

	return DIV_ROUND_CLOSEST(target_curr_ma * 1000, ires_ua) - 1;
}

static bool is_channel_configured(struct flash_node_data *fnode)
{
	int i;

	for (i = 0; i < fnode->led->num_fnodes; i++) {
		if (fnode->led->fnode[i].id == fnode->id &&
		    fnode->led->fnode[i].type != fnode->type &&
		    fnode->led->fnode[i].configured) {
			pr_debug("Channel %d for %s is already configured by %s\n", fnode->id,
				fnode->fdev.led_cdev.name, fnode->led->fnode[i].fdev.led_cdev.name);
			return true;
		}
	}
	return false;
}

static int qti_flash_led_read(struct qti_flash_led *led, u16 offset,
				u8 *data, u8 len)
{
	int rc;

	rc = regmap_bulk_read(led->regmap, (led->base + offset), data, len);
	if (rc < 0)
		pr_err("Failed to read from 0x%04X rc = %d\n",
			(led->base + offset), rc);
	else
		pr_debug("Read %*ph from addr %#x\n", len, data,
			(led->base + offset));

	return rc;
}

static int qti_flash_led_write(struct qti_flash_led *led, u16 offset,
				u8 *data, u8 len)
{
	int rc;

	rc = regmap_bulk_write(led->regmap, (led->base + offset), data,
			len);
	if (rc < 0)
		pr_err("Failed to write to 0x%04X rc = %d\n",
			(led->base + offset), rc);
	else
		pr_debug("Wrote %*ph to addr %#x\n", len, data,
			(led->base + offset));

	return rc;
}

static int qti_flash_led_masked_write(struct qti_flash_led *led,
					u16 offset, u8 mask, u8 data)
{
	int rc;

	rc = regmap_update_bits(led->regmap, (led->base + offset),
			mask, data);
	if (rc < 0)
		pr_err("Failed to update bits from 0x%04X, rc = %d\n",
			(led->base + offset), rc);
	else
		pr_debug("Wrote %#x mask %#x to addr %#x\n", data, mask,
			(led->base + offset));

	return rc;
}

static int qti_flash_get_iio_chan(struct qti_flash_led *led,
				  enum qti_flash_iio_props chan)
{
	int rc = 0;

	/*
	 * if the channel pointer is not-NULL and has a ERR value it has
	 * already been queried upon earlier, hence return from here.
	 */
	if (IS_ERR(led->iio_channels[chan]))
		return -EINVAL;

	if (!led->iio_channels[chan]) {
		led->iio_channels[chan] = iio_channel_get(&led->pdev->dev,
						qti_flash_iio_prop_names[chan]);
		if (IS_ERR(led->iio_channels[chan])) {
			rc = PTR_ERR(led->iio_channels[chan]);
			if (rc == -EPROBE_DEFER) {
				led->iio_channels[chan] = NULL;
				return rc;
			}
			pr_err("%s channel unavailable %d\n",
				qti_flash_iio_prop_names[chan], rc);
			return rc;
		}
	}

	return 0;
}

static int qti_flash_iio_getprop(struct qti_flash_led *led,
				 enum qti_flash_iio_props chan, int *data)
{
	int rc = 0;

	rc = qti_flash_get_iio_chan(led, chan);
	if (rc < 0)
		return rc;

	rc = iio_read_channel_processed(led->iio_channels[chan], data);
	if (rc < 0)
		pr_err("Error in reading IIO channel data rc = %d\n", rc);

	return rc;
}

static int qti_flash_iio_setprop(struct qti_flash_led *led,
				  enum qti_flash_iio_props chan, int data)
{
	int rc = 0;

	rc = qti_flash_get_iio_chan(led, chan);
	if (rc < 0)
		return rc;

	rc = iio_write_channel_raw(led->iio_channels[chan], data);
	if (rc < 0)
		pr_err("Error in writing IIO channel data rc = %d\n", rc);

	return rc;
}

static int qti_flash_poll_vreg_ok(struct qti_flash_led *led)
{
	int rc, i, pval;

	if (led->data->pmic_type != PM2250)
		return 0;

	for (i = 0; i < 60; i++) {
		/* wait for the flash vreg_ok to be set */
		mdelay(5);

		rc = qti_flash_iio_getprop(led, F_TRIGGER, &pval);
		if (rc < 0) {
			pr_err("Failure reading Flash trigger prop rc = %d\n",
			   rc);
			return rc;
		}

		if (pval > 0) {
			pr_debug("Flash trigger set\n");
			break;
		}

		if (pval < 0) {
			pr_err("Error during flash trigger %d\n", pval);
			return pval;
		}
	}

	if (!pval) {
		pr_err("Failed to enable the module\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int qti_flash_led_module_control(struct qti_flash_led *led,
				bool enable)
{
	int rc = 0;
	u8 val;

	if (enable) {
		if (!led->module_en && led->chan_en_map) {
			val = FLASH_MODULE_ENABLE;
			rc = qti_flash_led_write(led, FLASH_ENABLE_CONTROL,
						&val, 1);
			if (rc < 0)
				return rc;

			led->module_en = true;
		}

		val = FLASH_MODULE_DISABLE;
		rc = qti_flash_poll_vreg_ok(led);
		if (rc < 0) {
			/* Disable the module */
			rc = qti_flash_led_write(led, FLASH_ENABLE_CONTROL,
						&val, 1);
			if (rc < 0)
				return rc;

			led->module_en = false;
		}
	} else {
		if (led->module_en && !led->chan_en_map) {
			val = FLASH_MODULE_DISABLE;
			rc = qti_flash_led_write(led, FLASH_ENABLE_CONTROL,
						&val, 1);
			if (rc < 0)
				return rc;

			led->module_en = false;
		}
	}

	return rc;
}

static int qti_flash_lmh_mitigation_config(struct qti_flash_led *led,
						bool enable)
{
	u8 val = enable ? FLASH_LED_LMH_MITIGATION_SW_EN : 0;
	int rc;

	if (led->debug_board_present || enable == led->trigger_lmh)
		return 0;

	rc = qti_flash_led_write(led, FLASH_LED_MITIGATION_SW, &val, 1);
	if (rc < 0) {
		pr_err("Failed to %s LMH mitigation, rc=%d\n",
			enable ? "enable" : "disable", rc);
	} else {
		pr_debug("%s LMH mitigation\n",
			enable ? "enabled" : "disabled");
		led->trigger_lmh = enable;
	}

	return rc;
}

static int qti_flash_led_strobe(struct qti_flash_led *led,
				struct flash_switch_data *snode,
				u8 mask, u8 value)
{
	int rc, i;
	bool enable = mask & value;

	spin_lock(&led->lock);

	if (enable) {
		for (i = 0; i < led->data->max_channels; i++)
			if ((mask & BIT(i)) && (value & BIT(i)))
				led->chan_en_map |= BIT(i);

		rc = qti_flash_led_module_control(led, enable);
		if (rc < 0)
			goto error;

		for (i = 0; i < led->num_fnodes; i++) {
			if ((mask & BIT(led->fnode[i].id)) &&
				led->fnode[i].configured &&
				led->fnode[i].type == FLASH_LED_TYPE_TORCH &&
						led->subtype == 0x6) {
				rc = qti_flash_led_masked_write(led,
						FORCE_TORCH_MODE,
					FORCE_TORCH, FORCE_TORCH);
				if (rc < 0)
					goto error;
			}
		}

		if (snode && snode->off_time_ms) {
			pr_debug("Off timer started with delay %d ms\n",
				snode->off_time_ms);
			hrtimer_start(&snode->off_timer,
					ms_to_ktime(snode->off_time_ms),
					HRTIMER_MODE_REL);
		}

		rc = qti_flash_led_masked_write(led, FLASH_EN_LED_CTRL,
				mask, value);
		if (rc < 0)
			goto error;
	} else {
		for (i = 0; i < led->data->max_channels; i++)
			if ((led->chan_en_map & BIT(i)) &&
			    (mask & BIT(i)) && !(value & BIT(i)))
				led->chan_en_map &= ~(BIT(i));

		rc = qti_flash_led_masked_write(led, FLASH_EN_LED_CTRL,
				mask, value);
		if (rc < 0)
			goto error;

		for (i = 0; i < led->num_fnodes; i++) {
			if ((mask & BIT(led->fnode[i].id)) &&
				led->fnode[i].configured &&
				led->fnode[i].type == FLASH_LED_TYPE_TORCH &&
					led->subtype == 0x6) {
				rc = qti_flash_led_masked_write(led,
					FORCE_TORCH_MODE, FORCE_TORCH, 0);
				if (rc < 0)
					goto error;
			}
		}

		if (led->trigger_lmh) {
			rc = qti_flash_lmh_mitigation_config(led, false);
			if (rc < 0)
				goto error;
		}

		rc = qti_flash_led_module_control(led, enable);
		if (rc < 0)
			goto error;
	}

error:
	spin_unlock(&led->lock);

	return rc;
}

static int qti_flash_led_enable(struct flash_node_data *fnode)
{
	struct qti_flash_led *led = fnode->led;
	int rc;
	u8 val, addr_offset;

	addr_offset = fnode->id;

	spin_lock(&led->lock);
	val = (fnode->updated_ires_idx ? 0 : 1) << fnode->id;
	rc = qti_flash_led_masked_write(led, FLASH_LED_IRESOLUTION,
		FLASH_LED_IRESOLUTION_MASK(fnode->id), val);
	if (rc < 0)
		goto out;

	rc = qti_flash_led_masked_write(led,
		FLASH_LED_ITARGET(addr_offset), FLASH_LED_ITARGET_MASK,
		current_to_code(fnode->current_ma, fnode->ires_ua));
	if (rc < 0)
		goto out;

	/*
	 * For dynamic brightness control of Torch LEDs,
	 * just configure the target current.
	 */
	if ((fnode->type == FLASH_LED_TYPE_TORCH
	    || fnode->type == FLASH_LED_TYPE_INDICATOR)
	    && fnode->enabled) {
		spin_unlock(&led->lock);
		return 0;
	}

	if (fnode->type == FLASH_LED_TYPE_FLASH) {
		val = fnode->duration | FLASH_LED_SAFETY_TIMER_EN;
		rc = qti_flash_led_write(led,
			FLASH_LED_SAFETY_TIMER(addr_offset), &val, 1);
		if (rc < 0)
			goto out;
	}

	fnode->configured = true;

	if ((fnode->strobe_sel == HW_STROBE) &&
		gpio_is_valid(led->hw_strobe_gpio[fnode->id]))
		gpio_set_value(led->hw_strobe_gpio[fnode->id], 1);

out:
	spin_unlock(&led->lock);
	return rc;
}

static int qti_flash_led_disable(struct flash_node_data *fnode)
{
	struct qti_flash_led *led = fnode->led;
	int rc;

	if (!fnode->configured) {
		pr_debug("%s is not configured\n", fnode->fdev.led_cdev.name);
		return 0;
	}

	spin_lock(&led->lock);
	if ((fnode->strobe_sel == HW_STROBE) &&
		gpio_is_valid(led->hw_strobe_gpio[fnode->id]))
		gpio_set_value(led->hw_strobe_gpio[fnode->id], 0);

	rc = qti_flash_led_masked_write(led,
		FLASH_LED_ITARGET(fnode->id), FLASH_LED_ITARGET_MASK, 0);
	if (rc < 0)
		goto out;

	rc = qti_flash_led_masked_write(led,
		FLASH_LED_SAFETY_TIMER(fnode->id),
		FLASH_LED_SAFETY_TIMER_EN_MASK, 0);
	if (rc < 0)
		goto out;

	fnode->configured = false;
	fnode->current_ma = 0;
	fnode->user_current_ma = 0;

out:
	spin_unlock(&led->lock);
	return rc;
}

static enum led_brightness qti_flash_led_brightness_get(
						struct led_classdev *led_cdev)
{
	return led_cdev->brightness;
}

static int __qti_flash_led_brightness_set(struct led_classdev *led_cdev,
					enum led_brightness brightness)
{
	struct flash_node_data *fnode = NULL;
	struct led_classdev_flash *fdev = NULL;
	int rc;
	u32 current_ma = brightness;
	u32 min_current_ma;

	fdev = container_of(led_cdev, struct led_classdev_flash, led_cdev);
	fnode = container_of(fdev, struct flash_node_data, fdev);

	if (!brightness) {
		rc = qti_flash_led_strobe(fnode->led, NULL,
			FLASH_LED_ENABLE(fnode->id), 0);
		if (rc < 0) {
			pr_err("Failed to destrobe LED, rc=%d\n", rc);
			return rc;
		}

		rc = qti_flash_led_disable(fnode);
		if (rc < 0)
			pr_err("Failed to disable LED\n");

		led_cdev->brightness = 0;

		return rc;
	}

	min_current_ma = DIV_ROUND_CLOSEST(fnode->ires_ua, 1000);
	if (current_ma < min_current_ma)
		current_ma = min_current_ma;

	fnode->updated_ires_idx = fnode->ires_idx;
	fnode->ires_ua = fnode->default_ires_ua;

	current_ma = min(current_ma, fnode->max_current);
	if (current_ma > flash_led_max_ires_values[fnode->ires_idx]) {
		if (current_ma > IRES_5P0_MAX_CURR_MA)
			fnode->ires_ua = IRES_12P5_UA;
		else
			fnode->ires_ua = IRES_5P0_UA;
		fnode->ires_idx = get_ires_idx(fnode->ires_ua);
	}

	fnode->current_ma = current_ma;
	led_cdev->brightness = current_ma;

	rc = qti_flash_led_enable(fnode);
	if (rc < 0) {
		pr_err("Failed to set brightness %d to LED\n", brightness);
		return rc;
	}

	if (fnode->type == FLASH_LED_TYPE_INDICATOR) {
		rc = qti_flash_led_strobe(fnode->led, NULL,
			FLASH_LED_ENABLE(fnode->id), FLASH_LED_ENABLE(fnode->id));
		if (rc < 0)
			pr_err("Failed to strobe LED, rc=%d\n", rc);
	}

	return rc;
}

static int qti_flash_config_group_symmetry(struct qti_flash_led *led,
					   enum flash_led_type type,
					   u32 led_mask)
{
	int i, rc = 0, total_curr_ma = 0, symmetric_leds = 0, per_led_curr_ma;

	for (i = 0; i < led->num_fnodes; i++) {
		if ((led_mask & BIT(led->fnode[i].id)) &&
			(led->fnode[i].type == type)) {
			total_curr_ma += led->fnode[i].user_current_ma;
			symmetric_leds++;
		}
	}

	if (!symmetric_leds) {
		pr_err("led-mask %#x has zero symmetric leds\n", led_mask);
		return -EINVAL;
	}

	per_led_curr_ma = total_curr_ma / symmetric_leds;

	pr_debug("mask: %#x symmetric_leds: %d total: %d per_led_curr_ma: %d\n",
		led_mask, symmetric_leds, total_curr_ma, per_led_curr_ma);

	for (i = 0; i < led->num_fnodes; i++) {
		if (led_mask & BIT(led->fnode[i].id) &&
			led->fnode[i].type == type) {
			rc = __qti_flash_led_brightness_set(
				&led->fnode[i].fdev.led_cdev, per_led_curr_ma);
			if (rc < 0)
				return rc;
		}
	}

	return rc;
}

static int qti_flash_led_symmetry_config(struct flash_switch_data *snode)
{
	struct qti_flash_led *led = snode->led;
	enum flash_led_type type = FLASH_LED_TYPE_UNKNOWN;
	int i, rc = 0;

	/* Determine which LED type has triggered switch ON */
	for (i = 0; i < led->num_fnodes; i++) {
		if ((snode->led_mask & BIT(led->fnode[i].id)) &&
			(led->fnode[i].configured))
			type = led->fnode[i].type;
	}

	if (type == FLASH_LED_TYPE_UNKNOWN) {
		/* No channels are configured */
		return 0;
	}

	if (snode->led_mask == LED_MASK_ALL(led) &&
			led->non_all_mask_switch_present) {
		/*
		 * Gather masks from the other switches and configure symmetry
		 * accordingly.
		 */
		for (i = 0; i < led->num_snodes; i++) {
			if (led->snode[i].led_mask != LED_MASK_ALL(led)) {
				rc = qti_flash_config_group_symmetry(led, type,
						led->snode[i].led_mask);
				if (rc < 0)
					return rc;
			}
		}
	} else {
		rc = qti_flash_config_group_symmetry(led, type,
				snode->led_mask);
	}

	return rc;
}

static void qti_flash_led_brightness_set(struct led_classdev *led_cdev,
					enum led_brightness brightness)
{
	struct led_classdev_flash *fdev;
	struct flash_node_data *fnode;
	struct qti_flash_led *led;
	int i, rc;

	fdev = container_of(led_cdev, struct led_classdev_flash, led_cdev);
	fnode = container_of(fdev, struct flash_node_data, fdev);
	led = fnode->led;

	if (is_channel_configured(fnode))
		return;

	rc = __qti_flash_led_brightness_set(led_cdev, brightness);
	if (!rc)
		fnode->user_current_ma = brightness;
	else
		return;

	for (i = 0; i < led->num_snodes; i++) {
		pr_debug("snode[%d] symm %d, enabled %d\n", i,
				led->snode[i].symmetry_en,
				led->snode[i].enabled);
		if (led->snode[i].symmetry_en && led->snode[i].enabled) {
			qti_flash_led_symmetry_config(&led->snode[i]);
			break;
		}
	}
}

#define FLASH_LMH_TRIGGER_LIMIT_MA 1000

static int qti_flash_switch_enable(struct flash_switch_data *snode)
{
	struct qti_flash_led *led = snode->led;
	int rc = 0, total_curr_ma = 0, i;
	enum flash_led_type type = FLASH_LED_TYPE_UNKNOWN;
	u8 led_en = 0;

	/* If symmetry enabled switch, then turn ON all its LEDs */
	if (snode->symmetry_en) {
		rc = qti_flash_led_symmetry_config(snode);
		if (rc < 0) {
			pr_err("Failed to configure switch symmetrically, rc=%d\n",
				rc);
			return rc;
		}
	}

	for (i = 0; i < led->num_fnodes; i++) {
		/*
		 * Do not turn ON flash/torch device if
		 * i. the device is not under this switch or
		 * ii. brightness is not configured for device under this switch
		 */
		if (!(snode->led_mask & BIT(led->fnode[i].id)) ||
			!led->fnode[i].configured)
			continue;

		/*
		 * For flash, LMH mitigation needs to be enabled
		 * if total current used is greater than or
		 * equal to 1A.
		 */

		type = led->fnode[i].type;
		if (type == FLASH_LED_TYPE_FLASH)
			total_curr_ma += led->fnode[i].user_current_ma;

		led_en |= (1 << led->fnode[i].id);
	}

	if (total_curr_ma >= FLASH_LMH_TRIGGER_LIMIT_MA) {
		rc = qti_flash_lmh_mitigation_config(led, true);
		if (rc < 0)
			return rc;

		/* Wait for lmh mitigation to take effect */
		udelay(500);
	} else if (led->trigger_lmh) {
		rc = qti_flash_lmh_mitigation_config(led, false);
		if (rc < 0)
			return rc;
	}

	return qti_flash_led_strobe(led, snode, snode->led_mask, led_en);
}

static int qti_flash_switch_disable(struct flash_switch_data *snode)
{
	struct qti_flash_led *led = snode->led;
	int rc = 0, i;
	u8 led_dis = 0;

	for (i = 0; i < led->num_fnodes; i++) {
		if (!(snode->led_mask & BIT(led->fnode[i].id)) ||
				!led->fnode[i].configured)
			continue;

		led_dis |= BIT(led->fnode[i].id);
	}

	rc = qti_flash_led_strobe(led, NULL, led_dis, ~led_dis);
	if (rc < 0) {
		pr_err("Failed to destrobe LEDs under with switch, rc=%d\n",
			rc);
		return rc;
	}

	for (i = 0; i < led->num_fnodes; i++) {
		/*
		 * Do not turn OFF flash/torch device if
		 * i. the device is not under this switch or
		 * ii. brightness is not configured for device under this switch
		 */
		if (!(snode->led_mask & BIT(led->fnode[i].id)) ||
				!led->fnode[i].configured)
			continue;

		rc = qti_flash_led_disable(&led->fnode[i]);
		if (rc < 0) {
			pr_err("Failed to disable LED%d\n",
				&led->fnode[i].id);
			break;
		}
	}

	snode->on_time_ms = 0;
	snode->off_time_ms = 0;

	return rc;
}

static void qti_flash_led_switch_brightness_set(
		struct led_classdev *led_cdev, enum led_brightness value)
{
	struct flash_switch_data *snode = NULL;
	int rc = 0;
	bool state = value > 0;

	snode = container_of(led_cdev, struct flash_switch_data, cdev);

	if (snode->enabled == state) {
		pr_debug("Switch  is already %s!\n",
			state ? "enabled" : "disabled");
		return;
	}

	if (state) {
		if (snode->on_time_ms) {
			pr_debug("On timer started with delay %d ms\n",
				snode->on_time_ms);
			hrtimer_start(&snode->on_timer,
					ms_to_ktime(snode->on_time_ms),
					HRTIMER_MODE_REL);
			return;
		}

		rc = qti_flash_switch_enable(snode);
	} else {
		rc = qti_flash_switch_disable(snode);
	}

	if (rc < 0)
		pr_err("Failed to %s switch, rc=%d\n",
			state ? "enable" : "disable", rc);
	else
		snode->enabled = state;
}

static struct led_classdev *trigger_to_lcdev(struct led_trigger *trig)
{
	struct led_classdev *led_cdev;

	read_lock(&trig->leddev_list_lock);
	list_for_each_entry(led_cdev, &trig->led_cdevs, trig_list) {
		if (!strcmp(led_cdev->default_trigger, trig->name)) {
			read_unlock(&trig->leddev_list_lock);
			return led_cdev;
		}
	}

	read_unlock(&trig->leddev_list_lock);
	return NULL;
}

static enum hrtimer_restart off_timer_function(struct hrtimer *timer)
{
	struct flash_switch_data *snode = container_of(timer,
			struct flash_switch_data, off_timer);
	int rc = 0;

	rc = qti_flash_switch_disable(snode);
	if (rc < 0)
		pr_err("Failed to disable flash LED switch %s, rc=%d\n",
			snode->cdev.name, rc);
	else
		snode->enabled = false;

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart on_timer_function(struct hrtimer *timer)
{
	struct flash_switch_data *snode = container_of(timer,
			struct flash_switch_data, on_timer);
	int rc = 0;

	rc = qti_flash_switch_enable(snode);
	if (rc < 0) {
		snode->enabled = false;
		pr_err("Failed to enable flash LED switch %s, rc=%d\n",
			snode->cdev.name, rc);
	} else {
		snode->enabled = true;
	}

	return HRTIMER_NORESTART;
}

int qti_flash_led_set_param(struct led_trigger *trig,
					struct flash_led_param param)
					{
	struct led_classdev *led_cdev = trigger_to_lcdev(trig);
	struct flash_switch_data *snode;

	if (!led_cdev) {
		pr_err("Invalid led_cdev in trigger %s\n", trig->name);
		return -EINVAL;
	}

	if (!param.on_time_ms && !param.off_time_ms) {
		pr_err("Invalid param, on_time/off_time cannot be 0\n");
		return -EINVAL;
	}

	snode = container_of(led_cdev, struct flash_switch_data, cdev);
	snode->on_time_ms = param.on_time_ms;
	snode->off_time_ms = param.off_time_ms;

	return 0;
}
EXPORT_SYMBOL(qti_flash_led_set_param);

#if IS_ENABLED(CONFIG_LEDS_QTI_FLASH) && IS_ENABLED(CONFIG_SENSORS_STK6D2X)
#define FLASH_LED_MULTI_STROBE_CTRL			0x67
#define FLASH_LED_MULTI_STROBE_SEL			BIT(0)

int qti_flash_led_set_strobe_sel(struct led_trigger *trig,
					int strobe_sel)
{
	struct led_classdev *led_cdev = trigger_to_lcdev(trig);
	struct flash_switch_data *snode;
	struct qti_flash_led *led;
	int rc = 0, i;

	if (!led_cdev) {
		pr_err("Invalid led_cdev in trigger %s\n", trig->name);
		return -EINVAL;
	}

	snode = container_of(led_cdev, struct flash_switch_data, cdev);
	led = snode->led;

	for (i = 0; i < led->num_fnodes; i++) {
		led->fnode[i].strobe_sel = strobe_sel; //0:SW_STROBE, 1:HW_STROBE
		rc = qti_flash_led_masked_write(led,
			FLASH_LED_STROBE_CTRL(led->fnode[i].id), FLASH_LED_HW_SW_STROBE_SEL, led->fnode[i].strobe_sel << FLASH_LED_STROBE_SEL_SHIFT);
		if (rc < 0)
			return rc;
	}
	qti_flash_led_masked_write(led,
		FLASH_LED_MULTI_STROBE_CTRL, FLASH_LED_MULTI_STROBE_SEL, strobe_sel? 0 : 1); 

	return 0;
}
EXPORT_SYMBOL(qti_flash_led_set_strobe_sel);
#endif

static int is_usb_psy_available(struct qti_flash_led *led)
{
	if (!led->usb_psy) {
		led->usb_psy = power_supply_get_by_name("usb");
		if (!led->usb_psy) {
			pr_err_ratelimited("Couldn't get usb_psy\n");
			return -ENODEV;
		}
	}

	return 0;
}

#define UCONV			1000000LL
#define MCONV			1000LL
#define VIN_FLASH_MIN_UV	3300000LL
#define BOB_EFFICIENCY	900LL
#define VFLASH_DIP_MARGIN_UV	50000
#define VOLTAGE_HDRM_DEFAULT_MV		400
#define VDIP_THRESH_DEFAULT_UV		2800000LL
#define CHGBST_EFFICIENCY		800LL
#define CHGBST_FLASH_VDIP_MARGIN	10000
#define VIN_FLASH_UV			5000000
#define VIN_FLASH_RANGE_1		4250000
#define VIN_FLASH_RANGE_2		4500000
#define VIN_FLASH_RANGE_3		4750000
#define OCV_RANGE_1			3800000
#define OCV_RANGE_2			4100000
#define OCV_RANGE_3			4350000
#define BHARGER_FLASH_LED_MAX_TOTAL_CURRENT_MA		1000
static int qti_flash_led_calc_bharger_max_current(struct qti_flash_led *led,
							int *max_current)
{
	union power_supply_propval pval = {0, };
	int ocv_uv, ibat_now, flash_led_max_total_curr_ma, rc;
	int rbatt_uohm = 0, usb_present, otg_enable;
	int64_t ibat_flash_ua, avail_flash_ua, avail_flash_power_fw;
	int64_t ibat_safe_ua, vin_flash_uv = 0, vph_flash_uv, vph_flash_vdip;

	if (led->data->pmic_type != PM2250)
		return 0;

	rc = is_usb_psy_available(led);
	if (rc < 0)
		return rc;

	rc = power_supply_get_property(led->usb_psy, POWER_SUPPLY_PROP_SCOPE,
					&pval);
	if (rc < 0) {
		pr_err("usb psy does not support usb present, rc=%d\n", rc);
		return rc;
	}
	otg_enable = pval.intval;

	/* RESISTANCE = esr_uohm + rslow_uohm */
	rc = qti_flash_iio_getprop(led, RBATT, &rbatt_uohm);
	if (rc < 0) {
		pr_err("bms psy does not support resistance, rc=%d\n", rc);
		return rc;
	}

	/* If no battery is connected, return max possible flash current */
	if (!rbatt_uohm) {
		*max_current = BHARGER_FLASH_LED_MAX_TOTAL_CURRENT_MA;
		return 0;
	}

	rc = qti_flash_iio_getprop(led, OCV, &ocv_uv);
	if (rc < 0) {
		pr_err("bms psy does not support OCV, rc=%d\n", rc);
		return rc;
	}

	rc = qti_flash_iio_getprop(led, IBAT, &ibat_now);
	if (rc < 0) {
		pr_err("bms psy does not support current, rc=%d\n", rc);
		return rc;
	}

	rc = power_supply_get_property(led->usb_psy, POWER_SUPPLY_PROP_PRESENT,
							&pval);
	if (rc < 0) {
		pr_err("usb psy does not support usb present, rc=%d\n", rc);
		return rc;
	}
	usb_present = pval.intval;

	rbatt_uohm += FLASH_LED_RPARA_DEFAULT_UOHM;

	vph_flash_vdip = VPH_DROOP_THRESH_VAL_UV + CHGBST_FLASH_VDIP_MARGIN;

	/*
	 * Calculate the maximum current that can pulled out of the battery
	 * before the battery voltage dips below a safe threshold.
	 */
	ibat_safe_ua = div_s64((ocv_uv - vph_flash_vdip) * UCONV,
				rbatt_uohm);

	if (ibat_safe_ua <= led->ibatt_ocp_threshold_ua) {
		/*
		 * If the calculated current is below the OCP threshold, then
		 * use it as the possible flash current.
		 */
		ibat_flash_ua = ibat_safe_ua - ibat_now;
		vph_flash_uv = vph_flash_vdip;
	} else {
		/*
		 * If the calculated current is above the OCP threshold, then
		 * use the ocp threshold instead.
		 *
		 * Any higher current will be tripping the battery OCP.
		 */
		ibat_flash_ua = led->ibatt_ocp_threshold_ua - ibat_now;
		vph_flash_uv = ocv_uv - div64_s64((int64_t)rbatt_uohm
				* led->ibatt_ocp_threshold_ua, UCONV);
	}

	/* when USB is present or OTG is enabled, VIN_FLASH is always at 5V */
	if (usb_present || (otg_enable == POWER_SUPPLY_SCOPE_SYSTEM))
		vin_flash_uv = VIN_FLASH_UV;
	else if (ocv_uv <= OCV_RANGE_1)
		vin_flash_uv = VIN_FLASH_RANGE_1;
	else if (ocv_uv  > OCV_RANGE_1 && ocv_uv <= OCV_RANGE_2)
		vin_flash_uv = VIN_FLASH_RANGE_2;
	else if (ocv_uv > OCV_RANGE_2 && ocv_uv <= OCV_RANGE_3)
		vin_flash_uv = VIN_FLASH_RANGE_3;

	/* Calculate the available power for the flash module. */
	avail_flash_power_fw = CHGBST_EFFICIENCY * vph_flash_uv * ibat_flash_ua;
	/*
	 * Calculate the available amount of current the flash module can draw
	 * before collapsing the battery. (available power/ flash input voltage)
	 */
	avail_flash_ua = div64_s64(avail_flash_power_fw, vin_flash_uv * MCONV);

	flash_led_max_total_curr_ma = BHARGER_FLASH_LED_MAX_TOTAL_CURRENT_MA;
	*max_current = min(flash_led_max_total_curr_ma,
			(int)(div64_s64(avail_flash_ua, MCONV)));

	pr_debug("avail_iflash=%lld, ocv=%d, ibat=%d, rbatt=%d,max_current=%lld, usb_present=%d, otg_enable = %d\n",
		avail_flash_ua, ocv_uv, ibat_now, rbatt_uohm,
		(*max_current * MCONV), usb_present, otg_enable);

	return 0;
}

static int qti_flash_led_get_voltage_headroom(
					struct qti_flash_led *led)
{
	static const struct flash_current_headroom *hdrm_map;
	int i, j, voltage_hdrm_mv = 0, voltage_hdrm_max = 0;
	u32 map_size = 0;

	if (led->subtype == FLASH_LED_4_CHAN) {
		hdrm_map = pm8350c_map;
		map_size = ARRAY_SIZE(pm8350c_map);
	}

	for (i = 0; i < led->num_fnodes; i++) {
		if (!led->fnode[i].configured)
			continue;

		voltage_hdrm_mv = VOLTAGE_HDRM_DEFAULT_MV;

		for (j = 0; j < map_size; j++) {
			if (led->fnode[i].current_ma <= hdrm_map[j].current_ma)
				voltage_hdrm_mv = hdrm_map[j].headroom_mv;
		}

		voltage_hdrm_max = max(voltage_hdrm_max, voltage_hdrm_mv);
	}

	if (!voltage_hdrm_max)
		voltage_hdrm_max = VOLTAGE_HDRM_DEFAULT_MV;

	return voltage_hdrm_max;
}

static int qti_flash_led_calc_max_avail_current(
			struct qti_flash_led *led,
			int *max_current_ma)
{
	int rc;
	int rbatt_uohm, ocv_uv, ibatt_now_ua, voltage_hdrm_mv;
	int64_t ibatt_safe_ua, i_flash_ua, i_avail_ua, vflash_vdip,
		vph_flash_uv, vin_flash_uv, p_flash_fw;
	union power_supply_propval prop = {};

//Temp code added because QTI Flash has dependency with QTI battery charger. Kernel panic while taking capture with flash.
#if 1 
	*max_current_ma = MAX_FLASH_CURRENT_MA;
	return 0;
#endif

	rc = qti_battery_charger_get_prop("battery", BATTERY_RESISTANCE,
						&rbatt_uohm);
	if (rc < 0) {
		pr_err("Failed to get battery resistance, rc=%d\n",
				rc);
		return rc;
	}

	if (!rbatt_uohm) {
		*max_current_ma = MAX_FLASH_CURRENT_MA;
		led->debug_board_present = true;
		return 0;
	}

	if (!led->batt_psy)
		led->batt_psy = power_supply_get_by_name("battery");

	if (!led->batt_psy) {
		pr_err("Failed to get battery power supply, rc=%d\n", rc);
		return -EINVAL;
	}

	rc = power_supply_get_property(led->batt_psy,
		POWER_SUPPLY_PROP_VOLTAGE_OCV, &prop);
	if (rc < 0) {
		pr_err("Failed to get battery OCV, rc=%d\n", rc);
		return rc;
	}
	ocv_uv = prop.intval;

	rc = power_supply_get_property(led->batt_psy,
		POWER_SUPPLY_PROP_CURRENT_NOW, &prop);
	if (rc < 0) {
		pr_err("Failed to get battery current, rc=%d\n", rc);
		return rc;
	}

	/* Battery power supply returns -ve value for discharging */
	ibatt_now_ua = -(prop.intval);

	voltage_hdrm_mv = qti_flash_led_get_voltage_headroom(led);
	vflash_vdip = VDIP_THRESH_DEFAULT_UV;

	ibatt_safe_ua = DIV_ROUND_CLOSEST((ocv_uv -
				(vflash_vdip + VFLASH_DIP_MARGIN_UV)) * UCONV,
				rbatt_uohm);

	if (ibatt_safe_ua < IBATT_OCP_THRESH_DEFAULT_UA) {
		i_flash_ua = ibatt_safe_ua - ibatt_now_ua;
		vph_flash_uv = vflash_vdip + VFLASH_DIP_MARGIN_UV;
	} else {
		i_flash_ua = IBATT_OCP_THRESH_DEFAULT_UA - ibatt_now_ua;
		vph_flash_uv = ocv_uv - DIV_ROUND_CLOSEST((int64_t)rbatt_uohm
				* IBATT_OCP_THRESH_DEFAULT_UA, UCONV);
	}

	vin_flash_uv = max(VLED_MAX_DEFAULT_UV +
				(voltage_hdrm_mv * MCONV), VIN_FLASH_MIN_UV);

	p_flash_fw = BOB_EFFICIENCY * vph_flash_uv * i_flash_ua;
	i_avail_ua = DIV_ROUND_CLOSEST(p_flash_fw, (vin_flash_uv * MCONV));

	*max_current_ma = min(MAX_FLASH_CURRENT_MA,
				(int)(DIV_ROUND_CLOSEST(i_avail_ua, MCONV)));

	pr_debug("rbatt_uohm=%d ocv_uv=%d ibatt_now_ua=%d i_avail_ua=%lld\n",
			rbatt_uohm, ocv_uv, ibatt_now_ua, i_avail_ua);

	return 0;
}

static int qti_flash_led_calc_thermal_current(
			struct qti_flash_led *led,
			int *thermal_current_limit)
{
	int rc;
	u8 otst_status, thrsh_min = 0;

	if (led->subtype == FLASH_LED_4_CHAN) {
		thrsh_min = FLASH_LED_V1_OTST1_THRSH_MIN;

		if (led->revision == FLASH_LED_REVISION_2P0)
			thrsh_min = FLASH_LED_V2_OTST1_THRSH_MIN;
	}

	rc = qti_flash_led_masked_write(led,
			FLASH_LED_THERMAL_OTST1_CFG1,
			FLASH_LED_THERMAL_THRSH_MASK,
			thrsh_min);
	if (rc < 0)
		return rc;

	rc = qti_flash_led_masked_write(led,
			FLASH_LED_THERMAL_OTST2_CFG1,
			FLASH_LED_THERMAL_THRSH_MASK,
			FLASH_LED_OTST2_THRSH_MIN);
	if (rc < 0)
		return rc;

	/* Check THERMAL OTST status */
	rc = qti_flash_led_read(led, FLASH_LED_STATUS2, &otst_status, 1);
	if (rc < 0)
		return rc;

	if (otst_status & FLASH_LED_OTST1_STATUS)
		*thermal_current_limit = led->thermal_derate_current[OTST1_IDX];
	else if (otst_status & FLASH_LED_OTST2_STATUS)
		*thermal_current_limit = led->thermal_derate_current[OTST2_IDX];

	pr_debug("thermal_current_limit=%d\n", *thermal_current_limit);

	return 0;
}

static int qti_flash_led_get_max_avail_current(
				struct qti_flash_led *led, int *max_current_ma)
{
	int thermal_current_limit = 0, rc;

	if (led->secure_vm) {
		led->max_current = MAX_FLASH_CURRENT_MA;
		return 0;
	}

	rc = qti_flash_led_calc_max_avail_current(led, max_current_ma);
	if (rc < 0) {
		pr_err("Failed to calculate max avail current, rc=%d\n", rc);
		return rc;
	}

	rc = qti_flash_led_calc_thermal_current(led,
			&thermal_current_limit);
	if (rc < 0) {
		pr_err("Failed to calculate thermal current limit, rc=%d\n",
			rc);
		return rc;
	}

	if (thermal_current_limit)
		*max_current_ma = min(*max_current_ma, thermal_current_limit);

	led->max_current = *max_current_ma;

	pr_debug("max_current_ma=%d\n", *max_current_ma);

	return 0;
}


static int qti_flash_led_regulator_control(struct led_classdev *led_cdev,
					int options)
{
	struct flash_switch_data *snode;
	int rc = 0, ret;
	struct led_classdev_flash *fdev = NULL;
	struct qti_flash_led *led;
	struct flash_node_data *fnode;

	fdev = container_of(led_cdev, struct led_classdev_flash, led_cdev);
	fnode = container_of(fdev, struct flash_node_data, fdev);
	led = fnode->led;

	snode = container_of(led_cdev, struct flash_switch_data, cdev);

	if (snode->led->data->pmic_type != PM2250)
		return 0;

	if (options & ENABLE_REGULATOR) {
		ret = 1;
		rc = qti_flash_iio_setprop(led, F_ACTIVE, ret);
		if (rc < 0) {
			pr_err("Failed to set FLASH_ACTIVE on charger rc=%d\n",
							rc);
			return rc;
		}

		pr_debug("FLASH_ACTIVE = 1\n");
	} else if (options & DISABLE_REGULATOR) {
		ret = 0;
		rc = qti_flash_iio_setprop(led, F_ACTIVE, ret);
		if (rc < 0) {
			pr_err("Failed to set FLASH_ACTIVE on charger rc=%d\n",
							rc);
			return rc;
		}

		pr_debug("FLASH_ACTIVE = 0\n");
	}

	return 0;
}

int qti_flash_led_prepare(struct led_trigger *trig, int options,
				int *max_current)
{
	struct led_classdev *led_cdev;
	struct flash_switch_data *snode;
	int rc = 0;

	if (!trig) {
		pr_err("Invalid led_trigger\n");
		return -EINVAL;
	}

	led_cdev = trigger_to_lcdev(trig);
	if (!led_cdev) {
		pr_err("Invalid led_cdev in trigger %s\n", trig->name);
		return -ENODEV;
	}

	snode = container_of(led_cdev, struct flash_switch_data, cdev);

	if (options & QUERY_MAX_AVAIL_CURRENT) {
		if (!max_current) {
			pr_err("Invalid max_current pointer\n");
			return -EINVAL;
		}

		if (snode->led->data->pmic_type == PM2250) {
			rc = qti_flash_led_calc_bharger_max_current(snode->led,
					max_current);
			if (rc < 0) {
				pr_err("Failed to query max avail current, rc=%d\n",
					rc);
				*max_current = snode->led->max_current;
				return rc;
			}
		} else {
			rc = qti_flash_led_get_max_avail_current(snode->led,
						max_current);
			if (rc < 0) {
				pr_err("Failed to query max avail current, rc=%d\n",
					rc);
				return rc;
			}
		}
	}

	rc = qti_flash_led_regulator_control(led_cdev, options);
	if (rc < 0)
		pr_err("Failed to set flash control options\n");

	return rc;
}
EXPORT_SYMBOL(qti_flash_led_prepare);

static ssize_t qti_flash_led_prepare_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int rc, options;
	u32 val;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	if (val != 0 && val != 1)
		return count;

	options = val ? ENABLE_REGULATOR : DISABLE_REGULATOR;

	rc = qti_flash_led_regulator_control(led_cdev, options);
	if (rc < 0) {
		pr_err("failed to query led regulator\n");
		return rc;
	}

	return count;
}

static ssize_t qti_flash_led_max_current_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct flash_switch_data *snode;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	int rc;

	snode = container_of(led_cdev, struct flash_switch_data, cdev);
	if (snode->led->data->pmic_type == PM2250) {
		rc = qti_flash_led_calc_bharger_max_current(snode->led,
					&snode->led->max_current);
	} else {
		rc = qti_flash_led_get_max_avail_current(snode->led,
					&snode->led->max_current);
	}
	if (rc < 0) {
		pr_err("Failed to get max current, rc=%d\n", rc);
		return -EINVAL;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", snode->led->max_current);
}

static ssize_t qti_flash_on_time_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct flash_switch_data *snode;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	u64 val;

	rc = kstrtou64(buf, 0, &val);
	if (rc < 0)
		return rc;

	if (!val)
		return -EINVAL;

	snode = container_of(led_cdev, struct flash_switch_data, cdev);
	snode->on_time_ms = val;

	return count;
}

static ssize_t qti_flash_on_time_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct flash_switch_data *snode;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	snode = container_of(led_cdev, struct flash_switch_data, cdev);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", snode->on_time_ms * 1000);
}

static ssize_t qti_flash_off_time_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct flash_switch_data *snode;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	u64 val;

	rc = kstrtou64(buf, 0, &val);
	if (rc < 0)
		return rc;

	val = min_t(u64, val, SAFETY_TIMER_MAX_TIMEOUT_MS);

	snode = container_of(led_cdev, struct flash_switch_data, cdev);
	snode->off_time_ms = val;

	return count;
}

static ssize_t qti_flash_off_time_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct flash_switch_data *snode;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	snode = container_of(led_cdev, struct flash_switch_data, cdev);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", snode->off_time_ms * 1000);
}

static struct device_attribute qti_flash_led_attrs[] = {
	__ATTR(max_current, 0400, qti_flash_led_max_current_show, NULL),
	__ATTR(on_time, 0600, qti_flash_on_time_show,
		qti_flash_on_time_store),
	__ATTR(off_time, 0600, qti_flash_off_time_show,
		qti_flash_off_time_store),
	__ATTR(enable, 0664, NULL, qti_flash_led_prepare_store),
};

static int qti_flash_brightness_set_blocking(
		struct led_classdev *led_cdev, enum led_brightness value)
{
	qti_flash_led_brightness_set(led_cdev, value);

	return 0;
}

static int qti_flash_brightness_set(
		struct led_classdev_flash *fdev, u32 brightness)
{
	qti_flash_led_brightness_set(&fdev->led_cdev, brightness);

	return 0;
}

static int qti_flash_brightness_get(
		struct led_classdev_flash *fdev, u32 *brightness)
{
	*brightness = qti_flash_led_brightness_get(&fdev->led_cdev);

	return 0;
}

static int qti_flash_strobe_set(struct led_classdev_flash *fdev,
				bool state)
{
	struct flash_node_data *fnode;
	int rc;
	u8 mask, value;

	fnode = container_of(fdev, struct flash_node_data, fdev);

	if (fnode->enabled == state)
		return 0;

	if (state && !fnode->configured)
		return -EINVAL;

	if (!fnode->duration) {
		pr_debug("Safety time duration is zero, strobe not set\n");
		return -EINVAL;
	}

	mask = FLASH_LED_ENABLE(fnode->id);
	value = state ? FLASH_LED_ENABLE(fnode->id) : 0;

	rc = qti_flash_led_strobe(fnode->led, NULL, mask, value);
	if (rc < 0) {
		pr_err("Failed to %s LED, rc=%d\n",
			state ? "strobe" : "desrobe", rc);
		return rc;
	}
	fnode->enabled = state;

	if (!state) {
		rc = qti_flash_led_disable(fnode);
		if (rc < 0)
			pr_err("Failed to disable LED %u\n", fnode->id);
	}

	return rc;
}

static int qti_flash_strobe_get(struct led_classdev_flash *fdev,
				bool *state)
{
	struct flash_node_data *fnode = container_of(fdev,
			struct flash_node_data, fdev);

	*state = fnode->enabled;

	return 0;
}

static int qti_flash_timeout_set(struct led_classdev_flash *fdev,
				u32 timeout)
{
	struct qti_flash_led *led;
	struct flash_node_data *fnode;
	int rc = 0;
	u8 val;

	fnode = container_of(fdev, struct flash_node_data, fdev);
	led = fnode->led;

	if (!timeout) {
		fnode->duration = 0;
		return 0;
	}

	timeout = timeout / 1000;

	rc = timeout_to_code(timeout);
	if (rc < 0)
		return rc;
	fnode->duration = rc;
	val = fnode->duration | FLASH_LED_SAFETY_TIMER_EN;
	rc = qti_flash_led_write(led,
			FLASH_LED_SAFETY_TIMER(fnode->id), &val, 1);

	return rc;
}

static const struct led_flash_ops flash_ops = {
	.flash_brightness_set	= qti_flash_brightness_set,
	.flash_brightness_get	= qti_flash_brightness_get,
	.strobe_set			= qti_flash_strobe_set,
	.strobe_get			= qti_flash_strobe_get,
	.timeout_set			= qti_flash_timeout_set,
};

static int qti_flash_led_setup(struct qti_flash_led *led)
{
	int rc = 0, i, addr_offset;
	u8 val, mask;

	rc = qti_flash_led_read(led, FLASH_LED_REVISION1, &val, 1);
	if (rc < 0)
		return rc;

	led->revision = val;

	rc = qti_flash_led_read(led, FLASH_LED_PERIPH_SUBTYPE, &val, 1);
	if (rc < 0)
		return rc;

	led->subtype = val;

	for (i = 0; i < led->num_fnodes; i++) {
		addr_offset = led->fnode[i].id;
		rc = qti_flash_led_masked_write(led,
			FLASH_LED_SAFETY_TIMER(addr_offset),
			FLASH_LED_SAFETY_TIMER_EN_MASK, 0);
		if (rc < 0)
			return rc;

		val = (led->fnode[i].strobe_config <<
				FLASH_LED_STROBE_CFG_SHIFT) |
				(led->fnode[i].strobe_sel <<
				FLASH_LED_STROBE_SEL_SHIFT);
		mask = FLASH_LED_STROBE_CFG_MASK | FLASH_LED_HW_SW_STROBE_SEL;
		rc = qti_flash_led_masked_write(led,
			FLASH_LED_STROBE_CTRL(addr_offset), mask, val);
		if (rc < 0)
			return rc;
	}

	led->max_current = MAX_FLASH_CURRENT_MA;

	led->thermal_derate_current[OTST1_IDX] = OTST1_CURR_LIM_MA;
	led->thermal_derate_current[OTST2_IDX] = OTST2_CURR_LIM_MA;

	return rc;
}

static irqreturn_t qti_flash_led_irq_handler(int irq, void *_led)
{
	struct qti_flash_led *led = _led;
	int rc;
	u8 irq_status, led_status1, led_status2;

	rc = qti_flash_led_read(led, FLASH_INT_RT_STS, &irq_status, 1);
	if (rc < 0)
		goto exit;

	if (irq == led->led_fault_irq) {
		if (irq_status & FLASH_LED_FAULT_RT_STS)
			pr_debug("Led fault open/short/vreg_not_ready detected\n");
	} else if (irq == led->all_ramp_down_done_irq) {
		if (irq_status & FLASH_LED_ALL_RAMP_DN_DONE_RT_STS)
			pr_debug("All LED channels ramp down done detected\n");
	} else if (irq == led->all_ramp_up_done_irq) {
		if (irq_status & FLASH_LED_ALL_RAMP_UP_DONE_RT_STS)
			pr_debug("All LED channels ramp up done detected\n");
	}

	rc = qti_flash_led_read(led, FLASH_LED_STATUS1, &led_status1, 1);
	if (rc < 0)
		goto exit;

	if (led_status1)
		pr_debug("LED channel open/short fault detected\n");

	rc = qti_flash_led_read(led, FLASH_LED_STATUS2, &led_status2, 1);
	if (rc < 0)
		goto exit;

	if (led_status2 & FLASH_LED_VPH_PWR_LOW)
		pr_debug("LED vph_droop fault detected!\n");

	pr_debug("LED irq handled, irq_status=%02x led_status1=%02x led_status2=%02x\n",
			irq_status, led_status1, led_status2);

exit:
	return IRQ_HANDLED;
}

static int qti_flash_led_register_interrupts(struct qti_flash_led *led)
{
	int rc;

	if (led->all_ramp_up_done_irq >= 0) {
		rc = devm_request_threaded_irq(&led->pdev->dev,
			led->all_ramp_up_done_irq, NULL, qti_flash_led_irq_handler,
			IRQF_ONESHOT, "flash_all_ramp_up", led);
		if (rc < 0) {
			pr_err("Failed to request all_ramp_up_done(%d) IRQ(err:%d)\n",
				led->all_ramp_up_done_irq, rc);
			return rc;
		}
	}

	if (led->all_ramp_down_done_irq >= 0) {
		rc = devm_request_threaded_irq(&led->pdev->dev,
			led->all_ramp_down_done_irq, NULL, qti_flash_led_irq_handler,
			IRQF_ONESHOT, "flash_all_ramp_down", led);
		if (rc < 0) {
			pr_err("Failed to request all_ramp_down_done(%d) IRQ(err:%d)\n",
				led->all_ramp_down_done_irq,
				rc);
			return rc;
		}
	}

	if (led->led_fault_irq >= 0) {
		rc = devm_request_threaded_irq(&led->pdev->dev,
			led->led_fault_irq, NULL, qti_flash_led_irq_handler,
			IRQF_ONESHOT, "flash_fault", led);
		if (rc < 0) {
			pr_err("Failed to request led_fault(%d) IRQ(err:%d)\n",
				led->led_fault_irq, rc);
			return rc;
		}
	}

	return 0;
}

static int register_switch_device(struct qti_flash_led *led,
		struct flash_switch_data *snode, struct device_node *node)
{
	int rc, i;

	rc = of_property_read_string(node, "qcom,led-name",
				&snode->cdev.name);
	if (rc < 0) {
		pr_err("Failed to read switch node name, rc=%d\n", rc);
		return rc;
	}

	rc = of_property_read_string(node, "qcom,default-led-trigger",
					&snode->cdev.default_trigger);
	if (rc < 0) {
		pr_err("Failed to read trigger name, rc=%d\n", rc);
		return rc;
	}

	rc = of_property_read_u32(node, "qcom,led-mask", &snode->led_mask);
	if (rc < 0) {
		pr_err("Failed to read led mask rc=%d\n", rc);
		return rc;
	}
	if (!snode->led_mask || snode->led_mask > LED_MASK_ALL(led)) {
		pr_err("led-mask %#x invalid\n", snode->led_mask);
		return -EINVAL;
	} else if (snode->led_mask < LED_MASK_ALL(led)) {
		led->non_all_mask_switch_present = true;
	}

	snode->symmetry_en = of_property_read_bool(node, "qcom,symmetry-en");

	snode->on_time_ms = 0;
	snode->off_time_ms = 0;
	hrtimer_init(&snode->on_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&snode->off_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	snode->on_timer.function = on_timer_function;
	snode->off_timer.function = off_timer_function;

	snode->led = led;
	snode->cdev.brightness_set = qti_flash_led_switch_brightness_set;
	snode->cdev.brightness_get = qti_flash_led_brightness_get;

	rc = devm_led_classdev_register(&led->pdev->dev, &snode->cdev);
	if (rc < 0) {
		pr_err("Failed to register led switch device:%s\n",
			snode->cdev.name);
		return rc;
	}

	for (i = 0; i < ARRAY_SIZE(qti_flash_led_attrs); i++) {
		rc = sysfs_create_file(&snode->cdev.dev->kobj,
				&qti_flash_led_attrs[i].attr);
		if (rc < 0) {
			pr_err("Failed to create sysfs attrs, rc=%d\n", rc);
			goto sysfs_fail;
		}
	}

	return 0;

sysfs_fail:
	while (i >= 0)
		sysfs_remove_file(&snode->cdev.dev->kobj,
			&qti_flash_led_attrs[i--].attr);
	return rc;
}

static int register_flash_device(struct qti_flash_led *led,
			struct flash_node_data *fnode, struct device_node *node)
{
	struct led_flash_setting *setting;
	const char *temp_string;
	int rc;
	u32 val, default_curr_ma, duration;

	rc = of_property_read_string(node, "qcom,led-name",
					&fnode->fdev.led_cdev.name);
	if (rc < 0) {
		pr_err("Failed to read flash LED names\n");
		return rc;
	}

	rc = of_property_read_string(node, "label", &temp_string);
	if (rc < 0) {
		pr_err("Failed to read flash LED label\n");
		return rc;
	}

	if (!strcmp(temp_string, "flash")) {
		fnode->type = FLASH_LED_TYPE_FLASH;
	} else if (!strcmp(temp_string, "torch")) {
		fnode->type = FLASH_LED_TYPE_TORCH;
	} else if (!strcmp(temp_string, "indicator")) {
		fnode->type = FLASH_LED_TYPE_INDICATOR;
	} else {
		pr_err("Incorrect flash LED type %s\n", temp_string);
		return rc;
	}

	rc = of_property_read_u32(node, "qcom,id", &val);
	if (rc < 0) {
		pr_err("Failed to read flash LED ID\n");
		return rc;
	}
	fnode->id = (u8)val;

	rc = of_property_read_string(node, "qcom,default-led-trigger",
				&fnode->fdev.led_cdev.default_trigger);
	if ((rc < 0) && (fnode->type != FLASH_LED_TYPE_INDICATOR)) {
		pr_err("Failed to read trigger name\n");
		return rc;
	}

	rc = of_property_read_u32(node, "qcom,ires-ua", &val);
	if (rc < 0) {
		pr_err("Failed to read current resolution, rc=%d\n", rc);
		return rc;
	} else if (!rc) {
		rc = get_ires_idx(val);
		if (rc < 0) {
			pr_err("Incorrect ires-ua configured, ires-ua=%u\n",
				val);
			return rc;
		}
		fnode->default_ires_ua = fnode->ires_ua = val;
		fnode->updated_ires_idx = fnode->ires_idx = rc;
	}

	rc = of_property_read_u32(node, "qcom,max-current-ma", &val);
	if (rc < 0) {
		pr_err("Failed to read max current, rc=%d\n", rc);
		return rc;
	}

	if (fnode->type == FLASH_LED_TYPE_FLASH &&
		(val > IRES_12P5_MAX_CURR_MA)) {
		pr_err("Incorrect max-current-ma for flash %u\n",
				val);
		return -EINVAL;
	}

	if (fnode->type == FLASH_LED_TYPE_TORCH &&
		(val > TORCH_MAX_CURR_MA)) {
		pr_err("Incorrect max-current-ma for torch %u\n",
				val);
		return -EINVAL;
	}

	if (fnode->type == FLASH_LED_TYPE_INDICATOR &&
			(val > INDICATOR_MAX_CURR_MA)) {
		pr_err("Incorrect max-current-ma for indicator %u\n", val);
		return -EINVAL;
	}

	fnode->max_current = val;
	fnode->fdev.led_cdev.max_brightness = val;

	duration = SAFETY_TIMER_DEFAULT_TIMEOUT_MS;
	rc = of_property_read_u32(node, "qcom,duration-ms", &val);
	if (!rc && (val >= SAFETY_TIMER_MIN_TIMEOUT_MS &&
			val <= SAFETY_TIMER_MAX_TIMEOUT_MS))
		duration = val;

	rc = timeout_to_code(duration);
	if (rc < 0) {
		pr_err("Incorrect timeout configured %u\n", duration);
		return rc;
	}
	fnode->duration = rc;

	fnode->strobe_sel = SW_STROBE;
	rc = of_property_read_u32(node, "qcom,strobe-sel", &val);
	if (!rc)
		fnode->strobe_sel = (u8)val;

	if (fnode->type == FLASH_LED_TYPE_INDICATOR)
		fnode->strobe_sel = SW_STROBE;

	if (fnode->strobe_sel == HW_STROBE) {
		rc = of_property_read_u32(node, "qcom,strobe-config", &val);
		if (!rc) {
			fnode->strobe_config = (u8)val;
		} else {
			pr_err("Failed to read qcom,strobe-config property\n");
			return rc;
		}

	}

	fnode->led = led;
	fnode->fdev.led_cdev.brightness_set = qti_flash_led_brightness_set;
	fnode->fdev.led_cdev.brightness_get = qti_flash_led_brightness_get;
	fnode->enabled = false;
	fnode->configured = false;
	if (fnode->type != FLASH_LED_TYPE_INDICATOR)
		fnode->fdev.ops = &flash_ops;

	if (fnode->type == FLASH_LED_TYPE_FLASH) {
		fnode->fdev.led_cdev.flags = LED_DEV_CAP_FLASH;
		fnode->fdev.led_cdev.brightness_set_blocking =
				qti_flash_brightness_set_blocking;
	}

	default_curr_ma = DIV_ROUND_CLOSEST(fnode->ires_ua, 1000);
	setting = &fnode->fdev.brightness;
	setting->min = 0;
	setting->max = fnode->max_current;
	setting->step = 1;
	setting->val = default_curr_ma;

	setting = &fnode->fdev.timeout;
	setting->min = 0;
	setting->max = SAFETY_TIMER_MAX_TIMEOUT_MS * 1000;
	setting->step = SAFETY_TIMER_STEP_SIZE * 1000;
	setting->val = SAFETY_TIMER_DEFAULT_TIMEOUT_MS * 1000;

	rc = led_classdev_flash_register(&led->pdev->dev, &fnode->fdev);
	if (rc < 0) {
		pr_err("Failed to register flash led device:%s\n",
			fnode->fdev.led_cdev.name);
		return rc;
	}

	return 0;
}

static int qti_flash_led_register_device(struct qti_flash_led *led,
				struct device_node *node)
{
	struct device_node *temp;
	char buffer[20];
	const char *label;
	int rc, i = 0, j = 0;
	u32 val;
	bool need_snode = false;

	rc = of_property_read_u32(node, "reg", &val);
	if (rc < 0) {
		pr_err("Failed to find reg in node %s, rc = %d\n",
			node->full_name, rc);
		return rc;
	}
	led->base = val;

	led->hw_strobe_gpio = devm_kcalloc(&led->pdev->dev,
			led->data->max_channels, sizeof(u32), GFP_KERNEL);
	if (!led->hw_strobe_gpio)
		return -ENOMEM;

	for (i = 0; i < led->data->max_channels; i++) {

		led->hw_strobe_gpio[i] = -EINVAL;

		rc = of_get_named_gpio(node, "hw-strobe-gpios", i);
		if (rc < 0) {
			pr_debug("Failed to get hw strobe gpio, rc = %d\n", rc);
			continue;
		}

		if (!gpio_is_valid(rc)) {
			pr_err("Error, Invalid gpio specified\n");
			return -EINVAL;
		}
		led->hw_strobe_gpio[i] = rc;

		scnprintf(buffer, sizeof(buffer), "hw_strobe_gpio%d", i);
		rc = devm_gpio_request_one(&led->pdev->dev,
				led->hw_strobe_gpio[i], GPIOF_DIR_OUT,
				buffer);
		if (rc < 0) {
			pr_err("Failed to acquire gpio rc = %d\n", rc);
			return rc;
		}

		gpio_direction_output(led->hw_strobe_gpio[i], 0);

	}

	led->secure_vm = of_property_read_bool(node, "qcom,secure-vm");

	led->all_ramp_up_done_irq = of_irq_get_byname(node,
			"all-ramp-up-done-irq");
	if (led->all_ramp_up_done_irq < 0)
		pr_debug("all-ramp-up-done-irq not used\n");

	led->all_ramp_down_done_irq = of_irq_get_byname(node,
			"all-ramp-down-done-irq");
	if (led->all_ramp_down_done_irq < 0)
		pr_debug("all-ramp-down-done-irq not used\n");

	led->led_fault_irq = of_irq_get_byname(node,
			"led-fault-irq");
	if (led->led_fault_irq < 0)
		pr_debug("led-fault-irq not used\n");

	for_each_available_child_of_node(node, temp) {
		rc = of_property_read_string(temp, "label", &label);
		if (rc < 0) {
			pr_err("Failed to parse label, rc=%d\n", rc);
			of_node_put(temp);
			return rc;
		}

		if (!strcmp("flash", label) || !strcmp("torch", label)) {
			need_snode = true;
			led->num_fnodes++;
		} else if (!strcmp("indicator", label)) {
			led->num_fnodes++;
		} else if (!strcmp("switch", label)) {
			led->num_snodes++;
		} else {
			pr_err("Invalid label for led node label=%s\n",
					label);
			of_node_put(temp);
			return -EINVAL;
		}
	}

	if (!led->num_fnodes) {
		pr_err("No flash/torch devices defined\n");
		return -ECHILD;
	}

	if (need_snode && !led->num_snodes) {
		pr_err("No switch devices defined\n");
		return -ENODEV;
	}

	led->fnode = devm_kcalloc(&led->pdev->dev, led->num_fnodes,
				sizeof(*led->fnode), GFP_KERNEL);
	if (!led->fnode)
		return -ENOMEM;

	if (led->num_snodes) {
		led->snode = devm_kcalloc(&led->pdev->dev, led->num_snodes,
					sizeof(*led->snode), GFP_KERNEL);
		if (!led->snode)
			return -ENOMEM;
	}

	i = 0;
	for_each_available_child_of_node(node, temp) {
		rc = of_property_read_string(temp, "label", &label);
		if (rc < 0) {
			pr_err("Failed to parse label, rc=%d\n", rc);
			of_node_put(temp);
			return rc;
		}

		if (!strcmp("flash", label) || !strcmp("torch", label) ||
				!strcmp("indicator", label)) {
			rc = register_flash_device(led, &led->fnode[i], temp);
			if (rc < 0) {
				pr_err("Failed to register flash device %s rc=%d\n",
					led->fnode[i].fdev.led_cdev.name, rc);
				of_node_put(temp);
				goto unreg_led;
			}
			led->fnode[i++].fdev.led_cdev.dev->of_node = temp;
		} else if (!strcmp("switch", label)) {
			rc = register_switch_device(led, &led->snode[j], temp);
			if (rc < 0) {
				pr_err("Failed to register switch device %s rc=%d\n",
					led->snode[j].cdev.name, rc);
				i--;
				of_node_put(temp);
				goto unreg_led;
			}
			led->snode[j++].cdev.dev->of_node = temp;
		}
	}

	led->ibatt_ocp_threshold_ua = FLASH_LED_IBATT_OCP_THRESH_DEFAULT_UA;
	rc = of_property_read_u32(node, "qcom,ibatt-ocp-threshold-ua", &val);
	if (!rc) {
		led->ibatt_ocp_threshold_ua = val;
	} else if (rc != -EINVAL) {
		pr_err("Unable to parse ibatt_ocp threshold, rc=%d\n", rc);
		return rc;
	}

	return 0;

unreg_led:
	while (i >= 0)
		led_classdev_flash_unregister(&led->fnode[i--].fdev);

	return rc;
}

static void qti_flash_led_free_interrupts(struct qti_flash_led *led)
{
	/* free irqs */
	if (led->all_ramp_up_done_irq >= 0)
		devm_free_irq(&led->pdev->dev,
			led->all_ramp_up_done_irq,
			led);

	if (led->all_ramp_down_done_irq >= 0)
		devm_free_irq(&led->pdev->dev,
			led->all_ramp_down_done_irq,
			led);

	if (led->led_fault_irq >= 0)
		devm_free_irq(&led->pdev->dev,
			led->led_fault_irq,
			led);
}

static int qti_flash_led_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct qti_flash_led *led;
	int rc;

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!led->regmap) {
		pr_err("Failed to get parent's regmap\n");
		return -EINVAL;
	}

	led->data = (struct pmic_data *)of_device_get_match_data(&pdev->dev);
	if (!led->data) {
		pr_err("Failed to get match_data\n");
		return -EINVAL;
	}

	led->pdev = pdev;
	led->iio_channels = devm_kcalloc(&pdev->dev,
				ARRAY_SIZE(qti_flash_iio_prop_names),
				sizeof(struct iio_channel *), GFP_KERNEL);
	if (!led->iio_channels)
		return -ENOMEM;

	spin_lock_init(&led->lock);

	rc = qti_flash_led_register_device(led, node);
	if (rc < 0) {
		pr_err("Failed to parse and register LED devices rc=%d\n", rc);
		return rc;
	}

	rc = qti_flash_led_setup(led);
	if (rc < 0) {
		pr_err("Failed to initialize flash LED, rc=%d\n", rc);
		return rc;
	}

	rc = qti_flash_led_register_interrupts(led);
	if (rc < 0) {
		pr_err("Failed to register LED interrupts rc=%d\n", rc);
		return rc;
	}

	dev_set_drvdata(&pdev->dev, led);

	return 0;
}

static int qti_flash_led_remove(struct platform_device *pdev)
{
	struct qti_flash_led *led = dev_get_drvdata(&pdev->dev);
	int i, j;

	if (led->batt_psy)
		power_supply_put(led->batt_psy);

	for (i = 0; (i < led->num_snodes); i++) {
		for (j = 0; j < ARRAY_SIZE(qti_flash_led_attrs); j++)
			sysfs_remove_file(&led->snode[i].cdev.dev->kobj,
				&qti_flash_led_attrs[j].attr);
	}

	for (i = 0; (i < led->num_fnodes); i++)
		led_classdev_flash_unregister(&led->fnode[i].fdev);

	return 0;
}

static int qti_flash_led_freeze(struct device *dev)
{
	struct qti_flash_led *led = dev_get_drvdata(dev);

	qti_flash_led_free_interrupts(led);

	return 0;
}

static int qti_flash_led_restore(struct device *dev)
{
	struct qti_flash_led *led = dev_get_drvdata(dev);
	int rc = 0;

	rc = qti_flash_led_setup(led);
	if (rc < 0) {
		pr_err("Failed to re-initialize flash LED in Restore, rc=%d\n", rc);
		return rc;
	}

	rc = qti_flash_led_register_interrupts(led);
	if (rc < 0)
		pr_err("Interrupt re-registration failed in Restore rc= %d\n", rc);

	return rc;
}

static const struct dev_pm_ops qti_flash_led_pm_ops = {
	.freeze = qti_flash_led_freeze,
	.restore = qti_flash_led_restore,
};

static const struct pmic_data data[] = {
	[PM2250] = {
		.max_channels = 1,
		.pmic_type = PM2250,
	},

	[PM8350C] = {
		.max_channels = 4,
		.pmic_type = PM8350C,
	},
};

static const struct of_device_id qti_flash_led_match_table[] = {
	{
		.compatible = "qcom,pm2250-flash-led",
		.data = &data[PM2250],
	},

	{
		.compatible = "qcom,pm8350c-flash-led",
		.data = &data[PM8350C],
	},

	{ },
};

static struct platform_driver qti_flash_led_driver = {
	.driver = {
		.name = "leds-qti-flash",
		.of_match_table = qti_flash_led_match_table,
		.pm = &qti_flash_led_pm_ops,
	},
	.probe = qti_flash_led_probe,
	.remove = qti_flash_led_remove,
};

module_platform_driver(qti_flash_led_driver);

MODULE_DESCRIPTION("QTI Flash LED driver");
MODULE_LICENSE("GPL v2");
