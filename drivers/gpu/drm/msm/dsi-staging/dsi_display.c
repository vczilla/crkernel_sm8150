/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"msm-dsi-display:[%s] " fmt, __func__

#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/err.h>

#include "msm_drv.h"
#include "sde_connector.h"
#include "msm_mmu.h"
#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_ctrl.h"
#include "dsi_ctrl_hw.h"
#include "dsi_drm.h"
#include "dsi_clk.h"
#include "dsi_pwr.h"
#include "sde_dbg.h"
#include "dsi_parser.h"
#include <linux/msm_drm_notify.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include <linux/pm_wakeup.h>
#include <linux/input.h>
#include <linux/proc_fs.h>
#include "../sde/sde_trace.h"
#include "dsi_parser.h"

#define to_dsi_display(x) container_of(x, struct dsi_display, host)
#define INT_BASE_10 10
#define NO_OVERRIDE -1

#define MISR_BUFF_SIZE	256
#define ESD_MODE_STRING_MAX_LEN 256

#define MAX_NAME_SIZE	64

#define DSI_CLOCK_BITRATE_RADIX 10
#define MAX_TE_SOURCE_ID  2

#define WU_SEED_REGISTER 0x67
#define UG_SEED_REGISTER 0xB1

DEFINE_MUTEX(dsi_display_clk_mutex);

static char dsi_display_primary[MAX_CMDLINE_PARAM_LEN];
static char dsi_display_secondary[MAX_CMDLINE_PARAM_LEN];
static char SERIAL_NUMBER_flag = 0;
static struct dsi_display_boot_param boot_displays[MAX_DSI_ACTIVE_DISPLAY] = {
	{.boot_param = dsi_display_primary},
	{.boot_param = dsi_display_secondary},
};

static const struct of_device_id dsi_display_dt_match[] = {
	{.compatible = "qcom,dsi-display"},
	{}
};

static int esd_black_count;
static int esd_greenish_count;
static struct dsi_display *primary_display;
static struct input_dev* fresh_rate_input_dev = NULL;
static struct proc_dir_entry *proc_entry_display = NULL;
static int    fresh_rate_report_enable = 0;
static bool   fresh_rate_input_dev_init = false;

#define to_dsi_bridge(x)  container_of((x), struct dsi_bridge, base)

static void dsi_display_mask_ctrl_error_interrupts(struct dsi_display *display,
			u32 mask, bool enable)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_mask_error_status_interrupts(ctrl->ctrl, mask, enable);
	}
}

static int dsi_display_config_clk_gating(struct dsi_display *display,
					bool enable)
{
	int rc = 0, i = 0;
	struct dsi_display_ctrl *mctrl, *ctrl;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (display->panel->host_config.force_hs_clk_lane) {
		pr_debug("no dsi clock gating for continuous clock mode\n");
		return 0;
	}

	mctrl = &display->ctrl[display->clk_master_idx];
	if (!mctrl) {
		pr_err("Invalid controller\n");
		return -EINVAL;
	}

	rc = dsi_ctrl_config_clk_gating(mctrl->ctrl, enable, PIXEL_CLK |
							DSI_PHY);
	if (rc) {
		pr_err("[%s] failed to %s clk gating, rc=%d\n",
				display->name, enable ? "enable" : "disable",
				rc);
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == mctrl))
			continue;
		/**
		 * In Split DSI usecase we should not enable clock gating on
		 * DSI PHY1 to ensure no display atrifacts are seen.
		 */
		rc = dsi_ctrl_config_clk_gating(ctrl->ctrl, enable, PIXEL_CLK);
		if (rc) {
			pr_err("[%s] failed to %s pixel clk gating, rc=%d\n",
				display->name, enable ? "enable" : "disable",
				rc);
			return rc;
		}
	}

	return 0;
}

static void dsi_display_set_ctrl_esd_check_flag(struct dsi_display *display,
			bool enable)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		ctrl->ctrl->esd_check_underway = enable;
	}
}

static void dsi_display_ctrl_irq_update(struct dsi_display *display, bool en)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_irq_update(ctrl->ctrl, en);
	}
}

void dsi_rect_intersect(const struct dsi_rect *r1,
		const struct dsi_rect *r2,
		struct dsi_rect *result)
{
	int l, t, r, b;

	if (!r1 || !r2 || !result)
		return;

	l = max(r1->x, r2->x);
	t = max(r1->y, r2->y);
	r = min((r1->x + r1->w), (r2->x + r2->w));
	b = min((r1->y + r1->h), (r2->y + r2->h));

	if (r <= l || b <= t) {
		memset(result, 0, sizeof(*result));
	} else {
		result->x = l;
		result->y = t;
		result->w = r - l;
		result->h = b - t;
	}
}

extern int aod_layer_hide;
int dsi_display_set_backlight(struct drm_connector *connector,
		void *display, u32 bl_lvl)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	u32 bl_scale, bl_scale_ad;
	u64 bl_temp;
	int rc = 0;
	static int gamma_read_flag;
	if (dsi_display == NULL || dsi_display->panel == NULL)
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&panel->panel_lock);
	if (!dsi_panel_initialized(panel)) {
		panel->hbm_backlight = bl_lvl;
		panel->bl_config.bl_level = bl_lvl;
		pr_err("HBM_backight =%d\n",panel->hbm_backlight);
		rc = -EINVAL;
		goto error;
	}

	if (dsi_display->panel->hw_type == DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		if (bl_lvl != 0 && panel->bl_config.bl_level == 0) {
			if (panel->naive_display_p3_mode) {
				mdelay(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_P3_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_P3_ON);
			}
			if (panel->naive_display_wide_color_mode) {
				mdelay(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_WIDE_COLOR_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_WIDE_COLOR_ON);
			}
			if (panel->naive_display_srgb_color_mode) {
				mdelay(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_SRGB_COLOR_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_SRGB_COLOR_ON);
			}
			if (panel->naive_display_customer_srgb_mode) {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_RGB_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_RGB_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_RGB_OFF cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_RGB_OFF);
			}
			if (panel->naive_display_customer_p3_mode) {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_P3_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_P3_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_P3_OFF cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_P3_OFF);
			}
		}
	}

	if (dsi_display->panel->hw_type == DSI_PANEL_SAMSUNG_S6E3HC2) {
		if (bl_lvl != 0 && panel->bl_config.bl_level == 0) {
			if (panel->naive_display_p3_mode) {
				mdelay(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_P3_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_P3_ON);
			}
			if (panel->naive_display_wide_color_mode) {
				mdelay(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_WIDE_COLOR_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_WIDE_COLOR_ON);
			}
			if (panel->naive_display_srgb_color_mode) {
				mdelay(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_SRGB_COLOR_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_SRGB_COLOR_ON);
			}
			if (panel->naive_display_loading_effect_mode) {
				pr_err("Send DSI_CMD_LOADING_EFFECT_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_EFFECT_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_EFFECT_OFF cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_EFFECT_OFF);
			}
			if (panel->naive_display_customer_srgb_mode) {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_RGB_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_RGB_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_RGB_OFF cmds\n");
				//rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_RGB_OFF);
			}
			if (panel->naive_display_customer_p3_mode) {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_P3_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_P3_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_P3_OFF cmds\n");
				//rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_P3_OFF);
			}
		}
	}

	if (dsi_display->panel->hw_type == DSI_PANEL_SAMSUNG_SOFEF03F_M) {
		if (bl_lvl != 0 && panel->bl_config.bl_level == 0) {
			if (panel->naive_display_p3_mode) {
				msleep(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_P3_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_P3_ON);
			}
			if (panel->naive_display_wide_color_mode) {
				msleep(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_WIDE_COLOR_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_WIDE_COLOR_ON);
			}
			if (panel->naive_display_srgb_color_mode) {
				msleep(20);
				pr_err("Send DSI_CMD_SET_NATIVE_DISPLAY_SRGB_COLOR_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NATIVE_DISPLAY_SRGB_COLOR_ON);
			}
			if (panel->naive_display_loading_effect_mode) {
				pr_err("Send DSI_CMD_LOADING_EFFECT_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_EFFECT_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_EFFECT_OFF cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_EFFECT_OFF);
			}
			if (panel->naive_display_customer_srgb_mode) {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_RGB_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_RGB_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_RGB_OFF cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_RGB_OFF);
			}
			if (panel->naive_display_customer_p3_mode) {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_P3_ON cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_P3_ON);
			} else {
				pr_err("Send DSI_CMD_LOADING_CUSTOMER_P3_OFF cmds\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_LOADING_CUSTOMER_P3_OFF);
			}
		}
	}

	panel->bl_config.bl_level = bl_lvl;

	/* scale backlight */
	bl_scale = panel->bl_config.bl_scale;
	bl_temp = bl_lvl * bl_scale / MAX_BL_SCALE_LEVEL;

	bl_scale_ad = panel->bl_config.bl_scale_ad;
	bl_temp = (u32)bl_temp * bl_scale_ad / MAX_AD_BL_SCALE_LEVEL;

	pr_debug("bl_scale = %u, bl_scale_ad = %u, bl_lvl = %u\n",
		bl_scale, bl_scale_ad, (u32)bl_temp);

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_backlight(panel, (u32)bl_temp);
	if (rc)
		pr_err("unable to set backlight\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);

	if (dsi_display->panel->hw_type != DSI_PANEL_DEFAULT && (0 == SERIAL_NUMBER_flag)) {
		dsi_display_get_serial_number_AT(connector);
	}

	if ((gamma_read_flag < 2) && dsi_display->panel->hw_type == DSI_PANEL_SAMSUNG_S6E3HC2) {
		if (gamma_read_flag < 1) {
			gamma_read_flag++;
		}
		else {
			schedule_delayed_work(&dsi_display->panel->gamma_read_work, 0);
			gamma_read_flag++;
		}
	}

	return rc;
}

static int dsi_display_cmd_engine_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	mutex_lock(&m_ctrl->ctrl->ctrl_lock);

	if (display->cmd_engine_refcount > 0) {
		display->cmd_engine_refcount++;
		goto done;
	}

	rc = dsi_ctrl_set_cmd_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_ON);
	if (rc) {
		pr_err("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		goto done;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_cmd_engine_state(ctrl->ctrl,
						   DSI_CTRL_ENGINE_ON);
		if (rc) {
			pr_err("[%s] failed to enable cmd engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_master;
		}
	}

	display->cmd_engine_refcount++;
	goto done;
error_disable_master:
	(void)dsi_ctrl_set_cmd_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_OFF);
done:
	mutex_unlock(&m_ctrl->ctrl->ctrl_lock);
	return rc;
}

static int dsi_display_cmd_engine_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	mutex_lock(&m_ctrl->ctrl->ctrl_lock);

	if (display->cmd_engine_refcount == 0) {
		pr_err("[%s] Invalid refcount\n", display->name);
		goto done;
	} else if (display->cmd_engine_refcount > 1) {
		display->cmd_engine_refcount--;
		goto done;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_cmd_engine_state(ctrl->ctrl,
						   DSI_CTRL_ENGINE_OFF);
		if (rc)
			pr_err("[%s] failed to enable cmd engine, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_ctrl_set_cmd_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_OFF);
	if (rc) {
		pr_err("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		goto error;
	}

error:
	display->cmd_engine_refcount = 0;
done:
	mutex_unlock(&m_ctrl->ctrl->ctrl_lock);
	return rc;
}

static void dsi_display_aspace_cb_locked(void *cb_data, bool is_detach)
{
	struct dsi_display *display;
	struct dsi_display_ctrl *display_ctrl;
	int rc, cnt;

	if (!cb_data) {
		pr_err("aspace cb called with invalid cb_data\n");
		return;
	}
	display = (struct dsi_display *)cb_data;

	/*
	 * acquire panel_lock to make sure no commands are in-progress
	 * while detaching the non-secure context banks
	 */
	dsi_panel_acquire_panel_lock(display->panel);

	if (is_detach) {
		/* invalidate the stored iova */
		display->cmd_buffer_iova = 0;

		/* return the virtual address mapping */
		msm_gem_put_vaddr(display->tx_cmd_buf);
		msm_gem_vunmap(display->tx_cmd_buf, OBJ_LOCK_NORMAL);

	} else {
		rc = msm_gem_get_iova(display->tx_cmd_buf,
				display->aspace, &(display->cmd_buffer_iova));
		if (rc) {
			pr_err("failed to get the iova rc %d\n", rc);
			goto end;
		}

		display->vaddr =
			(void *) msm_gem_get_vaddr(display->tx_cmd_buf);

		if (IS_ERR_OR_NULL(display->vaddr)) {
			pr_err("failed to get va rc %d\n", rc);
			goto end;
		}
	}

	display_for_each_ctrl(cnt, display) {
		display_ctrl = &display->ctrl[cnt];
		display_ctrl->ctrl->cmd_buffer_size = display->cmd_buffer_size;
		display_ctrl->ctrl->cmd_buffer_iova = display->cmd_buffer_iova;
		display_ctrl->ctrl->vaddr = display->vaddr;
		display_ctrl->ctrl->secure_mode = is_detach;
	}

end:
	/* release panel_lock */
	dsi_panel_release_panel_lock(display->panel);
}

static irqreturn_t dsi_display_panel_err_flag_irq_handler(int irq, void *data)
{
	struct dsi_display *display = (struct dsi_display *)data;
	/*
	 * This irq handler is used for sole purpose of identifying
	 * ESD attacks on panel and we can safely assume IRQ_HANDLED
	 * in case of display not being initialized yet
	 */
	if ((!display) || (!display->panel->is_err_flag_irq_enabled) || (!display->panel->panel_initialized))
		return IRQ_HANDLED;

	pr_err("%s\n", __func__);

	if (!display->panel->err_flag_status) {
		display->panel->err_flag_status = true;
		cancel_delayed_work_sync(sde_esk_check_delayed_work);
		schedule_delayed_work(sde_esk_check_delayed_work, 0);
		pr_err("schedule sde_esd_check_delayed_work\n");
	}

	return IRQ_HANDLED;
}

void dsi_display_change_err_flag_irq_status(struct dsi_display *display,
					bool enable)
{
	if (!display) {
		pr_err("Invalid params\n");
		return;
	}

	if (!gpio_is_valid(display->panel->err_flag_gpio))
		return;

	/* Handle unbalanced irq enable/disbale calls */
	if (enable && !display->panel->is_err_flag_irq_enabled) {
		enable_irq(gpio_to_irq(display->panel->err_flag_gpio));
		display->panel->is_err_flag_irq_enabled = true;
		pr_err("enable err flag irq\n");
	} else if (!enable && display->panel->is_err_flag_irq_enabled) {
		disable_irq(gpio_to_irq(display->panel->err_flag_gpio));
		display->panel->is_err_flag_irq_enabled = false;
		pr_err("disable err flag irq\n");
	}
}
EXPORT_SYMBOL(dsi_display_change_err_flag_irq_status);

static void dsi_display_register_err_flag_irq(struct dsi_display *display)
{
	int rc = 0;
	struct platform_device *pdev;
	struct device *dev;
	unsigned int err_flag_irq;

	pdev = display->pdev;
	if (!pdev) {
		pr_err("invalid platform device\n");
		return;
	}

	dev = &pdev->dev;
	if (!dev) {
		pr_err("invalid device\n");
		return;
	}

	if (!gpio_is_valid(display->panel->err_flag_gpio)) {
		pr_err("Failed to get err-flag-gpio\n");
		rc = -EINVAL;
		return;
	}

	err_flag_irq = gpio_to_irq(display->panel->err_flag_gpio);

	/* Avoid deferred spurious irqs with disable_irq() */
	irq_set_status_flags(err_flag_irq, IRQ_DISABLE_UNLAZY);

	rc = devm_request_irq(dev, err_flag_irq, dsi_display_panel_err_flag_irq_handler,
			      IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			      "ERR_FLAG_GPIO", display);
	if (rc) {
		pr_err("Err flag request_irq failed for ESD rc:%d\n", rc);
		irq_clear_status_flags(err_flag_irq, IRQ_DISABLE_UNLAZY);
		return;
	}

	disable_irq(err_flag_irq);
	display->panel->is_err_flag_irq_enabled = false;
}

static irqreturn_t dsi_display_panel_te_irq_handler(int irq, void *data)
{
	struct dsi_display *display = (struct dsi_display *)data;

	/*
	 * This irq handler is used for sole purpose of identifying
	 * ESD attacks on panel and we can safely assume IRQ_HANDLED
	 * in case of display not being initialized yet
	 */
	if (!display)
		return IRQ_HANDLED;

	SDE_EVT32(SDE_EVTLOG_FUNC_CASE1);
	complete_all(&display->esd_te_gate);
	return IRQ_HANDLED;
}

static void dsi_display_change_te_irq_status(struct dsi_display *display,
					bool enable)
{
	if (!display) {
		pr_err("Invalid params\n");
		return;
	}

	/* Handle unbalanced irq enable/disbale calls */
	if (enable && !display->is_te_irq_enabled) {
		enable_irq(gpio_to_irq(display->disp_te_gpio));
		display->is_te_irq_enabled = true;
	} else if (!enable && display->is_te_irq_enabled) {
		disable_irq(gpio_to_irq(display->disp_te_gpio));
		display->is_te_irq_enabled = false;
	}
}

static void dsi_display_register_te_irq(struct dsi_display *display)
{
	int rc = 0;
	struct platform_device *pdev;
	struct device *dev;
	unsigned int te_irq;

	pdev = display->pdev;
	if (!pdev) {
		pr_err("invalid platform device\n");
		return;
	}

	dev = &pdev->dev;
	if (!dev) {
		pr_err("invalid device\n");
		return;
	}

	if (!gpio_is_valid(display->disp_te_gpio)) {
		rc = -EINVAL;
		goto error;
	}

	init_completion(&display->esd_te_gate);
	te_irq = gpio_to_irq(display->disp_te_gpio);

	/* Avoid deferred spurious irqs with disable_irq() */
	irq_set_status_flags(te_irq, IRQ_DISABLE_UNLAZY);

	rc = devm_request_irq(dev, te_irq, dsi_display_panel_te_irq_handler,
			      IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			      "TE_GPIO", display);
	if (rc) {
		pr_err("TE request_irq failed for ESD rc:%d\n", rc);
		irq_clear_status_flags(te_irq, IRQ_DISABLE_UNLAZY);
		goto error;
	}

	disable_irq(te_irq);
	display->is_te_irq_enabled = false;

	return;

error:
	/* disable the TE based ESD check */
	pr_warn("Unable to register for TE IRQ\n");
	if (display->panel->esd_config.status_mode == ESD_MODE_PANEL_TE)
		display->panel->esd_config.esd_enabled = false;
}

static bool dsi_display_is_te_based_esd(struct dsi_display *display)
{
	u32 status_mode = 0;

	if (!display->panel) {
		pr_err("Invalid panel data\n");
		return false;
	}

	status_mode = display->panel->esd_config.status_mode;

	if (status_mode == ESD_MODE_PANEL_TE &&
			gpio_is_valid(display->disp_te_gpio))
		return true;
	return false;
}

/* Allocate memory for cmd dma tx buffer */
static int dsi_host_alloc_cmd_tx_buffer(struct dsi_display *display)
{
	int rc = 0, cnt = 0;
	struct dsi_display_ctrl *display_ctrl;

	display->tx_cmd_buf = msm_gem_new(display->drm_dev,
			SZ_4K,
			MSM_BO_UNCACHED);

	if ((display->tx_cmd_buf) == NULL) {
		pr_err("Failed to allocate cmd tx buf memory\n");
		rc = -ENOMEM;
		goto error;
	}

	display->cmd_buffer_size = SZ_4K;

	display->aspace = msm_gem_smmu_address_space_get(
			display->drm_dev, MSM_SMMU_DOMAIN_UNSECURE);
	if (!display->aspace) {
		pr_err("failed to get aspace\n");
		rc = -EINVAL;
		goto free_gem;
	}
	/* register to aspace */
	rc = msm_gem_address_space_register_cb(display->aspace,
			dsi_display_aspace_cb_locked, (void *)display);
	if (rc) {
		pr_err("failed to register callback %d", rc);
		goto free_gem;
	}

	rc = msm_gem_get_iova(display->tx_cmd_buf, display->aspace,
				&(display->cmd_buffer_iova));
	if (rc) {
		pr_err("failed to get the iova rc %d\n", rc);
		goto free_aspace_cb;
	}

	display->vaddr =
		(void *) msm_gem_get_vaddr(display->tx_cmd_buf);
	if (IS_ERR_OR_NULL(display->vaddr)) {
		pr_err("failed to get va rc %d\n", rc);
		rc = -EINVAL;
		goto put_iova;
	}

	display_for_each_ctrl(cnt, display) {
		display_ctrl = &display->ctrl[cnt];
		display_ctrl->ctrl->cmd_buffer_size = SZ_4K;
		display_ctrl->ctrl->cmd_buffer_iova =
					display->cmd_buffer_iova;
		display_ctrl->ctrl->vaddr = display->vaddr;
		display_ctrl->ctrl->tx_cmd_buf = display->tx_cmd_buf;
	}

	return rc;

put_iova:
	msm_gem_put_iova(display->tx_cmd_buf, display->aspace);
free_aspace_cb:
	msm_gem_address_space_unregister_cb(display->aspace,
			dsi_display_aspace_cb_locked, display);
free_gem:
	mutex_lock(&display->drm_dev->struct_mutex);
	msm_gem_free_object(display->tx_cmd_buf);
	mutex_unlock(&display->drm_dev->struct_mutex);
error:
	return rc;
}

static bool dsi_display_validate_reg_read(struct dsi_panel *panel)
{
	int i, j = 0;
	int len = 0, *lenp;
	int group = 0, count = 0;
	struct drm_panel_esd_config *config;

	if (!panel)
		return false;

	config = &(panel->esd_config);

	lenp = config->status_valid_params ?: config->status_cmds_rlen;
	count = config->status_cmd.count;

	for (i = 0; i < count; i++)
		len += lenp[i];

	for (i = 0; i < len; i++)
		j += len;

	for (j = 0; j < config->groups; ++j) {
		for (i = 0; i < len; ++i) {
			if (config->return_buf[i] !=
				config->status_value[group + i]) {
				DRM_ERROR("mismatch: 0x%x\n",
					  config->return_buf[i]);
				break;
			}
		}

		if (i == len)
			return true;
		group += len;
	}

	return false;
}

static void dsi_display_parse_te_data(struct dsi_display *display)
{
	struct platform_device *pdev;
	struct device *dev;
	int rc = 0;
	u32 val = 0;

	pdev = display->pdev;
	if (!pdev) {
		pr_err("Inavlid platform device\n");
		return;
	}

	dev = &pdev->dev;
	if (!dev) {
		pr_err("Inavlid platform device\n");
		return;
	}

	display->disp_te_gpio = of_get_named_gpio(dev->of_node,
					"qcom,platform-te-gpio", 0);

	if (display->fw)
		rc = dsi_parser_read_u32(display->parser_node,
			"qcom,panel-te-source", &val);
	else
		rc = of_property_read_u32(dev->of_node,
			"qcom,panel-te-source", &val);

	if (rc || (val  > MAX_TE_SOURCE_ID)) {
		pr_err("invalid vsync source selection\n");
		val = 0;
	}

	display->te_source = val;
}

static int dsi_display_read_status(struct dsi_display_ctrl *ctrl,
		struct dsi_panel *panel)
{
	int i, rc = 0, count = 0, start = 0, *lenp;
	struct drm_panel_esd_config *config;
	struct dsi_cmd_desc *cmds;
	u32 flags = 0;

	if (!panel || !ctrl || !ctrl->ctrl)
		return -EINVAL;

	/*
	 * When DSI controller is not in initialized state, we do not want to
	 * report a false ESD failure and hence we defer until next read
	 * happen.
	 */
	if (!dsi_ctrl_validate_host_state(ctrl->ctrl))
		return 1;

	config = &(panel->esd_config);
	lenp = config->status_valid_params ?: config->status_cmds_rlen;
	count = config->status_cmd.count;
	cmds = config->status_cmd.cmds;
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ |
		  DSI_CTRL_CMD_CUSTOM_DMA_SCHED);

	for (i = 0; i < count; ++i) {
		memset(config->status_buf, 0x0, SZ_4K);
		if (cmds[i].last_command) {
			cmds[i].msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		if (config->status_cmd.state == DSI_CMD_SET_STATE_LP)
			cmds[i].msg.flags |= MIPI_DSI_MSG_USE_LPM;
		cmds[i].msg.rx_buf = config->status_buf;
		cmds[i].msg.rx_len = config->status_cmds_rlen[i];
		rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds[i].msg, flags);
		if (rc <= 0) {
			pr_err("rx cmd transfer failed rc=%d\n", rc);
			return rc;
		}

		memcpy(config->return_buf + start,
			config->status_buf, lenp[i]);
		start += lenp[i];
	}

	return rc;
}

static int dsi_display_validate_status(struct dsi_display_ctrl *ctrl,
		struct dsi_panel *panel)
{
	int rc = 0;

	rc = dsi_display_read_status(ctrl, panel);
	if (rc <= 0) {
		goto exit;
	} else {
		/*
		 * panel status read successfully.
		 * check for validity of the data read back.
		 */
		rc = dsi_display_validate_reg_read(panel);
		if (!rc) {
			rc = -EINVAL;
			goto exit;
		}
	}

exit:
	return rc;
}

static int dsi_panel_tx_cmd_set_op(struct dsi_panel *panel,
				enum dsi_cmd_set_type type)
{
	int rc = 0, i = 0;
	ssize_t len;
	struct dsi_cmd_desc *cmds;
	u32 count;
	enum dsi_cmd_set_state state;
	struct dsi_display_mode *mode;
	const struct mipi_dsi_host_ops *ops = panel->host->ops;
	if (!panel || !panel->cur_mode)
		return -EINVAL;


	mode = panel->cur_mode;

	cmds = mode->priv_info->cmd_sets[type].cmds;
	count = mode->priv_info->cmd_sets[type].count;
	state = mode->priv_info->cmd_sets[type].state;

	if (count == 0) {
		pr_debug("[%s] No commands to be sent for state(%d)\n",
			 panel->name, type);
		goto error;
	}
	for (i = 0; i < count; i++) {
		if (state == DSI_CMD_SET_STATE_LP)
			cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;

		if (cmds->last_command)
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;

		len = ops->transfer(panel->host, &cmds->msg);
		if (len < 0) {
			rc = len;
			pr_err("failed to set cmds(%d), rc=%d\n", type, rc);
			goto error;
		}
		if (cmds->post_wait_ms)
			usleep_range(cmds->post_wait_ms*1000,
					((cmds->post_wait_ms*1000)+10));
		cmds++;
	}
error:
	return rc;
}

static int dsi_display_status_reg_read(struct dsi_display *display)
{
	int rc = 0, i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_display_mode *mode;
	u32 flags = 0;
	u32 count = 0;
	struct dsi_panel *panel = NULL;
	struct dsi_cmd_desc *cmds1;
	struct dsi_cmd_desc *cmds2;
	struct dsi_cmd_desc *cmds3;
	struct dsi_cmd_desc *cmds4;
	struct dsi_cmd_desc *cmds5;
	struct dsi_cmd_desc *cmds6;
	struct dsi_cmd_desc *cmds7;
	u8 temp_buffer_1[1] = {0};
	u8 temp_buffer_2[1] = {0};
	u8 temp_buffer_3[1] = {0};
	u8 temp_buffer_4[1] = {0};
	u8 temp_buffer_5[2] = {0,};
	u8 temp_buffer_6[16] = {0,};
	u8 temp_buffer_7[34] = {0,};
	u8 register_0a[1] = {0};
	u8 register_b6[1] = {0};
	u8 buf[48];
	memset(buf, 0, sizeof(buf));

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			pr_err("failed to allocate cmd tx buffer memory\n");
			goto done;
		}
	}

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		pr_err("cmd engine enable failed\n");
		return -EPERM;
	}
	mode = display->panel->cur_mode;
	panel = display->panel;

	if (panel->hw_type == DSI_PANEL_SAMSUNG_S6E3HC2) {
		count = mode->priv_info->cmd_sets[DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON].count;
		if (!count) {
			pr_err("This panel does not read register\n");
		} else {
			rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		}
		cmds1 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID1].cmds;
		if (cmds1->last_command) {
			cmds1->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds1->msg.rx_buf = buf;
		cmds1->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds1->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);

		memcpy(temp_buffer_1, cmds1->msg.rx_buf, 1);
		memset(buf, 0, sizeof(buf));

		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		cmds2 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID2].cmds;
		if (cmds2->last_command) {
			cmds2->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds2->msg.rx_buf = buf;
		cmds2->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds2->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
		
		memcpy(temp_buffer_2, cmds2->msg.rx_buf, 1);
		memset(buf, 0, sizeof(buf));

		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		cmds3 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID3].cmds;
		if (cmds3->last_command) {
			cmds3->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds3->msg.rx_buf = buf;
		cmds3->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds3->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
				
		memcpy(temp_buffer_3, cmds3->msg.rx_buf, 1);

		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		cmds4 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID4].cmds;
		if (cmds4->last_command) {
			cmds4->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds4->msg.rx_buf = buf;
		cmds4->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds4->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
				
		memcpy(temp_buffer_4, cmds4->msg.rx_buf, 1);
		memset(buf, 0, sizeof(buf));
		
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		cmds5 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID5].cmds;
		if (cmds5->last_command) {
			cmds5->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds5->msg.rx_buf = buf;
		cmds5->msg.rx_len = 2;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds5->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
						
		memcpy(temp_buffer_5, cmds5->msg.rx_buf, 2);
		memset(buf, 0, sizeof(buf));

		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		cmds6 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID6].cmds;
		if (cmds6->last_command) {
			cmds6->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds6->msg.rx_buf = buf;
		cmds6->msg.rx_len = 16;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds6->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
						
		memcpy(temp_buffer_6, cmds6->msg.rx_buf, 16);
		memset(buf, 0, sizeof(buf));

		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_SET_ESD_LOGREAD_PREREAD);

		cmds7 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID7].cmds;
		if (cmds7->last_command) {
			cmds7->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds7->msg.rx_buf = buf;
		cmds7->msg.rx_len = 34;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds7->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
								
		memcpy(temp_buffer_7, cmds7->msg.rx_buf, 34);

		if((temp_buffer_6[0] !=0x80) && (temp_buffer_2[0] != 0x80)) {
			rc = -1;
		}
		else {
			rc = 1;
		}
	} else if (panel->hw_type == DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		count = mode->priv_info->cmd_sets[DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON].count;
		if (!count) {
			pr_err("This panel does not read register\n");
		} else {
			rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		}
		cmds1 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID1].cmds;
		if (cmds1->last_command) {
			cmds1->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds1->msg.rx_buf = buf;
		cmds1->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds1->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		memcpy(temp_buffer_1, cmds1->msg.rx_buf, 1);
		count = mode->priv_info->cmd_sets[DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON].count;
		if (!count) {
			pr_err("This panel does not read register\n");
		} else {
			rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		}
		cmds2 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID6].cmds;
		if (cmds2->last_command) {
			cmds2->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds2->msg.rx_buf = buf;
		cmds2->msg.rx_len = 2;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds2->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);
		memcpy(temp_buffer_6, cmds2->msg.rx_buf, 2);
		count = mode->priv_info->cmd_sets[DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF].count;
		if (!count) {
			pr_err("This panel does not read register\n");
		} else {
			rc = dsi_panel_tx_cmd_set_op(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
		}
		if(((temp_buffer_6[0] == 132) && (temp_buffer_6[1] == 0))||(temp_buffer_1[0] != 159))
			 rc = -1;
		else
			rc = 1;
	} else 	if (panel->hw_type == DSI_PANEL_SAMSUNG_SOFEF03F_M) {
		count = mode->priv_info->cmd_sets[DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON].count;
		if (!count) {
			pr_err("This panel does not support esd register reading\n");
		} else {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_ON);
		}

		flags = 0;
		cmds1 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID1].cmds;
		if (cmds1->last_command) {
			cmds1->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds1->msg.rx_buf = register_0a;
		cmds1->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds1->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);

		flags = 0;
		cmds2 = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID2].cmds;
		if (cmds2->last_command) {
			cmds2->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		cmds2->msg.rx_buf = register_b6;
		cmds2->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds2->msg, flags);
		if (rc <= 0)
			pr_err("rx cmd transfer failed rc=%d\n", rc);

		count = mode->priv_info->cmd_sets[DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF].count;
		if (!count) {
			pr_err("This panel does not support esd register reading\n");
		} else {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_READ_SAMSUNG_PANEL_REGISTER_OFF);
		}

		if ((register_0a[0] != 0x9c) || (register_b6[0] != 0x0a)) {
			if (register_0a[0] != 0x9c)
				esd_black_count++;
			if (register_b6[0] != 0x0a)
				esd_greenish_count++;
			pr_err("%s:black_count=%d, greenish_count=%d, total=%d\n",
				__func__, esd_black_count, esd_greenish_count,
					esd_black_count + esd_greenish_count);
			rc = -1;
		}
		else {
			rc = 1;
		}
	} else {
		rc = dsi_display_validate_status(m_ctrl, display->panel);
	}

	if (rc <= 0) {
		pr_err("[%s] read status failed on master,rc=%d\n",
		       display->name, rc);
		goto exit;
	}

	if (!display->panel->sync_broadcast_en)
		goto exit;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (ctrl == m_ctrl)
			continue;

		rc = dsi_display_validate_status(ctrl, display->panel);
		if (rc <= 0) {
			pr_err("[%s] read status failed on slave,rc=%d\n",
			       display->name, rc);
			goto exit;
		}
	}
