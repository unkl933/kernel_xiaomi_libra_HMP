/* Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/qpnp/pin.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/qpnp/pwm.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <asm/fcntl.h>
#include <asm/uaccess.h>
#include <linux/string.h>
#include <linux/display_state.h>
#include "mdss_dsi.h"

#define DT_CMD_HDR 6
#define MIN_REFRESH_RATE 30
#define DEFAULT_MDP_TRANSFER_TIME 14000

DEFINE_LED_TRIGGER(bl_led_trigger);

bool display_on = true;

bool is_display_on()
{
	return display_on;
}

void mdss_dsi_panel_pwm_cfg(struct mdss_dsi_ctrl_pdata *ctrl)
{
	if (ctrl->pwm_pmi)
		return;

	ctrl->pwm_bl = pwm_request(ctrl->pwm_lpg_chan, "lcd-bklt");
	if (ctrl->pwm_bl == NULL || IS_ERR(ctrl->pwm_bl)) {
		pr_err("%s: Error: lpg_chan=%d pwm request failed",
				__func__, ctrl->pwm_lpg_chan);
	}
	ctrl->pwm_enabled = 0;
}

static void mdss_dsi_panel_bklt_pwm(struct mdss_dsi_ctrl_pdata *ctrl, int level)
{
	int ret;
	u32 duty;
	u32 period_ns;

	if (ctrl->pwm_bl == NULL) {
		pr_err("%s: no PWM\n", __func__);
		return;
	}

	if (level == 0) {
		if (ctrl->pwm_enabled) {
			ret = pwm_config_us(ctrl->pwm_bl, level,
					ctrl->pwm_period);
			if (ret)
				pr_err("%s: pwm_config_us() failed err=%d.\n",
						__func__, ret);
			pwm_disable(ctrl->pwm_bl);
		}
		ctrl->pwm_enabled = 0;
		return;
	}

	duty = level * ctrl->pwm_period;
	duty /= ctrl->bklt_max;

	pr_debug("%s: bklt_ctrl=%d pwm_period=%d pwm_gpio=%d pwm_lpg_chan=%d\n",
			__func__, ctrl->bklt_ctrl, ctrl->pwm_period,
				ctrl->pwm_pmic_gpio, ctrl->pwm_lpg_chan);

	pr_debug("%s: ndx=%d level=%d duty=%d\n", __func__,
					ctrl->ndx, level, duty);

	if (ctrl->pwm_period >= USEC_PER_SEC) {
		ret = pwm_config_us(ctrl->pwm_bl, duty, ctrl->pwm_period);
		if (ret) {
			pr_err("%s: pwm_config_us() failed err=%d.\n",
					__func__, ret);
			return;
		}
	} else {
		period_ns = ctrl->pwm_period * NSEC_PER_USEC;
		ret = pwm_config(ctrl->pwm_bl,
				level * period_ns / ctrl->bklt_max,
				period_ns);
		if (ret) {
			pr_err("%s: pwm_config() failed err=%d.\n",
					__func__, ret);
			return;
		}
	}

	if (!ctrl->pwm_enabled) {
		ret = pwm_enable(ctrl->pwm_bl);
		if (ret)
			pr_err("%s: pwm_enable() failed err=%d\n", __func__,
				ret);
		ctrl->pwm_enabled = 1;
	}
}

static char dcs_cmd[2] = {0x54, 0x00}; /* DTYPE_DCS_READ */
static char rbuf[10];
static struct dsi_cmd_desc dcs_read_cmd = {
	{DTYPE_DCS_READ, 1, 0, 1, 5, sizeof(dcs_cmd)},
	dcs_cmd
};

void mdss_dsi_dcs_read_cb(u32 cb_result)
{
	u32 i;

	pr_info("%s: 0x%x. \n", __func__, cb_result);
	for (i=0; i<cb_result; i++)
		pr_debug("0x%x ", rbuf[i]);
	pr_debug("\n");
}

u32 mdss_dsi_panel_cmd_read(struct mdss_dsi_ctrl_pdata *ctrl, char cmd0,
		char cmd1, void (*fxn)(int), char *rbuf, int len)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return -EINVAL;
	}

	dcs_cmd[0] = cmd0;
	dcs_cmd[1] = cmd1;
	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &dcs_read_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_RX | CMD_REQ_COMMIT;
	cmdreq.rlen = len;
	cmdreq.rbuf = rbuf;
	cmdreq.cb = fxn; /* call back */
	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
	/*
	 * blocked here, until call back called
	 */

	return 0;
}

static void mdss_dsi_panel_cmds_send(struct mdss_dsi_ctrl_pdata *ctrl,
			struct dsi_panel_cmds *pcmds, u32 flags)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return;
	}

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = pcmds->cmds;
	cmdreq.cmds_cnt = pcmds->cmd_cnt;
	cmdreq.flags = flags;

	/*Panel ON/Off commands should be sent in DSI Low Power Mode*/
	if (pcmds->link_state == DSI_LP_MODE)
		cmdreq.flags  |= CMD_REQ_LP_MODE;
	else if (pcmds->link_state == DSI_HS_MODE)
		cmdreq.flags |= CMD_REQ_HS_MODE;

	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static char led_pwm1[2] = {0x51, 0x0};	/* DTYPE_DCS_WRITE1 */
static struct dsi_cmd_desc backlight_cmd = {
	{DTYPE_DCS_WRITE1, 1, 0, 0, 1, sizeof(led_pwm1)},
	led_pwm1
};

static void mdss_dsi_panel_bklt_dcs(struct mdss_dsi_ctrl_pdata *ctrl, int level)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return;
	}

	pr_debug("%s: level=%d\n", __func__, level);

	led_pwm1[1] = (unsigned char)level;

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &backlight_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static int mdss_dsi_request_gpios(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	int rc = 0;

	if (gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
		rc = gpio_request(ctrl_pdata->disp_en_gpio,
						"disp_enable");
		if (rc) {
			pr_err("request disp_en gpio failed, rc=%d\n",
				       rc);
			goto disp_en_gpio_err;
		}
	}
	rc = gpio_request(ctrl_pdata->rst_gpio, "disp_rst_n");
	if (rc) {
		pr_err("request reset gpio failed, rc=%d\n",
			rc);
		goto rst_gpio_err;
	}
	if (gpio_is_valid(ctrl_pdata->bklt_en_gpio)) {
		rc = gpio_request(ctrl_pdata->bklt_en_gpio,
						"bklt_enable");
		if (rc) {
			pr_err("request bklt gpio failed, rc=%d\n",
				       rc);
			goto bklt_en_gpio_err;
		}
	}
	if (gpio_is_valid(ctrl_pdata->mode_gpio)) {
		rc = gpio_request(ctrl_pdata->mode_gpio, "panel_mode");
		if (rc) {
			pr_err("request panel mode gpio failed,rc=%d\n",
								rc);
			goto mode_gpio_err;
		}
	}
	if (gpio_is_valid(ctrl_pdata->dual_en_gpio)) {
		rc = gpio_request(ctrl_pdata->dual_en_gpio,
						"dual_enable");
		if (rc) {
			pr_err("request dual port enable gpio failed, rc=%d\n",
				       rc);
			goto dual_en_gpio_err;
		}
	}
	return rc;

dual_en_gpio_err:
	if (gpio_is_valid(ctrl_pdata->mode_gpio))
		gpio_free(ctrl_pdata->mode_gpio);
mode_gpio_err:
	if (gpio_is_valid(ctrl_pdata->bklt_en_gpio))
		gpio_free(ctrl_pdata->bklt_en_gpio);
bklt_en_gpio_err:
	gpio_free(ctrl_pdata->rst_gpio);
rst_gpio_err:
	if (gpio_is_valid(ctrl_pdata->disp_en_gpio))
		gpio_free(ctrl_pdata->disp_en_gpio);
disp_en_gpio_err:
	return rc;
}

int mdss_dsi_panel_reset(struct mdss_panel_data *pdata, int enable)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_panel_info *pinfo = NULL;
	static u64 timestamp_panelon = 0;
	int i, rc = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	if (!gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
		pr_debug("%s:%d, reset line not configured\n",
			   __func__, __LINE__);
	}

	if (!gpio_is_valid(ctrl_pdata->rst_gpio)) {
		pr_debug("%s:%d, reset line not configured\n",
			   __func__, __LINE__);
		return rc;
	}

	pr_debug("%s: enable = %d\n", __func__, enable);
	pinfo = &(ctrl_pdata->panel_data.panel_info);

	if (enable) {
		rc = mdss_dsi_request_gpios(ctrl_pdata);
		if (rc) {
			pr_err("gpio request failed\n");
			return rc;
		}
		if (!pinfo->cont_splash_enabled) {
			if (gpio_is_valid(ctrl_pdata->dual_en_gpio))
				gpio_set_value((ctrl_pdata->dual_en_gpio), 1);
			if (gpio_is_valid(ctrl_pdata->disp_en_gpio))
				gpio_set_value((ctrl_pdata->disp_en_gpio), 1);

			for (i = 0; i < pdata->panel_info.rst_seq_len; ++i) {
				gpio_set_value((ctrl_pdata->rst_gpio),
					pdata->panel_info.rst_seq[i]);
				if (pdata->panel_info.rst_seq[++i])
					usleep(pinfo->rst_seq[i] * 1000);
			}

			if (gpio_is_valid(ctrl_pdata->bklt_en_gpio))
				gpio_set_value((ctrl_pdata->bklt_en_gpio), 1);
		}

		if (gpio_is_valid(ctrl_pdata->mode_gpio)) {
			if (pinfo->mode_gpio_state == MODE_GPIO_HIGH)
				gpio_set_value((ctrl_pdata->mode_gpio), 1);
			else if (pinfo->mode_gpio_state == MODE_GPIO_LOW)
				gpio_set_value((ctrl_pdata->mode_gpio), 0);
		}
		if (ctrl_pdata->ctrl_state & CTRL_STATE_PANEL_INIT) {
			pr_debug("%s: Panel Not properly turned OFF\n",
						__func__);
			ctrl_pdata->ctrl_state &= ~CTRL_STATE_PANEL_INIT;
			pr_debug("%s: Reset panel done\n", __func__);
		}
		/* get panel on timestamp */
		timestamp_panelon = get_jiffies_64();
	} else {
		/* caculate panel active duration */
		pinfo->panel_active += get_jiffies_64() - timestamp_panelon;

		if (gpio_is_valid(ctrl_pdata->dual_en_gpio)) {
			gpio_set_value((ctrl_pdata->dual_en_gpio), 0);
			gpio_free(ctrl_pdata->dual_en_gpio);
		}
		if (gpio_is_valid(ctrl_pdata->bklt_en_gpio)) {
			gpio_set_value((ctrl_pdata->bklt_en_gpio), 0);
			gpio_free(ctrl_pdata->bklt_en_gpio);
		}
		if (gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
			gpio_set_value((ctrl_pdata->disp_en_gpio), 0);
			gpio_free(ctrl_pdata->disp_en_gpio);
		}
		gpio_set_value((ctrl_pdata->rst_gpio), 0);
		udelay(2000);
		gpio_free(ctrl_pdata->rst_gpio);
		if (gpio_is_valid(ctrl_pdata->mode_gpio))
			gpio_free(ctrl_pdata->mode_gpio);
	}
	return rc;
}

/**
 * mdss_dsi_roi_merge() -  merge two roi into single roi
 *
 * Function used by partial update with only one dsi intf take 2A/2B
 * (column/page) dcs commands.
 */
