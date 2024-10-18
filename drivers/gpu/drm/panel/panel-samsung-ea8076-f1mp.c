/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Samsung EA8076 FHD F1MP Dsi driver.
 * Copyright (c) 2023 Degdag Mohamed <degdagmohamed@gmail.com>;
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct samsung_ea8076_f1mp {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline
struct samsung_ea8076_f1mp *to_samsung_ea8076_f1mp(struct drm_panel *panel)
{
	return container_of(panel, struct samsung_ea8076_f1mp, panel);
}

static void samsung_ea8076_f1mp_reset(struct samsung_ea8076_f1mp *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int samsung_ea8076_f1mp_on(struct samsung_ea8076_f1mp *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_seq(dsi, 0xb7, 0x01, 0x4b);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);

	ret = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x0923);
	if (ret < 0) {
		dev_err(dev, "Failed to set page address: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq(dsi, 0xfc, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq(dsi, 0xb0, 0x23);
	mipi_dsi_dcs_write_seq(dsi, 0xd1, 0x11);
	mipi_dsi_dcs_write_seq(dsi, 0xe9,
			       0x11, 0x55, 0xa6, 0x75, 0xa3, 0xb9, 0xa1, 0x4a,
			       0x00, 0x1a, 0xb8);
	mipi_dsi_dcs_write_seq(dsi, 0xe1, 0x00, 0x00, 0x02, 0x02, 0x42, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xe2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xb0, 0x0c);
	mipi_dsi_dcs_write_seq(dsi, 0xe1, 0x19);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq(dsi, 0xfc, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	ret = mipi_dsi_dcs_set_display_brightness(dsi, 0x0000);
	if (ret < 0) {
		dev_err(dev, "Failed to set display brightness: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_POWER_SAVE, 0x00);
	msleep(67);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	return 0;
}

static int samsung_ea8076_f1mp_off(struct samsung_ea8076_f1mp *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	return 0;
}

static int samsung_ea8076_f1mp_prepare(struct drm_panel *panel)
{
	struct samsung_ea8076_f1mp *ctx = to_samsung_ea8076_f1mp(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	samsung_ea8076_f1mp_reset(ctx);

	ret = samsung_ea8076_f1mp_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int samsung_ea8076_f1mp_unprepare(struct drm_panel *panel)
{
	struct samsung_ea8076_f1mp *ctx = to_samsung_ea8076_f1mp(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = samsung_ea8076_f1mp_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode samsung_ea8076_f1mp_mode = {
	.clock = (1080 + 64 + 20 + 64) * (2340 + 64 + 27 + 64) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 64,
	.hsync_end = 1080 + 64 + 20,
	.htotal = 1080 + 64 + 20 + 64,
	.vdisplay = 2340,
	.vsync_start = 2340 + 64,
	.vsync_end = 2340 + 64 + 27,
	.vtotal = 2340 + 64 + 27 + 64,
	.width_mm = 68,
	.height_mm = 147,
};

static int samsung_ea8076_f1mp_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &samsung_ea8076_f1mp_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs samsung_ea8076_f1mp_panel_funcs = {
	.prepare = samsung_ea8076_f1mp_prepare,
	.unprepare = samsung_ea8076_f1mp_unprepare,
	.get_modes = samsung_ea8076_f1mp_get_modes,
};

static int samsung_ea8076_f1mp_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

// TODO: Check if /sys/class/backlight/.../actual_brightness actually returns
static int samsung_ea8076_f1mp_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops samsung_ea8076_f1mp_bl_ops = {
	.update_status = samsung_ea8076_f1mp_bl_update_status,
	.get_brightness = samsung_ea8076_f1mp_bl_get_brightness,
};

static struct backlight_device *
samsung_ea8076_f1mp_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1024,
		.max_brightness = 2047,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &samsung_ea8076_f1mp_bl_ops, &props);
}

static int samsung_ea8076_f1mp_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct samsung_ea8076_f1mp *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vcie";
	ctx->supplies[2].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	drm_panel_init(&ctx->panel, dev, &samsung_ea8076_f1mp_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->panel.backlight = samsung_ea8076_f1mp_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void samsung_ea8076_f1mp_remove(struct mipi_dsi_device *dsi)
{
	struct samsung_ea8076_f1mp *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id samsung_ea8076_f1mp_of_match[] = {
	{ .compatible = "samsung,ea8076-f1mp" },
};
MODULE_DEVICE_TABLE(of, samsung_ea8076_f1mp_of_match);

static struct mipi_dsi_driver samsung_ea8076_f1mp_driver = {
	.probe = samsung_ea8076_f1mp_probe,
	.remove = samsung_ea8076_f1mp_remove,
	.driver = {
		.name = "panel-samsung-ea8076-f1mp",
		.of_match_table = samsung_ea8076_f1mp_of_match,
	},
};
module_mipi_dsi_driver(samsung_ea8076_f1mp_driver);

MODULE_AUTHOR("degdag-mohamed <degdagmohamed@gmail.com>");
MODULE_DESCRIPTION("Samsung EA8076 Fhd F1MP Dsi Driver");
MODULE_LICENSE("GPL")