exit:
	dsi_display_cmd_engine_disable(display);

done:
	return rc;
}

static int dsi_display_status_bta_request(struct dsi_display *display)
{
	int rc = 0;

	pr_debug(" ++\n");
	/* TODO: trigger SW BTA and wait for acknowledgment */

	return rc;
}

static int dsi_display_status_check_te(struct dsi_display *display)
{
	int rc = 1;
	int const esd_te_timeout = msecs_to_jiffies(3*20);

	dsi_display_change_te_irq_status(display, true);

	reinit_completion(&display->esd_te_gate);
	if (!wait_for_completion_timeout(&display->esd_te_gate,
				esd_te_timeout)) {
		pr_err("TE check failed\n");
		rc = -EINVAL;
	}

	dsi_display_change_te_irq_status(display, false);

	return rc;
}

int dsi_display_check_status(struct drm_connector *connector, void *display,
					bool te_check_override)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	u32 status_mode;
	int rc = 0x1;
	u32 mask;

	if (!dsi_display || !dsi_display->panel)
		return -EINVAL;

	panel = dsi_display->panel;

	dsi_panel_acquire_panel_lock(panel);

	if (!panel->panel_initialized) {
		pr_debug("Panel not initialized\n");
		goto release_panel_lock;
	}

	/* Prevent another ESD check,when ESD recovery is underway */
	if (atomic_read(&panel->esd_recovery_pending))
		goto release_panel_lock;

	status_mode = panel->esd_config.status_mode;

	if (status_mode == ESD_MODE_SW_SIM_SUCCESS)
		goto release_panel_lock;

	if (status_mode == ESD_MODE_SW_SIM_FAILURE) {
		rc = -EINVAL;
		goto release_panel_lock;
	}
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	if (te_check_override && gpio_is_valid(dsi_display->disp_te_gpio))
		status_mode = ESD_MODE_PANEL_TE;

	if (status_mode == ESD_MODE_PANEL_TE) {
		rc = dsi_display_status_check_te(dsi_display);
		goto exit;
	}

	if (dsi_display->panel->err_flag_status == true) {
		esd_black_count++;
		pr_err("%s:black_count=%d, greenish_count=%d, total=%d\n",
			__func__, esd_black_count, esd_greenish_count,
				esd_black_count + esd_greenish_count);
		rc = -1;
		goto exit;
	}

	dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			     DSI_ALL_CLKS, DSI_CLK_ON);

	/* Mask error interrupts before attempting ESD read */
	mask = BIT(DSI_FIFO_OVERFLOW) | BIT(DSI_FIFO_UNDERFLOW);
	dsi_display_set_ctrl_esd_check_flag(dsi_display, true);
	dsi_display_mask_ctrl_error_interrupts(dsi_display, mask, true);

	if (status_mode == ESD_MODE_REG_READ) {
		rc = dsi_display_status_reg_read(dsi_display);
	} else if (status_mode == ESD_MODE_SW_BTA) {
		rc = dsi_display_status_bta_request(dsi_display);
	} else {
		pr_warn("Unsupported ESD check mode: %d\n", status_mode);
		panel->esd_config.esd_enabled = false;
	}

	/* Unmask error interrupts if check passed */
	if (rc > 0) {
		dsi_display_set_ctrl_esd_check_flag(dsi_display, false);
		dsi_display_mask_ctrl_error_interrupts(dsi_display,
						       mask, false);
	}
	dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			     DSI_ALL_CLKS, DSI_CLK_OFF);

exit:
	/* Handle Panel failures during display disable sequence */
	if (rc <= 0)
		atomic_set(&panel->esd_recovery_pending, 1);

release_panel_lock:
	dsi_panel_release_panel_lock(panel);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);

	return rc;
}

static int dsi_display_cmd_prepare(const char *cmd_buf, u32 cmd_buf_len,
		struct dsi_cmd_desc *cmd, u8 *payload, u32 payload_len)
{
	int i;

	memset(cmd, 0x00, sizeof(*cmd));
	cmd->msg.type = cmd_buf[0];
	cmd->last_command = (cmd_buf[1] == 1 ? true : false);
	cmd->msg.channel = cmd_buf[2];
	cmd->msg.flags = cmd_buf[3];
	cmd->msg.ctrl = 0;
	cmd->post_wait_ms = cmd->msg.wait_ms = cmd_buf[4];
	cmd->msg.tx_len = ((cmd_buf[5] << 8) | (cmd_buf[6]));

	if (cmd->msg.tx_len > payload_len) {
		pr_err("Incorrect payload length tx_len %zu, payload_len %d\n",
		       cmd->msg.tx_len, payload_len);
		return -EINVAL;
	}

	for (i = 0; i < cmd->msg.tx_len; i++)
		payload[i] = cmd_buf[7 + i];

	cmd->msg.tx_buf = payload;
	return 0;
}

static int dsi_display_ctrl_get_host_init_state(struct dsi_display *dsi_display,
		bool *state)
{
	struct dsi_display_ctrl *ctrl;
	int i, rc = -EINVAL;

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		rc = dsi_ctrl_get_host_engine_init_state(ctrl->ctrl, state);
		if (rc)
			break;
	}
	return rc;
}

int dsi_display_cmd_transfer(struct drm_connector *connector,
		void *display, const char *cmd_buf,
		u32 cmd_buf_len)
{
	struct dsi_display *dsi_display = display;
	struct dsi_cmd_desc cmd;
	u8 cmd_payload[MAX_CMD_PAYLOAD_SIZE];
	int rc = 0;
	bool state = false;

	if (!dsi_display || !cmd_buf) {
		pr_err("[DSI] invalid params\n");
		return -EINVAL;
	}

	pr_debug("[DSI] Display command transfer\n");

	rc = dsi_display_cmd_prepare(cmd_buf, cmd_buf_len,
			&cmd, cmd_payload, MAX_CMD_PAYLOAD_SIZE);
	if (rc) {
		pr_err("[DSI] command prepare failed. rc %d\n", rc);
		return rc;
	}

	mutex_lock(&dsi_display->display_lock);
	rc = dsi_display_ctrl_get_host_init_state(dsi_display, &state);

	/**
	 * Handle scenario where a command transfer is initiated through
	 * sysfs interface when device is in suepnd state.
	 */
	if (!rc && !state) {
		pr_warn_ratelimited("Command xfer attempted while device is in suspend state\n"
				);
		rc = -EPERM;
		goto end;
	}
	if (rc || !state) {
		pr_err("[DSI] Invalid host state %d rc %d\n",
				state, rc);
		rc = -EPERM;
		goto end;
	}

	rc = dsi_display->host.ops->transfer(&dsi_display->host,
			&cmd.msg);
end:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

static void _dsi_display_continuous_clk_ctrl(struct dsi_display *display,
					     bool enable)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display || !display->panel->host_config.force_hs_clk_lane)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];

		/**
		 * For phy ver 4.0 chipsets, configure DSI controller and
		 * DSI PHY to force clk lane to HS mode always whereas
		 * for other phy ver chipsets, configure DSI controller only.
		 */
		if (ctrl->phy->hw.ops.set_continuous_clk) {
			dsi_ctrl_hs_req_sel(ctrl->ctrl, true);
			dsi_ctrl_set_continuous_clk(ctrl->ctrl, enable);
			dsi_phy_set_continuous_clk(ctrl->phy, enable);
		} else {
			dsi_ctrl_set_continuous_clk(ctrl->ctrl, enable);
		}
	}
}

int dsi_display_soft_reset(void *display)
{
	struct dsi_display *dsi_display;
	struct dsi_display_ctrl *ctrl;
	int rc = 0;
	int i;

	if (!display)
		return -EINVAL;

	dsi_display = display;

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		rc = dsi_ctrl_soft_reset(ctrl->ctrl);
		if (rc) {
			pr_err("[%s] failed to soft reset host_%d, rc=%d\n",
					dsi_display->name, i, rc);
			break;
		}
	}

	return rc;
}

enum dsi_pixel_format dsi_display_get_dst_format(
		struct drm_connector *connector,
		void *display)
{
	enum dsi_pixel_format format = DSI_PIXEL_FORMAT_MAX;
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display || !dsi_display->panel) {
		pr_err("Invalid params(s) dsi_display %pK, panel %pK\n",
			dsi_display,
			((dsi_display) ? dsi_display->panel : NULL));
		return format;
	}

	format = dsi_display->panel->host_config.dst_format;
	return format;
}

static void _dsi_display_setup_misr(struct dsi_display *display)
{
	int i;

	display_for_each_ctrl(i, display) {
		dsi_ctrl_setup_misr(display->ctrl[i].ctrl,
				display->misr_enable,
				display->misr_frame_count);
	}
}

/**
 * dsi_display_get_cont_splash_status - Get continuous splash status.
 * @dsi_display:         DSI display handle.
 *
 * Return: boolean to signify whether continuous splash is enabled.
 */
static bool dsi_display_get_cont_splash_status(struct dsi_display *display)
{
	u32 val = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_ctrl_hw *hw;

	display_for_each_ctrl(i, display) {
		ctrl = &(display->ctrl[i]);
		if (!ctrl || !ctrl->ctrl)
			continue;

		hw = &(ctrl->ctrl->hw);
		val = hw->ops.get_cont_splash_status(hw);
		if (!val)
			return false;
	}
	return true;
}
extern int dsi_panel_set_aod_mode(struct dsi_panel *panel, int level);

int dsi_display_set_power(struct drm_connector *connector,
		int power_mode, void *disp)
{
	struct dsi_display *display = disp;
	int rc = 0;
	struct msm_drm_notifier notifier_data;
	int blank;

	if (!display || !display->panel) {
		pr_err("invalid display/panel\n");
		return -EINVAL;
	}

	switch (power_mode) {
	case SDE_MODE_DPMS_LP1:
		rc = dsi_panel_set_lp1(display->panel);
		break;
	case SDE_MODE_DPMS_LP2:
		rc = dsi_panel_set_lp2(display->panel);
		break;
	case SDE_MODE_DPMS_ON:
		if (display->panel->power_mode == SDE_MODE_DPMS_LP1 ||
			display->panel->power_mode == SDE_MODE_DPMS_LP2)
			rc = dsi_panel_set_nolp(display->panel);
		/*sned screen on cmd for TP start*/
		blank = MSM_DRM_BLANK_UNBLANK_CUST;
                notifier_data.data = &blank;
                notifier_data.id = connector_state_crtc_index;
                msm_drm_notifier_call_chain(MSM_DRM_EARLY_EVENT_BLANK,
                                            &notifier_data);
		/*sned screen on cmd for TP end*/
		break;
	case SDE_MODE_DPMS_OFF:
		/*sned screen off cmd for TP start*/
		blank = MSM_DRM_BLANK_POWERDOWN_CUST;
                notifier_data.data = &blank;
                notifier_data.id = connector_state_crtc_index;
                msm_drm_notifier_call_chain(MSM_DRM_EARLY_EVENT_BLANK,
                                            &notifier_data);
		/*sned screen off cmd for TP end*/
		break;
	default:
		return rc;
	}
	pr_debug("Power mode transition from %d to %d %s",
		 display->panel->power_mode, power_mode,
		 rc ? "failed" : "successful");
	if (!rc)
		display->panel->power_mode = power_mode;

	return rc;
}

static ssize_t debugfs_dump_info_read(struct file *file,
				      char __user *user_buf,
				      size_t user_len,
				      loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	u32 len = 0;
	int i;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(SZ_4K, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf + len, (SZ_4K - len), "name = %s\n", display->name);
	len += snprintf(buf + len, (SZ_4K - len),
			"\tResolution = %dx%d\n",
			display->config.video_timing.h_active,
			display->config.video_timing.v_active);

	display_for_each_ctrl(i, display) {
		len += snprintf(buf + len, (SZ_4K - len),
				"\tCTRL_%d:\n\t\tctrl = %s\n\t\tphy = %s\n",
				i, display->ctrl[i].ctrl->name,
				display->ctrl[i].phy->name);
	}

	len += snprintf(buf + len, (SZ_4K - len),
			"\tPanel = %s\n", display->panel->name);

	len += snprintf(buf + len, (SZ_4K - len),
			"\tClock master = %s\n",
			display->ctrl[display->clk_master_idx].ctrl->name);

	if (len > user_len)
		len = user_len;

	if (copy_to_user(user_buf, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos += len;

	kfree(buf);
	return len;
}

static ssize_t debugfs_misr_setup(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	int rc = 0;
	size_t len;
	u32 enable, frame_count;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(MISR_BUFF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* leave room for termination char */
	len = min_t(size_t, user_len, MISR_BUFF_SIZE - 1);
	if (copy_from_user(buf, user_buf, len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[len] = '\0'; /* terminate the string */

	if (sscanf(buf, "%u %u", &enable, &frame_count) != 2) {
		rc = -EINVAL;
		goto error;
	}

	display->misr_enable = enable;
	display->misr_frame_count = frame_count;

	mutex_lock(&display->display_lock);
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto unlock;
	}

	_dsi_display_setup_misr(display);

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto unlock;
	}

	rc = user_len;
unlock:
	mutex_unlock(&display->display_lock);
error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_misr_read(struct file *file,
				 char __user *user_buf,
				 size_t user_len,
				 loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	u32 len = 0;
	int rc = 0;
	struct dsi_ctrl *dsi_ctrl;
	int i;
	u32 misr;
	size_t max_len = min_t(size_t, user_len, MISR_BUFF_SIZE);

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(max_len, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	mutex_lock(&display->display_lock);
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		dsi_ctrl = display->ctrl[i].ctrl;
		misr = dsi_ctrl_collect_misr(display->ctrl[i].ctrl);

		len += snprintf((buf + len), max_len - len,
			"DSI_%d MISR: 0x%x\n", dsi_ctrl->cell_index, misr);

		if (len >= max_len)
			break;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	if (copy_to_user(user_buf, buf, max_len)) {
		rc = -EFAULT;
		goto error;
	}

	*ppos += len;

error:
	mutex_unlock(&display->display_lock);
	kfree(buf);
	return len;
}

static ssize_t debugfs_esd_trigger_check(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	int rc = 0;
	u32 esd_trigger;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	if (user_len > sizeof(u32))
		return -EINVAL;

	if (!user_len || !user_buf)
		return -EINVAL;

	if (!display->panel ||
		atomic_read(&display->panel->esd_recovery_pending))
		return user_len;

	buf = kzalloc(user_len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, user_buf, user_len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[user_len] = '\0'; /* terminate the string */

	if (kstrtouint(buf, 10, &esd_trigger)) {
		rc = -EINVAL;
		goto error;
	}

	if (esd_trigger != 1) {
		rc = -EINVAL;
		goto error;
	}

	display->esd_trigger = esd_trigger;

	if (display->esd_trigger) {
		pr_info("ESD attack triggered by user\n");
		rc = dsi_panel_trigger_esd_attack(display->panel);
		if (rc) {
			pr_err("Failed to trigger ESD attack\n");
			goto error;
		}
	}

	rc = user_len;
error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_alter_esd_check_mode(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	struct drm_panel_esd_config *esd_config;
	char *buf;
	int rc = 0;
	size_t len = min_t(size_t, user_len, ESD_MODE_STRING_MAX_LEN);

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(len + 1, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	if (copy_from_user(buf, user_buf, len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[len] = '\0'; /* terminate the string */
	if (!display->panel) {
		rc = -EINVAL;
		goto error;
	}

	esd_config = &display->panel->esd_config;
	if (!esd_config) {
		pr_err("Invalid panel esd config\n");
		rc = -EINVAL;
		goto error;
	}

	if (!esd_config->esd_enabled)
		goto error;

	if (!strcmp(buf, "te_signal_check\n")) {
		pr_info("ESD check is switched to TE mode by user\n");
		esd_config->status_mode = ESD_MODE_PANEL_TE;
		dsi_display_change_te_irq_status(display, true);
	}

	if (!strcmp(buf, "reg_read\n")) {
		pr_info("ESD check is switched to reg read by user\n");
		rc = dsi_panel_parse_esd_reg_read_configs(display->panel);
		if (rc) {
			pr_err("failed to alter esd check mode,rc=%d\n",
						rc);
			rc = user_len;
			goto error;
		}
		esd_config->status_mode = ESD_MODE_REG_READ;
		if (dsi_display_is_te_based_esd(display))
			dsi_display_change_te_irq_status(display, false);
	}

	if (!strcmp(buf, "esd_sw_sim_success\n"))
		esd_config->status_mode = ESD_MODE_SW_SIM_SUCCESS;

	if (!strcmp(buf, "esd_sw_sim_failure\n"))
		esd_config->status_mode = ESD_MODE_SW_SIM_FAILURE;

	rc = len;
error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_read_esd_check_mode(struct file *file,
				 char __user *user_buf,
				 size_t user_len,
				 loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	struct drm_panel_esd_config *esd_config;
	char *buf;
	int rc = 0;
	size_t len = min_t(size_t, user_len, ESD_MODE_STRING_MAX_LEN);

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	if (!display->panel) {
		pr_err("invalid panel data\n");
		return -EINVAL;
	}

	buf = kzalloc(len, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	esd_config = &display->panel->esd_config;
	if (!esd_config) {
		pr_err("Invalid panel esd config\n");
		rc = -EINVAL;
		goto error;
	}

	if (!esd_config->esd_enabled) {
		rc = snprintf(buf, len, "ESD feature not enabled");
		goto output_mode;
	}

	switch (esd_config->status_mode) {
	case ESD_MODE_REG_READ:
		rc = snprintf(buf, len, "reg_read");
		break;
	case ESD_MODE_PANEL_TE:
		rc = snprintf(buf, len, "te_signal_check");
		break;
	case ESD_MODE_SW_SIM_FAILURE:
		rc = snprintf(buf, len, "esd_sw_sim_failure");
		break;
	case ESD_MODE_SW_SIM_SUCCESS:
		rc = snprintf(buf, len, "esd_sw_sim_success");
		break;
	default:
		rc = snprintf(buf, len, "invalid");
		break;
	}

output_mode:
	if (!rc) {
		rc = -EINVAL;
		goto error;
	}

	if (copy_to_user(user_buf, buf, len)) {
		rc = -EFAULT;
		goto error;
	}

	*ppos += len;

error:
	kfree(buf);
	return len;
}

static const struct file_operations dump_info_fops = {
	.open = simple_open,
	.read = debugfs_dump_info_read,
};

static const struct file_operations misr_data_fops = {
	.open = simple_open,
	.read = debugfs_misr_read,
	.write = debugfs_misr_setup,
};

static const struct file_operations esd_trigger_fops = {
	.open = simple_open,
	.write = debugfs_esd_trigger_check,
};

static const struct file_operations esd_check_mode_fops = {
	.open = simple_open,
	.write = debugfs_alter_esd_check_mode,
	.read = debugfs_read_esd_check_mode,
};

static int dsi_display_debugfs_init(struct dsi_display *display)
{
	int rc = 0;
	struct dentry *dir, *dump_file, *misr_data;
	char name[MAX_NAME_SIZE];
	int i;

	dir = debugfs_create_dir(display->name, NULL);
	if (IS_ERR_OR_NULL(dir)) {
		rc = PTR_ERR(dir);
		pr_debug("[%s] debugfs create dir failed, rc = %d\n",
		       display->name, rc);
		goto error;
	}

	dump_file = debugfs_create_file("dump_info",
					0400,
					dir,
					display,
					&dump_info_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		pr_err("[%s] debugfs create dump info file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	dump_file = debugfs_create_file("esd_trigger",
					0644,
					dir,
					display,
					&esd_trigger_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		pr_err("[%s] debugfs for esd trigger file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	dump_file = debugfs_create_file("esd_check_mode",
					0644,
					dir,
					display,
					&esd_check_mode_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		pr_err("[%s] debugfs for esd check mode failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	misr_data = debugfs_create_file("misr_data",
					0600,
					dir,
					display,
					&misr_data_fops);
	if (IS_ERR_OR_NULL(misr_data)) {
		rc = PTR_ERR(misr_data);
		pr_err("[%s] debugfs create misr datafile failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	display_for_each_ctrl(i, display) {
		struct msm_dsi_phy *phy = display->ctrl[i].phy;

		if (!phy || !phy->name)
			continue;

		snprintf(name, ARRAY_SIZE(name),
				"%s_allow_phy_power_off", phy->name);
		dump_file = debugfs_create_bool(name, 0600, dir,
				&phy->allow_phy_power_off);
		if (IS_ERR_OR_NULL(dump_file)) {
			rc = PTR_ERR(dump_file);
			pr_err("[%s] debugfs create %s failed, rc=%d\n",
			       display->name, name, rc);
			goto error_remove_dir;
		}

		snprintf(name, ARRAY_SIZE(name),
				"%s_regulator_min_datarate_bps", phy->name);
		dump_file = debugfs_create_u32(name, 0600, dir,
				&phy->regulator_min_datarate_bps);
		if (IS_ERR_OR_NULL(dump_file)) {
			rc = PTR_ERR(dump_file);
			pr_err("[%s] debugfs create %s failed, rc=%d\n",
			       display->name, name, rc);
			goto error_remove_dir;
		}
	}

	if (!debugfs_create_bool("ulps_feature_enable", 0600, dir,
			&display->panel->ulps_feature_enabled)) {
		pr_err("[%s] debugfs create ulps feature enable file failed\n",
		       display->name);
		goto error_remove_dir;
	}

	if (!debugfs_create_bool("ulps_suspend_feature_enable", 0600, dir,
			&display->panel->ulps_suspend_enabled)) {
		pr_err("[%s] debugfs create ulps-suspend feature enable file failed\n",
		       display->name);
		goto error_remove_dir;
	}

	if (!debugfs_create_bool("ulps_status", 0400, dir,
			&display->ulps_enabled)) {
		pr_err("[%s] debugfs create ulps status file failed\n",
		       display->name);
		goto error_remove_dir;
	}

	display->root = dir;
	dsi_parser_dbg_init(display->parser, dir);

	return rc;
error_remove_dir:
	debugfs_remove(dir);
error:
	return rc;
}

static int dsi_display_debugfs_deinit(struct dsi_display *display)
{
	debugfs_remove_recursive(display->root);

	return 0;
}

static void adjust_timing_by_ctrl_count(const struct dsi_display *display,
					struct dsi_display_mode *mode)
{
	struct dsi_host_common_cfg *host = &display->panel->host_config;
	bool is_split_link = host->split_link.split_link_enabled;
	u32 sublinks_count = host->split_link.num_sublinks;

	if (is_split_link && sublinks_count > 1) {
		mode->timing.h_active /= sublinks_count;
		mode->timing.h_front_porch /= sublinks_count;
		mode->timing.h_sync_width /= sublinks_count;
		mode->timing.h_back_porch /= sublinks_count;
		mode->timing.h_skew /= sublinks_count;
		mode->pixel_clk_khz /= sublinks_count;
	} else {
		mode->timing.h_active /= display->ctrl_count;
		mode->timing.h_front_porch /= display->ctrl_count;
		mode->timing.h_sync_width /= display->ctrl_count;
		mode->timing.h_back_porch /= display->ctrl_count;
		mode->timing.h_skew /= display->ctrl_count;
		mode->pixel_clk_khz /= display->ctrl_count;
	}
}

static int dsi_display_is_ulps_req_valid(struct dsi_display *display,
		bool enable)
{
	/* TODO: make checks based on cont. splash */

	pr_debug("checking ulps req validity\n");

	if (atomic_read(&display->panel->esd_recovery_pending)) {
		pr_debug("%s: ESD recovery sequence underway\n", __func__);
		return false;
	}

	if (!dsi_panel_ulps_feature_enabled(display->panel) &&
			!display->panel->ulps_suspend_enabled) {
		pr_debug("%s: ULPS feature is not enabled\n", __func__);
		return false;
	}

	if (!dsi_panel_initialized(display->panel) &&
			!display->panel->ulps_suspend_enabled) {
		pr_debug("%s: panel not yet initialized\n", __func__);
		return false;
	}

	if (enable && display->ulps_enabled) {
		pr_debug("ULPS already enabled\n");
		return false;
	} else if (!enable && !display->ulps_enabled) {
		pr_debug("ULPS already disabled\n");
		return false;
	}

	/*
	 * No need to enter ULPS when transitioning from splash screen to
	 * boot animation since it is expected that the clocks would be turned
	 * right back on.
	 */
	if (enable && display->is_cont_splash_enabled)
		return false;

	return true;
}


/**
 * dsi_display_set_ulps() - set ULPS state for DSI lanes.
 * @dsi_display:         DSI display handle.
 * @enable:           enable/disable ULPS.
 *
 * ULPS can be enabled/disabled after DSI host engine is turned on.
 *
 * Return: error code.
 */
static int dsi_display_set_ulps(struct dsi_display *display, bool enable)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;


	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (!dsi_display_is_ulps_req_valid(display, enable)) {
		pr_debug("%s: skipping ULPS config, enable=%d\n",
			__func__, enable);
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	/*
	 * ULPS entry-exit can be either through the DSI controller or
	 * the DSI PHY depending on hardware variation. For some chipsets,
	 * both controller version and phy version ulps entry-exit ops can
	 * be present. To handle such cases, send ulps request through PHY,
	 * if ulps request is handled in PHY, then no need to send request
	 * through controller.
	 */

	rc = dsi_phy_set_ulps(m_ctrl->phy, &display->config, enable,
			display->clamp_enabled);

	if (rc == DSI_PHY_ULPS_ERROR) {
		pr_err("Ulps PHY state change(%d) failed\n", enable);
		return -EINVAL;
	}

	else if (rc == DSI_PHY_ULPS_HANDLED) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl || (ctrl == m_ctrl))
				continue;

			rc = dsi_phy_set_ulps(ctrl->phy, &display->config,
					enable, display->clamp_enabled);
			if (rc == DSI_PHY_ULPS_ERROR) {
				pr_err("Ulps PHY state change(%d) failed\n",
						enable);
				return -EINVAL;
			}
		}
	}

	else if (rc == DSI_PHY_ULPS_NOT_HANDLED) {
		rc = dsi_ctrl_set_ulps(m_ctrl->ctrl, enable);
		if (rc) {
			pr_err("Ulps controller state change(%d) failed\n",
					enable);
			return rc;
		}
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl || (ctrl == m_ctrl))
				continue;

			rc = dsi_ctrl_set_ulps(ctrl->ctrl, enable);
			if (rc) {
				pr_err("Ulps controller state change(%d) failed\n",
						enable);
				return rc;
			}
		}
	}

	display->ulps_enabled = enable;
	return 0;
}

/**
 * dsi_display_set_clamp() - set clamp state for DSI IO.
 * @dsi_display:         DSI display handle.
 * @enable:           enable/disable clamping.
 *
 * Return: error code.
 */
static int dsi_display_set_clamp(struct dsi_display *display, bool enable)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool ulps_enabled = false;


	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	ulps_enabled = display->ulps_enabled;

	/*
	 * Clamp control can be either through the DSI controller or
	 * the DSI PHY depending on hardware variation
	 */
	rc = dsi_ctrl_set_clamp_state(m_ctrl->ctrl, enable, ulps_enabled);
	if (rc) {
		pr_err("DSI ctrl clamp state change(%d) failed\n", enable);
		return rc;
	}

	rc = dsi_phy_set_clamp_state(m_ctrl->phy, enable);
	if (rc) {
		pr_err("DSI phy clamp state change(%d) failed\n", enable);
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_clamp_state(ctrl->ctrl, enable, ulps_enabled);
		if (rc) {
			pr_err("DSI Clamp state change(%d) failed\n", enable);
			return rc;
		}

		rc = dsi_phy_set_clamp_state(ctrl->phy, enable);
		if (rc) {
			pr_err("DSI phy clamp state change(%d) failed\n",
				enable);
			return rc;
		}

		pr_debug("Clamps %s for ctrl%d\n",
			enable ? "enabled" : "disabled", i);
	}

	display->clamp_enabled = enable;
	return 0;
}

/**
 * dsi_display_setup_ctrl() - setup DSI controller.
 * @dsi_display:         DSI display handle.
 *
 * Return: error code.
 */
static int dsi_display_ctrl_setup(struct dsi_display *display)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *ctrl, *m_ctrl;


	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	rc = dsi_ctrl_setup(m_ctrl->ctrl);
	if (rc) {
		pr_err("DSI controller setup failed\n");
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_setup(ctrl->ctrl);
		if (rc) {
			pr_err("DSI controller setup failed\n");
			return rc;
		}
	}
	return 0;
}

static int dsi_display_phy_enable(struct dsi_display *display);

/**
 * dsi_display_phy_idle_on() - enable DSI PHY while coming out of idle screen.
 * @dsi_display:         DSI display handle.
 * @mmss_clamp:          True if clamp is enabled.
 *
 * Return: error code.
 */
static int dsi_display_phy_idle_on(struct dsi_display *display,
		bool mmss_clamp)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;


	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (mmss_clamp && !display->phy_idle_power_off) {
		dsi_display_phy_enable(display);
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	rc = dsi_phy_idle_ctrl(m_ctrl->phy, true);
	if (rc) {
		pr_err("DSI controller setup failed\n");
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_idle_ctrl(ctrl->phy, true);
		if (rc) {
			pr_err("DSI controller setup failed\n");
			return rc;
		}
	}
	display->phy_idle_power_off = false;
	return 0;
}

/**
 * dsi_display_phy_idle_off() - disable DSI PHY while going to idle screen.
 * @dsi_display:         DSI display handle.
 *
 * Return: error code.
 */
static int dsi_display_phy_idle_off(struct dsi_display *display)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	display_for_each_ctrl(i, display) {
		struct msm_dsi_phy *phy = display->ctrl[i].phy;

		if (!phy)
			continue;

		if (!phy->allow_phy_power_off) {
			pr_debug("phy doesn't support this feature\n");
			return 0;
		}
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_phy_idle_ctrl(m_ctrl->phy, false);
	if (rc) {
		pr_err("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_idle_ctrl(ctrl->phy, false);
		if (rc) {
			pr_err("DSI controller setup failed\n");
			return rc;
		}
	}
	display->phy_idle_power_off = true;
	return 0;
}

void dsi_display_enable_event(struct drm_connector *connector,
		struct dsi_display *display,
		uint32_t event_idx, struct dsi_event_cb_info *event_info,
		bool enable)
{
	uint32_t irq_status_idx = DSI_STATUS_INTERRUPT_COUNT;
	int i;

	if (!display) {
		pr_err("invalid display\n");
		return;
	}

	if (event_info)
		event_info->event_idx = event_idx;

	switch (event_idx) {
	case SDE_CONN_EVENT_VID_DONE:
		irq_status_idx = DSI_SINT_VIDEO_MODE_FRAME_DONE;
		break;
	case SDE_CONN_EVENT_CMD_DONE:
		irq_status_idx = DSI_SINT_CMD_FRAME_DONE;
		break;
	case SDE_CONN_EVENT_VID_FIFO_OVERFLOW:
	case SDE_CONN_EVENT_CMD_FIFO_UNDERFLOW:
		if (event_info) {
			display_for_each_ctrl(i, display)
				display->ctrl[i].ctrl->recovery_cb =
							*event_info;
		}
	default:
		/* nothing to do */
		pr_debug("[%s] unhandled event %d\n", display->name, event_idx);
		return;
	}

	if (enable) {
		display_for_each_ctrl(i, display)
			dsi_ctrl_enable_status_interrupt(
					display->ctrl[i].ctrl, irq_status_idx,
					event_info);
	} else {
		display_for_each_ctrl(i, display)
			dsi_ctrl_disable_status_interrupt(
					display->ctrl[i].ctrl, irq_status_idx);
	}
}

/**
 * dsi_config_host_engine_state_for_cont_splash()- update host engine state
 *                                                 during continuous splash.
 * @display: Handle to dsi display
 *
 */
static void dsi_config_host_engine_state_for_cont_splash
					(struct dsi_display *display)
{
	int i;
	struct dsi_display_ctrl *ctrl;
	enum dsi_engine_state host_state = DSI_CTRL_ENGINE_ON;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		dsi_ctrl_update_host_engine_state_for_cont_splash(ctrl->ctrl,
							host_state);
	}
}

static int dsi_display_ctrl_power_on(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		rc = dsi_ctrl_set_power_state(ctrl->ctrl,
					      DSI_CTRL_POWER_VREG_ON);
		if (rc) {
			pr_err("[%s] Failed to set power state, rc=%d\n",
			       ctrl->ctrl->name, rc);
			goto error;
		}
	}

	return rc;
error:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		(void)dsi_ctrl_set_power_state(ctrl->ctrl,
			DSI_CTRL_POWER_VREG_OFF);
	}
	return rc;
}

static int dsi_display_ctrl_power_off(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		rc = dsi_ctrl_set_power_state(ctrl->ctrl,
			DSI_CTRL_POWER_VREG_OFF);
		if (rc) {
			pr_err("[%s] Failed to power off, rc=%d\n",
			       ctrl->ctrl->name, rc);
			goto error;
		}
	}
error:
	return rc;
}

static void dsi_display_parse_cmdline_topology(struct dsi_display *display,
					unsigned int display_type)
{
	char *boot_str = NULL;
	char *str = NULL;
	char *sw_te = NULL;
	unsigned long cmdline_topology = NO_OVERRIDE;
	unsigned long cmdline_timing = NO_OVERRIDE;

	if (display_type >= MAX_DSI_ACTIVE_DISPLAY) {
		pr_err("display_type=%d not supported\n", display_type);
		goto end;
	}

	if (display_type == DSI_PRIMARY)
		boot_str = dsi_display_primary;
	else
		boot_str = dsi_display_secondary;

	sw_te = strnstr(boot_str, ":swte", strlen(boot_str));
	if (sw_te)
		display->sw_te_using_wd = true;

	str = strnstr(boot_str, ":config", strlen(boot_str));
	if (!str)
		goto end;

	if (kstrtol(str + strlen(":config"), INT_BASE_10,
				(unsigned long *)&cmdline_topology)) {
		pr_err("invalid config index override: %s\n", boot_str);
		goto end;
	}

	str = strnstr(boot_str, ":timing", strlen(boot_str));
	if (!str)
		goto end;

	if (kstrtol(str + strlen(":timing"), INT_BASE_10,
				(unsigned long *)&cmdline_timing)) {
		pr_err("invalid timing index override: %s. resetting both timing and config\n",
			boot_str);
		cmdline_topology = NO_OVERRIDE;
		goto end;
	}
	pr_debug("successfully parsed command line topology and timing\n");
end:
	display->cmdline_topology = cmdline_topology;
	display->cmdline_timing = cmdline_timing;
}

/**
 * dsi_display_parse_boot_display_selection()- Parse DSI boot display name
 *
 * Return:	returns error status
 */
static int dsi_display_parse_boot_display_selection(void)
{
	char *pos = NULL;
	char disp_buf[MAX_CMDLINE_PARAM_LEN] = {'\0'};
	int i, j;

	for (i = 0; i < MAX_DSI_ACTIVE_DISPLAY; i++) {
		strlcpy(disp_buf, boot_displays[i].boot_param,
			MAX_CMDLINE_PARAM_LEN);

		pos = strnstr(disp_buf, ":", MAX_CMDLINE_PARAM_LEN);

		/* Use ':' as a delimiter to retrieve the display name */
		if (!pos) {
			pr_debug("display name[%s]is not valid\n", disp_buf);
			continue;
		}

		for (j = 0; (disp_buf + j) < pos; j++)
			boot_displays[i].name[j] = *(disp_buf + j);

		boot_displays[i].name[j] = '\0';

		boot_displays[i].boot_disp_en = true;
	}

	return 0;
}

static int dsi_display_phy_power_on(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		rc = dsi_phy_set_power_state(ctrl->phy, true);
		if (rc) {
			pr_err("[%s] Failed to set power state, rc=%d\n",
			       ctrl->phy->name, rc);
			goto error;
		}
	}

	return rc;
error:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		if (!ctrl->phy)
			continue;
		(void)dsi_phy_set_power_state(ctrl->phy, false);
	}
	return rc;
}

static int dsi_display_phy_power_off(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->phy)
			continue;

		rc = dsi_phy_set_power_state(ctrl->phy, false);
		if (rc) {
			pr_err("[%s] Failed to power off, rc=%d\n",
			       ctrl->ctrl->name, rc);
			goto error;
		}
	}
error:
	return rc;
}

static int dsi_display_set_clk_src(struct dsi_display *display, bool on)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_clk_link_set *src;

	/*
	 * For CPHY mode, the parent of mux_clks need to be set
	 * to Cphy_clks to have correct dividers for byte and
	 * pixel clocks.
	 */
	if (display->panel->host_config.phy_type == DSI_PHY_TYPE_CPHY) {
		rc = dsi_clk_update_parent(&display->clock_info.cphy_clks,
			      &display->clock_info.mux_clks);
		if (rc) {
			pr_err("failed update mux parent to CPHY\n");
			return rc;
		}
	}

	/* if XO clk is defined, select XO clk src when DSI is disabled */
	if (on)
		src = &display->clock_info.mux_clks;
	else if (display->clock_info.xo_clks.byte_clk)
		src = &display->clock_info.xo_clks;
	else
		return 0;

	/*
	 * In case of split DSI usecases, the clock for master controller should
	 * be enabled before the other controller. Master controller in the
	 * clock context refers to the controller that sources the clock.
	 */
	m_ctrl = &display->ctrl[display->clk_master_idx];

	rc = dsi_ctrl_set_clock_source(m_ctrl->ctrl, src);
	if (rc) {
		pr_err("[%s] failed to set source clocks for master, rc=%d\n",
			   display->name, rc);
		return rc;
	}

	/* Turn on rest of the controllers */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_clock_source(ctrl->ctrl, src);
		if (rc) {
			pr_err("[%s] failed to set source clocks, rc=%d\n",
				   display->name, rc);
			return rc;
		}
	}
	return 0;
}

