// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <video/mipi_display.h>

struct panel_cmd {
	size_t len;
	const char *data;
};

#define _INIT_CMD(...) { \
	.len = sizeof((char[]){__VA_ARGS__}), \
	.data = (char[]){__VA_ARGS__} }

static const char * const regulator_names[] = {
	"vddio",
	"lab_reg",
	"ibb_reg",
};

static unsigned long const regulator_enable_loads[] = {
	62000,
	100000,
	100000
};

static unsigned long const regulator_disable_loads[] = {
	80,
	100,
	100
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	const char *panel_name;

	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	const struct panel_cmd *on_cmds_1;

	const struct panel_cmd *off_cmds;
};

struct panel_info {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct panel_desc *desc;

	struct backlight_device *backlight;
	u32 brightness;
	u32 max_brightness;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *suspend;


	bool prepared;
	bool enabled;
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, base);
}

static int send_mipi_cmds(struct drm_panel *panel, const struct panel_cmd *cmds)
{
	struct panel_info *pinfo = to_panel_info(panel);
	unsigned int i = 0;
	int err;

	if (!cmds)
		return -EFAULT;

	for (i = 0; cmds[i].len != 0; i++) {
		const struct panel_cmd *cmd = &cmds[i];

		if (cmd->len == 2)
			err = mipi_dsi_dcs_write(pinfo->link,
						    cmd->data[1], NULL, 0);
		else
			err = mipi_dsi_dcs_write(pinfo->link,
						    cmd->data[1], cmd->data + 2,
						    cmd->len - 2);

		if (err < 0)
			return err;

	}

	return 0;
}

static int panel_set_pinctrl_state(struct panel_info *panel, bool enable)
{
	int rc = 0;
	struct pinctrl_state *state;

	if (enable)
		state = panel->active;
	else
		state = panel->suspend;

	rc = pinctrl_select_state(panel->pinctrl, state);
	if (rc)
		pr_err("[%s] failed to set pin state, rc=%d\n", panel->desc->panel_name,
			rc);
	return rc;
}

static int ebbg_panel_disable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);

	backlight_disable(pinfo->backlight);
	pinfo->enabled = false;

	return 0;
}

static int ebbg_panel_power_off(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i, ret = 0;

	gpiod_set_value(pinfo->reset_gpio, 0);

        ret = panel_set_pinctrl_state(pinfo, false);
        if (ret) {
                pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
        }

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(panel->dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret) {
		DRM_DEV_ERROR(panel->dev,
			"regulator_bulk_disable failed %d\n", ret);
	}
	return ret;
}

static int ebbg_panel_unprepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	if (!pinfo->prepared)
		return 0;

	/* send off cmds */
	ret = send_mipi_cmds(panel, pinfo->desc->off_cmds);

	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to send DCS off cmds: %d\n", ret);
	}

	ret = mipi_dsi_dcs_set_display_off(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"set_display_off cmd failed ret = %d\n",
			ret);
	}

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = mipi_dsi_dcs_enter_sleep_mode(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"enter_sleep cmd failed ret = %d\n", ret);
	}
	/* 0x3C = 60ms delay */
	msleep(90);

	ret = ebbg_panel_power_off(panel);
	if (ret < 0)
		DRM_DEV_ERROR(panel->dev, "power_off failed ret = %d\n", ret);

	pinfo->prepared = false;

	return ret;

}

static int ebbg_panel_power_on(struct panel_info *pinfo)
{
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	ret = panel_set_pinctrl_state(pinfo, true);
	if (ret) {
		pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
	}

	/*
	 * Reset sequence of ebbg fhd_ft8719 panel requires the panel to be
	 * out of reset for 10ms, followed by being held in reset
	 * for 10ms(too lazy to correct it)
	 */
	gpiod_set_value(pinfo->reset_gpio, 1);
	msleep(4);
	gpiod_set_value(pinfo->reset_gpio, 0);
	msleep(1);
	gpiod_set_value(pinfo->reset_gpio, 1);
        msleep(15);


	return 0;
}

static int ebbg_panel_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	if (pinfo->prepared)
		return 0;

	err = ebbg_panel_power_on(pinfo);
	if (err < 0)
		goto poweroff;

	/* send first part of init cmds */
	err = send_mipi_cmds(panel, pinfo->desc->on_cmds_1);

	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to send DCS Init 1st Code: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_set_display_on(pinfo->link);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to Set Display ON: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_exit_sleep_mode(pinfo->link);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev, "failed to exit sleep mode: %d\n",
			      err);
		goto poweroff;
	}

	pinfo->prepared = true;

	return 0;

poweroff:
	gpiod_set_value(pinfo->reset_gpio, 1);
	return err;
}

static int ebbg_panel_enable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	if (pinfo->enabled)
		return 0;

	backlight_enable(pinfo->backlight);
	pinfo->enabled = true;

	return 0;
}

static int ebbg_panel_get_modes(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, m);
	if (!mode) {
		DRM_DEV_ERROR(panel->drm->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, m->vrefresh);
		return -ENOMEM;
	}

	panel->connector->display_info.width_mm = pinfo->desc->width_mm;
	panel->connector->display_info.height_mm = pinfo->desc->height_mm;

	drm_mode_set_name(mode);
	drm_mode_probed_add(panel->connector, mode);

	return 1;
}

