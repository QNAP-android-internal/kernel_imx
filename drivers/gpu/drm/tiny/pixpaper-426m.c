// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for PIXPAPER e-ink panel
 *
 * Author: LiangCheng Wang <zaq14760@gmail.com>,
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

MODULE_IMPORT_NS("DMA_BUF");

/* Panel visible resolution */
#define PIXPAPER_WIDTH    800
#define PIXPAPER_HEIGHT   480

#define PIXPAPER_WIDTH_MM   24    /* approximate from 23.7046mm */
#define PIXPAPER_HEIGHT_MM  49    /* approximate from 48.55mm */

#define PIXPAPER_SPI_BITS_PER_WORD 8
#define PIXPAPER_SPI_SPEED_DEFAULT 1000000

#define PIXPAPER_PANEL_BUFFER_WIDTH 128

#define PIXPAPER_LUT_BYTES           111
#define PIXPAPER_LUT_WF_BYTES        100
#define PIXPAPER_LUT_IDX_CMD03       105
#define PIXPAPER_LUT_IDX_CMD04_START 106

static int pixpaper_gray_map[8] = { 3, 2, 1, 0, 7, 6, 5, 4 };

static const u8 pixpaper_lut_color1_update[] = {
	0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x02, 0x01, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x32, 0x32,
	0x26, 0x0F, 0x04,
};

static const u8 pixpaper_lut_color2_update[] = {
	0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x32, 0x32,
	0x1E, 0x0F, 0x04,
};

static const u8 pixpaper_pass_mask[] = {
	0x80,	/* pass 0: MSB */
	0x40,	/* pass 1 */
	0x20,	/* pass 2 */
};

struct pixpaper_error_ctx {
	int errno_code;
};

struct pixpaper_panel {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct spi_device *spi;
	struct gpio_desc *reset;
	struct gpio_desc *busy;
	struct gpio_desc *dc;

	bool grayscale;
};

static const uint32_t pixpaper_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static inline u8 pixpaper_quantize_map_level(u8 gray)
{
	u8 lvl = gray >> 5;
	int m;

	m = pixpaper_gray_map[lvl];
	if (m < 0)
		m = 0;
	else if (m > 7)
		m = 7;

	return (u8)m;
}

static inline struct pixpaper_panel *to_pixpaper_panel(struct drm_device *drm)
{
	return container_of(drm, struct pixpaper_panel, drm);
}

static void pixpaper_wait_busy(struct pixpaper_panel *panel)
{
	unsigned int timeout_ms = 10000;
	unsigned long timeout_jiffies = jiffies + msecs_to_jiffies(timeout_ms);

	usleep_range(1000, 1500);
	while (gpiod_get_value_cansleep(panel->busy) != 0) {
		if (time_after(jiffies, timeout_jiffies)) {
			drm_warn(&panel->drm, "Busy wait timed out\n");
			return;
		}
		usleep_range(100, 200);
	}
}

static void pixpaper_spi_sync(struct spi_device *spi, struct spi_message *msg,
			      struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	int ret = spi_sync(spi, msg);
	if (ret < 0)
		err->errno_code = ret;
}

static void pixpaper_send_cmd(struct pixpaper_panel *panel, u8 cmd,
			      struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	struct spi_transfer xfer = {
		.tx_buf = &cmd,
		.len = 1,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 0);
	usleep_range(1, 5);
	pixpaper_spi_sync(panel->spi, &msg, err);
}

static void pixpaper_send_data(struct pixpaper_panel *panel, u8 data,
			       struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	struct spi_transfer xfer = {
		.tx_buf = &data,
		.len = 1,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 1);
	usleep_range(1, 5);
	pixpaper_spi_sync(panel->spi, &msg, err);
}

static void pixpaper_upload_lut(struct pixpaper_panel *panel, const u8 *lut,
				struct pixpaper_error_ctx *err)
{
	int i;

	if (err->errno_code)
		return;

	if (!lut)
		return;

	pixpaper_send_cmd(panel, 0x03, err);
	pixpaper_send_data(panel, lut[PIXPAPER_LUT_IDX_CMD03], err);

	pixpaper_send_cmd(panel, 0x04, err);
	for (i = 0; i < 3; i++)
		pixpaper_send_data(panel,
				   lut[PIXPAPER_LUT_IDX_CMD04_START + i], err);

	pixpaper_send_cmd(panel, 0x32, err);
	for (i = 0; i < PIXPAPER_LUT_WF_BYTES; i++)
		pixpaper_send_data(panel, lut[i], err);

	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, 0x22, err);
	pixpaper_send_data(panel, 0xC0, err);
	pixpaper_send_cmd(panel, 0x20, err);
	pixpaper_wait_busy(panel);
}