static int dsi_display_phy_reset_config(struct dsi_display *display,
		bool enable)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_phy_reset_config(ctrl->ctrl, enable);
		if (rc) {
			pr_err("[%s] failed to %s phy reset, rc=%d\n",
			       display->name, enable ? "mask" : "unmask", rc);
			return rc;
		}
	}
	return 0;
}

static void dsi_display_toggle_resync_fifo(struct dsi_display *display)
{
	struct dsi_display_ctrl *ctrl;
	int i;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_toggle_resync_fifo(ctrl->phy);
	}

	/*
	 * After retime buffer synchronization we need to turn of clk_en_sel
	 * bit on each phy.
	 */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_reset_clk_en_sel(ctrl->phy);
	}

}

static int dsi_display_ctrl_update(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_host_timing_update(ctrl->ctrl);
		if (rc) {
			pr_err("[%s] failed to update host_%d, rc=%d\n",
				   display->name, i, rc);
			goto error_host_deinit;
		}
	}

	return 0;
error_host_deinit:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		(void)dsi_ctrl_host_deinit(ctrl->ctrl);
	}

	return rc;
}

static int dsi_display_ctrl_init(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* when ULPS suspend feature is enabled, we will keep the lanes in
	 * ULPS during suspend state and clamp DSI phy. Hence while resuming
	 * we will programe DSI controller as part of core clock enable.
	 * After that we should not re-configure DSI controller again here for
	 * usecases where we are resuming from ulps suspend as it might put
	 * the HW in bad state.
	 */
	if (!display->panel->ulps_suspend_enabled || !display->ulps_enabled) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_host_init(ctrl->ctrl,
					display->is_cont_splash_enabled);
			if (rc) {
				pr_err("[%s] failed to init host_%d, rc=%d\n",
				       display->name, i, rc);
				goto error_host_deinit;
			}
		}
	} else {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_update_host_state(ctrl->ctrl,
							DSI_CTRL_OP_HOST_INIT,
							true);
			if (rc)
				pr_debug("host init update failed rc=%d\n", rc);
		}
	}

	return rc;
error_host_deinit:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		(void)dsi_ctrl_host_deinit(ctrl->ctrl);
	}
	return rc;
}

static int dsi_display_ctrl_deinit(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_host_deinit(ctrl->ctrl);
		if (rc) {
			pr_err("[%s] failed to deinit host_%d, rc=%d\n",
			       display->name, i, rc);
		}
	}

	return rc;
}

static int dsi_display_ctrl_host_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	/* Host engine states are already taken care for
	 * continuous splash case
	 */
	if (display->is_cont_splash_enabled) {
		pr_debug("cont splash enabled, host enable not required\n");
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_ctrl_set_host_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_ON);
	if (rc) {
		pr_err("[%s] failed to enable host engine, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_host_engine_state(ctrl->ctrl,
						    DSI_CTRL_ENGINE_ON);
		if (rc) {
			pr_err("[%s] failed to enable sl host engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_master;
		}
	}

	return rc;
error_disable_master:
	(void)dsi_ctrl_set_host_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_OFF);
error:
	return rc;
}

static int dsi_display_ctrl_host_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	/*
	 * For platforms where ULPS is controlled by DSI controller block,
	 * do not disable dsi controller block if lanes are to be
	 * kept in ULPS during suspend. So just update the SW state
	 * and return early.
	 */
	if (display->panel->ulps_suspend_enabled &&
	    !m_ctrl->phy->hw.ops.ulps_ops.ulps_request) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_update_host_state(ctrl->ctrl,
							DSI_CTRL_OP_HOST_ENGINE,
							false);
			if (rc)
				pr_debug("host state update failed %d\n", rc);
		}
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_host_engine_state(ctrl->ctrl,
						    DSI_CTRL_ENGINE_OFF);
		if (rc)
			pr_err("[%s] failed to disable host engine, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_ctrl_set_host_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_OFF);
	if (rc) {
		pr_err("[%s] failed to disable host engine, rc=%d\n",
		       display->name, rc);
		goto error;
	}

error:
	return rc;
}

static int dsi_display_vid_engine_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->video_master_idx];

	rc = dsi_ctrl_set_vid_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_ON);
	if (rc) {
		pr_err("[%s] failed to enable vid engine, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_vid_engine_state(ctrl->ctrl,
						   DSI_CTRL_ENGINE_ON);
		if (rc) {
			pr_err("[%s] failed to enable vid engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_master;
		}
	}

	return rc;
error_disable_master:
	(void)dsi_ctrl_set_vid_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_OFF);
error:
	return rc;
}

static int dsi_display_vid_engine_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->video_master_idx];

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_vid_engine_state(ctrl->ctrl,
						   DSI_CTRL_ENGINE_OFF);
		if (rc)
			pr_err("[%s] failed to disable vid engine, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_ctrl_set_vid_engine_state(m_ctrl->ctrl, DSI_CTRL_ENGINE_OFF);
	if (rc)
		pr_err("[%s] failed to disable mvid engine, rc=%d\n",
		       display->name, rc);

	return rc;
}

static int dsi_display_phy_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	enum dsi_phy_pll_source m_src = DSI_PLL_SOURCE_STANDALONE;

	m_ctrl = &display->ctrl[display->clk_master_idx];
	if (display->ctrl_count > 1)
		m_src = DSI_PLL_SOURCE_NATIVE;

	rc = dsi_phy_enable(m_ctrl->phy,
			    &display->config,
			    m_src,
			    true,
			    display->is_cont_splash_enabled);
	if (rc) {
		pr_err("[%s] failed to enable DSI PHY, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_enable(ctrl->phy,
				    &display->config,
				    DSI_PLL_SOURCE_NON_NATIVE,
				    true,
				    display->is_cont_splash_enabled);
		if (rc) {
			pr_err("[%s] failed to enable DSI PHY, rc=%d\n",
			       display->name, rc);
			goto error_disable_master;
		}
	}

	return rc;

error_disable_master:
	(void)dsi_phy_disable(m_ctrl->phy);
error:
	return rc;
}

static int dsi_display_phy_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->clk_master_idx];

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_disable(ctrl->phy);
		if (rc)
			pr_err("[%s] failed to disable DSI PHY, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_phy_disable(m_ctrl->phy);
	if (rc)
		pr_err("[%s] failed to disable DSI PHY, rc=%d\n",
		       display->name, rc);

	return rc;
}

static int dsi_display_wake_up(struct dsi_display *display)
{
	return 0;
}

static int dsi_display_broadcast_cmd(struct dsi_display *display,
				     const struct mipi_dsi_msg *msg)
{
	int rc = 0;
	u32 flags, m_flags;
	struct dsi_display_ctrl *ctrl, *m_ctrl;
	int i;

	m_flags = (DSI_CTRL_CMD_BROADCAST | DSI_CTRL_CMD_BROADCAST_MASTER |
		   DSI_CTRL_CMD_DEFER_TRIGGER | DSI_CTRL_CMD_FETCH_MEMORY);
	flags = (DSI_CTRL_CMD_BROADCAST | DSI_CTRL_CMD_DEFER_TRIGGER |
		 DSI_CTRL_CMD_FETCH_MEMORY);

	if ((msg->flags & MIPI_DSI_MSG_LASTCOMMAND)) {
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
		m_flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	/*
	 * 1. Setup commands in FIFO
	 * 2. Trigger commands
	 */
	m_ctrl = &display->ctrl[display->cmd_master_idx];
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, msg, m_flags);
	if (rc) {
		pr_err("[%s] cmd transfer failed on master,rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (ctrl == m_ctrl)
			continue;

		rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, msg, flags);
		if (rc) {
			pr_err("[%s] cmd transfer failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}

		rc = dsi_ctrl_cmd_tx_trigger(ctrl->ctrl, flags);
		if (rc) {
			pr_err("[%s] cmd trigger failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	rc = dsi_ctrl_cmd_tx_trigger(m_ctrl->ctrl, m_flags);
	if (rc) {
		pr_err("[%s] cmd trigger failed for master, rc=%d\n",
		       display->name, rc);
		goto error;
	}

error:
	return rc;
}

static int dsi_display_phy_sw_reset(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	/* For continuous splash use case ctrl states are updated
	 * separately and hence we do an early return
	 */
	if (display->is_cont_splash_enabled) {
		pr_debug("cont splash enabled, phy sw reset not required\n");
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_ctrl_phy_sw_reset(m_ctrl->ctrl);
	if (rc) {
		pr_err("[%s] failed to reset phy, rc=%d\n", display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_phy_sw_reset(ctrl->ctrl);
		if (rc) {
			pr_err("[%s] failed to reset phy, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

error:
	return rc;
}

static int dsi_host_attach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dsi)
{
	return 0;
}

static int dsi_host_detach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dsi)
{
	return 0;
}

static ssize_t dsi_host_transfer(struct mipi_dsi_host *host,
				 const struct mipi_dsi_msg *msg)
{
	struct dsi_display *display;
	int rc = 0, ret = 0;

	if (!host || !msg) {
		pr_err("Invalid params\n");
		return 0;
	}

	display = to_dsi_display(host);

	/* Avoid sending DCS commands when ESD recovery is pending */
	if (atomic_read(&display->panel->esd_recovery_pending)) {
		pr_debug("ESD recovery pending\n");
		return 0;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable all DSI clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	rc = dsi_display_wake_up(display);
	if (rc) {
		pr_err("[%s] failed to wake up display, rc=%d\n",
		       display->name, rc);
		goto error_disable_clks;
	}

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		pr_err("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		goto error_disable_clks;
	}

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			pr_err("failed to allocate cmd tx buffer memory\n");
			goto error_disable_cmd_engine;
		}
	}

	if (display->ctrl_count > 1 && !(msg->flags & MIPI_DSI_MSG_UNICAST)) {
		rc = dsi_display_broadcast_cmd(display, msg);
		if (rc) {
			pr_err("[%s] cmd broadcast failed, rc=%d\n",
			       display->name, rc);
			goto error_disable_cmd_engine;
		}
	} else {
		int ctrl_idx = (msg->flags & MIPI_DSI_MSG_UNICAST) ?
				msg->ctrl : 0;

		rc = dsi_ctrl_cmd_transfer(display->ctrl[ctrl_idx].ctrl, msg,
					  DSI_CTRL_CMD_FETCH_MEMORY);
		if (rc) {
			pr_err("[%s] cmd transfer failed, rc=%d\n",
			       display->name, rc);
			goto error_disable_cmd_engine;
		}
	}

error_disable_cmd_engine:
	ret = dsi_display_cmd_engine_disable(display);
	if (ret) {
		pr_err("[%s]failed to disable DSI cmd engine, rc=%d\n",
				display->name, ret);
	}
error_disable_clks:
	ret = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	if (ret) {
		pr_err("[%s] failed to disable all DSI clocks, rc=%d\n",
		       display->name, ret);
	}
error:
	return rc;
}


static struct mipi_dsi_host_ops dsi_host_ops = {
	.attach = dsi_host_attach,
	.detach = dsi_host_detach,
	.transfer = dsi_host_transfer,
};

static int dsi_display_mipi_host_init(struct dsi_display *display)
{
	int rc = 0;
	struct mipi_dsi_host *host = &display->host;

	host->dev = &display->pdev->dev;
	host->ops = &dsi_host_ops;

	rc = mipi_dsi_host_register(host);
	if (rc) {
		pr_err("[%s] failed to register mipi dsi host, rc=%d\n",
		       display->name, rc);
		goto error;
	}

error:
	return rc;
}
static int dsi_display_mipi_host_deinit(struct dsi_display *display)
{
	int rc = 0;
	struct mipi_dsi_host *host = &display->host;

	mipi_dsi_host_unregister(host);

	host->dev = NULL;
	host->ops = NULL;

	return rc;
}

static int dsi_display_clocks_deinit(struct dsi_display *display)
{
	int rc = 0;
	struct dsi_clk_link_set *src = &display->clock_info.src_clks;
	struct dsi_clk_link_set *mux = &display->clock_info.mux_clks;
	struct dsi_clk_link_set *shadow = &display->clock_info.shadow_clks;

	if (src->byte_clk) {
		devm_clk_put(&display->pdev->dev, src->byte_clk);
		src->byte_clk = NULL;
	}

	if (src->pixel_clk) {
		devm_clk_put(&display->pdev->dev, src->pixel_clk);
		src->pixel_clk = NULL;
	}

	if (mux->byte_clk) {
		devm_clk_put(&display->pdev->dev, mux->byte_clk);
		mux->byte_clk = NULL;
	}

	if (mux->pixel_clk) {
		devm_clk_put(&display->pdev->dev, mux->pixel_clk);
		mux->pixel_clk = NULL;
	}

	if (shadow->byte_clk) {
		devm_clk_put(&display->pdev->dev, shadow->byte_clk);
		shadow->byte_clk = NULL;
	}

	if (shadow->pixel_clk) {
		devm_clk_put(&display->pdev->dev, shadow->pixel_clk);
		shadow->pixel_clk = NULL;
	}

	return rc;
}

static bool dsi_display_check_prefix(const char *clk_prefix,
					const char *clk_name)
{
	return !!strnstr(clk_name, clk_prefix, strlen(clk_name));
}

static int dsi_display_get_clocks_count(struct dsi_display *display)
{
	if (display->fw)
		return dsi_parser_count_strings(display->parser_node,
			"qcom,dsi-select-clocks");
	else
		return of_property_count_strings(display->disp_node,
			"qcom,dsi-select-clocks");
}

static void dsi_display_get_clock_name(struct dsi_display *display,
					int index, const char **clk_name)
{
	if (display->fw)
		dsi_parser_read_string_index(display->parser_node,
			"qcom,dsi-select-clocks", index, clk_name);
	else
		of_property_read_string_index(display->disp_node,
			"qcom,dsi-select-clocks", index, clk_name);
}

static int dsi_display_clocks_init(struct dsi_display *display)
{
	int i, rc = 0, num_clk = 0;
	const char *clk_name;
	const char *src_byte = "src_byte", *src_pixel = "src_pixel";
	const char *mux_byte = "mux_byte", *mux_pixel = "mux_pixel";
	const char *cphy_byte = "cphy_byte", *cphy_pixel = "cphy_pixel";
	const char *shadow_byte = "shadow_byte", *shadow_pixel = "shadow_pixel";
	struct clk *dsi_clk;
	struct dsi_clk_link_set *src = &display->clock_info.src_clks;
	struct dsi_clk_link_set *mux = &display->clock_info.mux_clks;
	struct dsi_clk_link_set *cphy = &display->clock_info.cphy_clks;
	struct dsi_clk_link_set *shadow = &display->clock_info.shadow_clks;
	struct dsi_clk_link_set *xo = &display->clock_info.xo_clks;
	struct dsi_dyn_clk_caps *dyn_clk_caps = &(display->panel->dyn_clk_caps);

	num_clk = dsi_display_get_clocks_count(display);

	pr_debug("clk count=%d\n", num_clk);

	dsi_clk = devm_clk_get(&display->pdev->dev, "xo_clk");
	if (!IS_ERR_OR_NULL(dsi_clk))
		xo->byte_clk = xo->pixel_clk = dsi_clk;

	for (i = 0; i < num_clk; i++) {
		dsi_display_get_clock_name(display, i, &clk_name);

		pr_debug("clock name:%s\n", clk_name);

		dsi_clk = devm_clk_get(&display->pdev->dev, clk_name);
		if (IS_ERR_OR_NULL(dsi_clk)) {
			rc = PTR_ERR(dsi_clk);

			pr_err("failed to get %s, rc=%d\n", clk_name, rc);

			if (dsi_display_check_prefix(mux_byte, clk_name)) {
				mux->byte_clk = NULL;
				goto error;
			}
			if (dsi_display_check_prefix(mux_pixel, clk_name)) {
				mux->pixel_clk = NULL;
				goto error;
			}

			if (dsi_display_check_prefix(cphy_byte, clk_name)) {
				cphy->byte_clk = NULL;
				goto error;
			}
			if (dsi_display_check_prefix(cphy_pixel, clk_name)) {
				cphy->pixel_clk = NULL;
				goto error;
			}

			if (dyn_clk_caps->dyn_clk_support &&
				(display->panel->panel_mode ==
					 DSI_OP_VIDEO_MODE)) {

				if (dsi_display_check_prefix(src_byte,
							clk_name))
					src->byte_clk = NULL;
				if (dsi_display_check_prefix(src_pixel,
							clk_name))
					src->pixel_clk = NULL;
				if (dsi_display_check_prefix(shadow_byte,
							clk_name))
					shadow->byte_clk = NULL;
				if (dsi_display_check_prefix(shadow_pixel,
							clk_name))
					shadow->pixel_clk = NULL;

				dyn_clk_caps->dyn_clk_support = false;
			}
		}

		if (dsi_display_check_prefix(src_byte, clk_name)) {
			src->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(src_pixel, clk_name)) {
			src->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(cphy_byte, clk_name)) {
			cphy->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(cphy_pixel, clk_name)) {
			cphy->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(mux_byte, clk_name)) {
			mux->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(mux_pixel, clk_name)) {
			mux->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(shadow_byte, clk_name)) {
			shadow->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(shadow_pixel, clk_name)) {
			shadow->pixel_clk = dsi_clk;
			continue;
		}
	}

	return 0;
error:
	(void)dsi_display_clocks_deinit(display);
	return rc;
}

static int dsi_display_clk_ctrl_cb(void *priv,
	struct dsi_clk_ctrl_info clk_state_info)
{
	int rc = 0;
	struct dsi_display *display = NULL;
	void *clk_handle = NULL;

	if (!priv) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	display = priv;

	if (clk_state_info.client == DSI_CLK_REQ_MDP_CLIENT) {
		clk_handle = display->mdp_clk_handle;
	} else if (clk_state_info.client == DSI_CLK_REQ_DSI_CLIENT) {
		clk_handle = display->dsi_clk_handle;
	} else {
		pr_err("invalid clk handle, return error\n");
		return -EINVAL;
	}

	/*
	 * TODO: Wait for CMD_MDP_DONE interrupt if MDP client tries
	 * to turn off DSI clocks.
	 */
	rc = dsi_display_clk_ctrl(clk_handle,
		clk_state_info.clk_type, clk_state_info.clk_state);
	if (rc) {
		pr_err("[%s] failed to %d DSI %d clocks, rc=%d\n",
		       display->name, clk_state_info.clk_state,
		       clk_state_info.clk_type, rc);
		return rc;
	}
	return 0;
}

static void dsi_display_ctrl_isr_configure(struct dsi_display *display, bool en)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_isr_configure(ctrl->ctrl, en);
	}
}

int dsi_pre_clkoff_cb(void *priv,
			   enum dsi_clk_type clk,
			   enum dsi_lclk_type l_type,
			   enum dsi_clk_state new_state)
{
	int rc = 0, i;
	struct dsi_display *display = priv;
	struct dsi_display_ctrl *ctrl;

	if ((clk & DSI_LINK_CLK) && (new_state == DSI_CLK_OFF) &&
		(l_type & DSI_LINK_LP_CLK)) {
		/*
		 * If continuous clock is enabled then disable it
		 * before entering into ULPS Mode.
		 */
		if (display->panel->host_config.force_hs_clk_lane)
			_dsi_display_continuous_clk_ctrl(display, false);
		/*
		 * If ULPS feature is enabled, enter ULPS first.
		 * However, when blanking the panel, we should enter ULPS
		 * only if ULPS during suspend feature is enabled.
		 */
		if (!dsi_panel_initialized(display->panel)) {
			if (display->panel->ulps_suspend_enabled)
				rc = dsi_display_set_ulps(display, true);
		} else if (dsi_panel_ulps_feature_enabled(display->panel)) {
			rc = dsi_display_set_ulps(display, true);
		}
		if (rc)
			pr_err("%s: failed enable ulps, rc = %d\n",
			       __func__, rc);
	}

	if ((clk & DSI_LINK_CLK) && (new_state == DSI_CLK_OFF) &&
		(l_type & DSI_LINK_HS_CLK)) {
		/*
		 * PHY clock gating should be disabled before the PLL and the
		 * branch clocks are turned off. Otherwise, it is possible that
		 * the clock RCGs may not be turned off correctly resulting
		 * in clock warnings.
		 */
		rc = dsi_display_config_clk_gating(display, false);
		if (rc)
			pr_err("[%s] failed to disable clk gating, rc=%d\n",
					display->name, rc);
	}

	if ((clk & DSI_CORE_CLK) && (new_state == DSI_CLK_OFF)) {
		/*
		 * Enable DSI clamps only if entering idle power collapse or
		 * when ULPS during suspend is enabled..
		 */
		if (dsi_panel_initialized(display->panel) ||
			display->panel->ulps_suspend_enabled) {
			dsi_display_phy_idle_off(display);
			rc = dsi_display_set_clamp(display, true);
			if (rc)
				pr_err("%s: Failed to enable dsi clamps. rc=%d\n",
					__func__, rc);

			rc = dsi_display_phy_reset_config(display, false);
			if (rc)
				pr_err("%s: Failed to reset phy, rc=%d\n",
						__func__, rc);
		} else {
			/* Make sure that controller is not in ULPS state when
			 * the DSI link is not active.
			 */
			rc = dsi_display_set_ulps(display, false);
			if (rc)
				pr_err("%s: failed to disable ulps. rc=%d\n",
					__func__, rc);
		}
		/* dsi will not be able to serve irqs from here on */
		dsi_display_ctrl_irq_update(display, false);

		/* cache the MISR values */
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl)
				continue;
			dsi_ctrl_cache_misr(ctrl->ctrl);
		}

	}

	return rc;
}

int dsi_post_clkon_cb(void *priv,
			   enum dsi_clk_type clk,
			   enum dsi_lclk_type l_type,
			   enum dsi_clk_state curr_state)
{
	int rc = 0;
	struct dsi_display *display = priv;
	bool mmss_clamp = false;

	if ((clk & DSI_LINK_CLK) && (l_type & DSI_LINK_LP_CLK)) {
		mmss_clamp = display->clamp_enabled;
		/*
		 * controller setup is needed if coming out of idle
		 * power collapse with clamps enabled.
		 */
		if (mmss_clamp)
			dsi_display_ctrl_setup(display);

		/*
		 * Phy setup is needed if coming out of idle
		 * power collapse with clamps enabled.
		 */
		if (display->phy_idle_power_off || mmss_clamp)
			dsi_display_phy_idle_on(display, mmss_clamp);

		if (display->ulps_enabled && mmss_clamp) {
			/*
			 * ULPS Entry Request. This is needed if the lanes were
			 * in ULPS prior to power collapse, since after
			 * power collapse and reset, the DSI controller resets
			 * back to idle state and not ULPS. This ulps entry
			 * request will transition the state of the DSI
			 * controller to ULPS which will match the state of the
			 * DSI phy. This needs to be done prior to disabling
			 * the DSI clamps.
			 *
			 * Also, reset the ulps flag so that ulps_config
			 * function would reconfigure the controller state to
			 * ULPS.
			 */
			display->ulps_enabled = false;
			rc = dsi_display_set_ulps(display, true);
			if (rc) {
				pr_err("%s: Failed to enter ULPS. rc=%d\n",
					__func__, rc);
				goto error;
			}
		}

		rc = dsi_display_phy_reset_config(display, true);
		if (rc) {
			pr_err("%s: Failed to reset phy, rc=%d\n",
						__func__, rc);
			goto error;
		}

		rc = dsi_display_set_clamp(display, false);
		if (rc) {
			pr_err("%s: Failed to disable dsi clamps. rc=%d\n",
				__func__, rc);
			goto error;
		}
	}

	if ((clk & DSI_LINK_CLK) && (l_type & DSI_LINK_HS_CLK)) {
		/*
		 * Toggle the resync FIFO everytime clock changes, except
		 * when cont-splash screen transition is going on.
		 * Toggling resync FIFO during cont splash transition
		 * can lead to blinks on the display.
		 */
		if (!display->is_cont_splash_enabled)
			dsi_display_toggle_resync_fifo(display);

		if (display->ulps_enabled) {
			rc = dsi_display_set_ulps(display, false);
			if (rc) {
				pr_err("%s: failed to disable ulps, rc= %d\n",
				       __func__, rc);
				goto error;
			}
		}

		if (display->panel->host_config.force_hs_clk_lane)
			_dsi_display_continuous_clk_ctrl(display, true);

		rc = dsi_display_config_clk_gating(display, true);
		if (rc) {
			pr_err("[%s] failed to enable clk gating %d\n",
					display->name, rc);
			goto error;
		}
	}

	/* enable dsi to serve irqs */
	if (clk & DSI_CORE_CLK)
		dsi_display_ctrl_irq_update(display, true);

error:
	return rc;
}

int dsi_post_clkoff_cb(void *priv,
			    enum dsi_clk_type clk_type,
			    enum dsi_lclk_type l_type,
			    enum dsi_clk_state curr_state)
{
	int rc = 0;
	struct dsi_display *display = priv;

	if (!display) {
		pr_err("%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	if ((clk_type & DSI_CORE_CLK) &&
	    (curr_state == DSI_CLK_OFF)) {
		rc = dsi_display_phy_power_off(display);
		if (rc)
			pr_err("[%s] failed to power off PHY, rc=%d\n",
				   display->name, rc);

		rc = dsi_display_ctrl_power_off(display);
		if (rc)
			pr_err("[%s] failed to power DSI vregs, rc=%d\n",
				   display->name, rc);
	}
	return rc;
}

int dsi_pre_clkon_cb(void *priv,
			  enum dsi_clk_type clk_type,
			  enum dsi_lclk_type l_type,
			  enum dsi_clk_state new_state)
{
	int rc = 0;
	struct dsi_display *display = priv;

	if (!display) {
		pr_err("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	if ((clk_type & DSI_CORE_CLK) && (new_state == DSI_CLK_ON)) {
		/*
		 * Enable DSI core power
		 * 1.> PANEL_PM are controlled as part of
		 *     panel_power_ctrl. Needed not be handled here.
		 * 2.> CORE_PM are controlled by dsi clk manager.
		 * 3.> CTRL_PM need to be enabled/disabled
		 *     only during unblank/blank. Their state should
		 *     not be changed during static screen.
		 */

	  pr_debug("updating power states for ctrl and phy\n");
		rc = dsi_display_ctrl_power_on(display);
		if (rc) {
			pr_err("[%s] failed to power on dsi controllers, rc=%d\n",
				   display->name, rc);
			return rc;
		}

		rc = dsi_display_phy_power_on(display);
		if (rc) {
			pr_err("[%s] failed to power on dsi phy, rc = %d\n",
				   display->name, rc);
			return rc;
		}

		pr_debug("%s: Enable DSI core power\n", __func__);
	}

	return rc;
}

static void __set_lane_map_v2(u8 *lane_map_v2,
	enum dsi_phy_data_lanes lane0,
	enum dsi_phy_data_lanes lane1,
	enum dsi_phy_data_lanes lane2,
	enum dsi_phy_data_lanes lane3)
{
	lane_map_v2[DSI_LOGICAL_LANE_0] = lane0;
	lane_map_v2[DSI_LOGICAL_LANE_1] = lane1;
	lane_map_v2[DSI_LOGICAL_LANE_2] = lane2;
	lane_map_v2[DSI_LOGICAL_LANE_3] = lane3;
}

static int dsi_display_parse_lane_map(struct dsi_display *display)
{
	int rc = 0, i = 0;
	const char *data;
	u8 temp[DSI_LANE_MAX - 1];

	if (!display) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	/* lane-map-v2 supersedes lane-map-v1 setting */
	rc = of_property_read_u8_array(display->pdev->dev.of_node,
		"qcom,lane-map-v2", temp, (DSI_LANE_MAX - 1));
	if (!rc) {
		for (i = DSI_LOGICAL_LANE_0; i < (DSI_LANE_MAX - 1); i++)
			display->lane_map.lane_map_v2[i] = BIT(temp[i]);
		return 0;
	} else if (rc != EINVAL) {
		pr_debug("Incorrect mapping, configure default\n");
		goto set_default;
	}

	/* lane-map older version, for DSI controller version < 2.0 */
	data = of_get_property(display->pdev->dev.of_node,
		"qcom,lane-map", NULL);
	if (!data)
		goto set_default;

	if (!strcmp(data, "lane_map_3012")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_3012;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_0);
	} else if (!strcmp(data, "lane_map_2301")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_2301;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_1);
	} else if (!strcmp(data, "lane_map_1230")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_1230;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_2);
	} else if (!strcmp(data, "lane_map_0321")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_0321;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_1);
	} else if (!strcmp(data, "lane_map_1032")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_1032;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_2);
	} else if (!strcmp(data, "lane_map_2103")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_2103;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_3);
	} else if (!strcmp(data, "lane_map_3210")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_3210;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_0);
	} else {
		pr_warn("%s: invalid lane map %s specified. defaulting to lane_map0123\n",
			__func__, data);
		goto set_default;
	}
	return 0;