static int ebbg_panel_backlight_update_status(struct backlight_device *bl)
{

	return 0;
}

static int ebbg_panel_backlight_get_brightness(struct backlight_device *bl)
{

	return 0xff;
}


static int ebbg_panel_backlight_init(struct panel_info *pinfo)
{

#if 0
	int rc = 0;

	led_trigger_register_simple("bkl-trigger", &pinfo->wled);

	/* LED APIs don't tell us directly whether a classdev has yet
	 * been registered to service this trigger. Until classdev is
	 * registered, calling led_trigger has no effect, and doesn't
	 * fail. Classdevs are associated with any registered triggers
	 * when they do register, but that is too late for FBCon.
	 * Check the cdev list directly and defer if appropriate.
	 */
	if (!bl->wled) {
		pr_err("[ebbg-fhd_ft8719] backlight registration failed\n");
		rc = -EINVAL;
	} else {
		read_lock(&pinfo->wled->leddev_list_lock);
		if (list_empty(&pinfo->wled->led_cdevs))
			rc = -EPROBE_DEFER;
		read_unlock(&pinfo->wled->leddev_list_lock);

		if (rc) {
			pr_info("[ebbg-fhd_ft8719] backlight:");
			pr_info(" %s not ready, defer probe\n",
				pinfo->wled->name);
			led_trigger_unregister_simple(pinfo->wled);
		}
	}

	return rc;
#endif
	return 0;
}


static const struct drm_panel_funcs panel_funcs = {
	.disable = ebbg_panel_disable,
	.unprepare = ebbg_panel_unprepare,
	.prepare = ebbg_panel_prepare,
	.enable = ebbg_panel_enable,
	.get_modes = ebbg_panel_get_modes,
};

static const struct panel_cmd ebbg_fhd_ft8719_on_cmds_1[] = {
	_INIT_CMD(0x00, 0x00, 0x00),
	_INIT_CMD(0x00, 0xFF, 0x87, 0x19, 0x01),
	_INIT_CMD(0x00, 0x00, 0x80),
	_INIT_CMD(0x00, 0xFF, 0x87, 0x19),

	_INIT_CMD(0x00, 0x00, 0xA0),
	_INIT_CMD(0x00, 0xCA, 0x0F, 0x0F, 0x0F),

	_INIT_CMD(0x00, 0x00, 0x80),
	_INIT_CMD(0x00, 0xCA, 0xBE, 0xB5, 0xAD, 0xA6, 0xA0, 0x9B, 0x96, 0x91, 0x8D, 0x8A, 0x87, 0x83),
	_INIT_CMD(0x00, 0x00, 0x90),
	_INIT_CMD(0x00, 0xCA, 0xFE, 0xFF, 0x66, 0xFB, 0xFF, 0x32),

	_INIT_CMD(0x00, 0x00, 0xA0),
	_INIT_CMD(0x00, 0xD6, 0x7A, 0x79, 0x74, 0x8C, 0x8C, 0x92, 0x97, 0x9B, 0x97, 0x8F, 0x80, 0x77),
        _INIT_CMD(0x00, 0x00, 0xB0),
	_INIT_CMD(0x00, 0xD6, 0x7E, 0x7D, 0x81, 0x7A, 0x7A, 0x7B, 0x7C, 0x81, 0x84, 0x85, 0x80, 0x82),
	_INIT_CMD(0x00, 0x00, 0xC0),
        _INIT_CMD(0x00, 0xD6, 0x7D, 0x7D, 0x78, 0x8A, 0x89, 0x8F, 0x97, 0x97, 0x8F, 0x8C, 0x80, 0x7A),
	_INIT_CMD(0x00, 0x00, 0xD0),
        _INIT_CMD(0x00, 0xD6, 0x7E, 0x7D, 0x81, 0x7C, 0x79, 0x7B, 0x7C, 0x80, 0x84, 0x85, 0x80, 0x82),
	_INIT_CMD(0x00, 0x00, 0xE0),
        _INIT_CMD(0x00, 0xD6, 0x7B, 0x7B, 0x7B, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80),
	_INIT_CMD(0x00, 0x00, 0xF0),
        _INIT_CMD(0x00, 0xD6, 0x7E, 0x7E, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80),
	_INIT_CMD(0x00, 0x00, 0x00),
        _INIT_CMD(0x00, 0xD7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80),
	_INIT_CMD(0x00, 0x00, 0x10),
        _INIT_CMD(0x00, 0xD7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80),

	_INIT_CMD(0x00, 0x00, 0x00),
	_INIT_CMD(0x00, 0xFF, 0x00, 0x00, 0x00),
	_INIT_CMD(0x00, 0x00, 0x80),
	_INIT_CMD(0x00, 0xFF, 0x00, 0x00),
	_INIT_CMD(0x00, 0x91, 0x00),
	_INIT_CMD(0x00, 0x51, 0xFF),
	_INIT_CMD(0x00, 0x53, 0x24),
	_INIT_CMD(0x00, 0x55, 0x00),
	_INIT_CMD(0x00, 0x11, 0x00),
	_INIT_CMD(0x00, 0x29, 0x00),

	{},
};