static void pixpaper_reset_ram_counters(struct pixpaper_panel *panel,
				        struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	pixpaper_send_cmd(panel, 0x4E, err);
	pixpaper_send_data(panel, 0x00, err);
	pixpaper_send_data(panel, 0x00, err);

	pixpaper_send_cmd(panel, 0x4F, err);
	pixpaper_send_data(panel, 0x00, err);
	pixpaper_send_data(panel, 0x00, err);
}

static void pixpaper_kick_pass(struct pixpaper_panel *panel, int pass,
			       struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	if (pass == 0) {
		pixpaper_send_cmd(panel, 0x22, err);
		pixpaper_send_data(panel, 0xF4, err);
		pixpaper_send_cmd(panel, 0x20, err);
	} else {
		pixpaper_send_cmd(panel, 0x21, err);
		pixpaper_send_data(panel, 0x40, err);
		pixpaper_send_data(panel, 0x00, err);

		pixpaper_send_cmd(panel, 0x22, err);
		pixpaper_send_data(panel, 0xCF, err);
		pixpaper_send_cmd(panel, 0x20, err);
	}

	pixpaper_wait_busy(panel);
}

static void pixpaper_full_clear(struct pixpaper_panel *panel,
				u32 dst_pitch, u32 height,
				struct pixpaper_error_ctx *err)
{
	u32 i, len = dst_pitch * height;

	if (err->errno_code)
		return;

	pixpaper_reset_ram_counters(panel, err);

	pixpaper_send_cmd(panel, 0x24, err);
	for (i = 0; i < len; i++)
		pixpaper_send_data(panel, 0xFF, err);

	pixpaper_kick_pass(panel, 0, err);
}

static int pixpaper_panel_hw_init(struct pixpaper_panel *panel)
{
	struct device *dev = &panel->spi->dev;
	int ret = 0;
	struct pixpaper_error_ctx err = { .errno_code = 0 };

	dev_info(dev, "%s: Starting hardware initialization\n", __func__);

	gpiod_set_value_cansleep(panel->reset, 0);
	msleep(50);
	gpiod_set_value_cansleep(panel->reset, 1);
	msleep(50);

	pixpaper_wait_busy(panel);
	dev_info(dev, "Hardware reset complete, panel idle.\n");

	pixpaper_send_cmd(panel, 0x18, &err);
	pixpaper_send_data(panel, 0x80, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x0C, &err);
	pixpaper_send_data(panel, 0xAE, &err);
	pixpaper_send_data(panel, 0xC7, &err);
	pixpaper_send_data(panel, 0xC3, &err);
	pixpaper_send_data(panel, 0xC0, &err);
	pixpaper_send_data(panel, 0x80, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x01, &err);
	pixpaper_send_data(panel, (480 - 1) & 0xFF, &err);
	pixpaper_send_data(panel, (480 - 1) >> 8, &err);
	pixpaper_send_data(panel, 0x02, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x3C, &err);
	pixpaper_send_data(panel, 0x01, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x44, &err);
	pixpaper_send_data(panel, 0x00, &err);
	pixpaper_send_data(panel, 0x00, &err);
	pixpaper_send_data(panel, (800 - 1) & 0xFF, &err);
	pixpaper_send_data(panel, (800 - 1) >> 8, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x45, &err);
	pixpaper_send_data(panel, 0x00, &err);
	pixpaper_send_data(panel, 0x00, &err);
	pixpaper_send_data(panel, (480 - 1) & 0xFF, &err);
	pixpaper_send_data(panel, (480 - 1) >> 8, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x4E, &err);
	pixpaper_send_data(panel, 0x00, &err);
	pixpaper_send_data(panel, 0x00, &err);

	if (ret)
		goto init_fail;

	pixpaper_send_cmd(panel, 0x4F, &err);
	pixpaper_send_data(panel, 0x00, &err);
	pixpaper_send_data(panel, 0x00, &err);

	if (ret)
		goto init_fail;

	dev_info(dev, "%s: Hardware initialization successful\n", __func__);
	return 0;

init_fail:
	dev_err(dev, "%s: Hardware initialization failed (err=%d)\n", __func__, ret);
	return 1;
}

