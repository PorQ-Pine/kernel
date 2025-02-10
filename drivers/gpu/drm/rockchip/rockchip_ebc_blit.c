/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_rect.h>

#include "rockchip_ebc.h"
#include "rockchip_ebc_blit.h"
#include "rockchip_ebc_blit_neon.h"
#include <asm/neon.h>

bool rockchip_ebc_blit_fb_r4(const struct rockchip_ebc_ctx *ctx,
			     const struct drm_rect *dst_clip, const void *vaddr,
			     const struct drm_framebuffer *fb,
			     const struct drm_rect *src_clip,
			     bool shrink_damage_clip)
{
	unsigned int dst_pitch = ctx->gray4_pitch;
	unsigned int src_pitch = fb->pitches[0];
	unsigned int y;
	const void *src;
	void *dst;
	pr_debug("%s starting", __func__);

	unsigned width = src_clip->x2 - src_clip->x1;
	unsigned int x1_bytes = src_clip->x1 / 2;
	unsigned int x2_bytes = src_clip->x2 / 2;
	width = x2_bytes - x1_bytes;

	src = vaddr + src_clip->y1 * src_pitch + x1_bytes;
	dst = ctx->final_atomic_update + dst_clip->y1 * dst_pitch +
	      dst_clip->x1 / 2;

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		memcpy(dst, src, width);
		dst += dst_pitch;
		src += src_pitch;
	}

	return true;
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_r4);

