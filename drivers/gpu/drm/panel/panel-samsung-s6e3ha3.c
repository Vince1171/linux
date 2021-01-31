// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2020, The Linux Foundation. All rights reserved.

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>

#include <video/mipi_display.h>
#include <video/videomode.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

struct s6e3ha3 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct device *dev;
	struct videomode *mode;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline struct s6e3ha3 *to_s6e3ha3(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3ha3, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {				\
	static const u8 d[] = { seq };					\
	int ret;							\
	ret = mipi_dsi_dcs_write_buffer(dsi[0], d, ARRAY_SIZE(d));	\
	if (ret < 0)							\
		return ret;						\
	ret = mipi_dsi_dcs_write_buffer(dsi[1], d, ARRAY_SIZE(d));	\
	if (ret < 0)							\
		return ret;						\
} while (0)

static void s6e3ha3_reset(struct s6e3ha3 *ctx)
{
	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
}

static int s6e3ha3_on(struct s6e3ha3 *ctx)
{
	struct mipi_dsi_device **dsi = ctx->dsi;
	struct device *dev = ctx->dev;
	int i;
	int ret;

	int hdisplay = ctx->mode->hactive;
	int vdisplay = ctx->mode->vactive;

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ret = mipi_dsi_dcs_exit_sleep_mode(dsi[i]);
		if (ret < 0) {
			dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
			return ret;
		}
	}
	usleep_range(5000, 6000);

	ret = mipi_dsi_dcs_set_column_address(dsi[0], 0, hdisplay / 2 - 1);
	if (ret < 0) {
		dev_err(dev, "failed to set column address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi[0], 0, vdisplay - 1);
	if (ret < 0) {
		dev_err(dev, "failed to set page address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_column_address(dsi[1], hdisplay / 2,
					      hdisplay - 1);
	if (ret < 0) {
		dev_err(dev, "failed to set column address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi[1], 0, vdisplay - 1);
	if (ret < 0) {
		dev_err(dev, "failed to set page address: %d\n", ret);
		return ret;
	}

	dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
	dsi_dcs_write_seq(dsi, 0xb0, 0x10);
	dsi_dcs_write_seq(dsi, 0xb5, 0xa0);
	dsi_dcs_write_seq(dsi, 0xc4, 0x03);
	dsi_dcs_write_seq(dsi, 0xf6,
			  0x42, 0x57, 0x37, 0x00, 0xaa, 0xcc, 0xd0, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xf9, 0x03);
	dsi_dcs_write_seq(dsi, 0xc2,
			  0x00, 0x00, 0xd8, 0xd8, 0x00, 0x80, 0x2b, 0x05, 0x08,
			  0x0e, 0x07, 0x0b, 0x05, 0x0d, 0x0a, 0x15, 0x13, 0x20,
			  0x1e);
	dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
	msleep(120);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ret = mipi_dsi_dcs_set_tear_on(dsi[i],
					       MIPI_DSI_DCS_TEAR_MODE_VBLANK);
		if (ret < 0) {
			dev_err(dev, "Failed to set tear on: %d\n", ret);
			return ret;
		}
	}

	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ret = mipi_dsi_dcs_set_display_brightness(dsi[i], 0x60);
		if (ret < 0) {
			dev_err(dev, "Failed to set display brightness: %d\n",
				ret);
			return ret;
		}
	}

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ret = mipi_dsi_dcs_set_display_on(dsi[i]);
		if (ret < 0) {
			dev_err(dev, "Failed to set display on: %d\n", ret);
			return ret;
		}
	}
	usleep_range(5000, 6000);

	return 0;
}

static int s6e3ha3_off(struct s6e3ha3 *ctx)
{
	struct mipi_dsi_device **dsi = ctx->dsi;
	struct device *dev = ctx->dev;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ret = mipi_dsi_dcs_set_display_off(dsi[i]);
		if (ret < 0) {
			dev_err(dev, "Failed to set display off: %d\n", ret);
			return ret;
		}
	}
	msleep(60);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ret = mipi_dsi_dcs_enter_sleep_mode(dsi[i]);
		if (ret < 0) {
			dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
			return ret;
		}
	}
	msleep(180);

	return 0;
}