static const struct panel_cmd ebbg_fhd_ft8719_off_cmds[] = {
	_INIT_CMD(0x00, 0x28, 0x00),
        _INIT_CMD(0x00, 0x10, 0x00),

	{},
};

static const struct drm_display_mode ebbg_panel_default_mode = {
	.clock = (1080 + 28 + 4 + 16) * (2246 + 120 + 4 + 12) * 60 / 1000,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 28,
	.hsync_end	= 1080 + 28 + 4,
	.htotal		= 1080 + 28 + 4 + 16,

	.vdisplay	= 2246,
	.vsync_start	= 2246 + 120,
	.vsync_end	= 2246 + 120 + 4,
	.vtotal		= 2246 + 120 + 4 + 12,
	.vrefresh	= 60,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct panel_desc ebbg_panel_desc = {
	.display_mode = &ebbg_panel_default_mode,

	.width_mm = 68,
	.height_mm = 141,

	.mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_VIDEO
			| MIPI_DSI_MODE_VIDEO_HSE
			| MIPI_DSI_CLOCK_NON_CONTINUOUS
			| MIPI_DSI_MODE_VIDEO_BURST,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = ebbg_fhd_ft8719_on_cmds_1,
	.off_cmds = ebbg_fhd_ft8719_off_cmds
};

static const struct of_device_id panel_of_match[] = {
	{ .compatible = "ebbg,fhd_ft8719",
	  .data = &ebbg_panel_desc
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, panel_of_match);


static int panel_pinctrl_init(struct panel_info *panel)
{
	struct device *dev = &panel->link->dev;
	int rc = 0;

	panel->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(panel->pinctrl)) {
		rc = PTR_ERR(panel->pinctrl);
		pr_err("failed to get pinctrl, rc=%d\n", rc);
		goto error;
	}

	panel->active = pinctrl_lookup_state(panel->pinctrl,
							"panel_active");
	if (IS_ERR_OR_NULL(panel->active)) {
		rc = PTR_ERR(panel->active);
		pr_err("failed to get pinctrl active state, rc=%d\n", rc);
		goto error;
	}

	panel->suspend =
		pinctrl_lookup_state(panel->pinctrl, "panel_suspend");

	if (IS_ERR_OR_NULL(panel->suspend)) {
		rc = PTR_ERR(panel->suspend);
		pr_err("failed to get pinctrl suspend state, rc=%d\n", rc);
		goto error;
	}

error:
	return rc;
}

static int panel_add(struct panel_info *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int i, ret;
pr_err("In fhd_ft8719 panel add\n");

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++)
		pinfo->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return ret;

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pinfo->reset_gpio)) {
		DRM_DEV_ERROR(dev, "cannot get reset gpio %ld\n",
			PTR_ERR(pinfo->reset_gpio));
		return PTR_ERR(pinfo->reset_gpio);
	}

	ret = panel_pinctrl_init(pinfo);
	if (ret < 0)
		return ret;

	pinfo->backlight = devm_of_find_backlight(dev);

	if (IS_ERR(pinfo->backlight))
		return PTR_ERR(pinfo->backlight);

	drm_panel_init(&pinfo->base);
pr_err("In fhd_ft8719 panel add: after drm_panel_init\n");
	pinfo->base.funcs = &panel_funcs;
	pinfo->base.dev = &pinfo->link->dev;

	ret = drm_panel_add(&pinfo->base);
	if (ret < 0)
		return ret;
pr_err("In fhd_ft8719 panel add: drm_panel_add returned %d\n", ret);
	return 0;
}

static void panel_del(struct panel_info *pinfo)
{
	if (pinfo->base.dev)
		drm_panel_remove(&pinfo->base);
}

static int panel_probe(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo;
	const struct panel_desc *desc;
	int err;

	pinfo = devm_kzalloc(&dsi->dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;

	pinfo->link = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);
pr_err("In fhd_ft8719 panel probe\n");

	err = panel_add(pinfo);
	if (err < 0)
		return err;

	err = mipi_dsi_attach(dsi);
pr_err("In fhd_ft8719 panel probe: mipi_dsi_attach returned: %d\n", err);
	return err;
}

static int panel_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = ebbg_panel_unprepare(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to unprepare panel: %d\n",
				err);

	err = ebbg_panel_disable(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to disable panel: %d\n", err);

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to detach from DSI host: %d\n",
				err);

	drm_panel_detach(&pinfo->base);
	panel_del(pinfo);

	return 0;
}

static void panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);

	ebbg_panel_disable(&pinfo->base);
	ebbg_panel_unprepare(&pinfo->base);
}

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-ebbg-fhd_ft8719",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
	.shutdown = panel_shutdown,
};
module_mipi_dsi_driver(panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_DESCRIPTION("ebbg fhd_ft8719 MIPI-DSI LCD panel");
MODULE_LICENSE("GPL");