static void pixpaper_fb_to_bitplane(void *src, void *dst, int height,
				int width, int dst_pitch, uint32_t format,
				u8 mask)
{
	uint8_t *dst_pixels = dst;
	int bit_idx;

	if (dst == NULL || src == NULL)
		return;

	switch (mask) {
		case 0x80:
			bit_idx = 2;
			break;
		case 0x40:
			bit_idx = 1;
			break;
		case 0x20:
			bit_idx = 0;
			break;
		default:
			bit_idx = 2;
			break;
	}

	for (int y = 0; y < height; y++) {
		uint8_t *dst_row = dst_pixels + y * dst_pitch;

		for (int x = 0; x < width; x++) {
			int src_x = width - 1 - x;
			uint8_t r, g, b;
			int bit_pos = x % 8;
			int byte_pos = x / 8;
			uint32_t gray_val;

			if (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) {
				uint32_t *src_pixels = src;
				uint32_t pixel = src_pixels[y * width + src_x];
				r = (pixel >> 16) & 0xFF;
				g = (pixel >> 8) & 0xFF;
				b = pixel & 0xFF;
			} else {
				continue;
			}

			gray_val = (r * 299 + g * 587 + b * 114 + 500) / 1000;

			u8 mapped = pixpaper_quantize_map_level(gray_val);
			u8 bit = (mapped >> bit_idx) & 0x1;

			if (bit)
				dst_row[byte_pos] |= BIT(7 - bit_pos);
			else
				dst_row[byte_pos] &= ~BIT(7 - bit_pos);
		}
	}
}

static void pixpaper_fb_to_mono(void *src, void *dst, int height, int width,
			       int dst_pitch, uint32_t format)
{
	pixpaper_fb_to_bitplane(src, dst, height, width, dst_pitch, format, 0x80);
}

static int pixpaper_plane_helper_atomic_check(struct drm_plane *plane,
					      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *new_crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	int ret;

	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state,
						  new_crtc_state, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret)
		return ret;
	else if (!new_plane_state->visible)
		return 0;

	return 0;
}

static int pixpaper_crtc_helper_atomic_check(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

static void pixpaper_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct pixpaper_panel *panel = to_pixpaper_panel(crtc->dev);
	struct drm_device *drm = &panel->drm;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;
	printk("Panel enabled and powered on\n");

	drm_dev_exit(idx);
}

static void pixpaper_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct pixpaper_panel *panel = to_pixpaper_panel(crtc->dev);
	struct drm_device *drm = &panel->drm;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;
	printk("Panel disabled\n");

	drm_dev_exit(idx);
}

static void pixpaper_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);
	struct pixpaper_panel *panel = to_pixpaper_panel(plane->dev);

	/* Prevent NULL deref when CRTC is detached or plane disabled */
	if (!plane_state->crtc || !plane_state->fb || !plane_state->visible) {
		printk("!plane_state->crtc || !plane_state->fb || !plane_state->visible\n");
		return;
	}

	{
		struct drm_device *drm = &panel->drm;
		struct drm_framebuffer *fb = plane_state->fb;
		struct iosys_map map = shadow_plane_state->data[0];
		void *vaddr = map.vaddr;
		int i, idx, pass;
		struct pixpaper_error_ctx err = { .errno_code = 0 };
		uint32_t dst_pitch;
		void *dst;

		if (!drm_dev_enter(drm, &idx))
			return;

		printk("Starting frame update (phys=%dx%d, buf_w=%d)\n",
		       PIXPAPER_WIDTH, PIXPAPER_HEIGHT, PIXPAPER_PANEL_BUFFER_WIDTH);

		if (!plane_state->crtc) {
			drm_dev_exit(idx);
			return;
		}

		if (!fb) {
			drm_dev_exit(idx);
			return;
		}

		if (!fb || !plane_state->visible) {
			printk("No framebuffer or plane not visible, skipping update\n");
			goto update_cleanup;
		}

		dst_pitch = (fb->width + 7) / 8;

		dst = kmalloc(dst_pitch * fb->height, GFP_KERNEL);
		if (!dst) {
			printk("Failed to allocate temporary buffer\n");
			goto update_cleanup;
		}

		if (panel->grayscale) {
			pixpaper_full_clear(panel, dst_pitch, fb->height, &err);
			if (err.errno_code)
				goto update_cleanup;

			for (pass = 0; pass < 3; pass++) {
				const u8 *lut = NULL;
				u8 mask = pixpaper_pass_mask[pass];

				if (pass == 1)
					lut = pixpaper_lut_color1_update;
				else if (pass == 2)
					lut = pixpaper_lut_color2_update;

				memset(dst, 0x00, dst_pitch * fb->height);
				pixpaper_fb_to_bitplane(vaddr, dst, fb->height, fb->width,
						dst_pitch, fb->format->format, mask);

				pixpaper_upload_lut(panel, lut, &err);
				pixpaper_reset_ram_counters(panel, &err);

				pixpaper_send_cmd(panel, 0x24, &err);
				if (err.errno_code)
					goto update_cleanup;

				for (u32 i = 0; i < dst_pitch * fb->height; i++) {
					pixpaper_send_data(panel, ((u8 *)dst)[i], &err);
					if (err.errno_code)
						goto update_cleanup;
				}

				pixpaper_kick_pass(panel, pass, &err);
				if (err.errno_code)
					goto update_cleanup;
			}
		} else {
			memset(dst, 0x00, dst_pitch * fb->height);
			pixpaper_fb_to_bitplane(vaddr, dst, fb->height, fb->width,
					dst_pitch, fb->format->format, 0x80);

			pixpaper_reset_ram_counters(panel, &err);
			pixpaper_send_cmd(panel, 0x24, &err);
			if (err.errno_code)
				goto update_cleanup;

			for (u32 i = 0; i < dst_pitch * fb->height; i++) {
				pixpaper_send_data(panel, ((u8 *)dst)[i], &err);
				if (err.errno_code)
					goto update_cleanup;
			}

			pixpaper_kick_pass(panel, 0, &err);
			if (err.errno_code)
				goto update_cleanup;
		}
	update_cleanup:
		if (err.errno_code && err.errno_code != -ETIMEDOUT)
			printk("Frame update function failed with error %d\n", err.errno_code);

		kfree(dst);
		drm_dev_exit(idx);
	}
}

