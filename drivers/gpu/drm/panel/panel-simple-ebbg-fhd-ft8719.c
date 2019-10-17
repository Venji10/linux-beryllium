// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2013, The Linux Foundation. All rights reserved.

static const struct drm_display_mode ebbg_fhd_ft8719_mode = {
	.clock = (1080 + 28 + 4 + 16) * (2246 + 120 + 4 + 12) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 28,
	.hsync_end = 1080 + 28 + 4,
	.htotal = 1080 + 28 + 4 + 16,
	.vdisplay = 2246,
	.vsync_start = 2246 + 120,
	.vsync_end = 2246 + 120 + 4,
	.vtotal = 2246 + 120 + 4 + 12,
	.vrefresh = 60,
	.width_mm = 68,
	.height_mm = 141,
};

static const struct panel_desc_dsi ebbg_fhd_ft8719 = {
	.desc = {
		.modes = &ebbg_fhd_ft8719_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 68,
			.height = 141,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		 MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};