set_default:
	/* default lane mapping */
	__set_lane_map_v2(display->lane_map.lane_map_v2, DSI_PHYSICAL_LANE_0,
		DSI_PHYSICAL_LANE_1, DSI_PHYSICAL_LANE_2, DSI_PHYSICAL_LANE_3);
	display->lane_map.lane_map_v1 = DSI_LANE_MAP_0123;
	return 0;
}

static int dsi_display_get_phandle_index(
			struct dsi_display *display,
			const char *propname, int count, int index)
{
	struct device_node *disp_node = display->disp_node;
	u32 *val = NULL;
	int rc = 0;

	val = kcalloc(count, sizeof(*val), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(val)) {
		rc = -ENOMEM;
		goto end;
	}

	if (index >= count)
		goto end;

	if (display->fw)
		rc = dsi_parser_read_u32_array(display->parser_node,
			propname, val, count);
	else
		rc = of_property_read_u32_array(disp_node, propname,
			val, count);
	if (rc)
		goto end;

	rc = val[index];

	pr_debug("%s index=%d\n", propname, rc);
end:
	kfree(val);
	return rc;
}

static int dsi_display_get_phandle_count(struct dsi_display *display,
			const char *propname)
{
	if (display->fw)
		return dsi_parser_count_u32_elems(display->parser_node,
				propname);
	else
		return of_property_count_u32_elems(display->disp_node,
				propname);
}

static int dsi_display_parse_dt(struct dsi_display *display)
{
	int i, rc = 0;
	u32 phy_count = 0;
	struct device_node *of_node = display->pdev->dev.of_node;
	struct device_node *disp_node = display->disp_node;

	display->ctrl_count = dsi_display_get_phandle_count(display,
				"qcom,dsi-ctrl-num");
	phy_count = dsi_display_get_phandle_count(display,
				"qcom,dsi-ctrl-num");

	pr_debug("ctrl count=%d, phy count=%d\n",
			display->ctrl_count, phy_count);

	if (!phy_count || !display->ctrl_count) {
		pr_err("no ctrl/phys found\n");
		rc = -ENODEV;
		goto error;
	}

	if (phy_count != display->ctrl_count) {
		pr_err("different ctrl and phy counts\n");
		rc = -ENODEV;
		goto error;
	}

	display_for_each_ctrl(i, display) {
		struct dsi_display_ctrl *ctrl = &display->ctrl[i];
		int index;

		index = dsi_display_get_phandle_index(display,
				"qcom,dsi-ctrl-num", display->ctrl_count, i);
		ctrl->ctrl_of_node = of_parse_phandle(of_node,
				"qcom,dsi-ctrl", index);
		of_node_put(ctrl->ctrl_of_node);

		index = dsi_display_get_phandle_index(display,
				"qcom,dsi-phy-num", display->ctrl_count, i);
		ctrl->phy_of_node = of_parse_phandle(of_node,
				"qcom,dsi-phy", index);
		of_node_put(ctrl->phy_of_node);
	}

	display->panel_of = of_parse_phandle(disp_node, "qcom,dsi-panel", 0);
	if (!display->panel_of) {
		pr_err("No Panel device present\n");
		rc = -ENODEV;
		goto error;
	}

	/* Parse TE data */
	dsi_display_parse_te_data(display);

	/* Parse all external bridges config, endpoint0 */
	for (i = 0; i < MAX_EXT_BRIDGE_PORT_CONFIG; i++) {
		display->ext_bridge[i].node_of =
			of_graph_get_remote_node(of_node, 0, i);
		if (!display->ext_bridge[i].node_of)
			break;
	}

	pr_debug("success\n");
error:
	return rc;
}

static int dsi_display_res_init(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		ctrl->ctrl = dsi_ctrl_get(ctrl->ctrl_of_node);
		if (IS_ERR_OR_NULL(ctrl->ctrl)) {
			rc = PTR_ERR(ctrl->ctrl);
			pr_err("failed to get dsi controller, rc=%d\n", rc);
			ctrl->ctrl = NULL;
			goto error_ctrl_put;
		}

		ctrl->phy = dsi_phy_get(ctrl->phy_of_node);
		if (IS_ERR_OR_NULL(ctrl->phy)) {
			rc = PTR_ERR(ctrl->phy);
			pr_err("failed to get phy controller, rc=%d\n", rc);
			dsi_ctrl_put(ctrl->ctrl);
			ctrl->phy = NULL;
			goto error_ctrl_put;
		}
	}

	display->panel = dsi_panel_get(&display->pdev->dev,
				display->panel_of,
				display->parser_node,
				display->dsi_type,
				display->cmdline_topology);
	if (IS_ERR_OR_NULL(display->panel)) {
		rc = PTR_ERR(display->panel);
		pr_err("failed to get panel, rc=%d\n", rc);
		display->panel = NULL;
		goto error_ctrl_put;
	}

	display_for_each_ctrl(i, display) {
		struct msm_dsi_phy *phy = display->ctrl[i].phy;

		phy->cfg.force_clk_lane_hs =
			display->panel->host_config.force_hs_clk_lane;
		phy->cfg.phy_type =
			display->panel->host_config.phy_type;
	}

	rc = dsi_display_parse_lane_map(display);
	if (rc) {
		pr_err("Lane map not found, rc=%d\n", rc);
		goto error_ctrl_put;
	}

	rc = dsi_display_clocks_init(display);
	if (rc) {
		pr_err("Failed to parse clock data, rc=%d\n", rc);
		goto error_ctrl_put;
	}

	if (display->panel->hw_type == DSI_PANEL_SAMSUNG_S6E3HC2) {
		INIT_DELAYED_WORK(&display->panel->gamma_read_work, dsi_display_gamma_read_work);
		pr_err("INIT_DELAYED_WORK: dsi_display_gamma_read_work\n");
	}

	return 0;
error_ctrl_put:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		dsi_ctrl_put(ctrl->ctrl);
		dsi_phy_put(ctrl->phy);
	}
	return rc;
}

static int dsi_display_res_deinit(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	rc = dsi_display_clocks_deinit(display);
	if (rc)
		pr_err("clocks deinit failed, rc=%d\n", rc);

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_put(ctrl->phy);
		dsi_ctrl_put(ctrl->ctrl);
	}

	if (display->panel)
		dsi_panel_put(display->panel);

	return rc;
}

static int dsi_display_validate_mode_set(struct dsi_display *display,
					 struct dsi_display_mode *mode,
					 u32 flags)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/*
	 * To set a mode:
	 * 1. Controllers should be turned off.
	 * 2. Link clocks should be off.
	 * 3. Phy should be disabled.
	 */

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if ((ctrl->power_state > DSI_CTRL_POWER_VREG_ON) ||
		    (ctrl->phy_enabled)) {
			rc = -EINVAL;
			goto error;
		}
	}

error:
	return rc;
}

static bool dsi_display_is_seamless_dfps_possible(
		const struct dsi_display *display,
		const struct dsi_display_mode *tgt,
		const enum dsi_dfps_type dfps_type)
{
	struct dsi_display_mode *cur;

	if (!display || !tgt || !display->panel) {
		pr_err("Invalid params\n");
		return false;
	}

	cur = display->panel->cur_mode;

	if (cur->timing.h_active != tgt->timing.h_active) {
		pr_debug("timing.h_active differs %d %d\n",
				cur->timing.h_active, tgt->timing.h_active);
		return false;
	}

	if (cur->timing.h_back_porch != tgt->timing.h_back_porch) {
		pr_debug("timing.h_back_porch differs %d %d\n",
				cur->timing.h_back_porch,
				tgt->timing.h_back_porch);
		return false;
	}

	if (cur->timing.h_sync_width != tgt->timing.h_sync_width) {
		pr_debug("timing.h_sync_width differs %d %d\n",
				cur->timing.h_sync_width,
				tgt->timing.h_sync_width);
		return false;
	}

	if (cur->timing.h_front_porch != tgt->timing.h_front_porch) {
		pr_debug("timing.h_front_porch differs %d %d\n",
				cur->timing.h_front_porch,
				tgt->timing.h_front_porch);
		if (dfps_type != DSI_DFPS_IMMEDIATE_HFP)
			return false;
	}

	if (cur->timing.h_skew != tgt->timing.h_skew) {
		pr_debug("timing.h_skew differs %d %d\n",
				cur->timing.h_skew,
				tgt->timing.h_skew);
		return false;
	}

	/* skip polarity comparison */

	if (cur->timing.v_active != tgt->timing.v_active) {
		pr_debug("timing.v_active differs %d %d\n",
				cur->timing.v_active,
				tgt->timing.v_active);
		return false;
	}

	if (cur->timing.v_back_porch != tgt->timing.v_back_porch) {
		pr_debug("timing.v_back_porch differs %d %d\n",
				cur->timing.v_back_porch,
				tgt->timing.v_back_porch);
		return false;
	}

	if (cur->timing.v_sync_width != tgt->timing.v_sync_width) {
		pr_debug("timing.v_sync_width differs %d %d\n",
				cur->timing.v_sync_width,
				tgt->timing.v_sync_width);
		return false;
	}

	if (cur->timing.v_front_porch != tgt->timing.v_front_porch) {
		pr_debug("timing.v_front_porch differs %d %d\n",
				cur->timing.v_front_porch,
				tgt->timing.v_front_porch);
		if (dfps_type != DSI_DFPS_IMMEDIATE_VFP)
			return false;
	}

	/* skip polarity comparison */

	if (cur->timing.refresh_rate == tgt->timing.refresh_rate)
		pr_debug("timing.refresh_rate identical %d %d\n",
				cur->timing.refresh_rate,
				tgt->timing.refresh_rate);

	if (cur->pixel_clk_khz != tgt->pixel_clk_khz)
		pr_debug("pixel_clk_khz differs %d %d\n",
				cur->pixel_clk_khz, tgt->pixel_clk_khz);

	if (cur->dsi_mode_flags != tgt->dsi_mode_flags)
		pr_debug("flags differs %d %d\n",
				cur->dsi_mode_flags, tgt->dsi_mode_flags);

	return true;
}

static int dsi_display_update_dsi_bitrate(struct dsi_display *display,
					  u32 bit_clk_rate)
{
	int rc = 0;
	int i;

	pr_debug("%s:bit rate:%d\n", __func__, bit_clk_rate);
	if (!display->panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (bit_clk_rate == 0) {
		pr_err("Invalid bit clock rate\n");
		return -EINVAL;
	}

	display->config.bit_clk_rate_hz = bit_clk_rate;

	display_for_each_ctrl(i, display) {
		struct dsi_display_ctrl *dsi_disp_ctrl = &display->ctrl[i];
		struct dsi_ctrl *ctrl = dsi_disp_ctrl->ctrl;
		u32 num_of_lanes = 0, bpp;
		u64 bit_rate, pclk_rate, bit_rate_per_lane, byte_clk_rate,
						byte_intf_clk_rate;
		u32 bits_per_symbol = 16, num_of_symbols = 7; /* For Cphy */
		struct dsi_host_common_cfg *host_cfg;

		mutex_lock(&ctrl->ctrl_lock);

		host_cfg = &display->panel->host_config;
		if (host_cfg->data_lanes & DSI_DATA_LANE_0)
			num_of_lanes++;
		if (host_cfg->data_lanes & DSI_DATA_LANE_1)
			num_of_lanes++;
		if (host_cfg->data_lanes & DSI_DATA_LANE_2)
			num_of_lanes++;
		if (host_cfg->data_lanes & DSI_DATA_LANE_3)
			num_of_lanes++;

		if (num_of_lanes == 0) {
			pr_err("Invalid lane count\n");
			rc = -EINVAL;
			goto error;
		}

		bpp = dsi_pixel_format_to_bpp(host_cfg->dst_format);

		bit_rate = display->config.bit_clk_rate_hz * num_of_lanes;

		pclk_rate = bit_rate;
		do_div(pclk_rate, bpp);
		if (host_cfg->phy_type == DSI_PHY_TYPE_DPHY) {
			bit_rate_per_lane = bit_rate;
			do_div(bit_rate_per_lane, num_of_lanes);
			byte_clk_rate = bit_rate_per_lane;
			do_div(byte_clk_rate, 8);
			byte_intf_clk_rate = byte_clk_rate;
			do_div(byte_intf_clk_rate, 2);
		} else {
			do_div(bit_rate, bits_per_symbol);
			bit_rate *= num_of_symbols;
			bit_rate_per_lane = bit_rate;
			do_div(bit_rate_per_lane, num_of_lanes);
			byte_clk_rate = bit_rate_per_lane;
			do_div(byte_clk_rate, 7);
			/* For CPHY, byte_intf_clk is same as byte_clk */
			byte_intf_clk_rate = byte_clk_rate;
		}

		pr_debug("bit_clk_rate = %llu, bit_clk_rate_per_lane = %llu\n",
			 bit_rate, bit_rate_per_lane);
		pr_debug("byte_clk_rate = %llu, byte_intf_clk_rate = %llu\n",
			byte_clk_rate, byte_intf_clk_rate);
		pr_debug("pclk_rate = %llu\n", pclk_rate);

		ctrl->clk_freq.byte_clk_rate = byte_clk_rate;
		ctrl->clk_freq.byte_intf_clk_rate = byte_intf_clk_rate;
		ctrl->clk_freq.pix_clk_rate = pclk_rate;
		rc = dsi_clk_set_link_frequencies(display->dsi_clk_handle,
			ctrl->clk_freq, ctrl->cell_index);
		if (rc) {
			pr_err("Failed to update link frequencies\n");
			goto error;
		}

		ctrl->host_config.bit_clk_rate_hz = bit_clk_rate;
error:
		mutex_unlock(&ctrl->ctrl_lock);

		/* TODO: recover ctrl->clk_freq in case of failure */
		if (rc)
			return rc;
	}

	return 0;
}

static void _dsi_display_calc_pipe_delay(struct dsi_display *display,
				    struct dsi_dyn_clk_delay *delay,
				    struct dsi_display_mode *mode)
{
	u32 esc_clk_rate_hz;
	u32 pclk_to_esc_ratio, byte_to_esc_ratio, hr_bit_to_esc_ratio;
	u32 hsync_period = 0;
	struct dsi_display_ctrl *m_ctrl;
	struct dsi_ctrl *dsi_ctrl;
	struct dsi_phy_cfg *cfg;

	m_ctrl = &display->ctrl[display->clk_master_idx];
	dsi_ctrl = m_ctrl->ctrl;

	cfg = &(m_ctrl->phy->cfg);

	esc_clk_rate_hz = dsi_ctrl->clk_freq.esc_clk_rate * 1000;
	pclk_to_esc_ratio = ((dsi_ctrl->clk_freq.pix_clk_rate * 1000) /
			     esc_clk_rate_hz);
	byte_to_esc_ratio = ((dsi_ctrl->clk_freq.byte_clk_rate * 1000) /
			     esc_clk_rate_hz);
	hr_bit_to_esc_ratio = ((dsi_ctrl->clk_freq.byte_clk_rate * 4 * 1000) /
					esc_clk_rate_hz);

	hsync_period = DSI_H_TOTAL_DSC(&mode->timing);
	delay->pipe_delay = (hsync_period + 1) / pclk_to_esc_ratio;
	if (!display->panel->video_config.eof_bllp_lp11_en)
		delay->pipe_delay += (17 / pclk_to_esc_ratio) +
			((21 + (display->config.common_config.t_clk_pre + 1) +
			  (display->config.common_config.t_clk_post + 1)) /
			 byte_to_esc_ratio) +
			((((cfg->timing.lane_v3[8] >> 1) + 1) +
			((cfg->timing.lane_v3[6] >> 1) + 1) +
			((cfg->timing.lane_v3[3] * 4) +
			 (cfg->timing.lane_v3[5] >> 1) + 1) +
			((cfg->timing.lane_v3[7] >> 1) + 1) +
			((cfg->timing.lane_v3[1] >> 1) + 1) +
			((cfg->timing.lane_v3[4] >> 1) + 1)) /
			 hr_bit_to_esc_ratio);

	delay->pipe_delay2 = 0;
	if (display->panel->host_config.force_hs_clk_lane)
		delay->pipe_delay2 = (6 / byte_to_esc_ratio) +
			((((cfg->timing.lane_v3[1] >> 1) + 1) +
			  ((cfg->timing.lane_v3[4] >> 1) + 1)) /
			 hr_bit_to_esc_ratio);

	/* 130 us pll delay recommended by h/w doc */
	delay->pll_delay = ((130 * esc_clk_rate_hz) / 1000000) * 2;
}

static int _dsi_display_dyn_update_clks(struct dsi_display *display,
					struct link_clk_freq *bkp_freq)
{
	int rc = 0, i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	m_ctrl = &display->ctrl[display->clk_master_idx];

	dsi_clk_prepare_enable(&display->clock_info.src_clks);

	rc = dsi_clk_update_parent(&display->clock_info.shadow_clks,
			      &display->clock_info.mux_clks);
	if (rc) {
		pr_err("failed update mux parent to shadow\n");
		goto exit;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		rc = dsi_clk_set_byte_clk_rate(display->dsi_clk_handle,
				ctrl->ctrl->clk_freq.byte_clk_rate,
				ctrl->ctrl->clk_freq.byte_intf_clk_rate, i);
		if (rc) {
			pr_err("failed to set byte rate for index:%d\n", i);
			goto recover_byte_clk;
		}
		rc = dsi_clk_set_pixel_clk_rate(display->dsi_clk_handle,
				   ctrl->ctrl->clk_freq.pix_clk_rate, i);
		if (rc) {
			pr_err("failed to set pix rate for index:%d\n", i);
			goto recover_pix_clk;
		}
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (ctrl == m_ctrl)
			continue;
		dsi_phy_dynamic_refresh_trigger(ctrl->phy, false);
	}
	dsi_phy_dynamic_refresh_trigger(m_ctrl->phy, true);

	/* wait for dynamic refresh done */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_wait4dynamic_refresh_done(ctrl->ctrl);
		if (rc) {
			pr_err("wait4dynamic refresh failed for dsi:%d\n", i);
			goto recover_pix_clk;
		} else {
			pr_info("dynamic refresh done on dsi: %s\n",
				i ? "slave" : "master");
		}
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_dynamic_refresh_clear(ctrl->phy);
	}

	rc = dsi_clk_update_parent(&display->clock_info.src_clks,
			      &display->clock_info.mux_clks);
	if (rc)
		pr_err("could not switch back to src clks %d\n", rc);

	dsi_clk_disable_unprepare(&display->clock_info.src_clks);

	return rc;

recover_pix_clk:
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		dsi_clk_set_pixel_clk_rate(display->dsi_clk_handle,
					   bkp_freq->pix_clk_rate, i);
	}

recover_byte_clk:
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		dsi_clk_set_byte_clk_rate(display->dsi_clk_handle,
					  bkp_freq->byte_clk_rate,
					  bkp_freq->byte_intf_clk_rate, i);
	}

exit:
	dsi_clk_disable_unprepare(&display->clock_info.src_clks);

	return rc;
}

static int dsi_display_dynamic_clk_switch_vid(struct dsi_display *display,
					  struct dsi_display_mode *mode)
{
	int rc = 0, mask, i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_dyn_clk_delay delay;
	struct link_clk_freq bkp_freq;

	dsi_panel_acquire_panel_lock(display->panel);

	m_ctrl = &display->ctrl[display->clk_master_idx];

	dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS, DSI_CLK_ON);

	/* mask PLL unlock, FIFO overflow and underflow errors */
	mask = BIT(DSI_PLL_UNLOCK_ERR) | BIT(DSI_FIFO_UNDERFLOW) |
		BIT(DSI_FIFO_OVERFLOW);
	dsi_display_mask_ctrl_error_interrupts(display, mask, true);

	/* update the phy timings based on new mode */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_update_phy_timings(ctrl->phy, &display->config);
	}

	/* back up existing rates to handle failure case */
	bkp_freq.byte_clk_rate = m_ctrl->ctrl->clk_freq.byte_clk_rate;
	bkp_freq.byte_intf_clk_rate = m_ctrl->ctrl->clk_freq.byte_intf_clk_rate;
	bkp_freq.pix_clk_rate = m_ctrl->ctrl->clk_freq.pix_clk_rate;
	bkp_freq.esc_clk_rate = m_ctrl->ctrl->clk_freq.esc_clk_rate;

	rc = dsi_display_update_dsi_bitrate(display, mode->timing.clk_rate_hz);
	if (rc) {
		pr_err("failed set link frequencies %d\n", rc);
		goto exit;
	}

	/* calculate pipe delays */
	_dsi_display_calc_pipe_delay(display, &delay, mode);

	/* configure dynamic refresh ctrl registers */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->phy)
			continue;
		if (ctrl == m_ctrl)
			dsi_phy_config_dynamic_refresh(ctrl->phy, &delay, true);
		else
			dsi_phy_config_dynamic_refresh(ctrl->phy, &delay,
						       false);
	}

	rc = _dsi_display_dyn_update_clks(display, &bkp_freq);

exit:
	dsi_display_mask_ctrl_error_interrupts(display, mask, false);

	dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
			     DSI_CLK_OFF);

	/* store newly calculated phy timings in mode private info */
	dsi_phy_dyn_refresh_cache_phy_timings(m_ctrl->phy,
					      mode->priv_info->phy_timing_val,
					      mode->priv_info->phy_timing_len);

	dsi_panel_release_panel_lock(display->panel);

	return rc;
}

static int dsi_display_dynamic_clk_configure_cmd(struct dsi_display *display,
		int clk_rate)
{
	int rc = 0;

	if (clk_rate <= 0) {
		pr_err("%s: bitrate should be greater than 0\n", __func__);
		return -EINVAL;
	}

	if (clk_rate == display->cached_clk_rate) {
		pr_info("%s: ignore duplicated DSI clk setting\n", __func__);
		return rc;
	}

	display->cached_clk_rate = clk_rate;

	rc = dsi_display_update_dsi_bitrate(display, clk_rate);
	if (!rc) {
		pr_info("%s: bit clk is ready to be configured to '%d'\n",
				__func__, clk_rate);
		atomic_set(&display->clkrate_change_pending, 1);
	} else {
		pr_err("%s: Failed to prepare to configure '%d'. rc = %d\n",
				__func__, clk_rate, rc);
		/* Caching clock failed, so don't go on doing so. */
		atomic_set(&display->clkrate_change_pending, 0);
		display->cached_clk_rate = 0;
	}

	return rc;
}

static int dsi_display_dfps_update(struct dsi_display *display,
				   struct dsi_display_mode *dsi_mode)
{
	struct dsi_mode_info *timing;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_display_mode *panel_mode;
	struct dsi_dfps_capabilities dfps_caps;
	int rc = 0;
	int i = 0;

	if (!display || !dsi_mode || !display->panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}
	timing = &dsi_mode->timing;

	dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (!dfps_caps.dfps_support) {
		pr_err("dfps not supported\n");
		return -ENOTSUPP;
	}

	if (dfps_caps.type == DSI_DFPS_IMMEDIATE_CLK) {
		pr_err("dfps clock method not supported\n");
		return -ENOTSUPP;
	}

	/* For split DSI, update the clock master first */

	pr_debug("configuring seamless dynamic fps\n\n");
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	m_ctrl = &display->ctrl[display->clk_master_idx];
	rc = dsi_ctrl_async_timing_update(m_ctrl->ctrl, timing);
	if (rc) {
		pr_err("[%s] failed to dfps update host_%d, rc=%d\n",
				display->name, i, rc);
		goto error;
	}

	/* Update the rest of the controllers */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_async_timing_update(ctrl->ctrl, timing);
		if (rc) {
			pr_err("[%s] failed to dfps update host_%d, rc=%d\n",
					display->name, i, rc);
			goto error;
		}
	}

	panel_mode = display->panel->cur_mode;
	memcpy(panel_mode, dsi_mode, sizeof(*panel_mode));
	/*
	 * dsi_mode_flags flags are used to communicate with other drm driver
	 * components, and are transient. They aren't inherently part of the
	 * display panel's mode and shouldn't be saved into the cached currently
	 * active mode.
	 */
	panel_mode->dsi_mode_flags = 0;

error:
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

static int dsi_display_dfps_calc_front_porch(
		u32 old_fps,
		u32 new_fps,
		u32 a_total,
		u32 b_total,
		u32 b_fp,
		u32 *b_fp_out)
{
	s32 b_fp_new;
	int add_porches, diff;

	if (!b_fp_out) {
		pr_err("Invalid params");
		return -EINVAL;
	}

	if (!a_total || !new_fps) {
		pr_err("Invalid pixel total or new fps in mode request\n");
		return -EINVAL;
	}

	/*
	 * Keep clock, other porches constant, use new fps, calc front porch
	 * new_vtotal = old_vtotal * (old_fps / new_fps )
	 * new_vfp - old_vfp = new_vtotal - old_vtotal
	 * new_vfp = old_vfp + old_vtotal * ((old_fps - new_fps)/ new_fps)
	 */
	diff = abs(old_fps - new_fps);
	add_porches = mult_frac(b_total, diff, new_fps);

	if (old_fps > new_fps)
		b_fp_new = b_fp + add_porches;
	else
		b_fp_new = b_fp - add_porches;

	pr_debug("fps %u a %u b %u b_fp %u new_fp %d\n",
			new_fps, a_total, b_total, b_fp, b_fp_new);

	if (b_fp_new < 0) {
		pr_err("Invalid new_hfp calcluated%d\n", b_fp_new);
		return -EINVAL;
	}

	/**
	 * TODO: To differentiate from clock method when communicating to the
	 * other components, perhaps we should set clk here to original value
	 */
	*b_fp_out = b_fp_new;

	return 0;
}

/**
 * dsi_display_get_dfps_timing() - Get the new dfps values.
 * @display:         DSI display handle.
 * @adj_mode:        Mode value structure to be changed.
 *                   It contains old timing values and latest fps value.
 *                   New timing values are updated based on new fps.
 * @curr_refresh_rate:  Current fps rate.
 *                      If zero , current fps rate is taken from
 *                      display->panel->cur_mode.
 * Return: error code.
 */
static int dsi_display_get_dfps_timing(struct dsi_display *display,
			struct dsi_display_mode *adj_mode,
				u32 curr_refresh_rate)
{
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_display_mode per_ctrl_mode;
	struct dsi_mode_info *timing;
	struct dsi_ctrl *m_ctrl;
	u32 overlap_pixels = 0;

	int rc = 0;

	if (!display || !adj_mode) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}
	m_ctrl = display->ctrl[display->clk_master_idx].ctrl;


	dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (!dfps_caps.dfps_support) {
		pr_err("dfps not supported by panel\n");
		return -EINVAL;
	}

	per_ctrl_mode = *adj_mode;
	adjust_timing_by_ctrl_count(display, &per_ctrl_mode);

	if (!curr_refresh_rate) {
		if (!dsi_display_is_seamless_dfps_possible(display,
				&per_ctrl_mode, dfps_caps.type)) {
			pr_err("seamless dynamic fps not supported for mode\n");
			return -EINVAL;
		}
		if (display->panel->cur_mode) {
			curr_refresh_rate =
				display->panel->cur_mode->timing.refresh_rate;
		} else {
			pr_err("cur_mode is not initialized\n");
			return -EINVAL;
		}
	}
	/* TODO: Remove this direct reference to the dsi_ctrl */
	timing = &per_ctrl_mode.timing;
	overlap_pixels = per_ctrl_mode.priv_info->overlap_pixels;

	switch (dfps_caps.type) {
	case DSI_DFPS_IMMEDIATE_VFP:
		rc = dsi_display_dfps_calc_front_porch(
				curr_refresh_rate,
				timing->refresh_rate,
				DSI_H_TOTAL_DSC(timing),
				DSI_V_TOTAL(timing),
				timing->v_front_porch,
				&adj_mode->timing.v_front_porch);
		break;

	case DSI_DFPS_IMMEDIATE_HFP:
		rc = dsi_display_dfps_calc_front_porch(
				curr_refresh_rate,
				timing->refresh_rate,
				DSI_V_TOTAL(timing),
				DSI_H_TOTAL_DSC(timing) + overlap_pixels,
				timing->h_front_porch,
				&adj_mode->timing.h_front_porch);
		if (!rc)
			adj_mode->timing.h_front_porch *= display->ctrl_count;
		break;

	default:
		pr_err("Unsupported DFPS mode %d\n", dfps_caps.type);
		rc = -ENOTSUPP;
	}

	return rc;
}

static bool dsi_display_validate_mode_seamless(struct dsi_display *display,
		struct dsi_display_mode *adj_mode)
{
	int rc = 0;

	if (!display || !adj_mode) {
		pr_err("Invalid params\n");
		return false;
	}

	/* Currently the only seamless transition is dynamic fps */
	rc = dsi_display_get_dfps_timing(display, adj_mode, 0);
	if (rc) {
		pr_debug("Dynamic FPS not supported for seamless\n");
	} else {
		pr_debug("Mode switch is seamless Dynamic FPS\n");
		adj_mode->dsi_mode_flags |= DSI_MODE_FLAG_DFPS |
				DSI_MODE_FLAG_VBLANK_PRE_MODESET;
	}

	return rc;
}