static int pixpaper_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	drm_dbg(connector->dev, "CALLED for connector %s (id: %d)\n",
		connector->name, connector->base.id);

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		drm_err(connector->dev,
			"Failed to create mode for connector %s\n",
			connector->name);
		return 0;
	}

	mode->hdisplay    = 800;
	mode->hsync_start = 800 + 40;
	mode->hsync_end   = 800 + 40 + 48;
	mode->htotal      = 800 + 40 + 48 + 40;

	mode->vdisplay    = 480;
	mode->vsync_start = 480 + 10;
	mode->vsync_end   = 480 + 10 + 3;
	mode->vtotal      = 480 + 10 + 3 + 32;

	mode->clock       = 29200;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);

	if (drm_mode_validate_size(mode, connector->dev->mode_config.max_width,
				   connector->dev->mode_config.max_height) != MODE_OK) {
		printk("Mode %s (%dx%d) failed size validation against max %dx%d\n",
		       mode->name, mode->hdisplay, mode->vdisplay,
		       connector->dev->mode_config.max_width,
		       connector->dev->mode_config.max_height);
		drm_mode_destroy(connector->dev, mode);
		return 0;
	}

	drm_mode_probed_add(connector, mode);
	printk("Added mode '%s' (%dx%d@%d) to connector %s\n",
	       mode->name, mode->hdisplay, mode->vdisplay,
	       drm_mode_vrefresh(mode), connector->name);

	connector->display_info.width_mm  = PIXPAPER_WIDTH_MM;
	connector->display_info.height_mm = PIXPAPER_HEIGHT_MM;

	return 1;
}

static enum drm_mode_status pixpaper_crtc_mode_valid(struct drm_crtc *crtc,
						     const struct drm_display_mode *mode)
{
	if (mode->hdisplay == PIXPAPER_WIDTH &&
	    mode->vdisplay == PIXPAPER_HEIGHT)
		return MODE_OK;

	return MODE_BAD;
}

static const struct drm_plane_funcs pixpaper_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_plane_helper_funcs pixpaper_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = pixpaper_plane_helper_atomic_check,
	.atomic_update = pixpaper_plane_atomic_update,
};

static const struct drm_crtc_funcs pixpaper_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs pixpaper_crtc_helper_funcs = {
	.mode_valid = pixpaper_crtc_mode_valid,
	.atomic_check = pixpaper_crtc_helper_atomic_check,
	.atomic_enable = pixpaper_crtc_atomic_enable,
	.atomic_disable = pixpaper_crtc_atomic_disable,
};

static const struct drm_encoder_funcs pixpaper_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_funcs pixpaper_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs pixpaper_connector_helper_funcs = {
	.get_modes = pixpaper_connector_get_modes,
};

DEFINE_DRM_GEM_DMA_FOPS(pixpaper_fops);

static struct drm_driver pixpaper_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &pixpaper_fops,
	.name = "pixpaper",
	.desc = "DRM driver for PIXPAPER e-ink",
	.major = 1,
	.minor = 0,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_GEM_DMA_DRIVER_OPS,
};