bool rockchip_ebc_blit_fb_xrgb8888(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, bool reflect_x, bool reflect_y,
	bool shrink_damage_clip, int bw_mode, int bw_threshold,
	int bw_dither_invert, int fourtone_low_threshold,
	int fourtone_mid_threshold, int fourtone_hi_threshold)
{
	unsigned int dst_pitch = ctx->gray4_pitch;
	unsigned int src_pitch = fb->pitches[0];
	unsigned int src_start_x, x, y;
	int x_changed_min = -1, x_changed_max = -1;
	int y_changed_min = -1, y_changed_max = -1;
	u8 refl_y4_mask_even = reflect_x ? y4_mask_odd : y4_mask_even;
	u8 refl_y4_mask_odd = reflect_x ? y4_mask_even : y4_mask_odd;
	u8 refl_y4_shift_even = reflect_x ? y4_shift_odd : y4_shift_even;
	u8 refl_y4_shift_odd = reflect_x ? y4_shift_even : y4_shift_odd;
	const void *src;
	u8 changed = 0;
	int delta_x;
	void *dst;
	u8 dither_low, dither_high;

	// original pattern
	/* int dither_pattern[4][4] = { */
	/* 	{0, 8, 2, 10}, */
	/* 	{12, 4, 14, 6}, */
	/* 	{3, 11, 1,  9}, */
	/* 	{15, 7, 13, 5}, */
	/* }; */
	int dither_pattern[4][4] = {
		{ 7, 8, 2, 10 },
		{ 12, 4, 14, 6 },
		{ 3, 11, 1, 9 },
		{ 15, 7, 13, 5 },
	};

	pr_debug("%s starting", __func__);

	dither_low = bw_dither_invert ? 15 : 0;
	dither_high = bw_dither_invert ? 0 : 15;
	/* printk(KERN_INFO "dither low/high: %u %u bw_mode: %i\n", dither_low, dither_high, bw_mode); */

	// Scan out: row in src order
	// possibly decrease x1 by one for Y4 alignment
	src_start_x = src_clip->x1 ^ (src_clip->x1 & 1);
	delta_x = reflect_x ? -1 : 1;

	dst = ctx->final_atomic_update +
	      (reflect_y ? dst_clip->y2 - 1 : dst_clip->y1) * dst_pitch +
	      (reflect_x ? (dst_clip->x2 - 1) / 2 : dst_clip->x1 / 2);
	src = vaddr + src_clip->y1 * src_pitch +
	      src_start_x * fb->format->cpp[0];

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u32 *sbuf = src;
		u8 *dbuf = dst;

		// Y4 alignment in dst buffer
		for (x = src_start_x; x < src_clip->x2; x += 2) {
			u32 rgb0, rgb1;
			u8 gray;

			rgb0 = *sbuf++;
			rgb1 = *sbuf++;

			/* Truncate the RGB values to 5 bits each. */
			rgb0 &= 0x00f8f8f8U;
			rgb1 &= 0x00f8f8f8U;
			/* Put the sum 2R+5G+B in bits 24-31. */
			rgb0 *= 0x0020a040U;
			rgb1 *= 0x0020a040U;
			/* Unbias the value for rounding to 4 bits. */
			rgb0 += 0x07000000U;
			rgb1 += 0x07000000U;

			rgb0 >>= 28;
			rgb1 >>= 28;

			if (x < src_clip->x1) {
				// rgb0 should be filled with the content of the dst pixel here
				rgb0 = (*dbuf & refl_y4_mask_even) >> refl_y4_shift_even;
			}
			if (x == src_clip->x2 - 1) {
				// rgb1 should be filled with the content of the dst pixel we
				rgb1 = (*dbuf & refl_y4_mask_odd) >> refl_y4_shift_odd;
			}

			switch (bw_mode) {
			// do nothing for case 0
			case 1:
				/* if (y >= 1800){ */
				/* 	printk(KERN_INFO "bw+dither, before, rgb0 : %i, rgb1: %i\n", rgb0, rgb1); */
				/* } */
				// bw + dithering
				// convert to black and white
				if (rgb0 >= dither_pattern[x & 3][y & 3]) {
					rgb0 = dither_high;
				} else {
					rgb0 = dither_low;
				}

				if (rgb1 >=
				    dither_pattern[(x + 1) & 3][y & 3]) {
					rgb1 = dither_high;
				} else {
					rgb1 = dither_low;
				}
				/* printk(KERN_INFO "bw+dither, after, rgb0 : %i, rgb1: %i\n", rgb0, rgb1); */
				break;
			case 2:
				// bw
				// convert to black and white
				if (rgb0 >= bw_threshold) {
					rgb0 = dither_high;
				} else {
					rgb0 = dither_low;
				}

				if (rgb1 >= bw_threshold) {
					rgb1 = dither_high;
				} else {
					rgb1 = dither_low;
				}

				break;
			case 3:
				// downsample to 4 bw values corresponding to the DU4
				// transitions: 0, 5, 10, 15
				if (rgb0 < fourtone_low_threshold) {
					rgb0 = 0;
				} else if (rgb0 < fourtone_mid_threshold) {
					rgb0 = 5;
				} else if (rgb0 < fourtone_hi_threshold) {
					rgb0 = 10;
				} else {
					rgb0 = 15;
				}

				if (rgb1 < fourtone_low_threshold) {
					rgb1 = 0;
				} else if (rgb1 < fourtone_mid_threshold) {
					rgb1 = 5;
				} else if (rgb1 < fourtone_hi_threshold) {
					rgb1 = 10;
				} else {
					rgb1 = 15;
				}
			}

			gray = rgb0 << refl_y4_shift_even | rgb1 << refl_y4_shift_odd;
			changed = gray ^ *dbuf;
			if (changed) {
				y_changed_max = y;
				if (y_changed_min == -1)
					y_changed_min = y;
				int x_min, x_max;
				x_min = changed & refl_y4_mask_even ? x : x + 1;
				x_max = changed & refl_y4_mask_odd ? x + 1 : x;
				if (x_changed_min == -1 ||
				    x_changed_min > x_min)
					x_changed_min = x_min;
				if (x_changed_max < x_max)
					x_changed_max = x_max;
			}
			*dbuf = gray;
			dbuf += delta_x;
		}

		src += src_pitch;
		if (reflect_y)
			dst -= dst_pitch;
		else
			dst += dst_pitch;
	}
	if (shrink_damage_clip) {
		pr_debug(
			"blitted original dst_clip %d,%d-%d,%d with changed %d,%d-%d,%d",
			dst_clip->x1, dst_clip->y1, dst_clip->x2, dst_clip->y2,
			x_changed_min, y_changed_min, x_changed_max + 1,
			y_changed_max + 1);
		if (y_changed_min != -1) {
			if (reflect_y) {
				dst_clip->y1 +=
					src_clip->y2 - y_changed_max - 1;
				dst_clip->y2 -= y_changed_min - src_clip->y1;
			} else {
				dst_clip->y1 += y_changed_min - src_clip->y1;
				dst_clip->y2 -=
					src_clip->y2 - y_changed_max - 1;
			}
			if (reflect_x) {
				dst_clip->x1 +=
					src_clip->x2 - x_changed_max - 1;
				dst_clip->x2 -= x_changed_min - src_clip->x1;
			} else {
				dst_clip->x1 += x_changed_min - src_clip->x1;
				dst_clip->x2 -=
					src_clip->x2 - x_changed_max - 1;
			}
		} else {
			dst_clip->x1 = dst_clip->x2 = dst_clip->y1 =
				dst_clip->y2 = 0;
		}
		pr_debug("now dst_clip %d,%d %d %d", dst_clip->x1, dst_clip->y1,
			 dst_clip->x2, dst_clip->y2);
	}

	return y_changed_min != -1;
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_xrgb8888);