static int dsi_display_set_mode_sub(struct dsi_display *display,
				    struct dsi_display_mode *mode,
				    u32 flags)
{
	int rc = 0, clk_rate = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_display_mode_priv_info *priv_info;

	priv_info = mode->priv_info;
	if (!priv_info) {
		pr_err("[%s] failed to get private info of the display mode",
			display->name);
		return -EINVAL;
	}

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_POMS) {
		display->config.panel_mode = mode->panel_mode;
		display->panel->panel_mode = mode->panel_mode;
	}

	rc = dsi_panel_get_host_cfg_for_mode(display->panel,
					     mode,
					     &display->config);
	if (rc) {
		pr_err("[%s] failed to get host config for mode, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	memcpy(&display->config.lane_map, &display->lane_map,
	       sizeof(display->lane_map));

	if (mode->dsi_mode_flags &
			(DSI_MODE_FLAG_DFPS | DSI_MODE_FLAG_VRR)) {
		rc = dsi_display_dfps_update(display, mode);
		if (rc) {
			pr_err("[%s]DSI dfps update failed, rc=%d\n",
					display->name, rc);
			goto error;
		}
	} else if (mode->dsi_mode_flags & DSI_MODE_FLAG_DYN_CLK) {
		if (display->panel->panel_mode == DSI_OP_VIDEO_MODE) {
			rc = dsi_display_dynamic_clk_switch_vid(display, mode);
			if (rc)
				pr_err("dynamic clk change failed %d\n", rc);
			/*
			 * skip rest of the opearations since
			 * dsi_display_dynamic_clk_switch_vid() already takes
			 * care of them.
			 */
			return rc;
		} else if (display->panel->panel_mode == DSI_OP_CMD_MODE) {
			clk_rate = mode->timing.clk_rate_hz;
			rc = dsi_display_dynamic_clk_configure_cmd(display,
					clk_rate);
			if (rc) {
				pr_err("Failed to configure dynamic clk\n");
				return rc;
			}
		}
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_update_host_config(ctrl->ctrl, &display->config,
				mode->dsi_mode_flags, display->dsi_clk_handle);
		if (rc) {
			pr_err("[%s] failed to update ctrl config, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	if ((mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) &&
			(display->panel->panel_mode == DSI_OP_CMD_MODE))
		atomic_set(&display->clkrate_change_pending, 1);


	if (priv_info->phy_timing_len) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			 rc = dsi_phy_set_timing_params(ctrl->phy,
				priv_info->phy_timing_val,
				priv_info->phy_timing_len);
			if (rc)
				pr_err("failed to add DSI PHY timing params");
		}
	}
error:
	return rc;
}

/**
 * _dsi_display_dev_init - initializes the display device
 * Initialization will acquire references to the resources required for the
 * display hardware to function.
 * @display:         Handle to the display
 * Returns:          Zero on success
 */
static int _dsi_display_dev_init(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("invalid display\n");
		return -EINVAL;
	}

	if (!display->disp_node)
		return 0;

	mutex_lock(&display->display_lock);

	display->display_type = of_get_property(display->disp_node,
					"qcom,display-type", NULL);
	if (!display->display_type)
		display->display_type = "unknown";

	display->parser = dsi_parser_get(&display->pdev->dev);
	if (display->fw && display->parser)
		display->parser_node = dsi_parser_get_head_node(
				display->parser, display->fw->data,
				display->fw->size);

	rc = dsi_display_parse_dt(display);
	if (rc) {
		pr_err("[%s] failed to parse dt, rc=%d\n", display->name, rc);
		goto error;
	}

	rc = dsi_display_res_init(display);
	if (rc) {
		pr_err("[%s] failed to initialize resources, rc=%d\n",
		       display->name, rc);
		goto error;
	}
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

/**
 * _dsi_display_dev_deinit - deinitializes the display device
 * All the resources acquired during device init will be released.
 * @display:        Handle to the display
 * Returns:         Zero on success
 */
static int _dsi_display_dev_deinit(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("invalid display\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	rc = dsi_display_res_deinit(display);
	if (rc)
		pr_err("[%s] failed to deinitialize resource, rc=%d\n",
		       display->name, rc);

	mutex_unlock(&display->display_lock);

	return rc;
}

/**
 * dsi_display_cont_splash_config() - Initialize resources for continuous splash
 * @dsi_display:    Pointer to dsi display
 * Returns:     Zero on success
 */
int dsi_display_cont_splash_config(void *dsi_display)
{
	struct dsi_display *display = dsi_display;
	int rc = 0;

	/* Vote for gdsc required to read register address space */
	if (!display) {
		pr_err("invalid input display param\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	/* Vote for gdsc required to read register address space */
	display->cont_splash_client = sde_power_client_create(display->phandle,
						"cont_splash_client");
	rc = sde_power_resource_enable(display->phandle,
			display->cont_splash_client, true);
	if (rc) {
		pr_err("failed to vote gdsc for continuous splash, rc=%d\n",
							rc);
		mutex_unlock(&display->display_lock);
		return -EINVAL;
	}

	/* Verify whether continuous splash is enabled or not */
	display->is_cont_splash_enabled =
		dsi_display_get_cont_splash_status(display);
	if (!display->is_cont_splash_enabled) {
		pr_err("Continuous splash is not enabled\n");
		goto splash_disabled;
	}

	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				display->is_cont_splash_enabled);

	/* Set up ctrl isr before enabling core clk */
	dsi_display_ctrl_isr_configure(display, true);

	/* Vote for Core clk and link clk. Votes on ctrl and phy
	 * regulator are inplicit from  pre clk on callback
	 */
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI link clocks, rc=%d\n",
		       display->name, rc);
		goto clk_manager_update;
	}

	/* Vote on panel regulator will be removed during suspend path */
	rc = dsi_pwr_enable_regulator(&display->panel->power_info, true);
	if (rc) {
		pr_err("[%s] failed to enable vregs, rc=%d\n",
				display->panel->name, rc);
		goto clks_disabled;
	}

	dsi_config_host_engine_state_for_cont_splash(display);
	mutex_unlock(&display->display_lock);

	/* Set the current brightness level */
	dsi_panel_bl_handoff(display->panel);

	return rc;

clks_disabled:
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);

clk_manager_update:
	dsi_display_ctrl_isr_configure(display, false);
	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				false);

splash_disabled:
	(void)sde_power_resource_enable(display->phandle,
			display->cont_splash_client, false);
	display->is_cont_splash_enabled = false;
	mutex_unlock(&display->display_lock);
	return rc;
}

/**
 * dsi_display_splash_res_cleanup() - cleanup for continuous splash
 * @display:    Pointer to dsi display
 * Returns:     Zero on success
 */
int dsi_display_splash_res_cleanup(struct  dsi_display *display)
{
	int rc = 0;

	if (!display->is_cont_splash_enabled)
		return 0;

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	if (rc)
		pr_err("[%s] failed to disable DSI link clocks, rc=%d\n",
		       display->name, rc);

	rc = sde_power_resource_enable(display->phandle,
			display->cont_splash_client, false);
	if (rc)
		pr_err("failed to remove vote on gdsc for continuous splash, rc=%d\n",
				rc);

	display->is_cont_splash_enabled = false;
	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				display->is_cont_splash_enabled);

	return rc;
}

static int dsi_display_link_clk_force_update_ctrl(void *handle)
{
	int rc = 0;

	if (!handle) {
		pr_err("%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&dsi_display_clk_mutex);

	rc = dsi_display_link_clk_force_update(handle);

	mutex_unlock(&dsi_display_clk_mutex);

	return rc;
}

int dsi_display_clk_ctrl(void *handle,
	enum dsi_clk_type clk_type, enum dsi_clk_state clk_state)
{
	int rc = 0;

	if (!handle) {
		pr_err("%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&dsi_display_clk_mutex);
	rc = dsi_clk_req_state(handle, clk_type, clk_state);
	if (rc)
		pr_err("%s: failed set clk state, rc = %d\n", __func__, rc);
	mutex_unlock(&dsi_display_clk_mutex);

	return rc;
}

static int dsi_display_force_update_dsi_clk(struct dsi_display *display)
{
	int rc = 0;

	rc = dsi_display_link_clk_force_update_ctrl(display->dsi_clk_handle);

	if (!rc) {
		pr_info("dsi bit clk has been configured to %d\n",
			display->cached_clk_rate);

		atomic_set(&display->clkrate_change_pending, 0);
	} else {
		pr_err("Failed to configure dsi bit clock '%d'. rc = %d\n",
			display->cached_clk_rate, rc);
	}

	return rc;
}

static ssize_t sysfs_dynamic_dsi_clk_read(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int rc = 0;
	struct dsi_display *display;
	struct dsi_display_ctrl *m_ctrl;
	struct dsi_ctrl *ctrl;

	display = dev_get_drvdata(dev);
	if (!display) {
		pr_err("Invalid display\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	ctrl = m_ctrl->ctrl;
	if (ctrl)
		display->cached_clk_rate = ctrl->clk_freq.byte_clk_rate
					     * 8;

	rc = snprintf(buf, PAGE_SIZE, "%d\n", display->cached_clk_rate);
	pr_debug("%s: read dsi clk rate %d\n", __func__,
		display->cached_clk_rate);

	mutex_unlock(&display->display_lock);

	return rc;
}

static ssize_t sysfs_dynamic_dsi_clk_write(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc = 0;
	int clk_rate;
	struct dsi_display *display;

	display = dev_get_drvdata(dev);
	if (!display) {
		pr_err("Invalid display\n");
		return -EINVAL;
	}

	rc = kstrtoint(buf, DSI_CLOCK_BITRATE_RADIX, &clk_rate);
	if (rc) {
		pr_err("%s: kstrtoint failed. rc=%d\n", __func__, rc);
		return rc;
	}

	if (display->panel->panel_mode != DSI_OP_CMD_MODE) {
		pr_err("only supported for command mode\n");
		return -ENOTSUPP;
	}

	pr_info("%s: bitrate param value: '%d'\n", __func__, clk_rate);

	mutex_lock(&display->display_lock);
	mutex_lock(&dsi_display_clk_mutex);

	rc = dsi_display_dynamic_clk_configure_cmd(display, clk_rate);
	if (rc)
		pr_err("Failed to configure dynamic clk\n");
	else
		rc = count;

	mutex_unlock(&dsi_display_clk_mutex);
	mutex_unlock(&display->display_lock);

	return rc;

}

static DEVICE_ATTR(dynamic_dsi_clock, 0644,
			sysfs_dynamic_dsi_clk_read,
			sysfs_dynamic_dsi_clk_write);

static struct attribute *dynamic_dsi_clock_fs_attrs[] = {
	&dev_attr_dynamic_dsi_clock.attr,
	NULL,
};
static struct attribute_group dynamic_dsi_clock_fs_attrs_group = {
	.attrs = dynamic_dsi_clock_fs_attrs,
};

static int dsi_display_validate_split_link(struct dsi_display *display)
{
	int i, rc = 0;
	struct dsi_display_ctrl *ctrl;
	struct dsi_host_common_cfg *host = &display->panel->host_config;

	if (!host->split_link.split_link_enabled)
		return 0;

	if (display->panel->panel_mode == DSI_OP_CMD_MODE) {
		pr_err("[%s] split link is not supported in command mode\n",
			display->name);
		rc = -ENOTSUPP;
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl->split_link_supported) {
			pr_err("[%s] split link is not supported by hw\n",
				display->name);
			rc = -ENOTSUPP;
			goto error;
		}

		set_bit(DSI_PHY_SPLIT_LINK, ctrl->phy->hw.feature_map);
	}

	pr_debug("Split link is enabled\n");
	return 0;

error:
	host->split_link.split_link_enabled = false;
	return rc;
}

static int dsi_display_sysfs_init(struct dsi_display *display)
{
	int rc = 0;
	struct device *dev = &display->pdev->dev;

	if (display->panel->panel_mode == DSI_OP_CMD_MODE)
		rc = sysfs_create_group(&dev->kobj,
			&dynamic_dsi_clock_fs_attrs_group);

	return rc;

}

static int dsi_display_sysfs_deinit(struct dsi_display *display)
{
	struct device *dev = &display->pdev->dev;

	if (display->panel->panel_mode == DSI_OP_CMD_MODE)
		sysfs_remove_group(&dev->kobj,
			&dynamic_dsi_clock_fs_attrs_group);

	return 0;

}

/**
 * dsi_display_bind - bind dsi device with controlling device
 * @dev:        Pointer to base of platform device
 * @master:     Pointer to container of drm device
 * @data:       Pointer to private data
 * Returns:     Zero on success
 */
static int dsi_display_bind(struct device *dev,
		struct device *master,
		void *data)
{
	struct dsi_display_ctrl *display_ctrl;
	struct drm_device *drm;
	struct dsi_display *display;
	struct dsi_clk_info info;
	struct clk_ctrl_cb clk_cb;
	struct msm_drm_private *priv;
	void *handle = NULL;
	struct platform_device *pdev = to_platform_device(dev);
	char *client1 = "dsi_clk_client";
	char *client2 = "mdp_event_client";
	char dsi_client_name[DSI_CLIENT_NAME_SIZE];
	int i, j, rc = 0;

	if (!dev || !pdev || !master) {
		pr_err("invalid param(s), dev %pK, pdev %pK, master %pK\n",
				dev, pdev, master);
		return -EINVAL;
	}

	drm = dev_get_drvdata(master);
	display = platform_get_drvdata(pdev);
	if (!drm || !display) {
		pr_err("invalid param(s), drm %pK, display %pK\n",
				drm, display);
		return -EINVAL;
	}
	priv = drm->dev_private;

	if (!display->disp_node)
		return 0;

	/* defer bind if ext bridge driver is not loaded */
	for (i = 0; i < display->panel->host_config.ext_bridge_num; i++) {
		j = display->panel->host_config.ext_bridge_map[i];
		if (!display->ext_bridge[j].node_of) {
			pr_err("invalid ext bridge node\n");
			return -EINVAL;
		}

		if (!of_drm_find_bridge(display->ext_bridge[j].node_of)) {
			pr_debug("defer for bridge[%d] %s\n", j,
				display->ext_bridge[j].node_of->full_name);
			return -EPROBE_DEFER;
		}
	}

	mutex_lock(&display->display_lock);

	rc = dsi_display_validate_split_link(display);
	if (rc) {
		pr_err("[%s] split link validation failed, rc=%d\n",
						 display->name, rc);
		goto error;
	}

	dsi_display_debugfs_init(display);

	atomic_set(&display->clkrate_change_pending, 0);
	display->cached_clk_rate = 0;

	rc = dsi_display_sysfs_init(display);
	if (rc) {
		pr_err("[%s] sysfs init failed, rc=%d\n", display->name, rc);
		goto error;
	}

	memset(&info, 0x0, sizeof(info));

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];
		rc = dsi_ctrl_drv_init(display_ctrl->ctrl, display->root);
		if (rc) {
			pr_err("[%s] failed to initialize ctrl[%d], rc=%d\n",
			       display->name, i, rc);
			goto error_ctrl_deinit;
		}
		display_ctrl->ctrl->horiz_index = i;

		rc = dsi_phy_drv_init(display_ctrl->phy);
		if (rc) {
			pr_err("[%s] Failed to initialize phy[%d], rc=%d\n",
				display->name, i, rc);
			(void)dsi_ctrl_drv_deinit(display_ctrl->ctrl);
			goto error_ctrl_deinit;
		}

		memcpy(&info.c_clks[i],
				(&display_ctrl->ctrl->clk_info.core_clks),
				sizeof(struct dsi_core_clk_info));
		memcpy(&info.l_hs_clks[i],
				(&display_ctrl->ctrl->clk_info.hs_link_clks),
				sizeof(struct dsi_link_hs_clk_info));
		memcpy(&info.l_lp_clks[i],
				(&display_ctrl->ctrl->clk_info.lp_link_clks),
				sizeof(struct dsi_link_lp_clk_info));

		info.c_clks[i].phandle = &priv->phandle;
		info.bus_handle[i] =
			display_ctrl->ctrl->axi_bus_info.bus_handle;
		info.ctrl_index[i] = display_ctrl->ctrl->cell_index;
		snprintf(dsi_client_name, DSI_CLIENT_NAME_SIZE,
						"dsi_core_client%u", i);
		info.c_clks[i].dsi_core_client = sde_power_client_create(
				info.c_clks[i].phandle, dsi_client_name);
		if (IS_ERR_OR_NULL(info.c_clks[i].dsi_core_client)) {
			pr_err("[%s] client creation failed for ctrl[%d]",
					dsi_client_name, i);
			goto error_ctrl_deinit;
		}
	}

	display->phandle = &priv->phandle;
	info.pre_clkoff_cb = dsi_pre_clkoff_cb;
	info.pre_clkon_cb = dsi_pre_clkon_cb;
	info.post_clkoff_cb = dsi_post_clkoff_cb;
	info.post_clkon_cb = dsi_post_clkon_cb;
	info.priv_data = display;
	info.master_ndx = display->clk_master_idx;
	info.dsi_ctrl_count = display->ctrl_count;
	snprintf(info.name, MAX_STRING_LEN,
			"DSI_MNGR-%s", display->name);

	display->clk_mngr = dsi_display_clk_mngr_register(&info);
	if (IS_ERR_OR_NULL(display->clk_mngr)) {
		rc = PTR_ERR(display->clk_mngr);
		display->clk_mngr = NULL;
		pr_err("dsi clock registration failed, rc = %d\n", rc);
		goto error_ctrl_deinit;
	}

	handle = dsi_register_clk_handle(display->clk_mngr, client1);
	if (IS_ERR_OR_NULL(handle)) {
		rc = PTR_ERR(handle);
		pr_err("failed to register %s client, rc = %d\n",
		       client1, rc);
		goto error_clk_deinit;
	} else {
		display->dsi_clk_handle = handle;
	}

	handle = dsi_register_clk_handle(display->clk_mngr, client2);
	if (IS_ERR_OR_NULL(handle)) {
		rc = PTR_ERR(handle);
		pr_err("failed to register %s client, rc = %d\n",
		       client2, rc);
		goto error_clk_client_deinit;
	} else {
		display->mdp_clk_handle = handle;
	}

	clk_cb.priv = display;
	clk_cb.dsi_clk_cb = dsi_display_clk_ctrl_cb;

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];

		rc = dsi_ctrl_clk_cb_register(display_ctrl->ctrl, &clk_cb);
		if (rc) {
			pr_err("[%s] failed to register ctrl clk_cb[%d], rc=%d\n",
			       display->name, i, rc);
			goto error_ctrl_deinit;
		}

		rc = dsi_phy_clk_cb_register(display_ctrl->phy, &clk_cb);
		if (rc) {
			pr_err("[%s] failed to register phy clk_cb[%d], rc=%d\n",
			       display->name, i, rc);
			goto error_ctrl_deinit;
		}
	}

	rc = dsi_display_mipi_host_init(display);
	if (rc) {
		pr_err("[%s] failed to initialize mipi host, rc=%d\n",
		       display->name, rc);
		goto error_ctrl_deinit;
	}

	rc = dsi_panel_drv_init(display->panel, &display->host);
	if (rc) {
		if (rc != -EPROBE_DEFER)
			pr_err("[%s] failed to initialize panel driver, rc=%d\n",
			       display->name, rc);
		goto error_host_deinit;
	}

	pr_info("Successfully bind display panel '%s'\n", display->name);
	display->drm_dev = drm;

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];

		if (!display_ctrl->phy || !display_ctrl->ctrl)
			continue;

		rc = dsi_phy_set_clk_freq(display_ctrl->phy,
				&display_ctrl->ctrl->clk_freq);
		if (rc) {
			pr_err("[%s] failed to set phy clk freq, rc=%d\n",
					display->name, rc);
			goto error;
		}
	}

	/* register te irq handler */
	dsi_display_register_te_irq(display);
	/* register err flag irq handler */
	dsi_display_register_err_flag_irq(display);

	goto error;

error_host_deinit:
	(void)dsi_display_mipi_host_deinit(display);
error_clk_client_deinit:
	(void)dsi_deregister_clk_handle(display->dsi_clk_handle);
error_clk_deinit:
	(void)dsi_display_clk_mngr_deregister(display->clk_mngr);
error_ctrl_deinit:
	for (i = i - 1; i >= 0; i--) {
		display_ctrl = &display->ctrl[i];
		(void)dsi_phy_drv_deinit(display_ctrl->phy);
		(void)dsi_ctrl_drv_deinit(display_ctrl->ctrl);
	}
	(void)dsi_display_sysfs_deinit(display);
	(void)dsi_display_debugfs_deinit(display);
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

/**
 * dsi_display_unbind - unbind dsi from controlling device
 * @dev:        Pointer to base of platform device
 * @master:     Pointer to container of drm device
 * @data:       Pointer to private data
 */
static void dsi_display_unbind(struct device *dev,
		struct device *master, void *data)
{
	struct dsi_display_ctrl *display_ctrl;
	struct dsi_display *display;
	struct platform_device *pdev = to_platform_device(dev);
	int i, rc = 0;

	if (!dev || !pdev) {
		pr_err("invalid param(s)\n");
		return;
	}

	display = platform_get_drvdata(pdev);
	if (!display) {
		pr_err("invalid display\n");
		return;
	}

	mutex_lock(&display->display_lock);

	rc = dsi_panel_drv_deinit(display->panel);
	if (rc)
		pr_err("[%s] failed to deinit panel driver, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_mipi_host_deinit(display);
	if (rc)
		pr_err("[%s] failed to deinit mipi hosts, rc=%d\n",
		       display->name,
		       rc);

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];

		rc = dsi_phy_drv_deinit(display_ctrl->phy);
		if (rc)
			pr_err("[%s] failed to deinit phy%d driver, rc=%d\n",
			       display->name, i, rc);

		rc = dsi_ctrl_drv_deinit(display_ctrl->ctrl);
		if (rc)
			pr_err("[%s] failed to deinit ctrl%d driver, rc=%d\n",
			       display->name, i, rc);
	}

	atomic_set(&display->clkrate_change_pending, 0);
	(void)dsi_display_sysfs_deinit(display);
	(void)dsi_display_debugfs_deinit(display);

	mutex_unlock(&display->display_lock);
}

static const struct component_ops dsi_display_comp_ops = {
	.bind = dsi_display_bind,
	.unbind = dsi_display_unbind,
};

static struct platform_driver dsi_display_driver = {
	.probe = dsi_display_dev_probe,
	.remove = dsi_display_dev_remove,
	.driver = {
		.name = "msm-dsi-display",
		.of_match_table = dsi_display_dt_match,
		.suppress_bind_attrs = true,
	},
};

static int dsi_display_init(struct dsi_display *display)
{
	int rc = 0;
	 struct platform_device *pdev = display->pdev;

	mutex_init(&display->display_lock);

	rc = _dsi_display_dev_init(display);
	if (rc) {
		pr_err("device init failed, rc=%d\n", rc);
		goto end;
	}

	rc = component_add(&pdev->dev, &dsi_display_comp_ops);
	if (rc)
		pr_err("component add failed, rc=%d\n", rc);

	pr_debug("component add success: %s\n", display->name);
end:
	return rc;
}

static void dsi_display_firmware_display(const struct firmware *fw,
				void *context)
{
	struct dsi_display *display = context;

	if (fw) {
		pr_debug("reading data from firmware, size=%zd\n",
			fw->size);

		display->fw = fw;
		display->name = "dsi_firmware_display";
	}

	pr_debug("success\n");
}

static ssize_t fresh_rate_read(struct file *file, char __user * user_buf,
			       size_t count, loff_t * ppos)
{
	int ret = 0;
	char fresh_rate[4] = { 0 };

	if (mode_fps == 90)
		strcpy(fresh_rate, "90");
	else if (mode_fps == 60)
		strcpy(fresh_rate, "60");
	else
		strcpy(fresh_rate, "-1");

	pr_info("fresh_rate : %s\n", fresh_rate);
	ret =
	    simple_read_from_buffer(user_buf, count, ppos, fresh_rate,
				    strlen(fresh_rate));
	return ret;
}

static const struct file_operations fresh_rate_fops = {
	.read = fresh_rate_read,
	.open = simple_open,
	.owner = THIS_MODULE,
};

static ssize_t fresh_rate_event_num_read(struct file *file,
					 char __user * user_buf, size_t count,
					 loff_t * ppos)
{
	int ret = 0;
	const char *devname = NULL;
	struct input_handle *handle;
	if (!fresh_rate_input_dev)
		return count;
	list_for_each_entry(handle, &(fresh_rate_input_dev->h_list), d_node) {
		if (strncmp(handle->name, "event", 5) == 0) {
			devname = handle->name;
			break;
		}
	}
	ret =
	    simple_read_from_buffer(user_buf, count, ppos, devname,
				    strlen(devname));
	return ret;
}

static const struct file_operations fresh_rate_event_num_fops = {
	.read = fresh_rate_event_num_read,
	.open = simple_open,
	.owner = THIS_MODULE,
};

static ssize_t fresh_rate_enable_read(struct file *file, char __user * user_buf,
				      size_t count, loff_t * ppos)
{
	ssize_t ret = 0;
	char page[4];

	pr_info("the fresh_rate_report_enable is: %d\n",
		fresh_rate_report_enable);
	ret = sprintf(page, "%d\n", fresh_rate_report_enable);
	ret =
	    simple_read_from_buffer(user_buf, count, ppos, page, strlen(page));
	return ret;

}

static ssize_t fresh_rate_enable_write(struct file *file,
				       const char __user * buffer, size_t count,
				       loff_t * ppos)
{
	char buf[8] = { 0 };

	if (count > 2)
		count = 2;
	if (copy_from_user(buf, buffer, count)) {
		pr_err("%s: read proc input error.\n", __func__);
		return count;
	}
	if ('0' == buf[0]) {
		fresh_rate_report_enable = 0;
	} else if ('1' == buf[0]) {
		fresh_rate_report_enable = 1;
	}

	return count;
}

static const struct file_operations fresh_rate_enable_fops = {
	.read = fresh_rate_enable_read,
	.write = fresh_rate_enable_write,
	.open = simple_open,
	.owner = THIS_MODULE,
};

int dsi_display_dev_probe(struct platform_device *pdev)
{
	struct dsi_display *display = NULL;
	struct device_node *node = NULL, *disp_node = NULL;
	const char *dsi_type = NULL, *name = NULL;
	const char *disp_list = "qcom,dsi-display-list";
	const char *disp_active = "qcom,dsi-display-active";
	int i, count, rc = 0, index;
	bool firm_req = false;
	struct dsi_display_boot_param *boot_disp;
	struct proc_dir_entry *proc_entry_tmp = NULL;

	if (fresh_rate_input_dev_init == false) {
		proc_entry_display = proc_mkdir("fresh_rate_for_sensor", NULL);
		if (proc_entry_display == NULL) {
			pr_err
			    ("Couldn't create fresh_rate_for_sensor directory\n");
		}
	}

	if (!pdev || !pdev->dev.of_node) {
		pr_err("pdev not found\n");
		rc = -ENODEV;
		goto end;
	}

	dsi_type = of_get_property(pdev->dev.of_node, "label", NULL);
	if (!dsi_type)
		dsi_type = "primary";

	if (!strcmp(dsi_type, "primary"))
		index = DSI_PRIMARY;
	else
		index = DSI_SECONDARY;

	boot_disp = &boot_displays[index];

	display = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);
	if (!display) {
		rc = -ENOMEM;
		goto end;
	}

	node = pdev->dev.of_node;
	count = of_count_phandle_with_args(node, disp_list, NULL);

	for (i = 0; i < count; i++) {
		struct device_node *np;

		np = of_parse_phandle(node, disp_list, i);
		name = of_get_property(np, "label", NULL);
		if (!name) {
			pr_err("display name not defined\n");
			continue;
		}

		if (boot_disp->boot_disp_en) {
			if (!strcmp(boot_disp->name, name)) {
				disp_node = np;
				break;
			}
			continue;
		}

		if (of_property_read_bool(np, disp_active)) {
			disp_node = np;

			if (IS_ENABLED(CONFIG_DSI_PARSER))
				firm_req =
				    !request_firmware_nowait(THIS_MODULE, 1,
							     "dsi_prop",
							     &pdev->dev,
							     GFP_KERNEL,
							     display,
							     dsi_display_firmware_display);
			break;
		}

		of_node_put(np);
	}

	boot_disp->node = pdev->dev.of_node;
	boot_disp->disp = display;

	display->disp_node = disp_node;
	display->name = name;
	display->pdev = pdev;
	display->boot_disp = boot_disp;
	display->dsi_type = dsi_type;

	dsi_display_parse_cmdline_topology(display, index);

	platform_set_drvdata(pdev, display);

	rc = dsi_display_init(display);
	if (rc)
		goto end;


	if (fresh_rate_input_dev_init == false) {
		//create fresh_rate
		proc_entry_tmp = proc_create("fresh_rate", 0664,
					     proc_entry_display,
					     &fresh_rate_fops);
		if (proc_entry_tmp == NULL) {
			pr_err("Couldn't create fresh_rate_fops\n");
			goto fresh_rate_report_failed;
		}
		//create fresh_rate_event_num
		proc_entry_tmp = proc_create("fresh_rate_event_num", 0664,
					     proc_entry_display,
					     &fresh_rate_event_num_fops);
		if (proc_entry_tmp == NULL) {
			pr_err("Couldn't create fresh_rate_event_num_fops\n");
			goto fresh_rate_report_failed;
		}
		//create fresh_rate_enable
		proc_entry_tmp = proc_create("fresh_rate_enable", 0666,
					     proc_entry_display,
					     &fresh_rate_enable_fops);
		if (proc_entry_tmp == NULL) {
			pr_err("Couldn't create fresh_rate_enable_fops\n");
			goto fresh_rate_report_failed;
		}
		//create input event
		fresh_rate_input_dev = input_allocate_device();
		if (fresh_rate_input_dev == NULL) {
			pr_err("Failed to allocate fresh rate input device\n");
			goto fresh_rate_report_failed;
		}
		fresh_rate_input_dev->name = "oneplus,fresh_rate";

		set_bit(EV_MSC, fresh_rate_input_dev->evbit);
		set_bit(MSC_RAW, fresh_rate_input_dev->mscbit);

		if (input_register_device(fresh_rate_input_dev)) {
			pr_err
			    ("%s: Failed to register fresh rate input device\n",
			     __func__);
			input_free_device(fresh_rate_input_dev);
			goto fresh_rate_report_failed;
		}
	}

	fresh_rate_input_dev_init = true;

	return 0;
 end:
	if (display)
		devm_kfree(&pdev->dev, display);
	return rc;

 fresh_rate_report_failed:
	pr_err("%s: fresh rate_report_failed\n", __func__);
	return 0;
}

int dsi_display_dev_remove(struct platform_device *pdev)
{
	int rc = 0;
	struct dsi_display *display;

	if (!pdev) {
		pr_err("Invalid device\n");
		return -EINVAL;
	}

	display = platform_get_drvdata(pdev);

	/* decrement ref count */
	of_node_put(display->disp_node);

	(void)_dsi_display_dev_deinit(display);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, display);

	//unregister_device
	pr_err("unregister_device fresh_rate_input_dev...\n");
	input_unregister_device(fresh_rate_input_dev);
	input_free_device(fresh_rate_input_dev);

	return rc;
}

int dsi_display_get_num_of_displays(void)
{
	int i, count = 0;

	for (i = 0; i < MAX_DSI_ACTIVE_DISPLAY; i++) {
		struct dsi_display *display = boot_displays[i].disp;

		if (display && display->disp_node)
			count++;
	}

	return count;
}

int dsi_display_get_active_displays(void **display_array, u32 max_display_count)
{
	int index = 0, count = 0;

	if (!display_array || !max_display_count) {
		pr_err("invalid params\n");
		return 0;
	}

	for (index = 0; index < MAX_DSI_ACTIVE_DISPLAY; index++) {
		struct dsi_display *display = boot_displays[index].disp;

		if (display && display->disp_node)
			display_array[count++] = display;
	}

	return count;
}

int dsi_display_drm_bridge_init(struct dsi_display *display,
		struct drm_encoder *enc)
{
	int rc = 0;
	struct dsi_bridge *bridge;
	struct msm_drm_private *priv = NULL;