static int pixpaper_mode_valid(struct drm_device *dev,
			       const struct drm_display_mode *mode)
{
	if (mode->hdisplay == PIXPAPER_WIDTH &&
	    mode->vdisplay == PIXPAPER_HEIGHT) {
		return MODE_OK;
	}
	return MODE_BAD;
}

static const struct drm_mode_config_funcs pixpaper_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.mode_valid = pixpaper_mode_valid,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int pixpaper_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct pixpaper_panel *panel;
	struct drm_device *drm;
	int ret;

	panel = devm_drm_dev_alloc(dev, &pixpaper_drm_driver,
				   struct pixpaper_panel, drm);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	drm = &panel->drm;
	panel->spi = spi;
	spi_set_drvdata(spi, panel);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = PIXPAPER_SPI_BITS_PER_WORD;

	if (!spi->max_speed_hz) {
		drm_warn(drm,
			 "spi-max-frequency not specified in DT, using default %u Hz\n",
			 PIXPAPER_SPI_SPEED_DEFAULT);
		spi->max_speed_hz = PIXPAPER_SPI_SPEED_DEFAULT;
	}

	ret = spi_setup(spi);
	if (ret < 0) {
		drm_err(drm, "SPI setup failed: %d\n", ret);
		return ret;
	}

	if (!dev->dma_mask)
		dev->dma_mask = &dev->coherent_dma_mask;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		drm_err(drm, "Failed to set DMA mask: %d\n", ret);
		return ret;
	}

	panel->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset))
		return PTR_ERR(panel->reset);

	panel->busy = devm_gpiod_get(dev, "busy", GPIOD_IN);
	if (IS_ERR(panel->busy))
		return PTR_ERR(panel->busy);

	panel->dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->dc))
		return PTR_ERR(panel->dc);

	panel->grayscale =
	device_property_read_bool(&panel->spi->dev,
		"pixpaper,grayscale");

	ret = pixpaper_panel_hw_init(panel);
	if (ret) {
		drm_err(drm, "Panel hardware initialization failed: %d\n", ret);
		return ret;
	}

	drm->mode_config.funcs = &pixpaper_mode_config_funcs;
	drm->mode_config.min_width = PIXPAPER_WIDTH;
	drm->mode_config.max_width = PIXPAPER_WIDTH;
	drm->mode_config.min_height = PIXPAPER_HEIGHT;
	drm->mode_config.max_height = PIXPAPER_HEIGHT;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	ret = drm_universal_plane_init(drm, &panel->plane, 1, &pixpaper_plane_funcs,
				       pixpaper_formats, ARRAY_SIZE(pixpaper_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&panel->plane, &pixpaper_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &panel->crtc, &panel->plane, NULL,
					&pixpaper_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&panel->crtc, &pixpaper_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &panel->encoder, &pixpaper_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;

	ret = drm_connector_init(drm, &panel->connector,
				 &pixpaper_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	drm_connector_helper_add(&panel->connector,
				 &pixpaper_connector_helper_funcs);
	drm_connector_attach_encoder(&panel->connector, &panel->encoder);

	drm_mode_config_reset(drm);

	panel->encoder.possible_crtcs = drm_crtc_mask(&panel->crtc);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_dma_setup(drm, 32);

	return 0;
}

static void pixpaper_remove(struct spi_device *spi)
{
	struct pixpaper_panel *panel = spi_get_drvdata(spi);

	if (!panel)
		return;

	drm_dev_unplug(&panel->drm);
	drm_atomic_helper_shutdown(&panel->drm);
}

static const struct spi_device_id pixpaper_ids[] = { { "pixpaper-426m", 0 }, {} };
MODULE_DEVICE_TABLE(spi, pixpaper_ids);

static const struct of_device_id pixpaper_dt_ids[] = {
	{ .compatible = "mayqueen,pixpaper-426m" },
	{}
};
MODULE_DEVICE_TABLE(of, pixpaper_dt_ids);

static struct spi_driver pixpaper_spi_driver = {
	.driver = {
		.name = "pixpaper-426m",
		.of_match_table = pixpaper_dt_ids,
	},
	.id_table = pixpaper_ids,
	.probe = pixpaper_probe,
	.remove = pixpaper_remove,
};

module_spi_driver(pixpaper_spi_driver);

MODULE_AUTHOR("LiangCheng Wang");
MODULE_DESCRIPTION("DRM SPI driver for PIXPAPER e-ink panel");
MODULE_LICENSE("GPL");