static int s6e3ha3_prepare(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	struct device *dev = ctx->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	s6e3ha3_reset(ctx);

	ret = s6e3ha3_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int s6e3ha3_unprepare(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	struct device *dev = ctx->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = s6e3ha3_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode s6e3ha3_mode = {
	.clock = (1440 + 200 + 80 + 200) * (2560 + 30 + 8 + 31) * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 200,
	.hsync_end = 1440 + 200 + 80,
	.htotal = 1440 + 200 + 80 + 200,
	.vdisplay = 2560,
	.vsync_start = 2560 + 30,
	.vsync_end = 2560 + 30 + 8,
	.vtotal = 2560 + 30 + 8 + 31,
	.width_mm = 68,
	.height_mm = 122,
};

static int s6e3ha3_get_modes(struct drm_panel *panel,
					 struct drm_connector *connector)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &s6e3ha3_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	drm_display_mode_from_videomode(ctx->mode, mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs s6e3ha3_panel_funcs = {
	.prepare = s6e3ha3_prepare,
	.unprepare = s6e3ha3_unprepare,
	.get_modes = s6e3ha3_get_modes,
};

static int s6e3ha3_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = bl->props.brightness;
	int ret;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	if (ret < 0)
		return ret;

	return 0;
}

static int s6e3ha3_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = bl->props.brightness;
	int ret;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0)
		return ret;

	return brightness & 0xff;
}

static const struct backlight_ops s6e3ha3_bl_ops = {
	.update_status = s6e3ha3_bl_update_status,
	.get_brightness = s6e3ha3_bl_get_brightness,
};

static struct backlight_device *
s6e3ha3_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 255,
		.max_brightness = 255,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &s6e3ha3_bl_ops, &props);
}

static int s6e3ha3_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct mipi_dsi_device *dsi1_device;
	struct device_node *dsi1;
	struct mipi_dsi_host *dsi1_host;
	struct mipi_dsi_device *dsi_dev;
	struct s6e3ha3 *ctx;
	int i;
	int ret;

	const struct mipi_dsi_device_info info = {
		.type = "s6e3ha3",
		.channel = 0,
		.node = NULL,
	};

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->enable_gpio)) {
		ret = PTR_ERR(ctx->enable_gpio);
		dev_err(dev, "Failed to get enable-gpios: %d\n", ret);
		return ret;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "Failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	dsi1 = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
	if (!dsi1) {
		DRM_DEV_ERROR(dev,
			"failed to get remote node for dsi1_device\n");
		return -ENODEV;
	}

	dsi1_host = of_find_mipi_dsi_host_by_node(dsi1);
	of_node_put(dsi1);
	if (!dsi1_host) {
		DRM_DEV_ERROR(dev, "failed to find dsi host\n");
		return -EPROBE_DEFER;
	}

	/* register the second DSI device */
	dsi1_device = mipi_dsi_device_register_full(dsi1_host, &info);
	if (IS_ERR(dsi1_device)) {
		DRM_DEV_ERROR(dev, "failed to create dsi device\n");
		return PTR_ERR(dsi1_device);
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	ctx->dsi[0] = dsi;
	ctx->dsi[1] = dsi1_device;

	drm_panel_init(&ctx->panel, dev, &s6e3ha3_panel_funcs,
			DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		dsi_dev = ctx->dsi[i];
		dsi_dev->lanes = 4;
		dsi_dev->format = MIPI_DSI_FMT_RGB888;
		ret = mipi_dsi_attach(dsi_dev);
		if (ret < 0) {
			DRM_DEV_ERROR(dev,
				"dsi attach failed i = %d\n", i);
			goto err_dsi_attach;
		}
	}

	ctx->panel.backlight = s6e3ha3_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight)) {
		ret = PTR_ERR(ctx->panel.backlight);
		dev_err(dev, "Failed to create backlight: %d\n", ret);
		return ret;
	}

	return 0;

err_dsi_attach:
	drm_panel_remove(&ctx->panel);
	return ret;
}

static int s6e3ha3_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3ha3 *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx->dsi[0])
		mipi_dsi_detach(ctx->dsi[0]);
	if (ctx->dsi[1]) {
		mipi_dsi_detach(ctx->dsi[1]);
		mipi_dsi_device_unregister(ctx->dsi[1]);
	}

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id s6e3ha3_of_match[] = {
	{ .compatible = "samsung,s6e3ha3" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s6e3ha3_of_match);

static struct mipi_dsi_driver s6e3ha3_driver = {
	.probe = s6e3ha3_probe,
	.remove = s6e3ha3_remove,
	.driver = {
		.name = "panel-samsung-s6e3ha3",
		.of_match_table = s6e3ha3_of_match,
	},
};
module_mipi_dsi_driver(s6e3ha3_driver);

MODULE_DESCRIPTION("DRM driver for Samsung S6E3HA3 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