void rockchip_ebc_blit_direct_fnum(const struct rockchip_ebc_ctx *ctx,
				   u8 *phase, u8 *frame_num,
				   u8 *next, u8 *prev,
				   const struct drm_epd_lut *lut,
				   const struct drm_rect *clip)
{
	unsigned int phase_pitch = ctx->phase_pitch;
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;
	// 8 byte alignment for neon
	unsigned int x_start = max(0, min(clip->x1 & ~31, (int) ctx->frame_num_pitch - 32));
	unsigned int x_end = min((int) ctx->frame_num_pitch, (clip->x2 + 31) & ~31);

	u8 *phase_line = phase + clip->y1 * phase_pitch + x_start / 4;
	u8 *next_line = next + clip->y1 * gray4_pitch + x_start / 2;
	u8 *prev_line = prev + clip->y1 * gray4_pitch + x_start / 2;
	u8 *fnum_line = frame_num + clip->y1 * frame_num_pitch + x_start;
	/* Each byte in the phase buffer [DCBA] maps to a horizontal block of four pixels in Y4 format [BA] [DC] */
	for (y = clip->y1; y < clip->y2; ++y, next_line += gray4_pitch, prev_line += gray4_pitch, phase_line += phase_pitch, fnum_line += frame_num_pitch) {
		u8 *next_elm = next_line;
		u8 *prev_elm = prev_line;
		u8 *phase_elm = phase_line;
		u8 *fnum_elm = fnum_line;
		for (x = x_start; x < x_end; x += 4) {
			u8 prev0 = *prev_elm++;
			u8 next0 = *next_elm++;
			u8 prev1 = *prev_elm++;
			u8 next1 = *next_elm++;
			u8 fnum0 = *fnum_elm++;
			u8 fnum1 = *fnum_elm++;
			u8 fnum2 = *fnum_elm++;
			u8 fnum3 = *fnum_elm++;

			/*
			 * The LUT is 256 phases * 16 next * 16 previous levels.
			 * Each value is two bits, so the last dimension neatly
			 * fits in a 32-bit word.
			 */
			u8 data = ((((const u32 *)lut->buf + 16 * fnum0)[prev0 & 0xf] >> ((next0 & 0xf) << 1)) & 0x3) << 0 |
				  ((((const u32 *)lut->buf + 16 * fnum1)[prev0 >> 4] >> ((next0 >> 4) << 1)) & 0x3) << 2 |
				  ((((const u32 *)lut->buf + 16 * fnum2)[prev1 & 0xf] >> ((next1 & 0xf) << 1)) & 0x3) << 4 |
				  ((((const u32 *)lut->buf + 16 * fnum3)[prev1 >> 4] >> ((next1 >> 4) << 1)) & 0x3) << 6;
			*phase_elm++ = data;
		}
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_direct_fnum);

void rockchip_ebc_blit_direct(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			      u8 phase, const struct drm_epd_lut *lut,
			      const struct drm_rect *clip)
{
	// TODO: rewrite to blit from frame_num
	// TODO: with new approach: extend clip and do block-wise blitting, as diff has already happened and frame num is per-pixel
	const u32 *phase_lut = (const u32 *)lut->buf + 16 * phase;
	unsigned int dst_pitch = ctx->phase_pitch;
	unsigned int src_pitch = ctx->gray4_pitch;
	unsigned int x, y;
	u8 *dst_line;
	u32 src_line;
	unsigned int x_start = clip->x1 & ~3;

	/* Each byte in the phase buffer [DCBA] maps to a horizontal block of four pixels in Y4 format [BA] [DC] */
	u8 adjust_masks[] = { 0xff, 0xfc, 0xf0, 0xc0 };
	u8 x_start_mask = adjust_masks[clip->x1 & 3];
	u8 x_end_mask = ~adjust_masks[clip->x2 & 3];

	dst_line = dst + clip->y1 * dst_pitch + x_start / 4;
	src_line = clip->y1 * src_pitch + x_start / 2;

	for (y = clip->y1; y < clip->y2; y++) {
		u32 src_offset = src_line;
		u8 *dbuf = dst_line;

		for (x = x_start; x < clip->x2; x += 4) {
			u8 prev0 = ctx->prev[src_offset];
			u8 next0 = ctx->next[src_offset++];
			u8 prev1 = ctx->prev[src_offset];
			u8 next1 = ctx->next[src_offset++];

			/*
			 * The LUT is 256 phases * 16 next * 16 previous levels.
			 * Each value is two bits, so the last dimension neatly
			 * fits in a 32-bit word.
			 */
			u8 data = ((phase_lut[prev0 & 0xf] >>
				    ((next0 & 0xf) << 1)) &
				   0x3) << 0 |
				  ((phase_lut[prev0 >> 4] >>
				    ((next0 >> 4) << 1)) &
				   0x3) << 2 |
				  ((phase_lut[prev1 & 0xf] >>
				    ((next1 & 0xf) << 1)) &
				   0x3) << 4 |
				  ((phase_lut[prev1 >> 4] >>
				    ((next1 >> 4) << 1)) &
				   0x3) << 6;

			// Restore phase data written by other areas
			u8 data_mask = 0x00;

			// Don't assume 4-byte alignment
			if (x + 4 > clip->x2) {
				data &= x_end_mask;
				data_mask |= ~x_end_mask;
			}
			if (x == x_start) {
				data &= x_start_mask;
				data_mask |= ~x_start_mask;
			}

			*dbuf++ = data | (*dbuf & data_mask);
		}

		dst_line += dst_pitch;
		src_line += src_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_direct);

void rockchip_ebc_blit_frame_num(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			     u8 phase, const struct drm_rect *clip,
			     u8 *other_buffer, int last_phase, int frame,
			     int check_blit_frame_num)
{
	unsigned int pitch = ctx->frame_num_pitch;
	unsigned int width = clip->x2 - clip->x1;
	unsigned int y;
	u8 *dst_line;

	dst_line = dst + clip->y1 * pitch + clip->x1;
	u8 *dst_line2 = other_buffer + clip->y1 * pitch + clip->x1;

	for (y = clip->y1; y < clip->y2; y++) {
#ifdef ROCKCHIP_EBC_BLIT_FRAME_NUM_CHECK
		if (check_blit_frame_num == 1) {
			for (unsigned int x = 0; x < width; ++x) {
				int sched_err = 0;
				if (phase == 0) {
					sched_err |= dst_line[x] != 0xff &&
						     dst_line[x] != phase;
					sched_err |= (dst_line2[x] != 0xff)
						     << 1;
				} else if (phase == 1) {
					sched_err |= (dst_line[x] != 0xff &&
						      dst_line[x] != phase)
						     << 2;
					sched_err |= (dst_line2[x] != 0) << 3;
				} else if (phase == 0xff) {
					sched_err |=
						(!((dst_line2[x] == 0xff &&
						    (dst_line[x] ==
							     last_phase - 1 ||
						     dst_line[x] == 0xff)) ||
						   (dst_line2[x] ==
							    last_phase - 1 &&
						    (dst_line[x] ==
							     last_phase - 2 ||
						     dst_line[x] == 0xff))))
						<< 4;
				} else {
					sched_err |=
						(!(dst_line2[x] == phase - 1 &&
						   (dst_line[x] == phase - 2 ||
						    dst_line[x] == phase)))
						<< 5;
				}
				if (sched_err) {
					pr_info("scheduling error: err=%d frame=%d x=%d y=%d dst=%d other_dst=%d phase=%d, last_phase=%d",
						sched_err, frame, clip->x1 + x,
						y, dst_line[x], dst_line2[x],
						phase, last_phase);
				}
			}
		} else if (check_blit_frame_num == 2) {
			int x = width / 2;
			int sched_err = 0;
			if (phase == 0) {
				sched_err |= dst_line[x] != 0xff &&
					     dst_line[x] != phase;
				sched_err |= (dst_line2[x] != 0xff) << 1;
			} else if (phase == 1) {
				sched_err |= (dst_line[x] != 0xff &&
					      dst_line[x] != phase)
					     << 2;
				sched_err |= (dst_line2[x] != 0) << 3;
			} else if (phase == 0xff) {
				sched_err |=
					(!((dst_line2[x] == 0xff &&
					    (dst_line[x] == last_phase - 1 ||
					     dst_line[x] == 0xff)) ||
					   (dst_line2[x] == last_phase - 1 &&
					    (dst_line[x] == last_phase - 2 ||
					     dst_line[x] == 0xff))))
					<< 4;
			} else {
				sched_err |= (!(dst_line2[x] == phase - 1 &&
						(dst_line[x] == phase - 2 ||
						 dst_line[x] == phase)))
					     << 5;
			}
			if (sched_err) {
				pr_info("scheduling error: err=%d frame=%d x=%d y=%d dst=%d other_dst=%d phase=%d, last_phase=%d",
					sched_err, frame, clip->x1 + x, y,
					dst_line[x], dst_line2[x], phase,
					last_phase);
			}
		}
#endif
		memset(dst_line, phase, width);

		dst_line += pitch;
		dst_line2 += pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_frame_num);

// Increment frame_num by one within clip if < last_phase, otherwise set to 0xff
void rockchip_ebc_increment_frame_num(const struct rockchip_ebc_ctx *ctx,
				      u8 *frame_num, u8 *frame_num_prev,
				      struct drm_rect *clip,
				      u8 last_phase)
{
	unsigned int pitch = ctx->frame_num_pitch;
	unsigned int x, y;
	struct drm_rect clip_shrunk = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };

	for (y = clip->y1; y < clip->y2; ++y) {
		u8 *fnum_line = frame_num + y * pitch + clip->x1;
		u8 *fnum_line_prev = frame_num_prev + y * pitch + clip->x1;
		for (x = clip->x1; x < clip->x2; ++x, ++fnum_line, ++fnum_line_prev) {
			if (*fnum_line_prev < last_phase) {
				rockchip_ebc_drm_rect_extend(&clip_shrunk, x, y);
				*fnum_line = *fnum_line_prev + 1;
			} else if (*fnum_line != 0xff) {
				// Avoid writes to potentially speed up dma_sync
				*fnum_line = 0xff;
			}
		}
	}
	pr_debug("%s clip=" DRM_RECT_FMT " clip_shrunk=" DRM_RECT_FMT, __func__, DRM_RECT_ARG(clip), DRM_RECT_ARG(&clip_shrunk));
	*clip = clip_shrunk;
}
EXPORT_SYMBOL(rockchip_ebc_increment_frame_num);

// Blit next to prev within clip where frame_num is equal to last_phase. Clip can be adjusted.
bool rockchip_ebc_blit_pixels_last(const struct rockchip_ebc_ctx *ctx, u8 *prev,
				   u8 *next, u8 *frame_num_buffer,
				   struct drm_rect *clip, u8 last_phase)
{
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;
	 u16 double_last_phase = (u16) last_phase | ((u16) last_phase) << 8; // CONCAT11
	// TODO: be more selective to reduce writes. Either check destination or reintroduce last_phase in increment
	// u16 double_last_phase = 0xffff;
	// TODO: probably unnecessary. Consider always syncing instead
	bool changed = false;
	unsigned int x_start = clip->x1 & ~1;
	for (y = clip->y1; y < clip->y2; ++y) {
		u8 *prev_line = prev + y * gray4_pitch + x_start / 2;
		u8 *next_line = next + y * gray4_pitch + x_start / 2;
		u16 *frame_number_line = (u16 *)(frame_num_buffer + y * frame_num_pitch + x_start);
		for (x = x_start; x < clip->x2; x += 2, ++prev_line, ++next_line, ++frame_number_line) {
			if (*frame_number_line == double_last_phase) {
				*prev_line = *next_line;
				changed = true;
			} else if ((*frame_number_line & fnum_mask_even) == (double_last_phase & fnum_mask_even)) {
				*prev_line = (*prev_line & y4_mask_odd) | (*next_line & y4_mask_even);
				changed = true;
			} else if ((*frame_number_line & fnum_mask_odd) == (double_last_phase & fnum_mask_odd)) {
				*prev_line = (*prev_line & y4_mask_even) | (*next_line & y4_mask_odd);
				changed = true;
			}
		}
	}

	pr_debug("%s clip=" DRM_RECT_FMT " changed=%d", __func__, DRM_RECT_ARG(clip), changed);
	return changed;
}
EXPORT_SYMBOL(rockchip_ebc_blit_pixels_last);

// Blit 0 to frame_num and final to next within clip where final differs from next and frame_num is 0xff or 0x00. Shrink clip to smallest conflict area.
void rockchip_ebc_schedule_and_blit(const struct rockchip_ebc_ctx *ctx,
				    u8 *frame_num, u8 *next, u8 *final,
				    const struct drm_rect *clip_ongoing,
				    struct drm_rect *clip_ongoing_new_areas,
				    struct rockchip_ebc_area *area, u32 frame,
				    u8 last_phase,
				    struct rockchip_ebc_area *next_area)
{
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;
	// unsigned int x_start = area->clip.x1 & ~1;
	unsigned int x_start = max(0, min(area->clip.x1 & ~15, (int) ctx->frame_num_pitch - 32));
	unsigned int pixel_count = 0;
	u32 frame_begin = frame + last_phase + 1;
	struct drm_rect conflict = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	struct drm_rect area_started = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	for (y = area->clip.y1; y < area->clip.y2; ++y) {
		u8 *next_elm = next + y * gray4_pitch + x_start / 2;
		u8 *final_elm = final + y * gray4_pitch + x_start / 2;
		u16 *fnum = (u16 *)(frame_num + y * frame_num_pitch + x_start);
		for (x = x_start; x < area->clip.x2; x += 2, ++next_elm, ++final_elm, ++fnum) {
			bool blit_odd = false, blit_even = false;
			u8 diff_elm = *next_elm ^ *final_elm;
			if (diff_elm == 0)
				continue;
			if (diff_elm & y4_mask_even) {
				if ((*fnum & fnum_mask_even) == fnum_mask_even) {
					blit_even = true;
					++pixel_count;
					rockchip_ebc_drm_rect_extend(&area_started, x, y);
				} else if (*fnum & fnum_mask_even) {
					frame_begin = min(frame_begin, frame + 1 + last_phase - ((*fnum & fnum_mask_even) >> fnum_shift_even));
					rockchip_ebc_drm_rect_extend(&conflict, x, y);
				}
			}
			if (diff_elm & y4_mask_odd) {
				if ((*fnum & fnum_mask_odd) == fnum_mask_odd) {
					blit_odd = true;
					rockchip_ebc_drm_rect_extend(&area_started, x + 1, y);
					++pixel_count;
				} else if (*fnum & fnum_mask_odd) {
					frame_begin = min(frame_begin, frame + 1 + last_phase - ((*fnum & fnum_mask_odd) >> fnum_shift_odd));
					rockchip_ebc_drm_rect_extend(&conflict, x + 1, y);
				}
			}
			if (blit_odd && blit_even) {
				*fnum = 0;
				*next_elm = *final_elm;
			} else if (blit_odd) {
				*fnum = *fnum & fnum_mask_even;
				*next_elm = (*next_elm & y4_mask_even) | (*final_elm & y4_mask_odd);
			} else if(blit_even) {
				*fnum = *fnum & fnum_mask_odd;
				*next_elm = (*next_elm & y4_mask_odd) | (*final_elm & y4_mask_even);
			}
		}
	}
	pr_debug("%s pixel_count=%d frame=%d area->clip=" DRM_RECT_FMT " conflict=" DRM_RECT_FMT " clip_ongoing=" DRM_RECT_FMT, __func__, pixel_count, frame, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&conflict), DRM_RECT_ARG(clip_ongoing));
	area->clip = conflict;
	area->frame_begin = frame_begin;
	rockchip_ebc_drm_rect_extend_rect(clip_ongoing_new_areas, &area_started);
}
EXPORT_SYMBOL(rockchip_ebc_schedule_and_blit);

MODULE_LICENSE("GPL v2");
