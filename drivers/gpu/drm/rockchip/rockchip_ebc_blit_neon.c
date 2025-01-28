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

#include <asm/neon-intrinsics.h>

bool rockchip_ebc_blit_fb_xrgb8888_neon(
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
	}

	return y_changed_min != -1;
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_xrgb8888_neon);

void rockchip_ebc_blit_direct_fnum_neon(const struct rockchip_ebc_ctx *ctx,
					u8 *phase, u8 *frame_num, u8 *next,
					u8 *prev, const struct drm_epd_lut *lut,
					const struct drm_rect *clip)
{
	unsigned int phase_pitch = ctx->phase_pitch;
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;

	// 8 byte alignment for neon
	unsigned int x_start = max(0, min(clip->x1 & ~15, (int) ctx->frame_num_pitch - 32));
	unsigned int x_end = min((int) ctx->frame_num_pitch, (clip->x2 + 31) & ~15);

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
EXPORT_SYMBOL(rockchip_ebc_blit_direct_fnum_neon);

// Increment frame_num by one within clip if < last_phase, otherwise set to 0xff.
// In the latter case blit from next to prev. Clip can be increased for alignment
// and decreased after blitting the last update.
void rockchip_ebc_update_blit_fnum_prev_neon(const struct rockchip_ebc_ctx *ctx,
					     u8 *prev, u8 *next, u8 *fnum,
					     u8 *fnum_prev,
					     struct drm_rect *clip,
					     u8 last_phase)
{
	unsigned int fnum_pitch = ctx->frame_num_pitch;
	unsigned int y4_pitch = ctx->gray4_pitch;
	struct drm_rect clip_shrunk = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	unsigned int x, y;

	// 8 byte alignment for neon
	unsigned int x_start = max(0, min(clip->x1 & ~15, (int) ctx->frame_num_pitch - 32));
	uint8x16_t q8_last_phase = vdupq_n_u8(last_phase);
	uint8x16_t q8_0x01 = vdupq_n_u8(0x01);
	uint8x16_t q8_0xff = vdupq_n_u8(0xff);
	for (y = clip->y1; y < clip->y2; ++y) {
		u8 *prev_line = prev + y * y4_pitch + x_start / 2;
		u8 *next_line = next + y * y4_pitch + x_start / 2;
		u16 *fnum_line = (u16 *) (fnum + y * fnum_pitch + x_start);
		u16 *fnum_line_prev = (u16 *) (fnum_prev + y * fnum_pitch + x_start);
		for (x = x_start; x < clip->x2; x += 32, prev_line += 16, next_line += 16, fnum_line += 16, fnum_line_prev += 16) {
			uint8x16_t q8_prev, q8_next;
			uint16x8_t q16_fnum;
			uint8x16_t q8_fnum, q8_mask, q8_changed, q8_changed2;
			uint16x8_t q16_mask;
			uint8x8_t q8s_mask_finished1, q8s_mask_finished2;

			// 16 bytes u8 frame numbers
			// BBAA DDCC FFDD HHGG
			q16_fnum = vld1q_u16(fnum_line_prev);

			// BB AA DD CC FF DD HH GG
			q8_fnum = vreinterpretq_u8_u16(q16_fnum);
			// 0xff or 0x00 for each u8. These require blitting of next->prev and setting the frame number to 0xff.
			q8_mask = vceqq_u8(q8_fnum, q8_last_phase);
			// Increase frame number by one, saturating add
			q8_fnum = vqaddq_u8(q8_fnum, q8_0x01);
			// New frame number, either set to 0xff or increased by 1
			// q8_fnum = vbslq_u8(q8_mask, q8_mask, q8_fnum);
			q8_fnum = vorrq_u8(q8_fnum, q8_mask);
			q8_changed = vcltq_u8(q8_fnum, q8_0xff);

			// Write to memory
			q16_fnum = vreinterpretq_u16_u8(q8_fnum);
			vst1q_u16(fnum_line, q16_fnum);

			// Reinterpret mask of finished pixels as u16 before shifting
			// BBAA DDCC FFEE HHGG
			q16_mask = vreinterpretq_u16_u8(q8_mask);
			// Shift and narrow. Compatible with half of next/prev y4
			// BA DC FE HG
			q8s_mask_finished1 = vshrn_n_u16(q16_mask, 4);

			// Same for the next 16 frame numbers
			q16_fnum = vld1q_u16(fnum_line_prev + 8);
			q8_fnum = vreinterpretq_u8_u16(q16_fnum);
			q8_mask = vceqq_u8(q8_fnum, q8_last_phase);
			q8_fnum = vqaddq_u8(q8_fnum, q8_0x01);
			// q8_fnum = vbslq_u8(q8_mask, q8_mask, q8_fnum);
			q8_fnum = vorrq_u8(q8_fnum, q8_mask);
			q8_changed2 = vcltq_u8(q8_fnum, q8_0xff);
			q16_fnum = vreinterpretq_u16_u8(q8_fnum);
			vst1q_u16(fnum_line + 8, q16_fnum);
			q16_mask = vreinterpretq_u16_u8(q8_mask);
			q8s_mask_finished2 = vshrn_n_u16(q16_mask, 4);

			// Combine masks
			q8_mask = vcombine_u8(q8s_mask_finished1, q8s_mask_finished2);

			if (vmaxvq_u8(q8_mask)) {
				// Copy required bits from either next or prev
				// 16 bytes y4 corresponding to 32 pixels
				// BA DC FE HG
				q8_prev = vld1q_u8(prev_line);
				q8_next = vld1q_u8(next_line);
				q8_prev = vbslq_u8(q8_mask, q8_next, q8_prev);
				vst1q_u8(prev_line, q8_prev);
			}

			if (vmaxvq_u8(vorrq_u8(q8_changed, q8_changed2))) {
				rockchip_ebc_drm_rect_extend(&clip_shrunk, x, y);
			}
		}
	}
	*clip = clip_shrunk;
}
EXPORT_SYMBOL(rockchip_ebc_update_blit_fnum_prev_neon);

// Blit 0 to frame_num and final to next within clip where final differs from next and frame_num is 0xff. Shrink clip to smallest conflict area.
void rockchip_ebc_schedule_and_blit_neon(
	const struct rockchip_ebc_ctx *ctx, u8 *frame_num, u8 *next, u8 *final,
	const struct drm_rect *clip_ongoing,
	struct drm_rect *clip_ongoing_new_areas, struct rockchip_ebc_area *area,
	u32 frame, u8 last_phase, struct rockchip_ebc_area *next_area)
{
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;
	// 8 Byte alignment for neon
	unsigned int x_start = max(0, min(area->clip.x1 & ~31, (int) ctx->frame_num_pitch - 32));
	unsigned int pixel_count = 0;
	u32 frame_begin = frame + last_phase + 1;
	struct drm_rect conflict = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	struct drm_rect area_started = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	uint8x16_t q8_0xff = vdupq_n_u8(0xff);
	uint8x16_t q8_0x11 = vdupq_n_u8(0x11);
	uint8x16_t q8_0x00 = vdupq_n_u8(0x00);
	for (y = area->clip.y1; y < area->clip.y2; ++y) {
		u8 *next_line = next + y * gray4_pitch + x_start / 2;
		u8 *final_line = final + y * gray4_pitch + x_start / 2;
		u16 *fnum_line = (u16 *) (frame_num + y * frame_num_pitch + x_start);
		for (x = x_start; x < area->clip.x2; x += 32, next_line += 16, final_line += 16, fnum_line += 16) {
			u8 pixel_count_inc = 0;
			u8 maximum_conflicting_fnum;
			uint8x16_t q8_next, q8_final, q8_fnum1, q8_fnum2;
			uint16x8_t q16_fnum1, q16_fnum2;
			uint8x16_t q8_mask1_complete, q8_mask2_complete, q8_y4_mask_difference;
			uint8x16_t q8_y4_mask_complete, q8_y4_mask_blitted;
			uint8x8_t q8s_y4_mask1_complete, q8s_y4_mask2_complete;
			uint8x16_t q8_mask1_difference, q8_mask2_difference;

			// 16 bytes y4 corresponding to 32 pixels
			// BA DC FE HG
			q8_next = vld1q_u8(next_line);
			q8_final = vld1q_u8(final_line);

			// Check which pixels have changed. Any value between 0x0 and 0xf for each Y4
			// BA DC FE HG
			q8_y4_mask_difference = veorq_u8(q8_next, q8_final);
			if (!vmaxvq_u8(q8_y4_mask_difference)) {
				// No pixels have changed, skip block
				continue;
			}
			// Convert holey Y4 mask to complete mask by widening and comparison to zero. 0xff if pixels differ, o.w. 0x00
			// BB AA DD CC
			q8_mask1_difference = vcgtq_u8(vreinterpretq_u8_u16(vshll_n_u8(vget_low_u8(q8_y4_mask_difference), 4)), q8_0x00);
			q8_mask2_difference = vcgtq_u8(vreinterpretq_u8_u16(vshll_high_n_u8(q8_y4_mask_difference, 4)), q8_0x00);
			// BA DC
			q8_y4_mask_difference = vcombine_u8(
				vshrn_n_u16(vreinterpretq_u16_u8(q8_mask1_difference), 4),
				vshrn_n_u16(vreinterpretq_u16_u8(q8_mask2_difference), 4));

			// 16 bytes of u8 frame numbers
			// BBAA DDCC FFEE HHGG
			q16_fnum1 = vld1q_u16(fnum_line);
			// BB AA DD CC FF DD HH GG
			q8_fnum1 = vreinterpretq_u8_u16(q16_fnum1);
			// 0xff (not running) or 0x00 (running) for each u8
			q8_mask1_complete = vceqq_u8(q8_fnum1, q8_0xff);
			// BA DC FE HG
			q8s_y4_mask1_complete = vshrn_n_u16(vreinterpretq_u16_u8(q8_mask1_complete), 4);

			// Same for the next 16 frame numbers
			q16_fnum2 = vld1q_u16(fnum_line + 8);
			q8_fnum2 = vreinterpretq_u8_u16(q16_fnum2);
			q8_mask2_complete = vceqq_u8(q8_fnum2, q8_0xff);
			q8s_y4_mask2_complete = vshrn_n_u16(vreinterpretq_u16_u8(q8_mask2_complete), 4);

			// Concatenate the two masks
			q8_y4_mask_complete = vcombine_u8(q8s_y4_mask1_complete, q8s_y4_mask2_complete);

			// Y4 mask of elements that need changing and do not conflict
			q8_y4_mask_blitted = vandq_u8(q8_y4_mask_difference, q8_y4_mask_complete);

			// New next: select changing bytes from final, otherwise keep next
			q8_next = vbslq_u8(q8_y4_mask_blitted, q8_final, q8_next);
			vst1q_u8(next_line, q8_next);

			// Next, set the frame number of pixels that were blitted to 0. 0x00 if blitted, o.w. 0xff
			uint8x16_t q8_mask1_not_blitted = vmvnq_u8(vandq_u8(q8_mask1_complete, q8_mask1_difference));
			uint8x16_t q8_mask2_not_blitted = vmvnq_u8(vandq_u8(q8_mask2_complete, q8_mask2_difference));
			// And with fnum_data to zero the frame numbers of pixels that start now
			q16_fnum1 = vandq_u16(q16_fnum1, vreinterpretq_u16_u8(q8_mask1_not_blitted));
			q16_fnum2 = vandq_u16(q16_fnum2, vreinterpretq_u16_u8(q8_mask2_not_blitted));
			vst1q_u16(fnum_line, q16_fnum1);
			vst1q_u16(fnum_line + 8, q16_fnum2);

			// Update tracked regions and reschedule remaining area

			q8_mask1_not_blitted = vandq_u8(q8_mask1_not_blitted, q8_mask1_difference);
			q8_mask2_not_blitted = vandq_u8(q8_mask2_not_blitted, q8_mask2_difference);
			// At least one pixel from this block that differs was not started
			if (vmaxvq_u8(vorrq_u8(q8_mask1_not_blitted, q8_mask2_not_blitted))) {
				q8_fnum1 = vandq_u8(q8_fnum1, q8_mask1_not_blitted);
				q8_fnum2 = vandq_u8(q8_fnum2, q8_mask2_not_blitted);
				maximum_conflicting_fnum = max(vmaxvq_u8(q8_fnum1), vmaxvq_u8(q8_fnum2));
				// Reschedule area as soon as at least one now conflicting pixel can be started
				frame_begin = min(frame_begin, frame + 1 + last_phase - maximum_conflicting_fnum);
				rockchip_ebc_drm_rect_extend(&conflict, x, y);
			}

			pixel_count_inc = vaddvq_u8(vcntq_u8(vandq_u8(q8_y4_mask_blitted, q8_0x11)));
			// At least one pixel from this block was started
			if (pixel_count_inc) {
				rockchip_ebc_drm_rect_extend(&area_started, x, y);
			}
			pixel_count += pixel_count_inc;
		}
	}
	area->clip = conflict;
	area->frame_begin = frame_begin;
	rockchip_ebc_drm_rect_extend_rect(clip_ongoing_new_areas, &area_started);
}
EXPORT_SYMBOL(rockchip_ebc_schedule_and_blit_neon);

MODULE_LICENSE("GPL v2");
