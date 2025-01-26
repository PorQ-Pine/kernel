/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#ifndef _ROCKCHIP_EBC_BLIT_NEON_H
#define _ROCKCHIP_EBC_BLIT_NEON_H

bool rockchip_ebc_blit_fb_xrgb8888_neon(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, bool reflect_x, bool reflect_y,
	bool shrink_damage_clip, int bw_mode, int bw_threshold,
	int bw_dither_invert, int fourtone_low_threshold,
	int fourtone_mid_threshold, int fourtone_hi_threshold);

void rockchip_ebc_blit_direct_fnum_neon(const struct rockchip_ebc_ctx *ctx,
					u8 *phase, u8 *frame_num, u8 *next,
					u8 *prev, const struct drm_epd_lut *lut,
					const struct drm_rect *clip);

void rockchip_ebc_blit_frame_num_neon(const struct rockchip_ebc_ctx *ctx,
				      u8 *dst, u8 phase,
				      const struct drm_rect *clip,
				      u8 *other_buffer, int last_phase,
				      int frame, int check_blit_frame_num);

void rockchip_ebc_increment_frame_num_neon(const struct rockchip_ebc_ctx *ctx,
					   u8 *frame_num, u8 *frame_num_prev,
					   struct drm_rect *clip,
					   u8 last_phase);

bool rockchip_ebc_blit_pixels_last_neon(const struct rockchip_ebc_ctx *ctx,
					u8 *prev, u8 *next,
					u8 *frame_num_buffer,
					struct drm_rect *clip, u8 last_phase);

void rockchip_ebc_schedule_and_blit_neon(
	const struct rockchip_ebc_ctx *ctx, u8 *frame_num, u8 *next, u8 *final,
	const struct drm_rect *clip_ongoing,
	struct drm_rect *clip_ongoing_new_areas, struct rockchip_ebc_area *area,
	u32 frame, u8 last_phase, struct rockchip_ebc_area *next_area);

#endif /* _ROCKCHIP_EBC_BLIT_NEON_H */
