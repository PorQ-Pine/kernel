/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#ifndef _ROCKCHIP_EBC_BLIT_H
#define _ROCKCHIP_EBC_BLIT_H

bool rockchip_ebc_blit_fb_r4(const struct rockchip_ebc_ctx *ctx,
			     const struct drm_rect *dst_clip, const void *vaddr,
			     const struct drm_framebuffer *fb,
			     const struct drm_rect *src_clip,
			     bool shrink_damage_clip);

bool rockchip_ebc_blit_fb_xrgb8888(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, bool reflect_x, bool reflect_y,
	bool shrink_damage_clip, int bw_mode, int bw_threshold,
	int bw_dither_invert, int fourtone_low_threshold,
	int fourtone_mid_threshold, int fourtone_hi_threshold);

void rockchip_ebc_blit_pixels(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			      const u8 *src, const struct drm_rect *clip);

void rockchip_ebc_blit_direct(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			      u8 phase, const struct drm_epd_lut *lut,
			      const struct drm_rect *clip);

void rockchip_ebc_blit_phase(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			     u8 phase, const struct drm_rect *clip,
			     u8 *other_buffer, int last_phase, int frame,
			     int check_blit_phase);

#endif /* _ROCKCHIP_EBC_BLIT_H */