static int mdss_dsi_roi_merge(struct mdss_dsi_ctrl_pdata *ctrl,
					struct mdss_rect *roi)
{
	struct mdss_panel_info *l_pinfo;
	struct mdss_rect *l_roi;
	struct mdss_rect *r_roi;
	struct mdss_dsi_ctrl_pdata *other = NULL;
	int ans = 0;

	if (ctrl->ndx == DSI_CTRL_LEFT) {
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_RIGHT);
		if (!other)
			return ans;
		l_pinfo = &(ctrl->panel_data.panel_info);
		l_roi = &(ctrl->panel_data.panel_info.roi);
		r_roi = &(other->panel_data.panel_info.roi);
	} else  {
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);
		if (!other)
			return ans;
		l_pinfo = &(other->panel_data.panel_info);
		l_roi = &(other->panel_data.panel_info.roi);
		r_roi = &(ctrl->panel_data.panel_info.roi);
	}

	if (l_roi->w == 0 && l_roi->h == 0) {
		/* right only */
		*roi = *r_roi;
		roi->x += l_pinfo->xres;/* add left full width to x-offset */
	} else {
		/* left only and left+righ */
		*roi = *l_roi;
		roi->w +=  r_roi->w; /* add right width */
		ans = 1;
	}

	return ans;
}

static char caset[] = {0x2a, 0x00, 0x00, 0x03, 0x00};	/* DTYPE_DCS_LWRITE */
static char paset[] = {0x2b, 0x00, 0x00, 0x05, 0x00};	/* DTYPE_DCS_LWRITE */

/* pack into one frame before sent */
static struct dsi_cmd_desc set_col_page_addr_cmd[] = {
	{{DTYPE_DCS_LWRITE, 0, 0, 0, 1, sizeof(caset)}, caset},	/* packed */
	{{DTYPE_DCS_LWRITE, 1, 0, 0, 1, sizeof(paset)}, paset},
};