	if (!display || !display->drm_dev || !enc) {
		pr_err("invalid param(s)\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	priv = display->drm_dev->dev_private;

	if (!priv) {
		pr_err("Private data is not present\n");
		rc = -EINVAL;
		goto error;
	}

	if (display->bridge) {
		pr_err("display is already initialize\n");
		goto error;
	}

	bridge = dsi_drm_bridge_init(display, display->drm_dev, enc);
	if (IS_ERR_OR_NULL(bridge)) {
		rc = PTR_ERR(bridge);
		pr_err("[%s] brige init failed, %d\n", display->name, rc);
		goto error;
	}

	display->bridge = bridge;
	priv->bridges[priv->num_bridges++] = &bridge->base;

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_drm_bridge_deinit(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	dsi_drm_bridge_cleanup(display->bridge);
	display->bridge = NULL;

	mutex_unlock(&display->display_lock);
	return rc;
}

/* Hook functions to call external connector, pointer validation is
 * done in dsi_display_drm_ext_bridge_init.
 */
static enum drm_connector_status dsi_display_drm_ext_detect(
		struct drm_connector *connector,
		bool force,
		void *disp)
{
	struct dsi_display *display = disp;

	return display->ext_conn->funcs->detect(display->ext_conn, force);
}

static int dsi_display_drm_ext_get_modes(
		struct drm_connector *connector, void *disp)
{
	struct dsi_display *display = disp;
	struct drm_display_mode *pmode, *pt;
	int count;

	/* if there are modes defined in panel, ignore external modes */
	if (display->panel->num_timing_nodes)
		return dsi_connector_get_modes(connector, disp);

	count = display->ext_conn->helper_private->get_modes(
			display->ext_conn);

	list_for_each_entry_safe(pmode, pt,
			&display->ext_conn->probed_modes, head) {
		list_move_tail(&pmode->head, &connector->probed_modes);
	}

	connector->display_info = display->ext_conn->display_info;

	return count;
}

static enum drm_mode_status dsi_display_drm_ext_mode_valid(
		struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *disp)
{
	struct dsi_display *display = disp;
	enum drm_mode_status status;

	/* always do internal mode_valid check */
	status = dsi_conn_mode_valid(connector, mode, disp);
	if (status != MODE_OK)
		return status;

	return display->ext_conn->helper_private->mode_valid(
			display->ext_conn, mode);
}

static int dsi_display_drm_ext_atomic_check(struct drm_connector *connector,
		void *disp,
		struct drm_connector_state *c_state)
{
	struct dsi_display *display = disp;

	return display->ext_conn->helper_private->atomic_check(
			display->ext_conn, c_state);
}

static int dsi_display_ext_get_info(struct drm_connector *connector,
	struct msm_display_info *info, void *disp)
{
	struct dsi_display *display;
	int i;

	if (!info || !disp) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	display = disp;
	if (!display->panel) {
		pr_err("invalid display panel\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	memset(info, 0, sizeof(struct msm_display_info));

	info->intf_type = DRM_MODE_CONNECTOR_DSI;
	info->num_of_h_tiles = display->ctrl_count;
	for (i = 0; i < info->num_of_h_tiles; i++)
		info->h_tile_instance[i] = display->ctrl[i].ctrl->cell_index;

	info->is_connected = connector->status != connector_status_disconnected;

	if (!strcmp(display->dsi_type, "primary"))
		info->is_primary = true;
	else
		info->is_primary = false;

	info->capabilities |= (MSM_DISPLAY_CAP_VID_MODE |
		MSM_DISPLAY_CAP_EDID | MSM_DISPLAY_CAP_HOT_PLUG);
	info->curr_panel_mode = MSM_DISPLAY_VIDEO_MODE;

	mutex_unlock(&display->display_lock);
	return 0;
}

static int dsi_display_ext_get_mode_info(struct drm_connector *connector,
	const struct drm_display_mode *drm_mode,
	struct msm_mode_info *mode_info,
	u32 max_mixer_width, void *display)
{
	struct msm_display_topology *topology;

	if (!drm_mode || !mode_info)
		return -EINVAL;

	SDE_EVT32(mode_info, ((unsigned long long)mode_info) >> 32, 0x9999);
	memset(mode_info, 0, sizeof(*mode_info));
	mode_info->frame_rate = drm_mode->vrefresh;
	mode_info->vtotal = drm_mode->vtotal;

	topology = &mode_info->topology;
	topology->num_lm = (max_mixer_width <= drm_mode->hdisplay) ? 2 : 1;
	topology->num_enc = 0;
	topology->num_intf = topology->num_lm;

	mode_info->comp_info.comp_type = MSM_DISPLAY_COMPRESSION_NONE;

	return 0;
}

static struct dsi_display_ext_bridge *dsi_display_ext_get_bridge(
		struct drm_bridge *bridge)
{
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct dsi_display *display;
	int i, j, k;
	u32 bridge_num;

	if (!bridge || !bridge->encoder) {
		SDE_ERROR("invalid argument\n");
		return NULL;
	}

	priv = bridge->dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	for (i = 0; i < sde_kms->dsi_display_count; i++) {
		display = sde_kms->dsi_displays[i];
		bridge_num = display->panel->host_config.ext_bridge_num;
		for (j = 0; j < bridge_num; j++) {
			k = display->panel->host_config.ext_bridge_map[j];
			if (display->ext_bridge[k].bridge == bridge)
				return &display->ext_bridge[k];
		}
	}

	return NULL;
}

static void dsi_display_drm_ext_adjust_timing(
		const struct dsi_display *display,
		struct drm_display_mode *mode)
{
	mode->hdisplay /= display->ctrl_count;
	mode->hsync_start /= display->ctrl_count;
	mode->hsync_end /= display->ctrl_count;
	mode->htotal /= display->ctrl_count;
	mode->hskew /= display->ctrl_count;
	mode->clock /= display->ctrl_count;
}

static enum drm_mode_status dsi_display_drm_ext_bridge_mode_valid(
		struct drm_bridge *bridge,
		const struct drm_display_mode *mode)
{
	struct dsi_display_ext_bridge *ext_bridge;
	struct drm_display_mode tmp;

	ext_bridge = dsi_display_ext_get_bridge(bridge);
	if (!ext_bridge)
		return MODE_ERROR;

	tmp = *mode;
	dsi_display_drm_ext_adjust_timing(ext_bridge->display, &tmp);
	return ext_bridge->orig_funcs->mode_valid(bridge, &tmp);
}

static bool dsi_display_drm_ext_bridge_mode_fixup(
		struct drm_bridge *bridge,
		const struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
	struct dsi_display_ext_bridge *ext_bridge;
	struct drm_display_mode tmp;

	ext_bridge = dsi_display_ext_get_bridge(bridge);
	if (!ext_bridge)
		return false;

	tmp = *mode;
	dsi_display_drm_ext_adjust_timing(ext_bridge->display, &tmp);
	return ext_bridge->orig_funcs->mode_fixup(bridge, &tmp, &tmp);
}

static void dsi_display_drm_ext_bridge_mode_set(
		struct drm_bridge *bridge,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
	struct dsi_display_ext_bridge *ext_bridge;
	struct drm_display_mode tmp;

	ext_bridge = dsi_display_ext_get_bridge(bridge);
	if (!ext_bridge)
		return;

	tmp = *mode;
	dsi_display_drm_ext_adjust_timing(ext_bridge->display, &tmp);
	ext_bridge->orig_funcs->mode_set(bridge, &tmp, &tmp);
}

static int dsi_host_ext_attach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dsi)
{
	struct dsi_display *display = to_dsi_display(host);
	struct dsi_panel *panel;

	if (!host || !dsi || !display->panel) {
		pr_err("Invalid param\n");
		return -EINVAL;
	}

	pr_debug("DSI[%s]: channel=%d, lanes=%d, format=%d, mode_flags=%lx\n",
		dsi->name, dsi->channel, dsi->lanes,
		dsi->format, dsi->mode_flags);

	panel = display->panel;
	panel->host_config.data_lanes = 0;
	if (dsi->lanes > 0)
		panel->host_config.data_lanes |= DSI_DATA_LANE_0;
	if (dsi->lanes > 1)
		panel->host_config.data_lanes |= DSI_DATA_LANE_1;
	if (dsi->lanes > 2)
		panel->host_config.data_lanes |= DSI_DATA_LANE_2;
	if (dsi->lanes > 3)
		panel->host_config.data_lanes |= DSI_DATA_LANE_3;

	switch (dsi->format) {
	case MIPI_DSI_FMT_RGB888:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB888;
		break;
	case MIPI_DSI_FMT_RGB666:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB666_LOOSE;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB666;
		break;
	case MIPI_DSI_FMT_RGB565:
	default:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB565;
		break;
	}

	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		panel->panel_mode = DSI_OP_VIDEO_MODE;

		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
			panel->video_config.traffic_mode =
					DSI_VIDEO_TRAFFIC_BURST_MODE;
		else if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			panel->video_config.traffic_mode =
					DSI_VIDEO_TRAFFIC_SYNC_PULSES;
		else
			panel->video_config.traffic_mode =
					DSI_VIDEO_TRAFFIC_SYNC_START_EVENTS;

		panel->video_config.hsa_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HSA;
		panel->video_config.hbp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HBP;
		panel->video_config.hfp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HFP;
		panel->video_config.pulse_mode_hsa_he =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HSE;
		panel->video_config.bllp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BLLP;
		panel->video_config.eof_bllp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_EOF_BLLP;
	} else {
		panel->panel_mode = DSI_OP_CMD_MODE;
		pr_err("command mode not supported by ext bridge\n");
		return -ENOTSUPP;
	}

	panel->bl_config.type = DSI_BACKLIGHT_UNKNOWN;

	return 0;
}

static struct mipi_dsi_host_ops dsi_host_ext_ops = {
	.attach = dsi_host_ext_attach,
	.detach = dsi_host_detach,
	.transfer = dsi_host_transfer,
};

int dsi_display_drm_ext_bridge_init(struct dsi_display *display,
		struct drm_encoder *encoder, struct drm_connector *connector)
{
	struct drm_device *drm = encoder->dev;
	struct drm_bridge *bridge = encoder->bridge;
	struct drm_bridge *ext_bridge;
	struct drm_connector *ext_conn;
	struct sde_connector *sde_conn = to_sde_connector(connector);
	struct drm_bridge *prev_bridge = bridge;
	int rc = 0, i;

	for (i = 0; i < display->panel->host_config.ext_bridge_num; i++) {
		int j = display->panel->host_config.ext_bridge_map[i];
		struct dsi_display_ext_bridge *ext_bridge_info =
				&display->ext_bridge[j];

		/* return if ext bridge is already initialized */
		if (ext_bridge_info->bridge)
			return 0;

		ext_bridge = of_drm_find_bridge(ext_bridge_info->node_of);
		if (IS_ERR_OR_NULL(ext_bridge)) {
			rc = PTR_ERR(ext_bridge);
			pr_err("failed to find ext bridge\n");
			goto error;
		}

		/* override functions for mode adjustment */
		if (display->panel->host_config.ext_bridge_num > 1) {
			ext_bridge_info->bridge_funcs = *ext_bridge->funcs;
			if (ext_bridge->funcs->mode_fixup)
				ext_bridge_info->bridge_funcs.mode_fixup =
					dsi_display_drm_ext_bridge_mode_fixup;
			if (ext_bridge->funcs->mode_valid)
				ext_bridge_info->bridge_funcs.mode_valid =
					dsi_display_drm_ext_bridge_mode_valid;
			if (ext_bridge->funcs->mode_set)
				ext_bridge_info->bridge_funcs.mode_set =
					dsi_display_drm_ext_bridge_mode_set;
			ext_bridge_info->orig_funcs = ext_bridge->funcs;
			ext_bridge->funcs = &ext_bridge_info->bridge_funcs;
		}

		rc = drm_bridge_attach(encoder, ext_bridge, prev_bridge);
		if (rc) {
			pr_err("[%s] ext brige attach failed, %d\n",
				display->name, rc);
			goto error;
		}

		ext_bridge_info->display = display;
		ext_bridge_info->bridge = ext_bridge;
		prev_bridge = ext_bridge;

		/* ext bridge will init its own connector during attach,
		 * we need to extract it out of the connector list
		 */
		spin_lock_irq(&drm->mode_config.connector_list_lock);
		ext_conn = list_last_entry(&drm->mode_config.connector_list,
			struct drm_connector, head);
		if (ext_conn && ext_conn != connector &&
			ext_conn->encoder_ids[0] == bridge->encoder->base.id) {
			list_del_init(&ext_conn->head);
			display->ext_conn = ext_conn;
		}
		spin_unlock_irq(&drm->mode_config.connector_list_lock);

		/* if there is no valid external connector created, or in split
		 * mode, default setting is used from panel defined in DT file.
		 */
		if (!display->ext_conn ||
		    !display->ext_conn->funcs ||
		    !display->ext_conn->helper_private ||
		    display->panel->host_config.ext_bridge_num > 1) {
			display->ext_conn = NULL;
			continue;
		}

		/* otherwise, hook up the functions to use external connector */
		if (display->ext_conn->funcs->detect)
			sde_conn->ops.detect = dsi_display_drm_ext_detect;

		if (display->ext_conn->helper_private->get_modes)
			sde_conn->ops.get_modes =
				dsi_display_drm_ext_get_modes;

		if (display->ext_conn->helper_private->mode_valid)
			sde_conn->ops.mode_valid =
				dsi_display_drm_ext_mode_valid;

		if (display->ext_conn->helper_private->atomic_check)
			sde_conn->ops.atomic_check =
				dsi_display_drm_ext_atomic_check;

		sde_conn->ops.get_info =
				dsi_display_ext_get_info;
		sde_conn->ops.get_mode_info =
				dsi_display_ext_get_mode_info;

		/* add support to attach/detach */
		display->host.ops = &dsi_host_ext_ops;
	}

	return 0;
error:
	return rc;
}

int dsi_display_get_info(struct drm_connector *connector,
		struct msm_display_info *info, void *disp)
{
	struct dsi_display *display;
	struct dsi_panel_phy_props phy_props;
	struct dsi_host_common_cfg *host;
	int i, rc;

	if (!info || !disp) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	display = disp;
	if (!display->panel) {
		pr_err("invalid display panel\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	rc = dsi_panel_get_phy_props(display->panel, &phy_props);
	if (rc) {
		pr_err("[%s] failed to get panel phy props, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	memset(info, 0, sizeof(struct msm_display_info));
	info->intf_type = DRM_MODE_CONNECTOR_DSI;
	info->num_of_h_tiles = display->ctrl_count;
	for (i = 0; i < info->num_of_h_tiles; i++)
		info->h_tile_instance[i] = display->ctrl[i].ctrl->cell_index;

	info->is_connected = true;
	info->is_primary = false;

	if (!strcmp(display->dsi_type, "primary"))
		info->is_primary = true;

	info->width_mm = phy_props.panel_width_mm;
	info->height_mm = phy_props.panel_height_mm;
	info->max_width = 1920;
	info->max_height = 1080;
	info->qsync_min_fps =
		display->panel->qsync_min_fps;

	switch (display->panel->panel_mode) {
	case DSI_OP_VIDEO_MODE:
		info->curr_panel_mode = MSM_DISPLAY_VIDEO_MODE;
		info->capabilities |= MSM_DISPLAY_CAP_VID_MODE;
		if (display->panel->panel_mode_switch_enabled)
			info->capabilities |= MSM_DISPLAY_CAP_CMD_MODE;
		break;
	case DSI_OP_CMD_MODE:
		info->curr_panel_mode = MSM_DISPLAY_CMD_MODE;
		info->capabilities |= MSM_DISPLAY_CAP_CMD_MODE;
		if (display->panel->panel_mode_switch_enabled)
			info->capabilities |= MSM_DISPLAY_CAP_VID_MODE;
		info->is_te_using_watchdog_timer =
			display->panel->te_using_watchdog_timer |
			display->sw_te_using_wd;
		break;
	default:
		pr_err("unknwown dsi panel mode %d\n",
				display->panel->panel_mode);
		break;
	}

	if (display->panel->esd_config.esd_enabled)
		info->capabilities |= MSM_DISPLAY_ESD_ENABLED;

	info->te_source = display->te_source;

	host = &display->panel->host_config;
	if (host->split_link.split_link_enabled)
		info->capabilities |= MSM_DISPLAY_SPLIT_LINK;

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

static int dsi_display_get_mode_count_no_lock(struct dsi_display *display,
			u32 *count)
{
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	int num_dfps_rates, num_bit_clks, rc = 0;

	if (!display || !display->panel) {
		pr_err("invalid display:%d panel:%d\n", display != NULL,
				display ? display->panel != NULL : 0);
		return -EINVAL;
	}

	*count = display->panel->num_timing_nodes;

	rc = dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (rc) {
		pr_err("[%s] failed to get dfps caps from panel\n",
				display->name);
		return rc;
	}

	num_dfps_rates = !dfps_caps.dfps_support ? 1 : dfps_caps.dfps_list_len;

	dyn_clk_caps = &(display->panel->dyn_clk_caps);

	num_bit_clks = !dyn_clk_caps->dyn_clk_support ? 1 :
					dyn_clk_caps->bit_clk_list_len;

	/* Inflate num_of_modes by fps and bit clks in dfps */
	*count = display->panel->num_timing_nodes *
				num_dfps_rates * num_bit_clks;

	return 0;
}

int dsi_display_get_mode_count(struct dsi_display *display,
			u32 *count)
{
	int rc;

	if (!display || !display->panel) {
		pr_err("invalid display:%d panel:%d\n", display != NULL,
				display ? display->panel != NULL : 0);
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	rc = dsi_display_get_mode_count_no_lock(display, count);
	mutex_unlock(&display->display_lock);

	return 0;
}

static void _dsi_display_populate_bit_clks(struct dsi_display *display,
					   int start, int end, u32 *mode_idx)
{
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	struct dsi_display_mode *src, *dst;
	struct dsi_host_common_cfg *cfg;
	int i, j, total_modes, bpp, lanes = 0;

	if (!display || !mode_idx)
		return;

	dyn_clk_caps = &(display->panel->dyn_clk_caps);
	if (!dyn_clk_caps->dyn_clk_support)
		return;

	cfg = &(display->panel->host_config);
	bpp = dsi_pixel_format_to_bpp(cfg->dst_format);

	if (cfg->data_lanes & DSI_DATA_LANE_0)
		lanes++;
	if (cfg->data_lanes & DSI_DATA_LANE_1)
		lanes++;
	if (cfg->data_lanes & DSI_DATA_LANE_2)
		lanes++;
	if (cfg->data_lanes & DSI_DATA_LANE_3)
		lanes++;

	dsi_display_get_mode_count_no_lock(display, &total_modes);

	for (i = start; i < end; i++) {
		src = &display->modes[i];
		if (!src)
			return;
		/*
		 * TODO: currently setting the first bit rate in
		 * the list as preferred rate. But ideally should
		 * be based on user or device tree preferrence.
		 */
		src->timing.clk_rate_hz = dyn_clk_caps->bit_clk_list[0];
		src->pixel_clk_khz =
			div_u64(src->timing.clk_rate_hz * lanes, bpp);
		src->pixel_clk_khz /= 1000;
		src->pixel_clk_khz *= display->ctrl_count;
	}

	for (i = 1; i < dyn_clk_caps->bit_clk_list_len; i++) {
		if (*mode_idx >= total_modes)
			return;
		for (j = start; j < end; j++) {
			src = &display->modes[j];
			dst = &display->modes[*mode_idx];

			if (!src || !dst) {
				pr_err("invalid mode index\n");
				return;
			}
			memcpy(dst, src, sizeof(struct dsi_display_mode));
			dst->timing.clk_rate_hz = dyn_clk_caps->bit_clk_list[i];
			dst->pixel_clk_khz =
				div_u64(dst->timing.clk_rate_hz * lanes, bpp);
			dst->pixel_clk_khz /= 1000;
			dst->pixel_clk_khz *= display->ctrl_count;
			(*mode_idx)++;
		}
	}
}

void dsi_display_put_mode(struct dsi_display *display,
	struct dsi_display_mode *mode)
{
	dsi_panel_put_mode(mode);
}

int dsi_display_get_modes(struct dsi_display *display,
			  struct dsi_display_mode **out_modes)
{
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_host_common_cfg *host = &display->panel->host_config;
	bool is_split_link;
	u32 num_dfps_rates, panel_mode_count, total_mode_count;
	u32 sublinks_count, mode_idx, array_idx = 0;
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	int i, start, end, rc = -EINVAL;

	if (!display || !out_modes) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	*out_modes = NULL;

	mutex_lock(&display->display_lock);

	if (display->modes)
		goto exit;

	rc = dsi_display_get_mode_count_no_lock(display, &total_mode_count);
	if (rc)
		goto error;

	display->modes = kcalloc(total_mode_count, sizeof(*display->modes),
			GFP_KERNEL);
	if (!display->modes) {
		rc = -ENOMEM;
		goto error;
	}

	rc = dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (rc) {
		pr_err("[%s] failed to get dfps caps from panel\n",
				display->name);
		goto error;
	}

	dyn_clk_caps = &(display->panel->dyn_clk_caps);

	num_dfps_rates = !dfps_caps.dfps_support ? 1 : dfps_caps.dfps_list_len;

	panel_mode_count = display->panel->num_timing_nodes;

	for (mode_idx = 0; mode_idx < panel_mode_count; mode_idx++) {
		struct dsi_display_mode panel_mode;
		int topology_override = NO_OVERRIDE;

		if (display->cmdline_timing == mode_idx)
			topology_override = display->cmdline_topology;

		memset(&panel_mode, 0, sizeof(panel_mode));

		rc = dsi_panel_get_mode(display->panel, mode_idx,
						&panel_mode, topology_override);
		if (rc) {
			pr_err("[%s] failed to get mode idx %d from panel\n",
				   display->name, mode_idx);
			goto error;
		}

		is_split_link = host->split_link.split_link_enabled;
		sublinks_count = host->split_link.num_sublinks;
		if (is_split_link && sublinks_count > 1) {
			panel_mode.timing.h_active *= sublinks_count;
			panel_mode.timing.h_front_porch *= sublinks_count;
			panel_mode.timing.h_sync_width *= sublinks_count;
			panel_mode.timing.h_back_porch *= sublinks_count;
			panel_mode.timing.h_skew *= sublinks_count;
			panel_mode.pixel_clk_khz *= sublinks_count;
		} else {
			panel_mode.timing.h_active *= display->ctrl_count;
			panel_mode.timing.h_front_porch *= display->ctrl_count;
			panel_mode.timing.h_sync_width *= display->ctrl_count;
			panel_mode.timing.h_back_porch *= display->ctrl_count;
			panel_mode.timing.h_skew *= display->ctrl_count;
			panel_mode.pixel_clk_khz *= display->ctrl_count;
		}

		/* pixel overlap is not supported for single dsi panels */
		if (display->ctrl_count == 1)
			panel_mode.priv_info->overlap_pixels = 0;

		start = array_idx;

		for (i = 0; i < num_dfps_rates; i++) {
			struct dsi_display_mode *sub_mode =
					&display->modes[array_idx];
			u32 curr_refresh_rate;

			if (!sub_mode) {
				pr_err("invalid mode data\n");
				rc = -EFAULT;
				goto error;
			}

			memcpy(sub_mode, &panel_mode, sizeof(panel_mode));
			array_idx++;

			if (!dfps_caps.dfps_support)
				continue;

			curr_refresh_rate = sub_mode->timing.refresh_rate;
			sub_mode->timing.refresh_rate = dfps_caps.dfps_list[i];

			dsi_display_get_dfps_timing(display, sub_mode,
					curr_refresh_rate);
		}
		end = array_idx;
		/*
		 * if dynamic clk switch is supported then update all the bit
		 * clk rates.
		 */
		_dsi_display_populate_bit_clks(display, start, end, &array_idx);
	}

exit:
	*out_modes = display->modes;
	primary_display = display;
	rc = 0;

error:
	if (rc)
		kfree(display->modes);

	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_get_panel_vfp(void *dsi_display,
	int h_active, int v_active)
{
	int i, rc = 0;
	u32 count, refresh_rate = 0;
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_display *display = (struct dsi_display *)dsi_display;
	struct dsi_host_common_cfg *host;

	if (!display)
		return -EINVAL;

	rc = dsi_display_get_mode_count(display, &count);
	if (rc)
		return rc;

	mutex_lock(&display->display_lock);

	if (display->panel && display->panel->cur_mode)
		refresh_rate = display->panel->cur_mode->timing.refresh_rate;

	dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (dfps_caps.dfps_support)
		refresh_rate = dfps_caps.max_refresh_rate;

	if (!refresh_rate) {
		mutex_unlock(&display->display_lock);
		pr_err("Null Refresh Rate\n");
		return -EINVAL;
	}

	host = &display->panel->host_config;
	if (host->split_link.split_link_enabled)
		h_active *= host->split_link.num_sublinks;
	else
		h_active *= display->ctrl_count;

	for (i = 0; i < count; i++) {
		struct dsi_display_mode *m = &display->modes[i];

		if (m && v_active == m->timing.v_active &&
			h_active == m->timing.h_active &&
			refresh_rate == m->timing.refresh_rate) {
			rc = m->timing.v_front_porch;
			break;
		}
	}
	mutex_unlock(&display->display_lock);

	return rc;
}

int dsi_display_find_mode(struct dsi_display *display,
		const struct dsi_display_mode *cmp,
		struct dsi_display_mode **out_mode)
{
	u32 count, i;
	int rc;

	if (!display || !out_mode)
		return -EINVAL;

	*out_mode = NULL;

	rc = dsi_display_get_mode_count(display, &count);
	if (rc)
		return rc;

	if (!display->modes) {
		struct dsi_display_mode *m;

		rc = dsi_display_get_modes(display, &m);
		if (rc)
			return rc;
	}

	mutex_lock(&display->display_lock);
	for (i = 0; i < count; i++) {
		struct dsi_display_mode *m = &display->modes[i];

		if (cmp->timing.v_active == m->timing.v_active &&
			cmp->timing.h_active == m->timing.h_active &&
			cmp->timing.refresh_rate == m->timing.refresh_rate &&
			cmp->panel_mode == m->panel_mode &&
			cmp->pixel_clk_khz == m->pixel_clk_khz) {
			*out_mode = m;
			rc = 0;
			break;
		}
	}
	mutex_unlock(&display->display_lock);

	if (!*out_mode) {
		pr_err("[%s] failed to find mode for v_active %u h_active %u fps %u pclk %u\n",
				display->name, cmp->timing.v_active,
				cmp->timing.h_active, cmp->timing.refresh_rate,
				cmp->pixel_clk_khz);
		rc = -ENOENT;
	}

	return rc;
}

/**
 * dsi_display_validate_mode_change() - Validate mode change case.
 * @display:     DSI display handle.
 * @cur_mode:    Current mode.
 * @adj_mode:    Mode to be set.
 *               MSM_MODE_FLAG_SEAMLESS_VRR flag is set if there
 *               is change in fps but vactive and hactive are same.
 *               DSI_MODE_FLAG_DYN_CLK flag is set if there
 *               is change in clk but vactive and hactive are same.
 * Return: error code.
 */
 u32 mode_fps = 90;
EXPORT_SYMBOL(mode_fps);
extern void ts_switch_poll_rate(bool is_90);
int dsi_display_validate_mode_change(struct dsi_display *display,
			struct dsi_display_mode *cur_mode,
			struct dsi_display_mode *adj_mode)
{
	int rc = 0;
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_dyn_clk_caps *dyn_clk_caps;

	if (!display || !adj_mode) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel || !display->panel->cur_mode) {
		pr_debug("Current panel mode not set\n");
		return rc;
	}

	mutex_lock(&display->display_lock);

	if ((cur_mode->timing.v_active == adj_mode->timing.v_active) &&
		(cur_mode->timing.h_active == adj_mode->timing.h_active)) {
		/* dfps change use case */
		if (cur_mode->timing.refresh_rate !=
		    adj_mode->timing.refresh_rate) {
			dsi_panel_get_dfps_caps(display->panel, &dfps_caps);

			if (mode_fps != adj_mode->timing.refresh_rate) {
				mode_fps = adj_mode->timing.refresh_rate;
				pr_err("set fps: %d, fresh_rate_report_enable : %d\n", mode_fps, fresh_rate_report_enable);
				ts_switch_poll_rate(mode_fps == 90 ? true : false);

				if (fresh_rate_report_enable) {
					input_event(fresh_rate_input_dev, EV_MSC, MSC_RAW, mode_fps);
					input_sync(fresh_rate_input_dev);
				}

			}

			if (dfps_caps.dfps_support) {
				pr_debug("Mode switch is seamless variable refresh\n");
				adj_mode->dsi_mode_flags |= DSI_MODE_FLAG_VRR;
				SDE_EVT32(cur_mode->timing.refresh_rate,
					  adj_mode->timing.refresh_rate,
					  cur_mode->timing.h_front_porch,
					  adj_mode->timing.h_front_porch);
			}
		}

		/* dynamic clk change use case */
		if (cur_mode->pixel_clk_khz != adj_mode->pixel_clk_khz) {
			dyn_clk_caps = &(display->panel->dyn_clk_caps);
			if (dyn_clk_caps->dyn_clk_support) {
				pr_debug("dynamic clk change detected\n");
				if (adj_mode->dsi_mode_flags
						& DSI_MODE_FLAG_VRR) {
					pr_err("dfps and dyn clk not supported in same commit\n");
					rc = -ENOTSUPP;
					goto error;
				}

				adj_mode->dsi_mode_flags |=
						DSI_MODE_FLAG_DYN_CLK;
				SDE_EVT32(cur_mode->pixel_clk_khz,
						adj_mode->pixel_clk_khz);
			}
		}
	}

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_validate_mode(struct dsi_display *display,
			      struct dsi_display_mode *mode,
			      u32 flags)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_display_mode adj_mode;

	if (!display || !mode) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	adj_mode = *mode;
	adjust_timing_by_ctrl_count(display, &adj_mode);

	rc = dsi_panel_validate_mode(display->panel, &adj_mode);
	if (rc) {
		pr_err("[%s] panel mode validation failed, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_validate_timing(ctrl->ctrl, &adj_mode.timing);
		if (rc) {
			pr_err("[%s] ctrl mode validation failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}

		rc = dsi_phy_validate_mode(ctrl->phy, &adj_mode.timing);
		if (rc) {
			pr_err("[%s] phy mode validation failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	if ((flags & DSI_VALIDATE_FLAG_ALLOW_ADJUST) &&
			(mode->dsi_mode_flags & DSI_MODE_FLAG_SEAMLESS)) {
		rc = dsi_display_validate_mode_seamless(display, mode);
		if (rc) {
			pr_err("[%s] seamless not possible rc=%d\n",
				display->name, rc);
			goto error;
		}
	}

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_set_mode(struct dsi_display *display,
			 struct dsi_display_mode *mode,
			 u32 flags)
{
	int rc = 0;
	struct dsi_display_mode adj_mode;

	if (!display || !mode || !display->panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	adj_mode = *mode;
	adjust_timing_by_ctrl_count(display, &adj_mode);

	/*For dynamic DSI setting, use specified clock rate */
	if (display->cached_clk_rate > 0)
		adj_mode.priv_info->clk_rate_hz = display->cached_clk_rate;

	rc = dsi_display_validate_mode_set(display, &adj_mode, flags);
	if (rc) {
		pr_err("[%s] mode cannot be set\n", display->name);
		goto error;
	}

	rc = dsi_display_set_mode_sub(display, &adj_mode, flags);
	if (rc) {
		pr_err("[%s] failed to set mode\n", display->name);
		goto error;
	}

	if (!display->panel->cur_mode) {
		display->panel->cur_mode =
			kzalloc(sizeof(struct dsi_display_mode), GFP_KERNEL);
		if (!display->panel->cur_mode) {
			rc = -ENOMEM;
			goto error;
		}
	}

	memcpy(display->panel->cur_mode, &adj_mode, sizeof(adj_mode));

	mode_fps = display->panel->cur_mode->timing.refresh_rate;
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_set_tpg_state(struct dsi_display *display, bool enable)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_set_tpg_state(ctrl->ctrl, enable);
		if (rc) {
			pr_err("[%s] failed to set tpg state for host_%d\n",
			       display->name, i);
			goto error;
		}
	}

	display->is_tpg_enabled = enable;
error:
	return rc;
}

static int dsi_display_pre_switch(struct dsi_display *display)
{
	int rc = 0;

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	rc = dsi_display_ctrl_update(display);
	if (rc) {
		pr_err("[%s] failed to update DSI controller, rc=%d\n",
			   display->name, rc);
		goto error_ctrl_clk_off;
	}

	rc = dsi_display_set_clk_src(display, true);
	if (rc) {
		pr_err("[%s] failed to set DSI link clock source, rc=%d\n",
			display->name, rc);
		goto error_ctrl_deinit;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI link clocks, rc=%d\n",
			   display->name, rc);
		goto error_ctrl_deinit;
	}

	goto error;

error_ctrl_deinit:
	(void)dsi_display_ctrl_deinit(display);
error_ctrl_clk_off:
	(void)dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
error:
	return rc;
}

static bool _dsi_display_validate_host_state(struct dsi_display *display)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		if (!dsi_ctrl_validate_host_state(ctrl->ctrl))
			return false;
	}

	return true;
}

static void dsi_display_handle_fifo_underflow(struct work_struct *work)
{
	struct dsi_display *display = NULL;

	display = container_of(work, struct dsi_display, fifo_underflow_work);
	if (!display || !display->panel ||
	    atomic_read(&display->panel->esd_recovery_pending)) {
		pr_debug("Invalid recovery use case\n");
		return;
	}

	mutex_lock(&display->display_lock);

	if (!_dsi_display_validate_host_state(display)) {
		mutex_unlock(&display->display_lock);
		return;
	}

	pr_debug("handle DSI FIFO underflow error\n");

	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	dsi_display_soft_reset(display);
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);

	mutex_unlock(&display->display_lock);
}

static void dsi_display_handle_fifo_overflow(struct work_struct *work)
{
	struct dsi_display *display = NULL;
	struct dsi_display_ctrl *ctrl;
	int i, rc;
	int mask = BIT(20); /* clock lane */
	int (*cb_func)(void *event_usr_ptr,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3);
	void *data;
	u32 version = 0;

	display = container_of(work, struct dsi_display, fifo_overflow_work);
	if (!display || !display->panel ||
	    (display->panel->panel_mode != DSI_OP_VIDEO_MODE) ||
	    atomic_read(&display->panel->esd_recovery_pending)) {
		pr_debug("Invalid recovery use case\n");
		return;
	}

	mutex_lock(&display->display_lock);

	if (!_dsi_display_validate_host_state(display)) {
		mutex_unlock(&display->display_lock);
		return;
	}

	pr_debug("handle DSI FIFO overflow error\n");
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);

	/*
	 * below recovery sequence is not applicable to
	 * hw version 2.0.0, 2.1.0 and 2.2.0, so return early.
	 */
	ctrl = &display->ctrl[display->clk_master_idx];
	version = dsi_ctrl_get_hw_version(ctrl->ctrl);
	if (!version || (version < 0x20020001))
		goto end;

	/* reset ctrl and lanes */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_reset(ctrl->ctrl, mask);
		rc = dsi_phy_lane_reset(ctrl->phy);
	}

	/* wait for display line count to be in active area */
	ctrl = &display->ctrl[display->clk_master_idx];
	if (ctrl->ctrl->recovery_cb.event_cb) {
		cb_func = ctrl->ctrl->recovery_cb.event_cb;
		data = ctrl->ctrl->recovery_cb.event_usr_ptr;
		rc = cb_func(data, SDE_CONN_EVENT_VID_FIFO_OVERFLOW,
				display->clk_master_idx, 0, 0, 0, 0);
		if (rc < 0) {
			pr_debug("sde callback failed\n");
			goto end;
		}
	}

	/* Enable Video mode for DSI controller */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_ctrl_vid_engine_en(ctrl->ctrl, true);
	}
	/*
	 * Add sufficient delay to make sure
	 * pixel transmission has started
	 */
	udelay(200);
end:
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	mutex_unlock(&display->display_lock);
}

static void dsi_display_handle_lp_rx_timeout(struct work_struct *work)
{
	struct dsi_display *display = NULL;
	struct dsi_display_ctrl *ctrl;
	int i, rc;
	int mask = (BIT(20) | (0xF << 16)); /* clock lane and 4 data lane */
	int (*cb_func)(void *event_usr_ptr,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3);
	void *data;
	u32 version = 0;

	display = container_of(work, struct dsi_display, lp_rx_timeout_work);
	if (!display || !display->panel ||
	    (display->panel->panel_mode != DSI_OP_VIDEO_MODE) ||
	    atomic_read(&display->panel->esd_recovery_pending)) {
		pr_debug("Invalid recovery use case\n");
		return;
	}

	mutex_lock(&display->display_lock);

	if (!_dsi_display_validate_host_state(display)) {
		mutex_unlock(&display->display_lock);
		return;
	}

	pr_debug("handle DSI LP RX Timeout error\n");

	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);

	/*
	 * below recovery sequence is not applicable to
	 * hw version 2.0.0, 2.1.0 and 2.2.0, so return early.
	 */
	ctrl = &display->ctrl[display->clk_master_idx];
	version = dsi_ctrl_get_hw_version(ctrl->ctrl);
	if (!version || (version < 0x20020001))
		goto end;

	/* reset ctrl and lanes */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_reset(ctrl->ctrl, mask);
		rc = dsi_phy_lane_reset(ctrl->phy);
	}

	ctrl = &display->ctrl[display->clk_master_idx];
	if (ctrl->ctrl->recovery_cb.event_cb) {
		cb_func = ctrl->ctrl->recovery_cb.event_cb;
		data = ctrl->ctrl->recovery_cb.event_usr_ptr;
		rc = cb_func(data, SDE_CONN_EVENT_VID_FIFO_OVERFLOW,
				display->clk_master_idx, 0, 0, 0, 0);
		if (rc < 0) {
			pr_debug("Target is in suspend/shutdown\n");
			goto end;
		}
	}

	/* Enable Video mode for DSI controller */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_ctrl_vid_engine_en(ctrl->ctrl, true);
	}

	/*
	 * Add sufficient delay to make sure
	 * pixel transmission as started
	 */
	udelay(200);

end:
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	mutex_unlock(&display->display_lock);
}

static int dsi_display_cb_error_handler(void *data,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3)
{
	struct dsi_display *display =  data;

	if (!display || !(display->err_workq))
		return -EINVAL;

	switch (event_idx) {
	case DSI_FIFO_UNDERFLOW:
		queue_work(display->err_workq, &display->fifo_underflow_work);
		break;
	case DSI_FIFO_OVERFLOW:
		queue_work(display->err_workq, &display->fifo_overflow_work);
		break;
	case DSI_LP_Rx_TIMEOUT:
		queue_work(display->err_workq, &display->lp_rx_timeout_work);
		break;
	default:
		pr_warn("unhandled error interrupt: %d\n", event_idx);
		break;
	}

	return 0;
}

static void dsi_display_register_error_handler(struct dsi_display *display)
{
	int i = 0;
	struct dsi_display_ctrl *ctrl;
	struct dsi_event_cb_info event_info;

	if (!display)
		return;

	display->err_workq = create_singlethread_workqueue("dsi_err_workq");
	if (!display->err_workq) {
		pr_err("failed to create dsi workq!\n");
		return;
	}

	INIT_WORK(&display->fifo_underflow_work,
				dsi_display_handle_fifo_underflow);
	INIT_WORK(&display->fifo_overflow_work,
				dsi_display_handle_fifo_overflow);
	INIT_WORK(&display->lp_rx_timeout_work,
				dsi_display_handle_lp_rx_timeout);

	memset(&event_info, 0, sizeof(event_info));

	event_info.event_cb = dsi_display_cb_error_handler;
	event_info.event_usr_ptr = display;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		ctrl->ctrl->irq_info.irq_err_cb = event_info;
	}
}

static void dsi_display_unregister_error_handler(struct dsi_display *display)
{
	int i = 0;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		memset(&ctrl->ctrl->irq_info.irq_err_cb,
		       0, sizeof(struct dsi_event_cb_info));
	}

	if (display->err_workq) {
		destroy_workqueue(display->err_workq);
		display->err_workq = NULL;
	}
}

int dsi_display_prepare(struct dsi_display *display)
{
	int rc = 0;
	struct dsi_display_mode *mode;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel->cur_mode) {
		pr_err("no valid mode set for the display");
		return -EINVAL;
	}

	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	mode = display->panel->cur_mode;

	dsi_display_set_ctrl_esd_check_flag(display, false);

	/* Set up ctrl isr before enabling core clk */
	dsi_display_ctrl_isr_configure(display, true);

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) {
		if (display->is_cont_splash_enabled) {
			pr_err("DMS is not supposed to be set on first frame\n");
			rc = -EINVAL;
			goto error;
		}
		/* update dsi ctrl for new mode */
		rc = dsi_display_pre_switch(display);
		if (rc)
			pr_err("[%s] panel pre-prepare-res-switch failed, rc=%d\n",
					display->name, rc);
		goto error;
	}

	if (!(mode->dsi_mode_flags & DSI_MODE_FLAG_POMS) &&
		(!display->is_cont_splash_enabled)) {
		/*
		 * For continuous splash usecase we skip panel
		 * pre prepare since the regulator vote is already
		 * taken care in splash resource init
		 */
		rc = dsi_panel_pre_prepare(display->panel);
		if (rc) {
			pr_err("[%s] panel pre-prepare failed, rc=%d\n",
					display->name, rc);
			goto error;
		}
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error_panel_post_unprep;
	}

	/*
	 * If ULPS during suspend feature is enabled, then DSI PHY was
	 * left on during suspend. In this case, we do not need to reset/init
	 * PHY. This would have already been done when the CORE clocks are
	 * turned on. However, if cont splash is disabled, the first time DSI
	 * is powered on, phy init needs to be done unconditionally.
	 */
	if (!display->panel->ulps_suspend_enabled || !display->ulps_enabled) {
		rc = dsi_display_phy_sw_reset(display);
		if (rc) {
			pr_err("[%s] failed to reset phy, rc=%d\n",
				display->name, rc);
			goto error_ctrl_clk_off;
		}

		rc = dsi_display_phy_enable(display);
		if (rc) {
			pr_err("[%s] failed to enable DSI PHY, rc=%d\n",
			       display->name, rc);
			goto error_ctrl_clk_off;
		}
	}

	rc = dsi_display_set_clk_src(display, true);
	if (rc) {
		pr_err("[%s] failed to set DSI link clock source, rc=%d\n",
			display->name, rc);
		goto error_phy_disable;
	}

	rc = dsi_display_ctrl_init(display);
	if (rc) {
		pr_err("[%s] failed to setup DSI controller, rc=%d\n",
		       display->name, rc);
		goto error_phy_disable;
	}
	/* Set up DSI ERROR event callback */
	dsi_display_register_error_handler(display);

	rc = dsi_display_ctrl_host_enable(display);
	if (rc) {
		pr_err("[%s] failed to enable DSI host, rc=%d\n",
		       display->name, rc);
		goto error_ctrl_deinit;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI link clocks, rc=%d\n",
		       display->name, rc);
		goto error_host_engine_off;
	}

