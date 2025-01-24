/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#include <asm/neon.h>
#include <linux/module.h>
#include <linux/types.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_rect.h>

#include "rockchip_ebc.h"
#include "rockchip_ebc_blit.h"
#include "rockchip_ebc_blit_neon.h"

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
	u8 dst_x_mask_src_left = reflect_x ? 0x0f : 0xf0;
	u8 dst_x_mask_src_right = reflect_x ? 0xf0 : 0x0f;
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
				rgb0 = (*dbuf & (reflect_x ? 0xf0 : 0x0f)) >>
				       (reflect_x ? 4 : 0);
			}
			if (x == src_clip->x2 - 1) {
				// rgb1 should be filled with the content of the dst pixel we
				rgb1 = (*dbuf & (reflect_x ? 0x0f : 0xf0)) >>
				       (reflect_x ? 0 : 4);
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

			gray = reflect_x ? (rgb0 << 4 | rgb1) :
					   (rgb1 << 4 | rgb0);
			changed = gray ^ *dbuf;
			if (changed) {
				y_changed_max = y;
				if (y_changed_min == -1)
					y_changed_min = y;
				int x_min, x_max;
				x_min = changed & dst_x_mask_src_left ? x :
									x + 1;
				x_max = changed & dst_x_mask_src_right ? x + 1 :
									 x;
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

void rockchip_ebc_blit_pixels(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			      const u8 *src, const struct drm_rect *clip)
{
	bool start_x_is_odd = clip->x1 & 1;
	bool end_x_is_odd = clip->x2 & 1;

	unsigned int x1_bytes = clip->x1 / 2;
	unsigned int x2_bytes = clip->x2 / 2;

	unsigned int pitch = ctx->gray4_pitch;
	unsigned int width;
	const u8 *src_line;
	unsigned int y;
	u8 *dst_line;

	if (start_x_is_odd) {
		dst_line = dst + clip->y1 * pitch + x1_bytes;
		src_line = src + clip->y1 * pitch + x1_bytes;
		for (y = clip->y1; y < clip->y2; ++y) {
			// only set the uppoer bits
			*dst_line = (*dst_line & 0x0f) | (*src_line & 0xf0);
			dst_line += pitch;
			src_line += pitch;
		}
	}

	if (end_x_is_odd) {
		dst_line = dst + clip->y1 * pitch + x2_bytes;
		src_line = src + clip->y1 * pitch + x2_bytes;
		for (y = clip->y1; y < clip->y2; ++y) {
			// only set the lower bits
			*dst_line = (*dst_line & 0xf0) | (*src_line & 0x0f);
			dst_line += pitch;
			src_line += pitch;
		}
	}

	// The first one has already been blitted
	if (start_x_is_odd)
		x1_bytes += 1;

	width = x2_bytes - x1_bytes;
	dst_line = dst + clip->y1 * pitch + x1_bytes;
	src_line = src + clip->y1 * pitch + x1_bytes;

	if (false) {
		kernel_neon_begin();
		rockchip_ebc_blit_pixels_blocks_neon(
			dst_line, src_line, width, pitch, clip->y2 - clip->y1);
		kernel_neon_end();
	} else {
		for (y = clip->y1; y < clip->y2; y++) {
			memcpy(dst_line, src_line, width);
			dst_line += pitch;
			src_line += pitch;
		}
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_pixels);

void rockchip_ebc_blit_direct(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			      u8 phase, const struct drm_epd_lut *lut,
			      const struct drm_rect *clip)
{
	const u32 *phase_lut = (const u32 *)lut->buf + 16 * phase;
	unsigned int dst_pitch = ctx->phase_pitch;
	unsigned int src_pitch = ctx->gray4_pitch;
	unsigned int x, y;
	u8 *dst_line;
	u32 src_line;
	unsigned int x_start = clip->x1 ^ (clip->x1 & 3);

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

void rockchip_ebc_blit_phase(const struct rockchip_ebc_ctx *ctx, u8 *dst,
			     u8 phase, const struct drm_rect *clip,
			     u8 *other_buffer, int last_phase, int frame,
			     int check_blit_phase)
{
	unsigned int pitch = ctx->phase_pitch;
	unsigned int width = clip->x2 - clip->x1;
	unsigned int y;
	u8 *dst_line;

	dst_line = dst + clip->y1 * pitch + clip->x1;
	u8 *dst_line2 = other_buffer + clip->y1 * pitch + clip->x1;

	for (y = clip->y1; y < clip->y2; y++) {
#ifdef ROCKCHIP_EBC_BLIT_PHASE_CHECK
		if (check_blit_phase == 1) {
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
		} else if (check_blit_phase == 2) {
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
EXPORT_SYMBOL(rockchip_ebc_blit_phase);

MODULE_LICENSE("GPL v2");