static void inline mdss_panel_disparam_set(struct mdss_dsi_ctrl_pdata *ctrl, uint32_t param)
{
	uint32_t temp = 0;

	temp = param & 0x0000000F;
	switch(temp) {
	case 0x1:
		if (ctrl->dispparam_warm_cmds.cmd_cnt) {
			pr_info("warm\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_warm_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x2:
		if (ctrl->dispparam_default_cmds.cmd_cnt) {
			pr_info("normal\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_default_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x3:
		if (ctrl->dispparam_cold_cmds.cmd_cnt) {
			pr_info("cold\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_cold_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x4:
		if (ctrl->dispparam_gammareload_cmds.cmd_cnt) {
			pr_info("gammare load\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_gammareload_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x5:
		if (ctrl->dispparam_papermode_cmds.cmd_cnt) {
			pr_info("paper mode\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x6:
		if (ctrl->dispparam_papermode1_cmds.cmd_cnt) {
			pr_info("paper mode 1\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode1_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x7:
		if (ctrl->dispparam_papermode2_cmds.cmd_cnt) {
			pr_info("paper mode 2\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode2_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x8:
		if (ctrl->dispparam_papermode3_cmds.cmd_cnt) {
			pr_info("paper mode 3\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode3_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x9:
		if (ctrl->dispparam_papermode4_cmds.cmd_cnt) {
			pr_info("paper mode 4\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode4_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xa:
		if (ctrl->dispparam_papermode5_cmds.cmd_cnt) {
			pr_info("paper mode 5\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode5_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xb:
		if (ctrl->dispparam_papermode6_cmds.cmd_cnt) {
			pr_info("paper mode 6\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode6_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xc:
		if (ctrl->dispparam_papermode7_cmds.cmd_cnt) {
			pr_info("paper mode 7\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_papermode7_cmds, CMD_REQ_COMMIT);
		}
		break;

	default:
		break;
	}

	temp = param & 0x0000F000;
	switch(temp) {
	case 0xF000:
		if (ctrl->dispparam_level0_cmds.cmd_cnt) {
			pr_info("level 0\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level0_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x1000:
		if (ctrl->dispparam_level1_cmds.cmd_cnt) {
			pr_info("level 1\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level1_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x2000:
		if (ctrl->dispparam_level2_cmds.cmd_cnt) {
			pr_info("level 2\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level2_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x3000:
		if (ctrl->dispparam_level3_cmds.cmd_cnt) {
			pr_info("level 3\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level3_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x4000:
		if (ctrl->dispparam_level4_cmds.cmd_cnt) {
			pr_info("level4\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level4_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x5000:
		if (ctrl->dispparam_level5_cmds.cmd_cnt) {
			pr_info("level 5\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level5_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x6000:
		if (ctrl->dispparam_level6_cmds.cmd_cnt) {
			pr_info("level6\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_level6_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xA000:
		if (ctrl->dispparam_nightmode1_cmds.cmd_cnt) {
			pr_info("night mode 1\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_nightmode1_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xB000:
		if (ctrl->dispparam_nightmode2_cmds.cmd_cnt) {
			pr_info("night mode 2\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_nightmode2_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xC000:
		if (ctrl->dispparam_nightmode3_cmds.cmd_cnt) {
			pr_info("night mode 3\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_nightmode3_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xD000:
		if (ctrl->dispparam_nightmode4_cmds.cmd_cnt) {
			pr_info("night mode 4\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_nightmode4_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xE000:
		if (ctrl->dispparam_nightmode5_cmds.cmd_cnt) {
			pr_info("night mode5\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_nightmode5_cmds, CMD_REQ_COMMIT);
		}
		break;
	}

	temp = param & 0x000F0000;
	switch(temp) {
	case 0x10000:
		mdss_dsi_panel_cmd_read(ctrl, 0xAB, 0x00, (void *)mdss_dsi_dcs_read_cb, rbuf, 8);
		break;
	case 0x20000:
		if (ctrl->dispparam_test_cmds.cmd_cnt) {
			pr_info("test\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_test_cmds, CMD_REQ_COMMIT);
		}
		pr_info("NT 1-cut2 3-cut4: %d\n",
				mdss_dsi_panel_cmd_read(ctrl, 0x3A, 0x00, (void *)mdss_dsi_dcs_read_cb, rbuf, 8));

		break;
	case 0xA0000:
		if (ctrl->dispparam_idleon_cmds.cmd_cnt) {
			pr_info("idleon\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_idleon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xF0000:
		if (ctrl->dispparam_idleoff_cmds.cmd_cnt) {
			pr_info("idleoff\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_idleoff_cmds, CMD_REQ_COMMIT);
		}
		break;
	default:
		break;
	}

	temp = param & 0x000000F0;
	switch(temp) {
	case 0x10:
		if (ctrl->dispparam_ceon_cmds.cmd_cnt) {
			pr_info("ceon\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_ceon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xA0:
		if (ctrl->dispparam_vividweak_cmds.cmd_cnt) {
			pr_info("vividweak\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_vividweak_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xB0:
		if (ctrl->dispparam_vividstrong_cmds.cmd_cnt) {
			pr_info("vividstrong\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_vividstrong_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xC0:
		if (ctrl->dispparam_smartweak_cmds.cmd_cnt) {
			pr_info("smartweak\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_smartweak_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xD0:
		if (ctrl->dispparam_smartstrong_cmds.cmd_cnt) {
			pr_info("smartstrong\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_smartstrong_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xE0:
		if (ctrl->dispparam_vividoff_cmds.cmd_cnt) {
			pr_info("vivid off\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_vividoff_cmds, CMD_REQ_COMMIT);
		}
		if (ctrl->dispparam_smartoff_cmds.cmd_cnt) {
			pr_info("smart off\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_smartoff_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xF0:
		if (ctrl->dispparam_ceoff_cmds.cmd_cnt) {
			pr_info("ceoff\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_ceoff_cmds, CMD_REQ_COMMIT);
		}
		break;
	default:
		break;
	}

	temp = param & 0x00000F00;
	switch(temp) {
	case 0x100:
		if (ctrl->dispparam_cabcon_cmds.cmd_cnt) {
			pr_info("cabcon\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_cabcon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x200:
		if (ctrl->dispparam_cabcguion_cmds.cmd_cnt) {
			pr_info("cabcguion\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_cabcguion_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x300:
		if (ctrl->dispparam_cabcstillon_cmds.cmd_cnt) {
			pr_info("cabcstillon\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_cabcstillon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0x400:
		if (ctrl->dispparam_cabcmovieon_cmds.cmd_cnt) {
			pr_info("cabcmovieon\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_cabcmovieon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xA00:
		if (ctrl->dispparam_scon_cmds.cmd_cnt) {
			pr_info("smart contrast\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_scon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xB00:
		if (ctrl->dispparam_sreon_cmds.cmd_cnt) {
			pr_info("sreon\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_sreon_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xC00:
		if (ctrl->dispparam_sreoff_cmds.cmd_cnt) {
			pr_info("sreoff\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_sreoff_cmds, CMD_REQ_COMMIT);
		}
		break;
	case 0xF00:
		if (ctrl->dispparam_cabcoff_cmds.cmd_cnt) {
			pr_info("cabcoff\n");
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->dispparam_cabcoff_cmds, CMD_REQ_COMMIT);
		}
		break;
	default:
		break;
	}
}

static int mdss_dsi_panel_dispparam(struct mdss_panel_data *pdata)
{
	#define PANEL_ON_DISPARAM_MASK 0xF0000000

	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	int rc = 0;
	uint32_t param;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	if ((pdata->panel_info.panel_paramstatus & PANEL_ON_DISPARAM_MASK)
			== PANEL_ON_DISPARAM_MASK) {
		pdata->panel_info.panel_on_param =
			pdata->panel_info.panel_paramstatus & (~PANEL_ON_DISPARAM_MASK);
		pr_info("Panel on param updated 0x%x\n", pdata->panel_info.panel_on_param);
		return rc;
	}

	if (pdata->panel_info.panel_power_state == MDSS_PANEL_POWER_OFF) {
		pr_err("Opreration not permitted because panel is power-off\n");
		return -EINVAL;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);

	pr_debug("%s: ctrl=%p ndx=%d\n", __func__, ctrl, ctrl->ndx);

	param = pdata->panel_info.panel_paramstatus;
	pr_info("param 0x%x\n", param);

	if (!ctrl->dsi_pipe_ready) {
		WARN(1,"%s: index=%d pid=%d, mipi dsi pipe are not ready !!!\n", __func__,
				ctrl->ndx, current->pid);
		return rc;
	}

	mdss_panel_disparam_set(ctrl, param);

	return rc;
}

static void mdss_dsi_send_col_page_addr(struct mdss_dsi_ctrl_pdata *ctrl,
				struct mdss_rect *roi, int unicast)
{
	struct dcs_cmd_req cmdreq;

	caset[1] = (((roi->x) & 0xFF00) >> 8);
	caset[2] = (((roi->x) & 0xFF));
	caset[3] = (((roi->x - 1 + roi->w) & 0xFF00) >> 8);
	caset[4] = (((roi->x - 1 + roi->w) & 0xFF));
	set_col_page_addr_cmd[0].payload = caset;

	paset[1] = (((roi->y) & 0xFF00) >> 8);
	paset[2] = (((roi->y) & 0xFF));
	paset[3] = (((roi->y - 1 + roi->h) & 0xFF00) >> 8);
	paset[4] = (((roi->y - 1 + roi->h) & 0xFF));
	set_col_page_addr_cmd[1].payload = paset;

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds_cnt = 2;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	if (unicast)
		cmdreq.flags |= CMD_REQ_UNICAST;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	cmdreq.cmds = set_col_page_addr_cmd;
	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static int mdss_dsi_set_col_page_addr(struct mdss_panel_data *pdata,
		bool force_send)
{
	struct mdss_panel_info *pinfo;
	struct mdss_rect roi = {0};
	struct mdss_rect *p_roi;
	struct mdss_rect *c_roi;
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_dsi_ctrl_pdata *other = NULL;
	int left_or_both = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pinfo = &pdata->panel_info;
	p_roi = &pinfo->roi;

	/*
	 * to avoid keep sending same col_page info to panel,
	 * if roi_merge enabled, the roi of left ctrl is used
	 * to compare against new merged roi and saved new
	 * merged roi to it after comparing.
	 * if roi_merge disabled, then the calling ctrl's roi
	 * and pinfo's roi are used to compare.
	 */
	if (pinfo->partial_update_roi_merge) {
		left_or_both = mdss_dsi_roi_merge(ctrl, &roi);
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);
		c_roi = &other->roi;
	} else {
		c_roi = &ctrl->roi;
		roi = *p_roi;
	}

	/* roi had changed, do col_page update */
	if (force_send || !mdss_rect_cmp(c_roi, &roi)) {
		pr_debug("%s: ndx=%d x=%d y=%d w=%d h=%d\n",
				__func__, ctrl->ndx, p_roi->x,
				p_roi->y, p_roi->w, p_roi->h);

		*c_roi = roi; /* keep to ctrl */
		if (c_roi->w == 0 || c_roi->h == 0) {
			/* no new frame update */
			pr_debug("%s: ctrl=%d, no partial roi set\n",
						__func__, ctrl->ndx);
			return 0;
		}

		if (pinfo->dcs_cmd_by_left) {
			if (left_or_both && ctrl->ndx == DSI_CTRL_RIGHT) {
				/* 2A/2B sent by left already */
				return 0;
			}
		}

		if (!mdss_dsi_sync_wait_enable(ctrl)) {
			if (pinfo->dcs_cmd_by_left)
				ctrl = mdss_dsi_get_ctrl_by_index(
							DSI_CTRL_LEFT);
			mdss_dsi_send_col_page_addr(ctrl, &roi, 0);
		} else {
			/*
			 * when sync_wait_broadcast enabled,
			 * need trigger at right ctrl to
			 * start both dcs cmd transmission
			 */
			other = mdss_dsi_get_other_ctrl(ctrl);
			if (!other)
				goto end;

			if (mdss_dsi_is_left_ctrl(ctrl)) {
				if (pinfo->partial_update_roi_merge) {
					/*
					 * roi is the one after merged
					 * to dsi-1 only
					 */
					mdss_dsi_send_col_page_addr(other,
							&roi, 0);
				} else {
					mdss_dsi_send_col_page_addr(ctrl,
							&ctrl->roi, 1);
					mdss_dsi_send_col_page_addr(other,
							&other->roi, 1);
				}
			} else {
				if (pinfo->partial_update_roi_merge) {
					/*
					 * roi is the one after merged
					 * to dsi-1 only
					 */
					mdss_dsi_send_col_page_addr(ctrl,
							&roi, 0);
				} else {
					mdss_dsi_send_col_page_addr(other,
							&other->roi, 1);
					mdss_dsi_send_col_page_addr(ctrl,
							&ctrl->roi, 1);
				}
			}
		}
	}

end:
	return 0;
}

static void mdss_dsi_panel_switch_mode(struct mdss_panel_data *pdata,
							int mode)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mipi_panel_info *mipi;
	struct dsi_panel_cmds *pcmds;
	u32 flags = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	mipi  = &pdata->panel_info.mipi;

	if (!mipi->dms_mode)
		return;

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	if (mipi->dms_mode != DYNAMIC_MODE_RESOLUTION_SWITCH_IMMEDIATE) {
		if (mode == SWITCH_TO_CMD_MODE)
			pcmds = &ctrl_pdata->video2cmd;
		else
			pcmds = &ctrl_pdata->cmd2video;
	} else if ((mipi->dms_mode ==
				DYNAMIC_MODE_RESOLUTION_SWITCH_IMMEDIATE)
			&& pdata->current_timing
			&& !list_empty(&pdata->timings_list)) {
		struct dsi_panel_timing *pt;

		pt = container_of(pdata->current_timing,
				struct dsi_panel_timing, timing);

		pr_debug("%s: sending switch commands\n", __func__);
		pcmds = &pt->switch_cmds;
		flags |= CMD_REQ_DMA_TPG;
	} else {
		pr_warn("%s: Invalid mode switch attempted\n", __func__);
		return;
	}

	mdss_dsi_panel_cmds_send(ctrl_pdata, pcmds, flags);
}

static void mdss_dsi_panel_bl_ctrl(struct mdss_panel_data *pdata,
							u32 bl_level)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_dsi_ctrl_pdata *sctrl = NULL;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	/*
	 * Some backlight controllers specify a minimum duty cycle
	 * for the backlight brightness. If the brightness is less
	 * than it, the controller can malfunction.
	 */

	if ((bl_level < pdata->panel_info.bl_min) && (bl_level != 0))
		bl_level = pdata->panel_info.bl_min;
	switch (ctrl_pdata->bklt_ctrl) {
	case BL_WLED:
		led_trigger_event(bl_led_trigger, bl_level);
		break;
	case BL_PWM:
		mdss_dsi_panel_bklt_pwm(ctrl_pdata, bl_level);
		break;
	case BL_DCS_CMD:
		if (!mdss_dsi_sync_wait_enable(ctrl_pdata)) {
			mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
			break;
		}
		/*
		 * DCS commands to update backlight are usually sent at
		 * the same time to both the controllers. However, if
		 * sync_wait is enabled, we need to ensure that the
		 * dcs commands are first sent to the non-trigger
		 * controller so that when the commands are triggered,
		 * both controllers receive it at the same time.
		 */
		sctrl = mdss_dsi_get_other_ctrl(ctrl_pdata);
		if (mdss_dsi_sync_wait_trigger(ctrl_pdata)) {
			if (sctrl)
				mdss_dsi_panel_bklt_dcs(sctrl, bl_level);
			mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
		} else {
			mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
			if (sctrl)
				mdss_dsi_panel_bklt_dcs(sctrl, bl_level);
		}
		break;
	default:
		pr_err("%s: Unknown bl_ctrl configuration\n",
			__func__);
		break;
	}
}

static char string_to_hex(const char *str)
{
	char val_l = 0;
	char val_h = 0;

	if (str[0] >= '0' && str[0] <= '9')
		val_h = str[0] - '0';
	else if (str[0] <= 'f' && str[0] >= 'a')
		val_h = 10 + str[0] - 'a';
	else if (str[0] <= 'F' && str[0] >= 'A')
		val_h = 10 + str[0] - 'A';

	if (str[1] >= '0' && str[1] <= '9')
		val_l = str[1]-'0';
	else if (str[1] <= 'f' && str[1] >= 'a')
		val_l = 10 + str[1] - 'a';
	else if (str[1] <= 'F' && str[1] >= 'A')
		val_l = 10 + str[1] - 'A';

	return (val_h << 4) | val_l;
}

static int string_merge_into_buf(const char *str, int len, char *buf)
{
	int buf_size = 0;
	int i = 0;
	const char *p = str;

	while (i < len) {
		if (((p[0] >= '0' && p[0] <= '9') ||
			(p[0] <= 'f' && p[0] >= 'a') ||
			(p[0] <= 'F' && p[0] >= 'A'))
			&& ((i + 1) < len)) {
			buf[buf_size] = string_to_hex(p);
			buf_size++;
			i += 2;
			p += 2;
		} else {
			i++;
			p++;
		}
	}
	return buf_size;
}

static int parse_to_dcs_cmds(struct dsi_panel_cmds *pcmds)
{
	#define PANEL_INITIAL_CODE "/data/lcd.txt"

	int blen = 0, len;
	char *data, *buf, *bp;
	struct dsi_ctrl_hdr *dchdr;
	int i, cnt, file_size;

	int ret = 0;
	struct file *filp = NULL;
	mm_segment_t old_fs;
	const char *file_name = PANEL_INITIAL_CODE;

	filp = filp_open(file_name, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("%s open failed\n", file_name);
		return -EINVAL;
	}
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	file_size = filp->f_dentry->d_inode->i_size;

	data = kzalloc(file_size, GFP_KERNEL);
	if (!data) {
		set_fs(old_fs);
		filp_close(filp, NULL);
		return -ENOMEM;
	}
	buf = kzalloc(file_size/2, GFP_KERNEL);
	if (!buf) {
		kfree(data);
		set_fs(old_fs);
		filp_close(filp, NULL);
		return -ENOMEM;
	}

	ret = filp->f_op->read(filp, data, file_size, &filp->f_pos);
	if (ret < 0) {
		pr_err("%s read failed, return %d\n", file_name, ret);
		goto exit_free;
	}

	blen = string_merge_into_buf(data, file_size, buf);
	printk("%s: file size is %d, buf size is %d\n", __func__, file_size, blen);
	for (i = 0; i < blen; i++)
		printk("0x%02x ", buf[i]);
	pr_debug("\n");
	if (blen <= 0)
		goto exit_free;
	/* free the memory alloced before */
	if (pcmds->buf) {
		kfree(pcmds->buf);
		pcmds->buf = NULL;
	}

	if (pcmds->cmds) {
		kfree(pcmds->cmds);
		pcmds->cmds = NULL;
	}

	/* scan dcs commands */
	bp = buf;
	len = blen;
	cnt = 0;
	while (len > sizeof(*dchdr)) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		dchdr->dlen = ntohs(dchdr->dlen);
		if (dchdr->dlen > len) {
			pr_err("%s: dtsi cmd=%x error, len=%d",
				__func__, dchdr->dtype, dchdr->dlen);
			goto exit_free;
		}
		bp += sizeof(*dchdr);
		len -= sizeof(*dchdr);
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		cnt++;
	}

	if (len != 0) {
		pr_err("%s: dcs_cmd=%x len=%d error!",
				__func__, buf[0], blen);
		goto exit_free;
	}

	pcmds->cmds = kzalloc(cnt * sizeof(struct dsi_cmd_desc),
						GFP_KERNEL);
	if (!pcmds->cmds)
		goto exit_free;

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = blen;

	bp = buf;
	len = blen;
	for (i = 0; i < cnt; i++) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		len -= sizeof(*dchdr);
		bp += sizeof(*dchdr);
		pcmds->cmds[i].dchdr = *dchdr;
		pcmds->cmds[i].payload = bp;
		bp += dchdr->dlen;
		len -= dchdr->dlen;
	}


	pr_debug("%s: dcs_cmd=%x len=%d, cmd_cnt=%d link_state=%d\n", __func__,
		pcmds->buf[0], pcmds->blen, pcmds->cmd_cnt, pcmds->link_state);

	set_fs(old_fs);
	filp_close(filp, NULL);
	return 0;

exit_free:
	set_fs(old_fs);
	filp_close(filp, NULL);
	kfree(buf);
	kfree(data);
	return -ENOMEM;
}

static void panelon_delayed_work(struct work_struct *work)
{
	struct mdss_dsi_ctrl_pdata *ctrl = container_of(work,
				struct mdss_dsi_ctrl_pdata, cmds_work.work);
	mdss_panel_disparam_set(ctrl, ctrl->panel_data.panel_info.panel_on_param);
}

static int mdss_dsi_panel_on(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;
	struct dsi_panel_cmds *on_cmds;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	display_on = true;
	
	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_info("%s: ctrl=%pK ndx=%d\n", __func__, ctrl, ctrl->ndx);

	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			goto end;
	}

	on_cmds = &ctrl->on_cmds;

	if ((pinfo->mipi.dms_mode == DYNAMIC_MODE_SWITCH_IMMEDIATE) &&
			(pinfo->mipi.boot_mode != pinfo->mipi.mode))
		on_cmds = &ctrl->post_dms_on_cmds;

	if (on_cmds->cmd_cnt)
		mdss_dsi_panel_cmds_send(ctrl, on_cmds, CMD_REQ_COMMIT);

	if (pinfo->panel_on_param)
		schedule_delayed_work(&ctrl->cmds_work, msecs_to_jiffies(100));

end:
	pinfo->blank_state = MDSS_PANEL_BLANK_UNBLANK;
	pr_info("%s:-\n", __func__);
	return 0;
}

static int mdss_dsi_panel_off(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_info("%s: ctrl=%pK ndx=%d\n", __func__, ctrl, ctrl->ndx);

	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			goto end;
	}

	cancel_delayed_work_sync(&ctrl->cmds_work);

	if (ctrl->off_cmds.cmd_cnt)
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->off_cmds, CMD_REQ_COMMIT);
	
	display_on = false;
	
end:
	pinfo->blank_state = MDSS_PANEL_BLANK_BLANK;
	pr_info("%s:-\n", __func__);
	if (ctrl->on_cmds_tuning)
		parse_to_dcs_cmds(&ctrl->on_cmds);
	return 0;
}

static int mdss_dsi_panel_low_power_config(struct mdss_panel_data *pdata,
	int enable)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_debug("%s: ctrl=%pK ndx=%d enable=%d\n", __func__, ctrl, ctrl->ndx,
		enable);

	/* Any panel specific low power commands/config */
	if (enable)
		pinfo->blank_state = MDSS_PANEL_BLANK_LOW_POWER;
	else
		pinfo->blank_state = MDSS_PANEL_BLANK_UNBLANK;

	pr_debug("%s:-\n", __func__);
	return 0;
}

static void mdss_dsi_parse_lane_swap(struct device_node *np, char *dlane_swap)
{
	const char *data;

	*dlane_swap = DSI_LANE_MAP_0123;
	data = of_get_property(np, "qcom,mdss-dsi-lane-map", NULL);
	if (data) {
		if (!strcmp(data, "lane_map_3012"))
			*dlane_swap = DSI_LANE_MAP_3012;
		else if (!strcmp(data, "lane_map_2301"))
			*dlane_swap = DSI_LANE_MAP_2301;
		else if (!strcmp(data, "lane_map_1230"))
			*dlane_swap = DSI_LANE_MAP_1230;
		else if (!strcmp(data, "lane_map_0321"))
			*dlane_swap = DSI_LANE_MAP_0321;
		else if (!strcmp(data, "lane_map_1032"))
			*dlane_swap = DSI_LANE_MAP_1032;
		else if (!strcmp(data, "lane_map_2103"))
			*dlane_swap = DSI_LANE_MAP_2103;
		else if (!strcmp(data, "lane_map_3210"))
			*dlane_swap = DSI_LANE_MAP_3210;
	}
}

static void mdss_dsi_parse_trigger(struct device_node *np, char *trigger,
		char *trigger_key)
{
	const char *data;

	*trigger = DSI_CMD_TRIGGER_SW;
	data = of_get_property(np, trigger_key, NULL);
	if (data) {
		if (!strcmp(data, "none"))
			*trigger = DSI_CMD_TRIGGER_NONE;
		else if (!strcmp(data, "trigger_te"))
			*trigger = DSI_CMD_TRIGGER_TE;
		else if (!strcmp(data, "trigger_sw_seof"))
			*trigger = DSI_CMD_TRIGGER_SW_SEOF;
		else if (!strcmp(data, "trigger_sw_te"))
			*trigger = DSI_CMD_TRIGGER_SW_TE;
	}
}


static int mdss_dsi_parse_dcs_cmds(struct device_node *np,
		struct dsi_panel_cmds *pcmds, char *cmd_key, char *link_key)
{
	const char *data;
	int blen = 0, len;
	char *buf, *bp;
	struct dsi_ctrl_hdr *dchdr;
	int i, cnt;

	data = of_get_property(np, cmd_key, &blen);
	if (!data) {
		pr_err("%s: failed, key=%s\n", __func__, cmd_key);
		return -ENOMEM;
	}

	buf = kzalloc(sizeof(char) * blen, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, data, blen);

	/* scan dcs commands */
	bp = buf;
	len = blen;
	cnt = 0;
	while (len >= sizeof(*dchdr)) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		dchdr->dlen = ntohs(dchdr->dlen);
		if (dchdr->dlen > len) {
			pr_err("%s: dtsi cmd=%x error, len=%d",
				__func__, dchdr->dtype, dchdr->dlen);
			goto exit_free;
		}
		bp += sizeof(*dchdr);
		len -= sizeof(*dchdr);
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		cnt++;
	}

	if (len != 0) {
		pr_err("%s: dcs_cmd=%x len=%d error!",
				__func__, buf[0], blen);
		goto exit_free;
	}

	pcmds->cmds = kzalloc(cnt * sizeof(struct dsi_cmd_desc),
						GFP_KERNEL);
	if (!pcmds->cmds)
		goto exit_free;

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = blen;

	bp = buf;
	len = blen;
	for (i = 0; i < cnt; i++) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		len -= sizeof(*dchdr);
		bp += sizeof(*dchdr);
		pcmds->cmds[i].dchdr = *dchdr;
		pcmds->cmds[i].payload = bp;
		bp += dchdr->dlen;
		len -= dchdr->dlen;
	}

	/*Set default link state to LP Mode*/
	pcmds->link_state = DSI_LP_MODE;

	if (link_key) {
		data = of_get_property(np, link_key, NULL);
		if (data && !strcmp(data, "dsi_hs_mode"))
			pcmds->link_state = DSI_HS_MODE;
		else
			pcmds->link_state = DSI_LP_MODE;
	}

	pr_debug("%s: dcs_cmd=%x len=%d, cmd_cnt=%d link_state=%d\n", __func__,
		pcmds->buf[0], pcmds->blen, pcmds->cmd_cnt, pcmds->link_state);

	return 0;

exit_free:
	kfree(buf);
	return -ENOMEM;
}


int mdss_panel_get_dst_fmt(u32 bpp, char mipi_mode, u32 pixel_packing,
				char *dst_format)
{
	int rc = 0;
	switch (bpp) {
	case 3:
		*dst_format = DSI_CMD_DST_FORMAT_RGB111;
		break;
	case 8:
		*dst_format = DSI_CMD_DST_FORMAT_RGB332;
		break;
	case 12:
		*dst_format = DSI_CMD_DST_FORMAT_RGB444;
		break;
	case 16:
		switch (mipi_mode) {
		case DSI_VIDEO_MODE:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
			break;
		case DSI_CMD_MODE:
			*dst_format = DSI_CMD_DST_FORMAT_RGB565;
			break;
		default:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
			break;
		}
		break;
	case 18:
		switch (mipi_mode) {
		case DSI_VIDEO_MODE:
			if (pixel_packing == 0)
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
			else
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666_LOOSE;
			break;
		case DSI_CMD_MODE:
			*dst_format = DSI_CMD_DST_FORMAT_RGB666;
			break;
		default:
			if (pixel_packing == 0)
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
			else
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666_LOOSE;
			break;
		}
		break;
	case 24:
		switch (mipi_mode) {
		case DSI_VIDEO_MODE:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
			break;
		case DSI_CMD_MODE:
			*dst_format = DSI_CMD_DST_FORMAT_RGB888;
			break;
		default:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
			break;
		}
		break;
	default:
		rc = -EINVAL;
		break;
	}
	return rc;
}

static int mdss_dsi_parse_fbc_params(struct device_node *np,
				struct fbc_panel_info *fbc)
{
	int rc, fbc_enabled = 0;
	u32 tmp;

	fbc_enabled = of_property_read_bool(np,	"qcom,mdss-dsi-fbc-enable");
	if (fbc_enabled) {
		pr_debug("%s:%d FBC panel enabled.\n", __func__, __LINE__);
		fbc->enabled = 1;
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-bpp", &tmp);
		fbc->target_bpp =	(!rc ? tmp : 24);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-packing",
				&tmp);
		fbc->comp_mode = (!rc ? tmp : 0);
		fbc->qerr_enable = of_property_read_bool(np,
			"qcom,mdss-dsi-fbc-quant-error");
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-bias", &tmp);
		fbc->cd_bias = (!rc ? tmp : 0);
		fbc->pat_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-pat-mode");
		fbc->vlc_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-vlc-mode");
		fbc->bflc_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-bflc-mode");
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-h-line-budget",
				&tmp);
		fbc->line_x_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-budget-ctrl",
				&tmp);
		fbc->block_x_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-block-budget",
				&tmp);
		fbc->block_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossless-threshold", &tmp);
		fbc->lossless_mode_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossy-threshold", &tmp);
		fbc->lossy_mode_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-rgb-threshold",
				&tmp);
		fbc->lossy_rgb_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossy-mode-idx", &tmp);
		fbc->lossy_mode_idx = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-slice-height", &tmp);
		fbc->slice_height = (!rc ? tmp : 0);
		fbc->pred_mode = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-2d-pred-mode");
		fbc->enc_mode = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-ver2-mode");
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-max-pred-err", &tmp);
		fbc->max_pred_err = (!rc ? tmp : 0);
	} else {
		pr_debug("%s:%d Panel does not support FBC.\n",
				__func__, __LINE__);
		fbc->enabled = 0;
		fbc->target_bpp = 24;
	}
	return 0;
}

static void mdss_panel_parse_te_params(struct device_node *np,
			u32 sim_panel_mode, struct mdss_panel_timing *timing)
{
	struct mdss_mdp_pp_tear_check *te = &timing->te;
	u32 tmp;
	int rc = 0;
	/*
	 * TE default: dsi byte clock calculated base on 70 fps;
	 * around 14 ms to complete a kickoff cycle if te disabled;
	 * vclk_line base on 60 fps; write is faster than read;
	 * init == start == rdptr;
	 */
	te->tear_check_en =
		!of_property_read_bool(np, "qcom,mdss-tear-check-disable");
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-threshold-start", &tmp);
	te->sync_threshold_start = (!rc ? tmp : 4);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-threshold-continue", &tmp);
	te->sync_threshold_continue = (!rc ? tmp : 4);
	rc = of_property_read_u32(np, "qcom,mdss-tear-check-frame-rate", &tmp);
	te->refx100 = (!rc ? tmp : 6000);

	/* override te parameters if panel is in sw te mode */
	if (sim_panel_mode == SIM_SW_TE_MODE) {
		te->sync_cfg_height = timing->yres
				+ timing->v_front_porch
				+ timing->v_back_porch;
		te->vsync_init_val = 0;
		te->start_pos = 5;
		te->rd_ptr_irq = 1;
		pr_debug("SW TE override: read_ptr:%d,start_pos:%d,height:%d,init_val:%d\n",
			te->rd_ptr_irq, te->start_pos, te->sync_cfg_height,
			te->vsync_init_val);
	} else {
		rc = of_property_read_u32
			(np, "qcom,mdss-tear-check-sync-cfg-height", &tmp);
		te->sync_cfg_height = (!rc ? tmp : 0xfff0);
		rc = of_property_read_u32
			(np, "qcom,mdss-tear-check-sync-init-val", &tmp);
		te->vsync_init_val = (!rc ? tmp : timing->yres);
		rc = of_property_read_u32(np, "qcom,mdss-tear-check-start-pos",
				&tmp);
		te->start_pos = (!rc ? tmp : te->vsync_init_val);
		rc = of_property_read_u32
			(np, "qcom,mdss-tear-check-rd-ptr-trigger-intr", &tmp);
		te->rd_ptr_irq = (!rc ? tmp : te->vsync_init_val + 1);
	}
}


static int mdss_dsi_parse_reset_seq(struct device_node *np,
		u32 rst_seq[MDSS_DSI_RST_SEQ_LEN], u32 *rst_len,
		const char *name)
{
	int num = 0, i;
	int rc;
	struct property *data;
	u32 tmp[MDSS_DSI_RST_SEQ_LEN];
	*rst_len = 0;
	data = of_find_property(np, name, &num);
	num /= sizeof(u32);
	if (!data || !num || num > MDSS_DSI_RST_SEQ_LEN || num % 2) {
		pr_debug("%s:%d, error reading %s, length found = %d\n",
			__func__, __LINE__, name, num);
	} else {
		rc = of_property_read_u32_array(np, name, tmp, num);
		if (rc)
			pr_debug("%s:%d, error reading %s, rc = %d\n",
				__func__, __LINE__, name, rc);
		else {
			for (i = 0; i < num; ++i)
				rst_seq[i] = tmp[i];
			*rst_len = num;
		}
	}
	return 0;
}

static int mdss_dsi_gen_read_status(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	if (!mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
		ctrl_pdata->status_value, 0)) {
		pr_err("%s: Read back value from panel is incorrect\n",
							__func__);
		return -EINVAL;
	} else {
		return 1;
	}
}

static int mdss_dsi_nt35596_read_status(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	if (!mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
		ctrl_pdata->status_value, 0)) {
		ctrl_pdata->status_error_count = 0;
		pr_err("%s: Read back value from panel is incorrect\n",
							__func__);
		return -EINVAL;
	} else {
		if (!mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
			ctrl_pdata->status_value, 3)) {
			ctrl_pdata->status_error_count = 0;
		} else {
			if (mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
				ctrl_pdata->status_value, 4) ||
				mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
				ctrl_pdata->status_value, 5))
				ctrl_pdata->status_error_count = 0;
			else
				ctrl_pdata->status_error_count++;
			if (ctrl_pdata->status_error_count >=
					ctrl_pdata->max_status_error_count) {
				ctrl_pdata->status_error_count = 0;
				pr_err("%s: Read value bad. Error_cnt = %i\n",
					 __func__,
					ctrl_pdata->status_error_count);
				return -EINVAL;
			}
		}
		return 1;
	}
}

static void mdss_dsi_parse_roi_alignment(struct device_node *np,
		struct mdss_panel_info *pinfo)
{
	int len = 0;
	u32 value[6];
	struct property *data;
	data = of_find_property(np, "qcom,panel-roi-alignment", &len);
	len /= sizeof(u32);
	if (!data || (len != 6)) {
		pr_debug("%s: Panel roi alignment not found", __func__);
	} else {
		int rc = of_property_read_u32_array(np,
				"qcom,panel-roi-alignment", value, len);
		if (rc)
			pr_debug("%s: Error reading panel roi alignment values",
					__func__);
		else {
			pinfo->xstart_pix_align = value[0];
			pinfo->width_pix_align = value[1];
			pinfo->ystart_pix_align = value[2];
			pinfo->height_pix_align = value[3];
			pinfo->min_width = value[4];
			pinfo->min_height = value[5];
		}

		pr_debug("%s: ROI alignment: [%d, %d, %d, %d, %d, %d]",
				__func__, pinfo->xstart_pix_align,
				pinfo->width_pix_align, pinfo->ystart_pix_align,
				pinfo->height_pix_align, pinfo->min_width,
				pinfo->min_height);
	}
}

static void mdss_parse_night_brightness(struct device_node *np,
			struct mdss_panel_info *pinfo)
{
	int num = 0, i;
	int rc;
	struct property *data;
	u32 tmp[MDSS_DSI_RST_SEQ_LEN];
	data = of_find_property(np, "qcom,mdss-night-brightness", &num);
	num /= sizeof(u32);
	if (!data || !num) {
		pr_info("%s:%d, error reading %s, length found = %d\n",
			__func__, __LINE__, "qcom,mdss-night-brightness", num);
	} else {
		rc = of_property_read_u32_array(np, "qcom,mdss-night-brightness", tmp, num);
		if (rc)
			pr_info("%s:%d, error reading %s, rc = %d\n",
				__func__, __LINE__, "qcom,mdss-night-brightness", rc);
		else {
			for (i = 0; i < num; ++i){
				pinfo->night_map[i] = tmp[i];
			}
			pinfo->night_map_len = num;
		}
	}
	return ;
}
static void mdss_dsi_parse_dms_config(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct mdss_panel_info *pinfo = &ctrl->panel_data.panel_info;
	const char *data;
	bool dms_enabled;

	dms_enabled = of_property_read_bool(np,
		"qcom,dynamic-mode-switch-enabled");

	if (!dms_enabled) {
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_DISABLED;
		goto exit;
	}

	/* default mode is suspend_resume */
	pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_SUSPEND_RESUME;
	data = of_get_property(np, "qcom,dynamic-mode-switch-type", NULL);
	if (data && !strcmp(data, "dynamic-resolution-switch-immediate")) {
		if (!list_empty(&ctrl->panel_data.timings_list))
			pinfo->mipi.dms_mode =
				DYNAMIC_MODE_RESOLUTION_SWITCH_IMMEDIATE;
		else
			pinfo->mipi.dms_mode =
				DYNAMIC_MODE_SWITCH_DISABLED;

		goto exit;
	}

	if (data && !strcmp(data, "dynamic-switch-immediate"))
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_IMMEDIATE;
	else
		pr_debug("%s: default dms suspend/resume\n", __func__);

	mdss_dsi_parse_dcs_cmds(np, &ctrl->video2cmd,
		"qcom,video-to-cmd-mode-switch-commands", NULL);

	mdss_dsi_parse_dcs_cmds(np, &ctrl->cmd2video,
		"qcom,cmd-to-video-mode-switch-commands", NULL);

	mdss_dsi_parse_dcs_cmds(np, &ctrl->post_dms_on_cmds,
		"qcom,mdss-dsi-post-mode-switch-on-command",
		"qcom,mdss-dsi-post-mode-switch-on-command-state");

	if (pinfo->mipi.dms_mode == DYNAMIC_MODE_SWITCH_IMMEDIATE &&
		!ctrl->post_dms_on_cmds.cmd_cnt) {
		pr_warn("%s: No post dms on cmd specified\n", __func__);
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_DISABLED;
	}

	if (!ctrl->video2cmd.cmd_cnt || !ctrl->cmd2video.cmd_cnt) {
		pr_warn("%s: No commands specified for dynamic switch\n",
			__func__);
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_DISABLED;
	}
exit:
	pr_info("%s: dynamic switch feature enabled: %d\n", __func__,
		pinfo->mipi.dms_mode);
	return;
}

static void mdss_dsi_parse_esd_params(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 tmp;
	int rc;
	struct property *data;
	const char *string;
	struct mdss_panel_info *pinfo = &ctrl->panel_data.panel_info;

	pinfo->esd_check_enabled = of_property_read_bool(np,
		"qcom,esd-check-enabled");

	if (!pinfo->esd_check_enabled)
		return;

	mdss_dsi_parse_dcs_cmds(np, &ctrl->status_cmds,
			"qcom,mdss-dsi-panel-status-command",
				"qcom,mdss-dsi-panel-status-command-state");

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-status-read-length",
		&tmp);
	ctrl->status_cmds_rlen = (!rc ? tmp : 1);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-max-error-count",
		&tmp);
	ctrl->max_status_error_count = (!rc ? tmp : 0);

	ctrl->status_value = kzalloc(sizeof(u32) * ctrl->status_cmds_rlen,
				GFP_KERNEL);
	if (!ctrl->status_value) {
		pr_err("%s: Error allocating memory for status buffer\n",
			__func__);
		pinfo->esd_check_enabled = false;
		return;
	}

	data = of_find_property(np, "qcom,mdss-dsi-panel-status-value", &tmp);
	tmp /= sizeof(u32);
	if (!data || (tmp != ctrl->status_cmds_rlen)) {
		pr_debug("%s: Panel status values not found\n", __func__);
		memset(ctrl->status_value, 0, ctrl->status_cmds_rlen);
	} else {
		rc = of_property_read_u32_array(np,
			"qcom,mdss-dsi-panel-status-value",
			ctrl->status_value, tmp);
		if (rc) {
			pr_debug("%s: Error reading panel status values\n",
					__func__);
			memset(ctrl->status_value, 0, ctrl->status_cmds_rlen);
		}
	}

	ctrl->status_mode = ESD_MAX;
	rc = of_property_read_string(np,
			"qcom,mdss-dsi-panel-status-check-mode", &string);
	if (!rc) {
		if (!strcmp(string, "bta_check")) {
			ctrl->status_mode = ESD_BTA;
		} else if (!strcmp(string, "reg_read")) {
			ctrl->status_mode = ESD_REG;
			ctrl->check_read_status =
				mdss_dsi_gen_read_status;
		} else if (!strcmp(string, "reg_read_nt35596")) {
			ctrl->status_mode = ESD_REG_NT35596;
			ctrl->status_error_count = 0;
			ctrl->check_read_status =
				mdss_dsi_nt35596_read_status;
		} else if (!strcmp(string, "te_signal_check")) {
			if (pinfo->mipi.mode == DSI_CMD_MODE) {
				ctrl->status_mode = ESD_TE;
			} else {
				pr_err("TE-ESD not valid for video mode\n");
				goto error;
			}
		} else {
			pr_err("No valid panel-status-check-mode string\n");
			goto error;
		}
	}
	return;

error:
	kfree(ctrl->status_value);
	pinfo->esd_check_enabled = false;
}

static int mdss_dsi_parse_panel_features(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct mdss_panel_info *pinfo;

	if (!np || !ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -ENODEV;
	}

	ctrl->dual_en_gpio = of_get_named_gpio(np,
			 "qcom,mdss-dual-enable-gpio", 0);
	if (!gpio_is_valid(ctrl->dual_en_gpio))
		pr_err("%s:%d, Mipi dual enable gpio not specified\n",
						__func__, __LINE__);

	pinfo = &ctrl->panel_data.panel_info;

	pinfo->cont_splash_enabled = of_property_read_bool(np,
		"qcom,cont-splash-enabled");

	pinfo->partial_update_supported = of_property_read_bool(np,
		"qcom,partial-update-enabled");
	if (pinfo->mipi.mode == DSI_CMD_MODE) {
		pinfo->partial_update_enabled = pinfo->partial_update_supported;
		pr_info("%s: partial_update_enabled=%d\n", __func__,
					pinfo->partial_update_enabled);
		ctrl->set_col_page_addr = mdss_dsi_set_col_page_addr;
		if (pinfo->partial_update_enabled) {
			pinfo->partial_update_roi_merge =
					of_property_read_bool(np,
					"qcom,partial-update-roi-merge");
		}

		pinfo->dcs_cmd_by_left = of_property_read_bool(np,
						"qcom,dcs-cmd-by-left");
	}

	pinfo->ulps_feature_enabled = of_property_read_bool(np,
		"qcom,ulps-enabled");
	pr_info("%s: ulps feature %s\n", __func__,
		(pinfo->ulps_feature_enabled ? "enabled" : "disabled"));

	pinfo->ulps_suspend_enabled = of_property_read_bool(np,
		"qcom,suspend-ulps-enabled");
	pr_info("%s: ulps during suspend feature %s", __func__,
		(pinfo->ulps_suspend_enabled ? "enabled" : "disabled"));

	mdss_dsi_parse_dms_config(np, ctrl);

	pinfo->panel_ack_disabled = pinfo->sim_panel_mode ?
		1 : of_property_read_bool(np, "qcom,panel-ack-disabled");

	mdss_dsi_parse_esd_params(np, ctrl);

	if (pinfo->panel_ack_disabled && pinfo->esd_check_enabled) {
		pr_warn("ESD should not be enabled if panel ACK is disabled\n");
		pinfo->esd_check_enabled = false;
	}

	if (ctrl->disp_en_gpio <= 0) {
		ctrl->disp_en_gpio = of_get_named_gpio(
			np,
			"qcom,5v-boost-gpio", 0);

		if (!gpio_is_valid(ctrl->disp_en_gpio))
			pr_err("%s:%d, Disp_en gpio not specified\n",
					__func__, __LINE__);
	}

	return 0;
}

static void mdss_dsi_parse_panel_horizintal_line_idle(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	const u32 *src;
	int i, len, cnt;
	struct panel_horizontal_idle *kp;

	if (!np || !ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		return;
	}

	src = of_get_property(np, "qcom,mdss-dsi-hor-line-idle", &len);
	if (!src || len == 0)
		return;

	cnt = len % 3; /* 3 fields per entry */
	if (cnt) {
		pr_err("%s: invalid horizontal idle len=%d\n", __func__, len);
		return;
	}

	cnt = len / sizeof(u32);

	kp = kzalloc(sizeof(*kp) * (cnt / 3), GFP_KERNEL);
	if (kp == NULL) {
		pr_err("%s: No memory\n", __func__);
		return;
	}

	ctrl->line_idle = kp;
	for (i = 0; i < cnt; i += 3) {
		kp->min = be32_to_cpu(src[i]);
		kp->max = be32_to_cpu(src[i+1]);
		kp->idle = be32_to_cpu(src[i+2]);
		kp++;
		ctrl->horizontal_idle_cnt++;
	}

	pr_debug("%s: horizontal_idle_cnt=%d\n", __func__,
				ctrl->horizontal_idle_cnt);
}

static int mdss_dsi_set_refresh_rate_range(struct device_node *pan_node,
		struct mdss_panel_info *pinfo)
{
	int rc = 0;
	rc = of_property_read_u32(pan_node,
			"qcom,mdss-dsi-min-refresh-rate",
			&pinfo->min_fps);
	if (rc) {
		pr_warn("%s:%d, Unable to read min refresh rate\n",
				__func__, __LINE__);

		/*
		 * Since min refresh rate is not specified when dynamic
		 * fps is enabled, using minimum as 30
		 */
		pinfo->min_fps = MIN_REFRESH_RATE;
		rc = 0;
	}

	rc = of_property_read_u32(pan_node,
			"qcom,mdss-dsi-max-refresh-rate",
			&pinfo->max_fps);
	if (rc) {
		pr_warn("%s:%d, Unable to read max refresh rate\n",
				__func__, __LINE__);

		/*
		 * Since max refresh rate was not specified when dynamic
		 * fps is enabled, using the default panel refresh rate
		 * as max refresh rate supported.
		 */
		pinfo->max_fps = pinfo->mipi.frame_rate;
		rc = 0;
	}

	pr_info("dyn_fps: min = %d, max = %d\n",
			pinfo->min_fps, pinfo->max_fps);
	return rc;
}

static void mdss_dsi_parse_dfps_config(struct device_node *pan_node,
			struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	const char *data;
	bool dynamic_fps;
	struct mdss_panel_info *pinfo = &(ctrl_pdata->panel_data.panel_info);

	dynamic_fps = of_property_read_bool(pan_node,
			"qcom,mdss-dsi-pan-enable-dynamic-fps");

	if (!dynamic_fps)
		return;

	pinfo->dynamic_fps = true;
	data = of_get_property(pan_node, "qcom,mdss-dsi-pan-fps-update", NULL);
	if (data) {
		if (!strcmp(data, "dfps_suspend_resume_mode")) {
			pinfo->dfps_update = DFPS_SUSPEND_RESUME_MODE;
			pr_debug("dfps mode: suspend/resume\n");
		} else if (!strcmp(data, "dfps_immediate_clk_mode")) {
			pinfo->dfps_update = DFPS_IMMEDIATE_CLK_UPDATE_MODE;
			pr_debug("dfps mode: Immediate clk\n");
		} else if (!strcmp(data, "dfps_immediate_porch_mode_hfp")) {
			pinfo->dfps_update =
				DFPS_IMMEDIATE_PORCH_UPDATE_MODE_HFP;
			pr_debug("dfps mode: Immediate porch HFP\n");
		} else if (!strcmp(data, "dfps_immediate_porch_mode_vfp")) {
			pinfo->dfps_update =
				DFPS_IMMEDIATE_PORCH_UPDATE_MODE_VFP;
			pr_debug("dfps mode: Immediate porch VFP\n");
		} else {
			pinfo->dfps_update = DFPS_SUSPEND_RESUME_MODE;
			pr_debug("default dfps mode: suspend/resume\n");
		}
		mdss_dsi_set_refresh_rate_range(pan_node, pinfo);
	} else {
		pinfo->dynamic_fps = false;
		pr_debug("dfps update mode not configured: disable\n");
	}
	pinfo->new_fps = pinfo->mipi.frame_rate;

	return;
}

int mdss_dsi_panel_timing_switch(struct mdss_dsi_ctrl_pdata *ctrl,
			struct mdss_panel_timing *timing)
{
	struct dsi_panel_timing *pt;
	struct mdss_panel_info *pinfo = &ctrl->panel_data.panel_info;
	int i;

	if (!timing)
		return -EINVAL;

	if (timing == ctrl->panel_data.current_timing) {
		pr_warn("%s: panel timing \"%s\" already set\n", __func__,
				timing->name);
		return 0; /* nothing to do */
	}

	pr_debug("%s: ndx=%d switching to panel timing \"%s\"\n", __func__,
			ctrl->ndx, timing->name);

	mdss_panel_info_from_timing(timing, pinfo);

	pt = container_of(timing, struct dsi_panel_timing, timing);
	pinfo->mipi.t_clk_pre = pt->t_clk_pre;
	pinfo->mipi.t_clk_post = pt->t_clk_post;

	for (i = 0; i < ARRAY_SIZE(pt->phy_timing); i++)
		pinfo->mipi.dsi_phy_db.timing[i] = pt->phy_timing[i];

	ctrl->on_cmds = pt->on_cmds;

	ctrl->panel_data.current_timing = timing;
	if (!timing->clk_rate)
		ctrl->refresh_clk_rate = true;
	mdss_dsi_clk_refresh(&ctrl->panel_data);

	return 0;
}

static int mdss_dsi_panel_timing_from_dt(struct device_node *np,
	struct dsi_panel_timing *pt)
{
	u32 tmp;
	int rc, i, len;
	const char *data;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-width", &tmp);
	if (rc) {
		pr_err("%s:%d, panel width not specified\n",
						__func__, __LINE__);
		return -EINVAL;
	}
	pt->timing.xres = tmp;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-height", &tmp);
	if (rc) {
		pr_err("%s:%d, panel height not specified\n",
						__func__, __LINE__);
		return -EINVAL;
	}
	pt->timing.yres = tmp;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-front-porch", &tmp);
	pt->timing.h_front_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-back-porch", &tmp);
	pt->timing.h_back_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-pulse-width", &tmp);
	pt->timing.h_pulse_width = (!rc ? tmp : 2);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-sync-skew", &tmp);
	pt->timing.hsync_skew = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-back-porch", &tmp);
	pt->timing.v_back_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-front-porch", &tmp);
	pt->timing.v_front_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-pulse-width", &tmp);
	pt->timing.v_pulse_width = (!rc ? tmp : 2);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-left-border", &tmp);
	pt->timing.border_left = !rc ? tmp : 0;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-right-border", &tmp);
	pt->timing.border_right = !rc ? tmp : 0;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-top-border", &tmp);
	pt->timing.border_top = !rc ? tmp : 0;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-bottom-border", &tmp);
	pt->timing.border_bottom = !rc ? tmp : 0;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-framerate", &tmp);
	pt->timing.frame_rate = !rc ? tmp : DEFAULT_FRAME_RATE;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-clockrate", &tmp);
	pt->timing.clk_rate = !rc ? tmp : 0;

	data = of_get_property(np, "qcom,mdss-dsi-panel-timings", &len);
	if ((!data) || (len != 12)) {
		pr_err("%s:%d, Unable to read Phy timing settings",
		       __func__, __LINE__);
		return -EINVAL;
	}
	for (i = 0; i < len; i++)
		pt->phy_timing[i] = data[i];

	rc = of_property_read_u32(np, "qcom,mdss-dsi-t-clk-pre", &tmp);
	pt->t_clk_pre = (!rc ? tmp : 0x24);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-t-clk-post", &tmp);
	pt->t_clk_post = (!rc ? tmp : 0x03);

	if (np->name) {
		pt->timing.name = kstrdup(np->name, GFP_KERNEL);
		pr_info("%s: found new timing \"%s\" (%pK)\n", __func__,
				np->name, &pt->timing);
	}

	return 0;
}

static void  mdss_dsi_panel_config_res_properties(struct device_node *np,
		u32 sim_panel_mode, struct dsi_panel_timing *pt)
{
	mdss_dsi_parse_dcs_cmds(np, &pt->on_cmds,
			"qcom,mdss-dsi-on-command",
			"qcom,mdss-dsi-on-command-state");
	mdss_dsi_parse_dcs_cmds(np, &pt->switch_cmds,
			"qcom,mdss-dsi-timing-switch-command",
			"qcom,mdss-dsi-timing-switch-command-state");
	mdss_dsi_parse_fbc_params(np, &pt->timing.fbc);
	mdss_panel_parse_te_params(np, sim_panel_mode, &pt->timing);
}

static int mdss_dsi_panel_parse_display_timings(struct device_node *np,
		struct mdss_panel_data *panel_data)
{
	struct mdss_dsi_ctrl_pdata *ctrl;
	struct dsi_panel_timing *modedb;
	struct device_node *timings_np;
	struct device_node *entry;
	int num_timings, rc;
	int i = 0, active_ndx = 0;

	ctrl = container_of(panel_data, struct mdss_dsi_ctrl_pdata, panel_data);

	INIT_LIST_HEAD(&panel_data->timings_list);

	timings_np = of_get_child_by_name(np, "qcom,mdss-dsi-display-timings");
	if (!timings_np) {
		struct dsi_panel_timing pt;
		memset(&pt, 0, sizeof(struct dsi_panel_timing));

		/*
		 * display timings node is not available, fallback to reading
		 * timings directly from root node instead
		 */
		pr_debug("reading display-timings from panel node\n");
		rc = mdss_dsi_panel_timing_from_dt(np, &pt);
		if (!rc) {
			mdss_dsi_panel_config_res_properties(np,
				panel_data->panel_info.sim_panel_mode, &pt);
			rc = mdss_dsi_panel_timing_switch(ctrl, &pt.timing);
		}
		return rc;
	}

	num_timings = of_get_child_count(timings_np);
	if (num_timings == 0) {
		pr_err("no timings found within display-timings\n");
		rc = -EINVAL;
		goto exit;
	}

	modedb = kzalloc(num_timings * sizeof(*modedb), GFP_KERNEL);
	if (!modedb) {
		pr_err("unable to allocate modedb\n");
		rc = -ENOMEM;
		goto exit;
	}

	for_each_child_of_node(timings_np, entry) {
		rc = mdss_dsi_panel_timing_from_dt(entry, modedb + i);
		if (rc) {
			kfree(modedb);
			goto exit;
		}

		mdss_dsi_panel_config_res_properties(entry,
			panel_data->panel_info.sim_panel_mode, (modedb + i));

		/* if default is set, use it otherwise use first as default */
		if (of_property_read_bool(entry,
				"qcom,mdss-dsi-timing-default"))
			active_ndx = i;

		list_add(&modedb[i].timing.list,
				&panel_data->timings_list);
		i++;
	}

	/* Configure default timing settings */
	rc = mdss_dsi_panel_timing_switch(ctrl, &modedb[active_ndx].timing);
	if (rc)
		pr_err("unable to configure default timing settings\n");

exit:
	of_node_put(timings_np);

	return rc;
}

static int mdss_panel_parse_dt(struct device_node *np,
			struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	u32 tmp;
	int rc;
	const char *data;
	static const char *pdest;
	struct mdss_panel_info *pinfo = &(ctrl_pdata->panel_data.panel_info);

	rc = mdss_dsi_panel_parse_display_timings(np,
					&ctrl_pdata->panel_data);
	if (rc)
		return rc;
	rc = of_property_read_u32(np,
		"qcom,mdss-pan-physical-width-dimension", &tmp);
	pinfo->physical_width = (!rc ? tmp : 0);
	rc = of_property_read_u32(np,
		"qcom,mdss-pan-physical-height-dimension", &tmp);
	pinfo->physical_height = (!rc ? tmp : 0);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-bpp", &tmp);
	if (rc) {
		pr_err("%s:%d, bpp not specified\n", __func__, __LINE__);
		return -EINVAL;
	}
	pinfo->bpp = (!rc ? tmp : 24);
	pinfo->mipi.mode = DSI_VIDEO_MODE;
	data = of_get_property(np, "qcom,mdss-dsi-panel-type", NULL);
	if (data && !strncmp(data, "dsi_cmd_mode", 12))
		pinfo->mipi.mode = DSI_CMD_MODE;
	pinfo->mipi.boot_mode = pinfo->mipi.mode;
	tmp = 0;
	data = of_get_property(np, "qcom,mdss-dsi-pixel-packing", NULL);
	if (data && !strcmp(data, "loose"))
		pinfo->mipi.pixel_packing = 1;
	else
		pinfo->mipi.pixel_packing = 0;
	rc = mdss_panel_get_dst_fmt(pinfo->bpp,
		pinfo->mipi.mode, pinfo->mipi.pixel_packing,
		&(pinfo->mipi.dst_format));
	if (rc) {
		pr_debug("%s: problem determining dst format. Set Default\n",
			__func__);
		pinfo->mipi.dst_format =
			DSI_VIDEO_DST_FORMAT_RGB888;
	}
	pdest = of_get_property(np,
		"qcom,mdss-dsi-panel-destination", NULL);

	if (pdest) {
		if (strlen(pdest) != 9) {
			pr_err("%s: Unknown pdest specified\n", __func__);
			return -EINVAL;
		}
		if (!strcmp(pdest, "display_1"))
			pinfo->pdest = DISPLAY_1;
		else if (!strcmp(pdest, "display_2"))
			pinfo->pdest = DISPLAY_2;
		else {
			pr_debug("%s: incorrect pdest. Set Default\n",
				__func__);
			pinfo->pdest = DISPLAY_1;
		}
	} else {
		pr_debug("%s: pdest not specified. Set Default\n",
				__func__);
		pinfo->pdest = DISPLAY_1;
	}
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-underflow-color", &tmp);
	pinfo->lcdc.underflow_clr = (!rc ? tmp : 0xff);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-border-color", &tmp);
	pinfo->lcdc.border_clr = (!rc ? tmp : 0);
	data = of_get_property(np, "qcom,mdss-dsi-panel-orientation", NULL);
	if (data) {
		pr_debug("panel orientation is %s\n", data);
		if (!strcmp(data, "180"))
			pinfo->panel_orientation = MDP_ROT_180;
		else if (!strcmp(data, "hflip"))
			pinfo->panel_orientation = MDP_FLIP_LR;
		else if (!strcmp(data, "vflip"))
			pinfo->panel_orientation = MDP_FLIP_UD;
	}

	ctrl_pdata->bklt_ctrl = UNKNOWN_CTRL;
	data = of_get_property(np, "qcom,mdss-dsi-bl-pmic-control-type", NULL);
	if (data) {
		if (!strncmp(data, "bl_ctrl_wled", 12)) {
			led_trigger_register_simple("bkl-trigger",
				&bl_led_trigger);
			pr_debug("%s: SUCCESS-> WLED TRIGGER register\n",
				__func__);
			ctrl_pdata->bklt_ctrl = BL_WLED;
		} else if (!strncmp(data, "bl_ctrl_pwm", 11)) {
			ctrl_pdata->bklt_ctrl = BL_PWM;
			ctrl_pdata->pwm_pmi = of_property_read_bool(np,
					"qcom,mdss-dsi-bl-pwm-pmi");
			rc = of_property_read_u32(np,
				"qcom,mdss-dsi-bl-pmic-pwm-frequency", &tmp);
			if (rc) {
				pr_err("%s:%d, Error, panel pwm_period\n",
						__func__, __LINE__);
				return -EINVAL;
			}
			ctrl_pdata->pwm_period = tmp;
			if (ctrl_pdata->pwm_pmi) {
				ctrl_pdata->pwm_bl = of_pwm_get(np, NULL);
				if (IS_ERR(ctrl_pdata->pwm_bl)) {
					pr_err("%s: Error, pwm device\n",
								__func__);
					ctrl_pdata->pwm_bl = NULL;
					return -EINVAL;
				}
			} else {
				rc = of_property_read_u32(np,
					"qcom,mdss-dsi-bl-pmic-bank-select",
								 &tmp);
				if (rc) {
					pr_err("%s:%d, Error, lpg channel\n",
							__func__, __LINE__);
					return -EINVAL;
				}
				ctrl_pdata->pwm_lpg_chan = tmp;
				tmp = of_get_named_gpio(np,
					"qcom,mdss-dsi-pwm-gpio", 0);
				ctrl_pdata->pwm_pmic_gpio = tmp;
				pr_debug("%s: Configured PWM bklt ctrl\n",
								 __func__);
			}
		} else if (!strncmp(data, "bl_ctrl_dcs", 11)) {
			ctrl_pdata->bklt_ctrl = BL_DCS_CMD;
			pr_debug("%s: Configured DCS_CMD bklt ctrl\n",
								__func__);
		}
	}
	rc = of_property_read_u32(np, "qcom,mdss-brightness-max-level", &tmp);
	pinfo->brightness_max = (!rc ? tmp : MDSS_MAX_BL_BRIGHTNESS);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bl-min-level", &tmp);
	pinfo->bl_min = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bl-max-level", &tmp);
	pinfo->bl_max = (!rc ? tmp : 255);
	ctrl_pdata->bklt_max = pinfo->bl_max;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-interleave-mode", &tmp);
	pinfo->mipi.interleave_mode = (!rc ? tmp : 0);

	pinfo->mipi.vsync_enable = of_property_read_bool(np,
		"qcom,mdss-dsi-te-check-enable");

	if (pinfo->sim_panel_mode == SIM_SW_TE_MODE)
		pinfo->mipi.hw_vsync_mode = false;
	else
		pinfo->mipi.hw_vsync_mode = of_property_read_bool(np,
			"qcom,mdss-dsi-te-using-te-pin");

	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-h-sync-pulse", &tmp);
	pinfo->mipi.pulse_mode_hsa_he = (!rc ? tmp : false);

	pinfo->mipi.hfp_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-hfp-power-mode");
	pinfo->mipi.hsa_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-hsa-power-mode");
	pinfo->mipi.hbp_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-hbp-power-mode");
	pinfo->mipi.last_line_interleave_en = of_property_read_bool(np,
		"qcom,mdss-dsi-last-line-interleave");
	pinfo->mipi.bllp_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-bllp-power-mode");
	pinfo->mipi.eof_bllp_power_stop = of_property_read_bool(
		np, "qcom,mdss-dsi-bllp-eof-power-mode");
	pinfo->mipi.traffic_mode = DSI_NON_BURST_SYNCH_PULSE;
	data = of_get_property(np, "qcom,mdss-dsi-traffic-mode", NULL);
	if (data) {
		if (!strcmp(data, "non_burst_sync_event"))
			pinfo->mipi.traffic_mode = DSI_NON_BURST_SYNCH_EVENT;
		else if (!strcmp(data, "burst_mode"))
			pinfo->mipi.traffic_mode = DSI_BURST_MODE;
	}
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-te-dcs-command", &tmp);
	pinfo->mipi.insert_dcs_cmd =
			(!rc ? tmp : 1);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-wr-mem-continue", &tmp);
	pinfo->mipi.wr_mem_continue =
			(!rc ? tmp : 0x3c);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-wr-mem-start", &tmp);
	pinfo->mipi.wr_mem_start =
			(!rc ? tmp : 0x2c);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-te-pin-select", &tmp);
	pinfo->mipi.te_sel =
			(!rc ? tmp : 1);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-virtual-channel-id", &tmp);
	pinfo->mipi.vc = (!rc ? tmp : 0);
	pinfo->mipi.rgb_swap = DSI_RGB_SWAP_RGB;
	data = of_get_property(np, "qcom,mdss-dsi-color-order", NULL);
	if (data) {
		if (!strcmp(data, "rgb_swap_rbg"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_RBG;
		else if (!strcmp(data, "rgb_swap_bgr"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_BGR;
		else if (!strcmp(data, "rgb_swap_brg"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_BRG;
		else if (!strcmp(data, "rgb_swap_grb"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_GRB;
		else if (!strcmp(data, "rgb_swap_gbr"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_GBR;
	}
	pinfo->mipi.data_lane0 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-0-state");
	pinfo->mipi.data_lane1 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-1-state");
	pinfo->mipi.data_lane2 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-2-state");
	pinfo->mipi.data_lane3 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-3-state");

	pinfo->mipi.rx_eot_ignore = of_property_read_bool(np,
		"qcom,mdss-dsi-rx-eot-ignore");
	pinfo->mipi.tx_eot_append = of_property_read_bool(np,
		"qcom,mdss-dsi-tx-eot-append");

	rc = of_property_read_u32(np, "qcom,mdss-dsi-stream", &tmp);
	pinfo->mipi.stream = (!rc ? tmp : 0);

	data = of_get_property(np, "qcom,mdss-dsi-panel-mode-gpio-state", NULL);
	if (data) {
		if (!strcmp(data, "high"))
			pinfo->mode_gpio_state = MODE_GPIO_HIGH;
		else if (!strcmp(data, "low"))
			pinfo->mode_gpio_state = MODE_GPIO_LOW;
	} else {
		pinfo->mode_gpio_state = MODE_GPIO_NOT_VALID;
	}

	rc = of_property_read_u32(np, "qcom,mdss-mdp-transfer-time-us", &tmp);
	pinfo->mdp_transfer_time_us = (!rc ? tmp : DEFAULT_MDP_TRANSFER_TIME);

	pinfo->mipi.lp11_init = of_property_read_bool(np,
					"qcom,mdss-dsi-lp11-init");
	rc = of_property_read_u32(np, "qcom,mdss-dsi-init-delay-us", &tmp);
	pinfo->mipi.init_delay = (!rc ? tmp : 0);

	mdss_dsi_parse_roi_alignment(np, pinfo);

	mdss_dsi_parse_trigger(np, &(pinfo->mipi.mdp_trigger),
		"qcom,mdss-dsi-mdp-trigger");

	mdss_dsi_parse_trigger(np, &(pinfo->mipi.dma_trigger),
		"qcom,mdss-dsi-dma-trigger");

	mdss_dsi_parse_lane_swap(np, &(pinfo->mipi.dlane_swap));

	mdss_dsi_parse_reset_seq(np, pinfo->rst_seq, &(pinfo->rst_seq_len),
		"qcom,mdss-dsi-reset-sequence");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->off_cmds,
		"qcom,mdss-dsi-off-command", "qcom,mdss-dsi-off-command-state");

	pinfo->mipi.force_clk_lane_hs = of_property_read_bool(np,
		"qcom,mdss-dsi-force-clock-lane-hs");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cmds,
		"qcom,mdss-dsi-dispparam-command", "qcom,mdss-dsi-dispparam-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_ceon_cmds,
		"qcom,mdss-dsi-dispparam-ceon-command", "qcom,mdss-dsi-dispparam-ceon-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_ceoff_cmds,
		"qcom,mdss-dsi-dispparam-ceoff-command", "qcom,mdss-dsi-dispparam-ceoff-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cabcon_cmds,
		"qcom,mdss-dsi-dispparam-cabcon-command", "qcom,mdss-dsi-dispparam-cabcon-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cabcguion_cmds,
		"qcom,mdss-dsi-dispparam-cabcguion-command", "qcom,mdss-dsi-dispparam-cabcguion-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cabcstillon_cmds,
		"qcom,mdss-dsi-dispparam-cabcstillon-command", "qcom,mdss-dsi-dispparam-cabcstillon-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cabcmovieon_cmds,
		"qcom,mdss-dsi-dispparam-cabcmovieon-command", "qcom,mdss-dsi-dispparam-cabcmovieon-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cabcoff_cmds,
		"qcom,mdss-dsi-dispparam-cabcoff-command", "qcom,mdss-dsi-dispparam-cabcoff-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_gammareload_cmds,
		"qcom,mdss-dsi-dispparam-gammareload-command", "qcom,mdss-dsi-dispparam-gammareload-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_warm_cmds,
		"qcom,mdss-dsi-dispparam-warm-command", "qcom,mdss-dsi-dispparam-warm-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_default_cmds,
		"qcom,mdss-dsi-dispparam-default-command", "qcom,mdss-dsi-dispparam-default-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_cold_cmds,
		"qcom,mdss-dsi-dispparam-cold-command", "qcom,mdss-dsi-dispparam-cold-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode_cmds,
		"qcom,mdss-dsi-dispparam-papermode-command", "qcom,mdss-dsi-dispparam-papermode-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode1_cmds,
		"qcom,mdss-dsi-dispparam-papermode1-command", "qcom,mdss-dsi-dispparam-papermode1-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode2_cmds,
		"qcom,mdss-dsi-dispparam-papermode2-command", "qcom,mdss-dsi-dispparam-papermode2-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode3_cmds,
		"qcom,mdss-dsi-dispparam-papermode3-command", "qcom,mdss-dsi-dispparam-papermode3-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode4_cmds,
		"qcom,mdss-dsi-dispparam-papermode4-command", "qcom,mdss-dsi-dispparam-papermode4-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode5_cmds,
		"qcom,mdss-dsi-dispparam-papermode5-command", "qcom,mdss-dsi-dispparam-papermode5-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode6_cmds,
		"qcom,mdss-dsi-dispparam-papermode6-command", "qcom,mdss-dsi-dispparam-papermode6-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_papermode7_cmds,
		"qcom,mdss-dsi-dispparam-papermode7-command", "qcom,mdss-dsi-dispparam-papermode7-command-state");


	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_vividweak_cmds,
		"qcom,mdss-dsi-dispparam-vividweak-command", "qcom,mdss-dsi-dispparam-vividweak-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_vividstrong_cmds,
		"qcom,mdss-dsi-dispparam-vividstrong-command", "qcom,mdss-dsi-dispparam-vividstrong-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_vividoff_cmds,
		"qcom,mdss-dsi-dispparam-vividoff-command", "qcom,mdss-dsi-dispparam-vividoff-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_smartweak_cmds,
		"qcom,mdss-dsi-dispparam-smartweak-command", "qcom,mdss-dsi-dispparam-smartweak-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_smartstrong_cmds,
		"qcom,mdss-dsi-dispparam-smartstrong-command", "qcom,mdss-dsi-dispparam-smartstrong-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_smartoff_cmds,
		"qcom,mdss-dsi-dispparam-smartoff-command", "qcom,mdss-dsi-dispparam-smartoff-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_scon_cmds,
		"qcom,mdss-dsi-dispparam-scon-command", "qcom,mdss-dsi-dispparam-scon-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_sreon_cmds,
		"qcom,mdss-dsi-dispparam-sreon-command", "qcom,mdss-dsi-dispparam-sreon-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_sreoff_cmds,
		"qcom,mdss-dsi-dispparam-sreoff-command", "qcom,mdss-dsi-dispparam-sreoff-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level0_cmds,
			"qcom,mdss-dsi-dispparam-level0-command", "qcom,mdss-dsi-dispparam-level0-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level1_cmds,
			"qcom,mdss-dsi-dispparam-level1-command", "qcom,mdss-dsi-dispparam-level1-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level2_cmds,
			"qcom,mdss-dsi-dispparam-level2-command", "qcom,mdss-dsi-dispparam-level2-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level3_cmds,
			"qcom,mdss-dsi-dispparam-level3-command", "qcom,mdss-dsi-dispparam-level3-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level4_cmds,
			"qcom,mdss-dsi-dispparam-level4-command", "qcom,mdss-dsi-dispparam-level4-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level5_cmds,
			"qcom,mdss-dsi-dispparam-level5-command", "qcom,mdss-dsi-dispparam-level5-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_level6_cmds,
			"qcom,mdss-dsi-dispparam-level6-command", "qcom,mdss-dsi-dispparam-level6-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_nightmode1_cmds,
			"qcom,mdss-dsi-dispparam-nightmode1-command", "qcom,mdss-dsi-dispparam-nightmode1-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_nightmode2_cmds,
			"qcom,mdss-dsi-dispparam-nightmode2-command", "qcom,mdss-dsi-dispparam-nightmode2-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_nightmode3_cmds,
			"qcom,mdss-dsi-dispparam-nightmode3-command", "qcom,mdss-dsi-dispparam-nightmode3-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_nightmode4_cmds,
			"qcom,mdss-dsi-dispparam-nightmode4-command", "qcom,mdss-dsi-dispparam-nightmode4-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_nightmode5_cmds,
			"qcom,mdss-dsi-dispparam-nightmode5-command", "qcom,mdss-dsi-dispparam-nightmode5-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_idleon_cmds,
		"qcom,mdss-dsi-dispparam-idleon-command", "qcom,mdss-dsi-dispparam-idleon-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_idleoff_cmds,
		"qcom,mdss-dsi-dispparam-idleoff-command", "qcom,mdss-dsi-dispparam-idleoff-command-state");
	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->dispparam_test_cmds,
		"qcom,mdss-dsi-dispparam-test-command", "qcom,mdss-dsi-dispparam-test-command-state");

	rc = mdss_dsi_parse_panel_features(np, ctrl_pdata);
	if (rc) {
		pr_err("%s: failed to parse panel features\n", __func__);
		goto error;
	}

	mdss_dsi_parse_panel_horizintal_line_idle(np, ctrl_pdata);

	mdss_dsi_parse_dfps_config(np, ctrl_pdata);

	mdss_parse_night_brightness(np, pinfo);

	return 0;

error:
	return -EINVAL;
}

int mdss_dsi_panel_init(struct device_node *node,
	struct mdss_dsi_ctrl_pdata *ctrl_pdata,
	bool cmd_cfg_cont_splash)
{
	int rc = 0;
	static const char *panel_name;
	struct mdss_panel_info *pinfo;
	bool dispparam_enabled;

	if (!node || !ctrl_pdata) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -ENODEV;
	}

	pinfo = &ctrl_pdata->panel_data.panel_info;
	pinfo->panel_active = 0;
	pinfo->kickoff_count = 0;

	pr_debug("%s:%d\n", __func__, __LINE__);
	pinfo->panel_name[0] = '\0';
	panel_name = of_get_property(node, "qcom,mdss-dsi-panel-name", NULL);
	if (!panel_name) {
		pr_info("%s:%d, Panel name not specified\n",
						__func__, __LINE__);
	} else {
		pr_info("%s: Panel Name = %s\n", __func__, panel_name);
		strlcpy(&pinfo->panel_name[0], panel_name, MDSS_MAX_PANEL_LEN);
	}
	rc = mdss_panel_parse_dt(node, ctrl_pdata);
	if (rc) {
		pr_err("%s:%d panel dt parse failed\n", __func__, __LINE__);
		return rc;
	}
	INIT_DELAYED_WORK(&ctrl_pdata->cmds_work, panelon_delayed_work);

	if (!cmd_cfg_cont_splash || pinfo->sim_panel_mode)
		pinfo->cont_splash_enabled = false;
	pr_info("%s: Continuous splash %s\n", __func__,
		pinfo->cont_splash_enabled ? "enabled" : "disabled");

	pinfo->dynamic_switch_pending = false;
	pinfo->is_lpm_mode = false;
	pinfo->esd_rdy = false;
	pinfo->panel_on_param = 0;

	dispparam_enabled = of_property_read_bool(node,
						"qcom,dispparam-enabled");
	if (dispparam_enabled) {
		pr_info("%s:%d Dispparam enabled.\n", __func__, __LINE__);
		ctrl_pdata->panel_data.panel_info.dispparam_enabled = 1;
		ctrl_pdata->dispparam_fnc = mdss_dsi_panel_dispparam;
	} else {
		pr_info("%s:%d Dispparam disabled.\n", __func__, __LINE__);
		ctrl_pdata->panel_data.panel_info.dispparam_enabled = 0;
		ctrl_pdata->dispparam_fnc = NULL;
	}
	ctrl_pdata->on_cmds_tuning = of_property_read_bool(node,
					"qcom,mdss-dsi-on-command-tuning");

	ctrl_pdata->on = mdss_dsi_panel_on;
	ctrl_pdata->off = mdss_dsi_panel_off;
	ctrl_pdata->low_power_config = mdss_dsi_panel_low_power_config;
	ctrl_pdata->panel_data.set_backlight = mdss_dsi_panel_bl_ctrl;
	ctrl_pdata->switch_mode = mdss_dsi_panel_switch_mode;

	return 0;
}