	if (!display->is_cont_splash_enabled) {
		/*
		 * For continuous splash usecase, skip panel prepare and
		 * ctl reset since the pnael and ctrl is already in active
		 * state and panel on commands are not needed
		 */
		rc = dsi_display_soft_reset(display);
		if (rc) {
			pr_err("[%s] failed soft reset, rc=%d\n",
					display->name, rc);
			goto error_ctrl_link_off;
		}

		if (!(mode->dsi_mode_flags & DSI_MODE_FLAG_POMS)) {
			rc = dsi_panel_prepare(display->panel);
			if (rc) {
				pr_err("[%s] panel prepare failed, rc=%d\n",
						display->name, rc);
				goto error_ctrl_link_off;
			}
		}
	}
	goto error;

error_ctrl_link_off:
	(void)dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_OFF);
error_host_engine_off:
	(void)dsi_display_ctrl_host_disable(display);
error_ctrl_deinit:
	(void)dsi_display_ctrl_deinit(display);
error_phy_disable:
	(void)dsi_display_phy_disable(display);
error_ctrl_clk_off:
	(void)dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
error_panel_post_unprep:
	(void)dsi_panel_post_unprepare(display->panel);
error:
	mutex_unlock(&display->display_lock);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

static int dsi_display_calc_ctrl_roi(const struct dsi_display *display,
		const struct dsi_display_ctrl *ctrl,
		const struct msm_roi_list *req_rois,
		struct dsi_rect *out_roi)
{
	const struct dsi_rect *bounds = &ctrl->ctrl->mode_bounds;
	struct dsi_display_mode *cur_mode;
	struct msm_roi_caps *roi_caps;
	struct dsi_rect req_roi = { 0 };
	int rc = 0;

	cur_mode = display->panel->cur_mode;
	if (!cur_mode)
		return 0;

	roi_caps = &cur_mode->priv_info->roi_caps;
	if (req_rois->num_rects > roi_caps->num_roi) {
		pr_err("request for %d rois greater than max %d\n",
				req_rois->num_rects,
				roi_caps->num_roi);
		rc = -EINVAL;
		goto exit;
	}

	/**
	 * if no rois, user wants to reset back to full resolution
	 * note: h_active is already divided by ctrl_count
	 */
	if (!req_rois->num_rects) {
		*out_roi = *bounds;
		goto exit;
	}

	/* intersect with the bounds */
	req_roi.x = req_rois->roi[0].x1;
	req_roi.y = req_rois->roi[0].y1;
	req_roi.w = req_rois->roi[0].x2 - req_rois->roi[0].x1;
	req_roi.h = req_rois->roi[0].y2 - req_rois->roi[0].y1;
	dsi_rect_intersect(&req_roi, bounds, out_roi);

exit:
	/* adjust the ctrl origin to be top left within the ctrl */
	out_roi->x = out_roi->x - bounds->x;

	pr_debug("ctrl%d:%d: req (%d,%d,%d,%d) bnd (%d,%d,%d,%d) out (%d,%d,%d,%d)\n",
			ctrl->dsi_ctrl_idx, ctrl->ctrl->cell_index,
			req_roi.x, req_roi.y, req_roi.w, req_roi.h,
			bounds->x, bounds->y, bounds->w, bounds->h,
			out_roi->x, out_roi->y, out_roi->w, out_roi->h);

	return rc;
}

static int dsi_display_qsync(struct dsi_display *display, bool enable)
{
	int i;
	int rc = 0;

	if (!display->panel->qsync_min_fps) {
		pr_err("%s:ERROR: qsync set, but no fps\n", __func__);
		return 0;
	}

	mutex_lock(&display->display_lock);

	display_for_each_ctrl(i, display) {

		if (enable) {
			/* send the commands to enable qsync */
			rc = dsi_panel_send_qsync_on_dcs(display->panel, i);
			if (rc) {
				pr_err("fail qsync ON cmds rc:%d\n", rc);
				goto exit;
			}
		} else {
			/* send the commands to enable qsync */
			rc = dsi_panel_send_qsync_off_dcs(display->panel, i);
			if (rc) {
				pr_err("fail qsync OFF cmds rc:%d\n", rc);
				goto exit;
			}
		}

		dsi_ctrl_setup_avr(display->ctrl[i].ctrl, enable);
	}

exit:
	SDE_EVT32(enable, display->panel->qsync_min_fps, rc);
	mutex_unlock(&display->display_lock);
	return rc;
}

static int dsi_display_set_roi(struct dsi_display *display,
		struct msm_roi_list *rois)
{
	struct dsi_display_mode *cur_mode;
	struct msm_roi_caps *roi_caps;
	int rc = 0;
	int i;

	if (!display || !rois || !display->panel)
		return -EINVAL;

	cur_mode = display->panel->cur_mode;
	if (!cur_mode)
		return 0;

	roi_caps = &cur_mode->priv_info->roi_caps;
	if (!roi_caps->enabled)
		return 0;

	display_for_each_ctrl(i, display) {
		struct dsi_display_ctrl *ctrl = &display->ctrl[i];
		struct dsi_rect ctrl_roi;
		bool changed = false;

		rc = dsi_display_calc_ctrl_roi(display, ctrl, rois, &ctrl_roi);
		if (rc) {
			pr_err("dsi_display_calc_ctrl_roi failed rc %d\n", rc);
			return rc;
		}

		rc = dsi_ctrl_set_roi(ctrl->ctrl, &ctrl_roi, &changed);
		if (rc) {
			pr_err("dsi_ctrl_set_roi failed rc %d\n", rc);
			return rc;
		}

		if (!changed)
			continue;

		/* send the new roi to the panel via dcs commands */
		rc = dsi_panel_send_roi_dcs(display->panel, i, &ctrl_roi);
		if (rc) {
			pr_err("dsi_panel_set_roi failed rc %d\n", rc);
			return rc;
		}

		/* re-program the ctrl with the timing based on the new roi */
		rc = dsi_ctrl_setup(ctrl->ctrl);
		if (rc) {
			pr_err("dsi_ctrl_setup failed rc %d\n", rc);
			return rc;
		}
	}

	return rc;
}

int dsi_display_pre_kickoff(struct drm_connector *connector,
		struct dsi_display *display,
		struct msm_display_kickoff_params *params)
{
	int rc = 0;
	int i;

	SDE_ATRACE_BEGIN("dsi_display_pre_kickoff");
	/* check and setup MISR */
	if (display->misr_enable)
		_dsi_display_setup_misr(display);

	rc = dsi_display_set_roi(display, params->rois);

	/* dynamic DSI clock setting */
	if (atomic_read(&display->clkrate_change_pending)) {
		mutex_lock(&display->display_lock);
		/*
		 * acquire panel_lock to make sure no commands are in progress
		 */
		dsi_panel_acquire_panel_lock(display->panel);

		/*
		 * Wait for DSI command engine not to be busy sending data
		 * from display engine.
		 * If waiting fails, return "rc" instead of below "ret" so as
		 * not to impact DRM commit. The clock updating would be
		 * deferred to the next DRM commit.
		 */
		display_for_each_ctrl(i, display) {
			struct dsi_ctrl *ctrl = display->ctrl[i].ctrl;
			int ret = 0;

			ret = dsi_ctrl_wait_for_cmd_mode_mdp_idle(ctrl);
			if (ret)
				goto wait_failure;
		}

		/*
		 * Don't check the return value so as not to impact DRM commit
		 * when error occurs.
		 */
		(void)dsi_display_force_update_dsi_clk(display);
wait_failure:
		/* release panel_lock */
		dsi_panel_release_panel_lock(display->panel);
		mutex_unlock(&display->display_lock);
	}

	SDE_ATRACE_END("dsi_display_pre_kickoff");
	return rc;
}

int dsi_display_config_ctrl_for_cont_splash(struct dsi_display *display)
{
	int rc = 0;

	if (!display || !display->panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel->cur_mode) {
		pr_err("no valid mode set for the display");
		return -EINVAL;
	}

	if (!display->is_cont_splash_enabled)
		return 0;

	if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		rc = dsi_display_vid_engine_enable(display);
		if (rc) {
			pr_err("[%s]failed to enable DSI video engine, rc=%d\n",
			       display->name, rc);
			goto error_out;
		}
	} else if (display->config.panel_mode == DSI_OP_CMD_MODE) {
		rc = dsi_display_cmd_engine_enable(display);
		if (rc) {
			pr_err("[%s]failed to enable DSI cmd engine, rc=%d\n",
			       display->name, rc);
			goto error_out;
		}
	} else {
		pr_err("[%s] Invalid configuration\n", display->name);
		rc = -EINVAL;
	}

error_out:
	return rc;
}

int dsi_display_pre_commit(void *display,
		struct msm_display_conn_params *params)
{
	bool enable = false;
	int rc = 0;

	if (!display || !params) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (params->qsync_update) {
		enable = (params->qsync_mode > 0) ? true : false;
		rc = dsi_display_qsync(display, enable);
		if (rc)
			pr_err("%s failed to send qsync commands\n",
				__func__);
		SDE_EVT32(params->qsync_mode, rc);
	}

	return rc;
}

int dsi_display_enable(struct dsi_display *display)
{
	int rc = 0;
	struct dsi_display_mode *mode;

	if (!display || !display->panel) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel->cur_mode) {
		pr_err("no valid mode set for the display");
		return -EINVAL;
	}
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	/* Engine states and panel states are populated during splash
	 * resource init and hence we return early
	 */
	SDE_ATRACE_BEGIN("dsi_display_enable");

	if (display->is_cont_splash_enabled) {

		dsi_display_config_ctrl_for_cont_splash(display);

		rc = dsi_display_splash_res_cleanup(display);
		if (rc) {
			pr_err("Continuous splash res cleanup failed, rc=%d\n",
				rc);
			return -EINVAL;
		}

		display->panel->panel_initialized = true;
		pr_debug("cont splash enabled, display enable not required\n");
		return 0;
	}

	mutex_lock(&display->display_lock);

	mode = display->panel->cur_mode;

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) {
		rc = dsi_panel_post_switch(display->panel);
		if (rc) {
			pr_err("[%s] failed to switch DSI panel mode, rc=%d\n",
				   display->name, rc);
			goto error;
		}
	} else if (!(display->panel->cur_mode->dsi_mode_flags &
			DSI_MODE_FLAG_POMS)){
		rc = dsi_panel_enable(display->panel);
		if (rc) {
			pr_err("[%s] failed to enable DSI panel, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	if (mode->priv_info->dsc_enabled) {
		mode->priv_info->dsc.pic_width *= display->ctrl_count;
		rc = dsi_panel_update_pps(display->panel);
		if (rc) {
			pr_err("[%s] panel pps cmd update failed, rc=%d\n",
				display->name, rc);
			goto error;
		}
	}

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) {
		rc = dsi_panel_switch(display->panel);
		if (rc)
			pr_err("[%s] failed to switch DSI panel mode, rc=%d\n",
				   display->name, rc);

		goto error;
	}

	if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		pr_debug("%s:enable video timing eng\n", __func__);
		rc = dsi_display_vid_engine_enable(display);
		if (rc) {
			pr_err("[%s]failed to enable DSI video engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_panel;
		}
	} else if (display->config.panel_mode == DSI_OP_CMD_MODE) {
		pr_debug("%s:enable command timing eng\n", __func__);
		rc = dsi_display_cmd_engine_enable(display);
		if (rc) {
			pr_err("[%s]failed to enable DSI cmd engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_panel;
		}
	} else {
		pr_err("[%s] Invalid configuration\n", display->name);
		rc = -EINVAL;
		goto error_disable_panel;
	}

	goto error;

error_disable_panel:
	(void)dsi_panel_disable(display->panel);
error:
	mutex_unlock(&display->display_lock);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);

	SDE_ATRACE_END("dsi_display_enable");
	return rc;
}

int dsi_display_post_enable(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	if (display->panel->cur_mode->dsi_mode_flags & DSI_MODE_FLAG_POMS) {
		if (display->config.panel_mode == DSI_OP_CMD_MODE)
			dsi_panel_mode_switch_to_cmd(display->panel);

		if (display->config.panel_mode == DSI_OP_VIDEO_MODE)
			dsi_panel_mode_switch_to_vid(display->panel);
	} else {
		rc = dsi_panel_post_enable(display->panel);
		if (rc)
			pr_err("[%s] panel post-enable failed, rc=%d\n",
			       display->name, rc);
	}

	/* remove the clk vote for CMD mode panels */
	if (display->config.panel_mode == DSI_OP_CMD_MODE)
		dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);

	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_pre_disable(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	/* enable the clk vote for CMD mode panels */
	if (display->config.panel_mode == DSI_OP_CMD_MODE)
		dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);

	if (display->poms_pending) {
		if (display->config.panel_mode == DSI_OP_CMD_MODE)
			dsi_panel_pre_mode_switch_to_video(display->panel);

		if (display->config.panel_mode == DSI_OP_VIDEO_MODE)
			dsi_panel_pre_mode_switch_to_cmd(display->panel);
	} else {
		rc = dsi_panel_pre_disable(display->panel);
		if (rc)
			pr_err("[%s] panel pre-disable failed, rc=%d\n",
			       display->name, rc);
	}

	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_disable(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	rc = dsi_display_wake_up(display);
	if (rc)
		pr_err("[%s] display wake up failed, rc=%d\n",
		       display->name, rc);

	if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		rc = dsi_display_vid_engine_disable(display);
		if (rc)
			pr_err("[%s]failed to disable DSI vid engine, rc=%d\n",
			       display->name, rc);
	} else if (display->config.panel_mode == DSI_OP_CMD_MODE) {
		rc = dsi_display_cmd_engine_disable(display);
		if (rc)
			pr_err("[%s]failed to disable DSI cmd engine, rc=%d\n",
			       display->name, rc);
	} else {
		pr_err("[%s] Invalid configuration\n", display->name);
		rc = -EINVAL;
	}

	if (!display->poms_pending) {
		rc = dsi_panel_disable(display->panel);
		if (rc)
			pr_err("[%s] failed to disable DSI panel, rc=%d\n",
			       display->name, rc);
	}

	mutex_unlock(&display->display_lock);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

int dsi_display_update_pps(char *pps_cmd, void *disp)
{
	struct dsi_display *display;

	if (pps_cmd == NULL || disp == NULL) {
		pr_err("Invalid parameter\n");
		return -EINVAL;
	}

	display = disp;
	mutex_lock(&display->display_lock);
	memcpy(display->panel->dsc_pps_cmd, pps_cmd, DSI_CMD_PPS_SIZE);
	mutex_unlock(&display->display_lock);

	return 0;
}

int dsi_display_set_acl_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->acl_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}
	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_acl_mode(panel, level);
	if (rc)
		pr_err("unable to set acl mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_acl_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->acl_mode;
}

int dsi_display_get_gamma_para(struct dsi_display *dsi_display,
			       struct dsi_panel *panel)
{
	int i = 0;
	int j = 0;
	int rc = 0;
	int flags = 0;
	char fb[13] = { 0 };
	//char c8[135] = {0};
	//char c9[180] = {0};
	char b3[47] = { 0 };
	char fb_temp[13] = { 0 };
	char c8_temp[135] = { 0 };
	char c9_temp[180] = { 0 };
	char b3_temp[47] = { 0 };
	char gamma_para_60hz[452] = { 0 };
	char gamma_para_backup[413] = { 0 };
	int check_sum_60hz = 0;

	struct dsi_cmd_desc *cmds;
	struct dsi_display_mode *mode;
	struct dsi_display_ctrl *m_ctrl;

	pr_err("%s start\n", __func__);

	m_ctrl = &dsi_display->ctrl[dsi_display->cmd_master_idx];
	if (!panel || !m_ctrl)
		return -EINVAL;

	rc = dsi_display_cmd_engine_enable(dsi_display);
	if (rc) {
		pr_err("cmd engine enable failed\n");
		return -EINVAL;
	}

	dsi_panel_acquire_panel_lock(panel);
	mode = panel->cur_mode;

/* Read 60hz gamma para */
	memcpy(gamma_para_backup, gamma_para[0], 413);
	do {
		check_sum_60hz = 0;
		if (j > 0) {
			pr_err("Failed to read the 60hz gamma parameters %d!",
			       j);
			for (i = 0; i < 52; i++) {
				if (i != 51) {
					pr_err
					    ("[60hz][%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X",
					     i * 8, gamma_para[0][i * 8],
					     i * 8 + 1,
					     gamma_para[0][i * 8 + 1],
					     i * 8 + 2,
					     gamma_para[0][i * 8 + 2],
					     i * 8 + 3,
					     gamma_para[0][i * 8 + 3],
					     i * 8 + 4,
					     gamma_para[0][i * 8 + 4],
					     i * 8 + 5,
					     gamma_para[0][i * 8 + 5],
					     i * 8 + 6,
					     gamma_para[0][i * 8 + 6],
					     i * 8 + 7,
					     gamma_para[0][i * 8 + 7]);
				} else {
					pr_err
					    ("[60hz][%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X,[%d]0x%02X",
					     i * 8, gamma_para[0][i * 8],
					     i * 8 + 1,
					     gamma_para[0][i * 8 + 1],
					     i * 8 + 2,
					     gamma_para[0][i * 8 + 2],
					     i * 8 + 3,
					     gamma_para[0][i * 8 + 3],
					     i * 8 + 4,
					     gamma_para[0][i * 8 + 4]);
				}
			}
			mdelay(1000);
		}
		for (i = 0; i < 452; i++) {
			rc = dsi_panel_tx_cmd_set(panel,
						  DSI_CMD_SET_GAMMA_FLASH_PRE_READ_1);
			if (rc) {
				pr_err
				    ("Failed to send DSI_CMD_SET_GAMMA_FLASH_PRE_READ_1 command\n");
				goto error;
			}

			rc = dsi_panel_gamma_read_address_setting(panel, i);
			if (rc) {
				pr_err("Failed to set gamma read address\n");
				goto error;
			}

			rc = dsi_panel_tx_cmd_set(panel,
						  DSI_CMD_SET_GAMMA_FLASH_PRE_READ_2);
			if (rc) {
				pr_err
				    ("Failed to send DSI_CMD_SET_GAMMA_FLASH_PRE_READ_2 command\n");
				goto error;
			}

			flags = 0;
			cmds =
			    mode->priv_info->
			    cmd_sets[DSI_CMD_SET_GAMMA_FLASH_READ_FB].cmds;
			if (cmds->last_command) {
				cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
				flags |= DSI_CTRL_CMD_LAST_COMMAND;
			}
			flags |=
			    (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
			if (!m_ctrl->ctrl->vaddr)
				goto error;
			cmds->msg.rx_buf = fb_temp;
			cmds->msg.rx_len = 13;
			rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg,
						   flags);
			if (rc <= 0) {
				pr_err
				    ("Failed to read DSI_CMD_SET_GAMMA_FLASH_READ_FB\n");
				goto error;
			}
			memcpy(fb, cmds->msg.rx_buf, 13);

			rc = dsi_panel_tx_cmd_set(panel,
						  DSI_CMD_SET_LEVEL2_KEY_DISABLE);
			if (rc) {
				pr_err
				    ("Failed to send DSI_CMD_SET_LEVEL2_KEY_DISABLE command\n");
				goto error;
			}

			if (i < 135) {
				gamma_para[0][i + 18] = fb[12];
			} else if (i < 315) {
				gamma_para[0][i + 26] = fb[12];
			} else if (i < 360) {
				gamma_para[0][i + 43] = fb[12];
			}

			gamma_para_60hz[i] = fb[12];
			if (i < 449) {
				check_sum_60hz =
				    gamma_para_60hz[i] + check_sum_60hz;
			}
			j++;
		}
	}
	while ((check_sum_60hz !=
		(gamma_para_60hz[450] << 8) + gamma_para_60hz[451])
	       && (j < 10));

	if (check_sum_60hz ==
	    (gamma_para_60hz[450] << 8) + gamma_para_60hz[451]) {
		pr_err("Read 60hz gamma done\n");
	} else {
		pr_err("Failed to read 60hz gamma, use default 60hz gamma.\n");
		memcpy(gamma_para[0], gamma_para_backup, 413);
		gamma_read_flag = GAMMA_READ_ERROR;
	}

/* Read 90hz gamma para */
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LEVEL2_KEY_ENABLE);
	if (rc) {
		pr_err
		    ("Failed to send DSI_CMD_SET_LEVEL2_KEY_ENABLE command\n");
		goto error;
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_GAMMA_OTP_READ_C8_SMRPS);
	if (rc) {
		pr_err
		    ("Failed to send DSI_CMD_SET_GAMMA_OTP_READ_C8_SMRPS command\n");
		goto error;
	}

	flags = 0;
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_GAMMA_OTP_READ_C8].cmds;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
	cmds->msg.rx_buf = c8_temp;
	cmds->msg.rx_len = 135;
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
	if (rc <= 0) {
		pr_err("Failed to read DSI_CMD_SET_GAMMA_OTP_READ_C8\n");
		goto error;
	}
	memcpy(&gamma_para[1][18], cmds->msg.rx_buf, 135);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_GAMMA_OTP_READ_C9_SMRPS);
	if (rc) {
		pr_err
		    ("Failed to send DSI_CMD_SET_GAMMA_OTP_READ_C9_SMRPS command\n");
		goto error;
	}

	flags = 0;
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_GAMMA_OTP_READ_C9].cmds;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
	cmds->msg.rx_buf = c9_temp;
	cmds->msg.rx_len = 180;
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
	if (rc <= 0) {
		pr_err("Failed to read DSI_CMD_SET_GAMMA_OTP_READ_C9\n");
		goto error;
	}
	memcpy(&gamma_para[1][161], cmds->msg.rx_buf, 180);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_GAMMA_OTP_READ_B3_SMRPS);
	if (rc) {
		pr_err
		    ("Failed to send DSI_CMD_SET_GAMMA_OTP_READ_C9_SMRPS command\n");
		goto error;
	}

	flags = 0;
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_GAMMA_OTP_READ_B3].cmds;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
	cmds->msg.rx_buf = b3_temp;
	cmds->msg.rx_len = 47;
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
	if (rc <= 0) {
		pr_err("Failed to read DSI_CMD_SET_GAMMA_OTP_READ_B3\n");
		goto error;
	}
	memcpy(b3, cmds->msg.rx_buf, 47);
	memcpy(&gamma_para[1][358], &b3[2], 45);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LEVEL2_KEY_DISABLE);
	if (rc) {
		pr_err
		    ("Failed to send DSI_CMD_SET_GAMMA_OTP_READ_C9_SMRPS command\n");
		goto error;
	}
	pr_err("Read 90hz gamma done\n");

 error:
	dsi_panel_release_panel_lock(panel);
	dsi_display_cmd_engine_disable(dsi_display);
	pr_err("%s end\n", __func__);
	return rc;
}

