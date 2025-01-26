/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#ifndef _ROCKCHIP_EBC_BLIT_H
#define _ROCKCHIP_EBC_BLIT_H

const u8 y4_mask_even = 0x0f;
const unsigned int y4_shift_even = 0;
const u8 y4_mask_odd = 0xf0;
const unsigned int y4_shift_odd = 4;
const u16 fnum_mask_even = 0x00ff;
const unsigned int fnum_shift_even = 0;
const u16 fnum_mask_odd = 0xff00;
const unsigned int fnum_shift_odd = 8;

/**
 * rockchip_ebc_drm_rect_extend - extend rect to include (x, y)
 * @r: rectangle
 * @x: x coordinate
 * @y: y coordinate
 */
inline void rockchip_ebc_drm_rect_extend(struct drm_rect *r, int x, int y)
{
	r->x1 = min(r->x1, x);
	r->x2 = max(r->x2, x + 1);
	r->y1 = min(r->y1, y);
	r->y2 = max(r->y2, y + 1);
}

/**
 * rockchip_ebc_drm_rect_extend_rect - extend rect r1 to include r2
 * @r: rectangle
 * @x: x coordinate
 * @y: y coordinate
 */
inline void rockchip_ebc_drm_rect_extend_rect(struct drm_rect *r1, const struct drm_rect *r2)
{
	r1->x1 = min(r1->x1, r2->x1);
	r1->x2 = max(r1->x2, r2->x2);
	r1->y1 = min(r1->y1, r2->y1);
	r1->y2 = max(r1->y2, r2->y2);
}

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

void rockchip_ebc_blit_direct_fnum(const struct rockchip_ebc_ctx *ctx,
				   u8 *phase, u8 *frame_num,
				   u8 *next, u8 *prev,
				   const struct drm_epd_lut *lut,
				   const struct drm_rect *clip);

void rockchip_ebc_blit_direct(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			      u8 phase, const struct drm_epd_lut *lut,
			      const struct drm_rect *clip);

void rockchip_ebc_blit_frame_num(const struct rockchip_ebc_ctx *ctx, u8 *dst,
				 u8 phase, const struct drm_rect *clip,
				 u8 *other_buffer, int last_phase, int frame,
				 int check_blit_frame_num);

void rockchip_ebc_increment_frame_num(const struct rockchip_ebc_ctx *ctx,
				      u8 *frame_num, u8 *frame_num_prev,
				      struct drm_rect *clip,
				      u8 last_phase);

bool rockchip_ebc_blit_pixels_last(const struct rockchip_ebc_ctx *ctx, u8 *prev,
				   u8 *next, u8 *frame_num_buffer,
				   struct drm_rect *clip, u8 last_phase);

void rockchip_ebc_schedule_and_blit(const struct rockchip_ebc_ctx *ctx,
				    u8 *frame_num, u8 *next, u8 *final,
				    const struct drm_rect *clip_ongoing,
				    struct drm_rect *clip_ongoing_new_areas,
				    struct rockchip_ebc_area *area, u32 frame,
				    u8 last_phase,
				    struct rockchip_ebc_area *next_area);

#endif /* _ROCKCHIP_EBC_BLIT_H */
