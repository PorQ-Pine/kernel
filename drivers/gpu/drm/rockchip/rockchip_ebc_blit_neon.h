/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#ifndef _ROCKCHIP_EBC_BLIT_NEON_H
#define _ROCKCHIP_EBC_BLIT_NEON_H

void rockchip_ebc_blit_fb_xrgb8888_y4_neon(const struct rockchip_ebc_ctx *ctx,
					   struct drm_rect *dst_clip,
					   const void *vaddr,
					   const struct drm_framebuffer *fb,
					   const struct drm_rect *src_clip);

void rockchip_ebc_blit_fb_xrgb8888_y4_thresholded4_neon(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, u8 invert, u8 fourtone_low_threshold,
	u8 fourtone_mid_threshold, u8 fourtone_hi_threshold, bool dither);

void rockchip_ebc_blit_fb_xrgb8888_y4_dithered2_neon(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, u8 invert);

void rockchip_ebc_update_blit_fnum_prev_neon(const struct rockchip_ebc_ctx *ctx,
					     u8 *prev, u8 *next, u8 *fnum,
					     u8 *fnum_prev,
					     struct drm_rect *clip,
					     u8 last_phase);

void rockchip_ebc_blit_direct_fnum_a2_neon(const struct rockchip_ebc_ctx *ctx,
					u8 *phase, u8 *frame_num, u8 *next,
					u8 *prev, const struct drm_epd_lut *lut,
					u8 a2_shorten_waveform,
					const struct drm_rect *clip);

void rockchip_ebc_blit_frame_num_neon(const struct rockchip_ebc_ctx *ctx,
				      u8 *dst, u8 phase,
				      const struct drm_rect *clip,
				      u8 *other_buffer, int last_phase,
				      int frame, int check_blit_frame_num);

void rockchip_ebc_schedule_and_blit_neon(
	const struct rockchip_ebc_ctx *ctx, u8 *frame_num, u8 *next, u8 *final,
	struct drm_rect *clip_ongoing_new_areas, struct rockchip_ebc_area *area,
	u32 frame, u8 last_phase);

void rockchip_ebc_schedule_cancel_blit_a2_neon(
	const struct rockchip_ebc_ctx *ctx, u8 *frame_num, u8 *prev, u8 *next,
	u8 *final, struct drm_rect *clip_ongoing_new_areas,
	struct rockchip_ebc_area *area, u8 last_phase,
	u8 early_cancellation_addition);

#endif /* _ROCKCHIP_EBC_BLIT_NEON_H */