int dsi_display_gamma_read(struct dsi_display *dsi_display)
{
	int rc = 0;
	struct dsi_panel *panel = NULL;

	pr_err("%s start\n", __func__);
	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;
	mutex_lock(&dsi_display->display_lock);

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle, DSI_ALL_CLKS,
				  DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	dsi_display_get_gamma_para(dsi_display, panel);

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle, DSI_ALL_CLKS,
				  DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

 error:
	mutex_unlock(&dsi_display->display_lock);
	pr_err("%s end\n", __func__);
	return rc;
}

void dsi_display_gamma_read_work(struct work_struct *work)
{
	struct dsi_display *dsi_display;

	dsi_display = get_main_display();

	if (((dsi_display->panel->panel_production_info & 0x0F) == 0x0C)
	    || ((dsi_display->panel->panel_production_info & 0x0F) == 0x0E)
	    || ((dsi_display->panel->panel_production_info & 0x0F) == 0x0D))
		dsi_display_gamma_read(dsi_display);

	dsi_panel_parse_gamma_cmd_sets();
}

int dsi_display_read_serial_number(struct dsi_display *dsi_display,
				   struct dsi_panel *panel, char *buf, int len)
{
	int rc = 0;
	int flags = 0;
	int code_info = 0;
	int stage_info = 0;
	int prodution_info = 0;
	struct dsi_cmd_desc *cmds;
	struct dsi_display_mode *mode;
	struct dsi_display_ctrl *m_ctrl;

	pr_err("%s start\n", __func__);

	m_ctrl = &dsi_display->ctrl[dsi_display->cmd_master_idx];

	if (!panel || !m_ctrl)
		return -EINVAL;

	rc = dsi_display_cmd_engine_enable(dsi_display);
	if (rc) {
		pr_err("cmd engine enable failed\n");
		return -EINVAL;
	}

	dsi_panel_acquire_panel_lock(panel);
	mode = panel->cur_mode;

	if (dsi_display->panel->hw_type != DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LCDINFO_PRE);
		if (rc) {
			pr_err
			    ("Failed to send DSI_CMD_SET_LCDINFO_PRE commands\n");
			goto error;
		}
	}

	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_SERIAL_NUMBER].cmds;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
	if (!m_ctrl->ctrl->vaddr)
		goto error;
	cmds->msg.rx_buf = buf;
	cmds->msg.rx_len = len;
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
	if (rc <= 0)
		pr_err("Failed to get panel serial number, rc=%d\n", rc);

	if (dsi_display->panel->hw_type != DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		flags = 0;
		cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_CODE_INFO].cmds;
		if (cmds->last_command) {
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		if (!m_ctrl->ctrl->vaddr)
			goto error;
		cmds->msg.rx_buf = &code_info;
		cmds->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
		if (rc <= 0)
			pr_err("Failed to get code info, rc=%d\n", rc);

		panel->panel_code_info = code_info & 0xff;
		pr_err("Code info is 0x%X\n", panel->panel_code_info);

		flags = 0;
		cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_STAGE_INFO].cmds;
		if (cmds->last_command) {
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		if (!m_ctrl->ctrl->vaddr)
			goto error;
		cmds->msg.rx_buf = &stage_info;
		cmds->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
		if (rc <= 0)
			pr_err("Failed to get stage info, rc=%d\n", rc);

		panel->panel_stage_info = stage_info & 0xff;
		pr_err("Stage info is 0x%X\n", panel->panel_stage_info);

		flags = 0;
		cmds =
		    mode->priv_info->cmd_sets[DSI_CMD_SET_PRODUCTION_INFO].cmds;
		if (cmds->last_command) {
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
		if (!m_ctrl->ctrl->vaddr)
			goto error;
		cmds->msg.rx_buf = &prodution_info;
		cmds->msg.rx_len = 1;
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
		if (rc <= 0)
			pr_err("Failed to get production info, rc=%d\n", rc);

		panel->panel_production_info = prodution_info & 0xff;
		pr_err("Production info is 0x%X\n",
		       panel->panel_production_info);

		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LCDINFO_POST);
		if (rc) {
			pr_err
			    ("Failed to send DSI_CMD_SET_LCDINFO_POST commands\n");
			goto error;
		}
	}

 error:
	dsi_panel_release_panel_lock(panel);
	dsi_display_cmd_engine_disable(dsi_display);
	pr_err("%s end\n", __func__);

	return rc;
}

int dsi_display_get_serial_number(struct drm_connector *connector)
{
	struct dsi_display_mode *mode;
	struct dsi_panel *panel = NULL;
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	char buf[32];
	int panel_year = 0;
	int panel_mon = 0;
	int panel_day = 0;
	int panel_hour = 0;
	int panel_min = 0;
	int panel_sec = 0;
	int len = 0;
	int count;
	int rc = 0;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;
	mutex_lock(&dsi_display->display_lock);

	if (!dsi_panel_initialized(panel) || !panel->cur_mode)
		goto error;

	mode = panel->cur_mode;
	count =
	    mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_SERIAL_NUMBER].count;
	if (count) {
		len = panel->panel_min_index;
		if (len > sizeof(buf)) {
			pr_err("len is large than buf size!!!\n");
			goto error;
		}

		if ((panel->panel_year_index > len)
		    || (panel->panel_mon_index > len)
		    || (panel->panel_day_index > len)
		    || (panel->panel_hour_index > len)
		    || (panel->panel_min_index > len)) {
			pr_err("Panel serial number index not corrected.\n");
			goto error;
		}

		rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
					  DSI_ALL_CLKS, DSI_CLK_ON);
		if (rc) {
			pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
			       dsi_display->name, rc);
			goto error;
		}

		memset(buf, 0, sizeof(buf));
		dsi_display_read_serial_number(dsi_display, panel, buf, len);
		panel_year =
		    2011 + ((buf[panel->panel_year_index - 1] >> 4) & 0x0f);
		if (panel_year == 2011)
			panel_year = 0;
		panel_mon = buf[panel->panel_mon_index - 1] & 0x0f;
		if ((panel_mon > 12) || (panel_mon < 1)) {
			pr_err("Panel Mon not corrected.\n");
			panel_mon = 0;
		}
		panel_day = buf[panel->panel_day_index - 1] & 0x3f;
		if ((panel_day > 31) || (panel_day < 1)) {
			pr_err("Panel Day not corrected.\n");
			panel_day = 0;
		}
		panel_hour = buf[panel->panel_hour_index - 1] & 0x3f;
		if ((panel_hour > 23) || (panel_hour < 0)) {
			pr_err("Panel Hour not corrected.\n");
			panel_hour = 0;
		}
		panel_min = buf[panel->panel_min_index - 1] & 0x3f;
		if ((panel_min > 59) || (panel_min < 0)) {
			pr_err("Panel Min not corrected.\n");
			panel_min = 0;
		}
		panel_sec = buf[panel->panel_sec_index - 1] & 0x3f;
		if ((panel_sec > 59) || (panel_sec < 0)) {
			pr_err("Panel sec not corrected.\n");
			panel_sec = 0;
		}
		panel->panel_year = panel_year;
		panel->panel_mon = panel_mon;
		panel->panel_day = panel_day;
		panel->panel_hour = panel_hour;
		panel->panel_min = panel_min;
		panel->panel_sec = panel_sec;

		rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
					  DSI_ALL_CLKS, DSI_CLK_OFF);
		if (rc) {
			pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
			       dsi_display->name, rc);
			goto error;
		}
	} else {
		pr_err("This panel not support serial number.\n");
	}

 error:
	mutex_unlock(&dsi_display->display_lock);
	pr_err("%s end\n", __func__);
	return 0;
}

int dsi_display_get_serial_number_year(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_year;
}

int dsi_display_get_serial_number_mon(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_mon;
}

int dsi_display_get_serial_number_day(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_day;
}

int dsi_display_get_serial_number_hour(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_hour;
}

int dsi_display_get_serial_number_min(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_min;
}

int dsi_display_get_serial_number_sec(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_sec;
}

int dsi_display_get_code_info(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_code_info;
}

int dsi_display_get_stage_info(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_stage_info;
}

int dsi_display_get_production_info(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	pr_err("%s end\n", __func__);

	return dsi_display->panel->panel_production_info;
}

int dsi_display_get_serial_number_AT(struct drm_connector *connector)
{
	struct dsi_display_mode *mode;
	struct dsi_panel *panel = NULL;
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	char buf[32];
	int panel_year = 0;
	int panel_mon = 0;
	int panel_day = 0;
	int panel_hour = 0;
	int panel_min = 0;
	int panel_sec = 0;
	int len = 0;
	u32 count;
	int rc = 0;
	uint64_t serial_number;
	pr_err("%s start\n", __func__);

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;
	mutex_lock(&dsi_display->display_lock);

	if (!dsi_panel_initialized(panel) || !panel->cur_mode) {
		goto error;
	}
	mode = panel->cur_mode;
	count =
	    mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_SERIAL_NUMBER].count;

	if (count) {

		len = panel->panel_min_index;
		if (len > sizeof(buf)) {
			pr_err("len is large than buf size!!!\n");
			goto error;
		}

		if ((panel->panel_year_index > len)
		    || (panel->panel_mon_index > len)
		    || (panel->panel_day_index > len)
		    || (panel->panel_hour_index > len)
		    || (panel->panel_min_index > len)) {
			pr_err("Panel serial number index not corrected.\n");
			goto error;
		}

		rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
					  DSI_ALL_CLKS, DSI_CLK_ON);
		if (rc) {
			pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
			       dsi_display->name, rc);
			goto error;
		}
		memset(buf, 0, sizeof(buf));
		dsi_display_read_serial_number(dsi_display, panel, buf, len);

		panel_year =
		    2011 + ((buf[panel->panel_year_index - 1] >> 4) & 0x0f);
		if (panel_year == 2011) {
			panel_year = 0;
		}
		panel_mon = buf[panel->panel_mon_index - 1] & 0x0f;
		if ((panel_mon > 12) || (panel_mon < 1)) {
			pr_err("Panel Mon not corrected.\n");
			panel_mon = 0;
		}
		panel_day = buf[panel->panel_day_index - 1] & 0x3f;
		if ((panel_day > 31) || (panel_day < 1)) {
			pr_err("Panel Day not corrected.\n");
			panel_day = 0;
		}
		panel_hour = buf[panel->panel_hour_index - 1] & 0x3f;
		if ((panel_hour > 23) || (panel_hour < 0)) {
			pr_err("Panel Hour not corrected.\n");
			panel_hour = 0;
		}
		panel_min = buf[panel->panel_min_index - 1] & 0x3f;
		if ((panel_min > 59) || (panel_min < 0)) {
			pr_err("Panel Min not corrected.\n");
			panel_min = 0;
		}
		panel_sec = buf[panel->panel_sec_index - 1] & 0x3f;
		if ((panel_sec > 59) || (panel_sec < 0)) {
			pr_err("Panel sec not corrected.\n");
			panel_sec = 0;
		}
/*
		serial_number  =      ((uint64_t)panel_year << 56)
							+ ((uint64_t)panel_mon  << 48)
							+ ((uint64_t)panel_day  << 40)
							+ ((uint64_t)panel_hour << 32)
							+ ((uint64_t)panel_min  << 24)
							+ ((uint64_t)panel_sec  << 16)
							+ ((uint64_t)0 << 8)
							+ ((uint64_t)0);
*/
		serial_number =
		    (uint64_t) panel_year *10000000000 +
		    (uint64_t) panel_mon *100000000 +
		    (uint64_t) panel_day *1000000 +
		    (uint64_t) panel_hour *10000 + (uint64_t) panel_min *100 +
		    (uint64_t) panel_sec;

		dsi_display_get_serial_number_id(serial_number);

		rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
					  DSI_ALL_CLKS, DSI_CLK_OFF);
		if (rc) {
			pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
			       dsi_display->name, rc);
			goto error;
		}
	} else {
		pr_err("This panel not support serial number.\n");
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	pr_err("%s END\n", __func__);
	return 0;
}

uint64_t dsi_display_get_serial_number_id(uint64_t serial_number)
{

	static uint64_t serial_number_at;

	pr_err("%s start\n", __func__);
	if (0 == SERIAL_NUMBER_flag) {
		serial_number_at = serial_number;
		if (0 == serial_number_at)
			SERIAL_NUMBER_flag = 0;
		else
			SERIAL_NUMBER_flag = 1;
	}

	return serial_number_at;
}

int dsi_display_set_hbm_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->hbm_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_hbm_mode(panel, level);
	if (rc)
		pr_err("unable to set hbm mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_hbm_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->hbm_mode;
}

int dsi_display_set_hbm_brightness(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return -EINVAL;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	if (dsi_display->panel->hw_type == DSI_PANEL_DEFAULT) {
		dsi_display->panel->hbm_brightness = 0;
		return 0;
	}

	mutex_lock(&dsi_display->display_lock);

	panel->hbm_brightness = level;

	if (!dsi_panel_initialized(panel))
		goto error;

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_hbm_brightness(panel, level);
	if (rc)
		pr_err("Failed to set hbm brightness mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_hbm_brightness(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->hbm_brightness;
}

extern int oneplus_force_screenfp;

int dsi_display_set_fp_hbm_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->op_force_screenfp = level;
	oneplus_force_screenfp = panel->op_force_screenfp;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_op_set_hbm_mode(panel, level);
	if (rc)
		pr_err("unable to set hbm mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_fp_hbm_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->op_force_screenfp;
}

int dsi_display_set_dci_p3_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->dci_p3_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_dci_p3_mode(panel, level);
	if (rc)
		pr_err("unable to set dci_p3 mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_dci_p3_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->dci_p3_mode;
}

int dsi_display_set_night_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->night_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_night_mode(panel, level);
	if (rc)
		pr_err("unable to set night mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_night_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->night_mode;
}

int dsi_display_set_native_display_p3_mode(struct drm_connector *connector,
					   int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->naive_display_p3_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_native_display_p3_mode(panel, level);
	if (rc)
		pr_err("unable to set native display p3 mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_native_display_p3_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->naive_display_p3_mode;
}

int dsi_display_set_native_display_wide_color_mode(struct drm_connector
						   *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->naive_display_wide_color_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_native_display_wide_color_mode(panel, level);
	if (rc)
		pr_err("unable to set native display p3 mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_set_native_loading_effect_mode(struct drm_connector *connector,
					       int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->naive_display_loading_effect_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_native_loading_effect_mode(panel, level);
	if (rc)
		pr_err("unable to set loading effect mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_set_customer_srgb_mode(struct drm_connector *connector,
				       int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->naive_display_customer_srgb_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_customer_srgb_mode(panel, level);
	if (rc)
		pr_err("unable to set customer srgb mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_set_customer_p3_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->naive_display_customer_p3_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_customer_p3_mode(panel, level);
	if (rc)
		pr_err("unable to set customer srgb mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_set_native_display_srgb_color_mode(struct drm_connector
						   *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	panel->naive_display_srgb_color_mode = level;
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_native_display_srgb_color_mode(panel, level);
	if (rc)
		pr_err("unable to set native display p3 mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_native_display_srgb_color_mode(struct drm_connector
						   *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->naive_display_srgb_color_mode;
}

int dsi_display_get_native_display_wide_color_mode(struct drm_connector
						   *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->naive_display_wide_color_mode;
}

int dsi_display_get_native_display_loading_effect_mode(struct drm_connector
						       *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->naive_display_loading_effect_mode;
}

int dsi_display_get_customer_srgb_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->naive_display_customer_srgb_mode;
}

int dsi_display_get_customer_p3_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->naive_display_customer_p3_mode;
}

int dsi_display_set_aod_mode(struct drm_connector *connector, int level)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;
	panel->aod_mode = level;

	if (dsi_display->panel->hw_type == DSI_PANEL_DEFAULT) {
		dsi_display->panel->aod_mode = 0;
		return 0;
	}

	mutex_lock(&dsi_display->display_lock);
	if (!dsi_panel_initialized(panel)) {
		goto error;
	}

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
	rc = dsi_panel_set_aod_mode(panel, level);
	if (rc)
		pr_err("unable to set aod mode\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
				  DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}
 error:
	mutex_unlock(&dsi_display->display_lock);

	return rc;
}

int dsi_display_get_aod_mode(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->aod_mode;
}

int dsi_display_set_aod_disable(struct drm_connector *connector, int disable)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;
	mutex_lock(&dsi_display->display_lock);
	panel->aod_disable = disable;
	mutex_unlock(&dsi_display->display_lock);

	return rc;
}

int dsi_display_get_aod_disable(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

	if ((connector == NULL) || (connector->encoder == NULL)
	    || (connector->encoder->bridge == NULL))
		return 0;

	c_bridge = to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->aod_disable;
}

int dsi_display_read_panel_id(struct dsi_display *dsi_display,
			      struct dsi_panel *panel, char *buf, int len)
{
	int rc = 0;
	u32 flags = 0;
	struct dsi_cmd_desc *cmds;
	struct dsi_display_mode *mode;
	struct dsi_display_ctrl *m_ctrl;
	int retry_times;

	m_ctrl = &dsi_display->ctrl[dsi_display->cmd_master_idx];

	if (!panel || !m_ctrl)
		return -EINVAL;

	rc = dsi_display_cmd_engine_enable(dsi_display);
	if (rc) {
		pr_err("cmd engine enable failed\n");
		return -EINVAL;
	}

	dsi_panel_acquire_panel_lock(panel);

	mode = panel->cur_mode;
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID].cmds;;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
	if (!m_ctrl->ctrl->vaddr)
		goto error;

	cmds->msg.rx_buf = buf;
	cmds->msg.rx_len = len;
	retry_times = 0;
	do {
		rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmds->msg, flags);
		retry_times++;
	} while ((rc <= 0) && (retry_times < 3));

	if (rc <= 0)
		pr_err("rx cmd transfer failed rc=%d\n", rc);

 error:
	dsi_panel_release_panel_lock(panel);

	dsi_display_cmd_engine_disable(dsi_display);

	return rc;
}

char dsi_display_ascii_to_int(char ascii, int *ascii_err)
{
	char int_value;

	if ((ascii >= 48) && (ascii <= 57)){
		int_value = ascii - 48;
	}
	else if ((ascii >= 65) && (ascii <= 70)) {
		int_value = ascii - 65 + 10;
	}
	else if ((ascii >= 97) && (ascii <= 102)) {
		int_value = ascii - 97 + 10;
	}
	else {
		int_value = 0;
		*ascii_err = 1;
		pr_err("Bad para: %d , please enter the right value!", ascii);
	}

	return int_value;
}

int dsi_display_update_dsi_on_command(struct drm_connector *connector, const char *buf, size_t count)
{
	int i = 0;
	int j = 0;
	int ascii_err = 0;
	unsigned int length;
	char *data;
	struct dsi_panel_cmd_set *set;

	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
			|| (connector->encoder->bridge == NULL))
		return -EINVAL;

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&dsi_display->display_lock);

	length = count / 3;
	data = kzalloc(length + 1, GFP_KERNEL);

	for (i = 0; (buf[i+2] != 10) && (j < length); i = i+3) {
		data[j] = dsi_display_ascii_to_int(buf[i], &ascii_err) << 4;
		data[j] += dsi_display_ascii_to_int(buf[i+1], &ascii_err);
		j++;
	}
	data[j] = dsi_display_ascii_to_int(buf[i], &ascii_err) << 4;
	data[j] += dsi_display_ascii_to_int(buf[i+1], &ascii_err);
	if (ascii_err == 1) {
		pr_err("Bad Para, ignore this command\n");
		goto error;
	}

	set = &panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_ON];

	rc = dsi_panel_update_cmd_sets_sub(set, DSI_CMD_SET_ON, data, length);
	if (rc)
		pr_err("Failed to update_cmd_sets_sub, rc=%d\n", rc);

error:
	kfree(data);
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

static int dsi_display_get_mipi_dsi_msg(const struct mipi_dsi_msg *msg, char* buf)
{
	int len = 0;
	size_t i;
	char *tx_buf = (char*)msg->tx_buf;
	/* Packet Info */
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", msg->type);
	/* Last bit */
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", (msg->flags & MIPI_DSI_MSG_LASTCOMMAND) ? 1 : 0);
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", msg->channel);
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", (unsigned int)msg->flags);
	/* Delay */
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", msg->wait_ms);
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X %02X ", msg->tx_len >> 8, msg->tx_len & 0x00FF);

	/* Packet Payload */
	for (i = 0 ; i < msg->tx_len ; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", tx_buf[i]);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	return len;
}

int dsi_display_get_dsi_on_command(struct drm_connector *connector, char *buf)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	struct dsi_panel_cmd_set *cmd;
	int i = 0;
	int count = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
			|| (connector->encoder->bridge == NULL))
		return 0;

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	cmd = &dsi_display->panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_ON];

	for (i = 0; i < cmd->count; i++) {
		count += dsi_display_get_mipi_dsi_msg(&cmd->cmds[i].msg, &buf[count]);
	}

	return count;
}

int dsi_display_update_dsi_panel_command(struct drm_connector *connector, const char *buf, size_t count)
{
	int i = 0;
	int j = 0;
	int ascii_err = 0;
	unsigned int length;
	char *data;
	struct dsi_panel_cmd_set *set;

	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
			|| (connector->encoder->bridge == NULL))
		return -EINVAL;

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	if (panel->hw_type == DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		return 0;
	}

	mutex_lock(&dsi_display->display_lock);

	if (!dsi_panel_initialized(panel))
		goto error;

	length = count / 3;
	data = kzalloc(length + 1, GFP_KERNEL);

	for (i = 0; (buf[i+2] != 10) && (j < length); i = i+3) {
		data[j] = dsi_display_ascii_to_int(buf[i], &ascii_err) << 4;
		data[j] += dsi_display_ascii_to_int(buf[i+1], &ascii_err);
		j++;
	}
	data[j] = dsi_display_ascii_to_int(buf[i], &ascii_err) << 4;
	data[j] += dsi_display_ascii_to_int(buf[i+1], &ascii_err);
	if (ascii_err == 1) {
		pr_err("Bad Para, ignore this command\n");
		kfree(data);
		goto error;
	}

	set = &panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_COMMAND];

	rc = dsi_panel_update_cmd_sets_sub(set, DSI_CMD_SET_PANEL_COMMAND, data, length);
	if (rc)
		pr_err("Failed to update_cmd_sets_sub, rc=%d\n", rc);
	kfree(data);

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
			dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_send_dsi_panel_command(panel);
	if (rc)
		pr_err("Failed to send dsi panel command\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
			dsi_display->name, rc);
		goto error;
	}

error:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_get_dsi_panel_command(struct drm_connector *connector, char *buf)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	struct dsi_panel_cmd_set *cmd;
	int i = 0;
	int count = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
			|| (connector->encoder->bridge == NULL))
		return 0;

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	cmd = &dsi_display->panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_COMMAND];

	for (i = 0; i < cmd->count; i++)
		count += dsi_display_get_mipi_dsi_msg(&cmd->cmds[i].msg, &buf[count]);

	return count;
}

int dsi_display_update_dsi_seed_command(struct drm_connector *connector, const char *buf, size_t count)
{
	int i = 0;
	int j = 0;
	int ascii_err = 0;
	unsigned int length;
	char *data;
	struct dsi_panel_cmd_set *set;

	struct dsi_display *dsi_display = NULL;
	struct dsi_panel *panel = NULL;
	struct dsi_bridge *c_bridge;
	int rc = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
			|| (connector->encoder->bridge == NULL))
		return -EINVAL;

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;

	if (panel->hw_type == DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		return 0;
	}

	mutex_lock(&panel->panel_lock);

	if (!dsi_panel_initialized(panel))
		goto error;

	length = count / 3;
	if (length != 0x16) {
		pr_err("Insufficient parameters!\n");
		goto error;
	}
	data = kzalloc(length + 1, GFP_KERNEL);

	for (i = 0; (buf[i+2] != 10) && (j < length); i = i+3) {
		data[j] = dsi_display_ascii_to_int(buf[i], &ascii_err) << 4;
		data[j] += dsi_display_ascii_to_int(buf[i+1], &ascii_err);
		j++;
	}
	data[j] = dsi_display_ascii_to_int(buf[i], &ascii_err) << 4;
	data[j] += dsi_display_ascii_to_int(buf[i+1], &ascii_err);
	if (ascii_err == 1) {
		pr_err("Bad Para, ignore this command\n");
		kfree(data);
		goto error;
	}

	set = &panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_SEED_COMMAND];

	if (panel->hw_type == DSI_PANEL_SAMSUNG_S6E3HC2)
		data[0] = WU_SEED_REGISTER;
	if (panel->hw_type == DSI_PANEL_SAMSUNG_SOFEF03F_M)
		data[0] = UG_SEED_REGISTER;

	rc = dsi_panel_update_dsi_seed_command(set->cmds, DSI_CMD_SET_SEED_COMMAND, data);
	if (rc)
		pr_err("Failed to dsi_panel_update_dsi_seed_command, rc=%d\n", rc);
	kfree(data);

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		pr_err("[%s] failed to enable DSI core clocks, rc=%d\n",
			dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_send_dsi_seed_command(panel);
	if (rc)
		pr_err("Failed to send dsi seed command\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		pr_err("[%s] failed to disable DSI core clocks, rc=%d\n",
			dsi_display->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_display_get_dsi_seed_command(struct drm_connector *connector, char *buf)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	struct dsi_panel_cmd_set *cmd;
	int i = 0;
	int count = 0;

	if ((connector == NULL) || (connector->encoder == NULL)
			|| (connector->encoder->bridge == NULL))
		return 0;

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	if (dsi_display->panel->hw_type == DSI_PANEL_SAMSUNG_S6E3FC2X01) {
		return 0;
	}

	cmd = &dsi_display->panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_SEED_COMMAND];

	for (i = 0; i < cmd->count; i++) {
		count += dsi_display_get_mipi_dsi_msg(&cmd->cmds[i].msg, &buf[count]);
	}

	return count;
}

int dsi_display_panel_mismatch_check(struct drm_connector *connector)
{
    struct dsi_display_mode *mode;
	struct dsi_panel *panel = NULL;
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;
	char buf[32];
	int panel_id;
	u32 count;
	int rc = 0;

    if ((connector == NULL) || (connector->encoder == NULL)
            || (connector->encoder->bridge == NULL))
        return 0;

    c_bridge =  to_dsi_bridge(connector->encoder->bridge);
    dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return -EINVAL;

	panel = dsi_display->panel;
	mutex_lock(&dsi_display->display_lock);

    if (!dsi_panel_initialized(panel) || !panel->cur_mode) {
        panel->panel_mismatch = 0;
		goto error;
	}

	if (!panel->panel_mismatch_check) {
	    panel->panel_mismatch = 0;
	    pr_err("This hw not support panel mismatch check(dvt-mp)\n");
		goto error;
	}

    mode = panel->cur_mode;
	count = mode->priv_info->cmd_sets[DSI_CMD_SET_PANEL_ID].count;
    if (count) {
        rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
                DSI_ALL_CLKS, DSI_CLK_ON);
        if (rc) {
            pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
                dsi_display->name, rc);
            goto error;
        }

        memset(buf, 0, sizeof(buf));
        dsi_display_read_panel_id(dsi_display, panel, buf, 1);

        panel_id = buf[0];
        panel->panel_mismatch = (panel_id == 0x03)? 1 : 0;

        rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
            DSI_ALL_CLKS, DSI_CLK_OFF);
        if (rc) {
            pr_err("[%s] failed to enable DSI clocks, rc=%d\n",
                dsi_display->name, rc);
            goto error;
        }
    } else{
        panel->panel_mismatch = 0;
        pr_err("This panel not support panel mismatch check.\n");
    }
error:
	mutex_unlock(&dsi_display->display_lock);
	return 0;
}

int dsi_display_panel_mismatch(struct drm_connector *connector)
{
	struct dsi_display *dsi_display = NULL;
	struct dsi_bridge *c_bridge;

    if ((connector == NULL) || (connector->encoder == NULL)
            || (connector->encoder->bridge == NULL))
        return 0;

    c_bridge =  to_dsi_bridge(connector->encoder->bridge);
    dsi_display = c_bridge->display;

	if ((dsi_display == NULL) || (dsi_display->panel == NULL))
		return 0;

	return dsi_display->panel->panel_mismatch;
}

int dsi_display_unprepare(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	rc = dsi_display_wake_up(display);
	if (rc)
		pr_err("[%s] display wake up failed, rc=%d\n",
		       display->name, rc);

	if (!display->poms_pending) {
		rc = dsi_panel_unprepare(display->panel);
		if (rc)
			pr_err("[%s] panel unprepare failed, rc=%d\n",
			       display->name, rc);
	}

	dsi_display_set_clk_src(display, false);

	rc = dsi_display_ctrl_host_disable(display);
	if (rc)
		pr_err("[%s] failed to disable DSI host, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_OFF);
	if (rc)
		pr_err("[%s] failed to disable Link clocks, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_ctrl_deinit(display);
	if (rc)
		pr_err("[%s] failed to deinit controller, rc=%d\n",
		       display->name, rc);

	if (!display->panel->ulps_suspend_enabled) {
		rc = dsi_display_phy_disable(display);
		if (rc)
			pr_err("[%s] failed to disable DSI PHY, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc)
		pr_err("[%s] failed to disable DSI clocks, rc=%d\n",
		       display->name, rc);

	/* destrory dsi isr set up */
	dsi_display_ctrl_isr_configure(display, false);

	if (!display->poms_pending) {
		rc = dsi_panel_post_unprepare(display->panel);
		if (rc)
			pr_err("[%s] panel post-unprepare failed, rc=%d\n",
			       display->name, rc);
	}

	dsi_display_set_clk_src(display, false);

	mutex_unlock(&display->display_lock);

	/* Free up DSI ERROR event callback */
	dsi_display_unregister_error_handler(display);

	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}
//*mark.yao@PSW.MM.Display.LCD.Stability,2018/4/28,add for support aod,hbm,seed*/
struct dsi_display *get_main_display(void) {
		return primary_display;
}
EXPORT_SYMBOL(get_main_display);

static int __init dsi_display_register(void)
{
	dsi_phy_drv_register();
	dsi_ctrl_drv_register();

	dsi_display_parse_boot_display_selection();

	return platform_driver_register(&dsi_display_driver);
}

static void __exit dsi_display_unregister(void)
{
	platform_driver_unregister(&dsi_display_driver);
	dsi_ctrl_drv_unregister();
	dsi_phy_drv_unregister();
}
module_param_string(dsi_display0, dsi_display_primary, MAX_CMDLINE_PARAM_LEN,
								0600);
MODULE_PARM_DESC(dsi_display0,
	"msm_drm.dsi_display0=<display node>:<configX> where <display node> is 'primary dsi display node name' and <configX> where x represents index in the topology list");
module_param_string(dsi_display1, dsi_display_secondary, MAX_CMDLINE_PARAM_LEN,
								0600);
MODULE_PARM_DESC(dsi_display1,
	"msm_drm.dsi_display1=<display node>:<configX> where <display node> is 'secondary dsi display node name' and <configX> where x represents index in the topology list");
module_init(dsi_display_register);
module_exit(dsi_display_unregister);
