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

void rockchip_ebc_blit_fb_xrgb8888_y4_neon(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip)
{
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int rgba_pitch = fb->pitches[0];

	// 8 byte alignment for neon
	unsigned int src_start_x = max(0, min(src_clip->x1 & ~15, (int) ctx->frame_num_pitch - 32));
	unsigned int src_end_x = min((src_clip->x2 + 15) & ~15, (int) ctx->frame_num_pitch);
	unsigned int dst_start_x = max(0, min((dst_clip->x2 - 1) & ~15, (int) ctx->frame_num_pitch - 16));
	unsigned int x, y;

	// Force horizontal reflection for simplicity
	u8 *dst = ctx->final_atomic_update + dst_clip->y1 * gray4_pitch + dst_start_x / 2;
	const u8 *src = vaddr + src_clip->y1 * rgba_pitch + src_start_x * fb->format->cpp[0];

	uint8x8_t q8_yuv_r = vdup_n_u8(76);
	uint8x8_t q8_yuv_g = vdup_n_u8(150);
	uint8x8_t q8_yuv_b = vdup_n_u8(29);
	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u8 *sbuf = src;
		u8 *dbuf = dst;

		for (x = src_start_x; x < src_end_x; x += 16, sbuf += 64, dbuf -= 8) {
			uint8x8x4_t q8sx4_rgba;
			uint8x8_t q8s_gray1, q8s_gray2;
			uint8x16_t q8_gray;
			uint16x8_t q16_gray;

			// RGB -> Y8 using rounded YUV
			// Load 8 RGBA values
			q8sx4_rgba = vld4_u8(sbuf);
			q16_gray = vmull_u8(q8_yuv_r, q8sx4_rgba.val[0]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g, q8sx4_rgba.val[1]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, q8sx4_rgba.val[2]);
			// Aa Bb Cc Dd lower-case bits still need to be discarded
			q8s_gray1 = vshrn_n_u16(q16_gray, 8);

			// Same for the next 8 RGBA pixels
			q8sx4_rgba = vld4_u8(sbuf + 32);
			q16_gray = vmull_u8(q8_yuv_r, q8sx4_rgba.val[0]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g, q8sx4_rgba.val[1]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, q8sx4_rgba.val[2]);
			q8s_gray2 = vshrn_n_u16(q16_gray, 8);

			// Combine into single vector
			q8_gray = vcombine_u8(q8s_gray1, q8s_gray2);

			// Either here or after Y8 -> Y4: dithering or thresholding

			// AA BB CC DD duplicate higher bits by right shifting and inserting immediately
			q8_gray = vsriq_n_u8(q8_gray, q8_gray, 4);

			// BB AA DD CC
			q8_gray = vrev16q_u8(q8_gray);
			// BBAA DDCC
			q16_gray = vreinterpretq_u16_u8(q8_gray);
			// BA DC
			q8s_gray1 = vshrn_n_u16(q16_gray, 4);

			q8s_gray1 =  vrev64_u8(q8s_gray1);
			vst1_u8(dbuf, q8s_gray1);
		}

		src += rgba_pitch;
		dst += gray4_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_xrgb8888_y4_neon);

void rockchip_ebc_blit_fb_xrgb8888_y4_thresholded4_neon(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, u8 invert, u8 fourtone_low_threshold,
	u8 fourtone_mid_threshold, u8 fourtone_hi_threshold, bool dither)
{
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int rgba_pitch = fb->pitches[0];

	// 8 byte alignment for neon
	unsigned int src_start_x = max(0, min(src_clip->x1 & ~15, (int) ctx->frame_num_pitch - 32));
	unsigned int src_end_x = min((src_clip->x2 + 15) & ~15, (int) ctx->frame_num_pitch);
	unsigned int dst_start_x = max(0, min((dst_clip->x2 - 1) & ~15, (int) ctx->frame_num_pitch - 16));
	unsigned int x, y;

	// Force horizontal reflection for simplicity
	u8 *dst = ctx->final_atomic_update + dst_clip->y1 * gray4_pitch + dst_start_x / 2;
	const u8 *src = vaddr + src_clip->y1 * rgba_pitch + src_start_x * fb->format->cpp[0];

	const u32 y4_dither_pattern[] = {
		0 << 4 | 8 << 12 | 2 << 20 | 10 << 28,
		12 << 4 | 4 << 12 | 14 << 20 | 6 << 28,
		3 << 4 | 11 << 12 | 1 << 20 | 9 << 28,
		15 << 4 | 7 << 12 | 13 << 20 | 5 << 28,
	};
	uint8x16_t q8_threshold1 = vdupq_n_u8(fourtone_low_threshold << 4);
	uint8x16_t q8_threshold2 = vdupq_n_u8(fourtone_mid_threshold << 4);
	uint8x16_t q8_threshold3 = vdupq_n_u8(fourtone_hi_threshold << 4);
	// 0, 5, 10, 15: valid Y4 values for DU4
	uint8x16_t q8_lut = vreinterpretq_u8_u32(vdupq_n_u32(5 << 8 | 5 << 12 | 10 << 16 | 10 << 20 | 15 << 24 | 15 << 28));
	if (invert)
		q8_lut = vrev32q_u8(q8_lut);

	uint8x8_t q8_yuv_r = vdup_n_u8(76);
	uint8x8_t q8_yuv_g = vdup_n_u8(150);
	uint8x8_t q8_yuv_b = vdup_n_u8(29);
	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u8 *sbuf = src;
		u8 *dbuf = dst;
		uint8x16_t q8_dither_pattern;
		q8_dither_pattern = dither ? vreinterpretq_u8_u32(vdupq_n_u32(y4_dither_pattern[y & 3])) : vdupq_n_u8(0);

		for (x = src_start_x; x < src_end_x; x += 16, sbuf += 64, dbuf -= 8) {
			uint8x8x4_t q8sx4_rgba;
			uint8x8_t q8s_gray1, q8s_gray2;
			uint8x16_t q8_gray, q8_threshold_count;
			uint16x8_t q16_gray;

			// RGB -> Y8 using rounded YUV
			// Load 8 RGBA values
			q8sx4_rgba = vld4_u8(sbuf);
			q16_gray = vmull_u8(q8_yuv_r, q8sx4_rgba.val[0]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g, q8sx4_rgba.val[1]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, q8sx4_rgba.val[2]);
			// Aa Bb Cc Dd lower-case bits still need to be discarded
			q8s_gray1 = vshrn_n_u16(q16_gray, 8);

			// Same for the next 8 RGBA pixels
			q8sx4_rgba = vld4_u8(sbuf + 32);
			q16_gray = vmull_u8(q8_yuv_r, q8sx4_rgba.val[0]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g, q8sx4_rgba.val[1]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, q8sx4_rgba.val[2]);
			q8s_gray2 = vshrn_n_u16(q16_gray, 8);

			// Combine into single vector
			q8_gray = vcombine_u8(q8s_gray1, q8s_gray2);

			// Either here or after Y8 -> Y4: dithering or thresholding
			q8_gray = vqaddq_u8(q8_gray, q8_dither_pattern);
			q8_threshold_count = vshrq_n_u8(vcgeq_u8(q8_gray, q8_threshold1), 7);
			q8_threshold_count = vsraq_n_u8(q8_threshold_count, vcgeq_u8(q8_gray, q8_threshold2), 7);
			q8_threshold_count = vsraq_n_u8(q8_threshold_count, vcgeq_u8(q8_gray, q8_threshold3), 7);
			q8_gray = vqtbl1q_u8(q8_lut, q8_threshold_count);

			// AA BB CC DD duplicate higher bits by right shifting and inserting immediately
			// q8_gray = vsriq_n_u8(q8_gray, q8_gray, 4);

			// BB AA DD CC
			q8_gray = vrev16q_u8(q8_gray);
			// BBAA DDCC
			q16_gray = vreinterpretq_u16_u8(q8_gray);
			// BA DC
			q8s_gray1 = vshrn_n_u16(q16_gray, 4);

			q8s_gray1 =  vrev64_u8(q8s_gray1);
			vst1_u8(dbuf, q8s_gray1);
		}

		src += rgba_pitch;
		dst += gray4_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_xrgb8888_y4_thresholded4_neon);

void rockchip_ebc_blit_fb_xrgb8888_y4_dithered2_neon(
	const struct rockchip_ebc_ctx *ctx, struct drm_rect *dst_clip,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip, u8 invert)
{
	// TODO:
	// implement invert
	// implement bw_thresholding
	// implement configurable threshold

	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int rgba_pitch = fb->pitches[0];

	// 8 byte alignment for neon
	unsigned int src_start_x = max(0, min(src_clip->x1 & ~15, (int) ctx->frame_num_pitch - 32));
	unsigned int src_end_x = min((src_clip->x2 + 15) & ~15, (int) ctx->frame_num_pitch);
	unsigned int dst_start_x = max(0, min((dst_clip->x2 - 1) & ~15, (int) ctx->frame_num_pitch - 16));
	unsigned int x, y;

	// Force horizontal reflection for simplicity
	u8 *dst = ctx->final_atomic_update + dst_clip->y1 * gray4_pitch + dst_start_x / 2;
	const u8 *src = vaddr + src_clip->y1 * rgba_pitch + src_start_x * fb->format->cpp[0];

	// Changed first element
	const u32 y4_dither_pattern[] = {
		7 << 4 | 8 << 12 | 2 << 20 | 10 << 28,
		12 << 4 | 4 << 12 | 14 << 20 | 6 << 28,
		3 << 4 | 11 << 12 | 1 << 20 | 9 << 28,
		15 << 4 | 7 << 12 | 13 << 20 | 5 << 28,
	};

	uint8x8_t q8_yuv_r = vdup_n_u8(76);
	uint8x8_t q8_yuv_g = vdup_n_u8(150);
	uint8x8_t q8_yuv_b = vdup_n_u8(29);
	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u8 *sbuf = src;
		u8 *dbuf = dst;

		uint8x16_t q8_dither_pattern = vreinterpretq_u8_u32(vdupq_n_u32(y4_dither_pattern[y & 3]));
		for (x = src_start_x; x < src_end_x; x += 16, sbuf += 64, dbuf -= 8) {
			uint8x8x4_t q8sx4_rgba;
			uint8x8_t q8s_gray1, q8s_gray2;
			uint8x16_t q8_gray;
			uint16x8_t q16_gray;

			// RGB -> Y8 using rounded YUV
			// Load 8 RGBA values
			q8sx4_rgba = vld4_u8(sbuf);
			q16_gray = vmull_u8(q8_yuv_r, q8sx4_rgba.val[0]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g, q8sx4_rgba.val[1]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, q8sx4_rgba.val[2]);
			// Aa Bb Cc Dd lower-case bits still need to be discarded
			q8s_gray1 = vshrn_n_u16(q16_gray, 8);

			// Same for the next 8 RGBA pixels
			q8sx4_rgba = vld4_u8(sbuf + 32);
			q16_gray = vmull_u8(q8_yuv_r, q8sx4_rgba.val[0]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g, q8sx4_rgba.val[1]);
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, q8sx4_rgba.val[2]);
			q8s_gray2 = vshrn_n_u16(q16_gray, 8);

			// Combine into single vector
			q8_gray = vcombine_u8(q8s_gray1, q8s_gray2);

			// Either here or after Y8 -> Y4: dithering or thresholding
			q8_gray = vcgeq_u8(q8_gray, q8_dither_pattern);

			// AA BB CC DD duplicate higher bits by right shifting and inserting immediately
			// q8_gray = vsriq_n_u8(q8_gray, q8_gray, 4);

			// BB AA DD CC
			q8_gray = vrev16q_u8(q8_gray);
			// BBAA DDCC
			q16_gray = vreinterpretq_u16_u8(q8_gray);
			// BA DC
			q8s_gray1 = vshrn_n_u16(q16_gray, 4);

			q8s_gray1 =  vrev64_u8(q8s_gray1);
			vst1_u8(dbuf, q8s_gray1);
		}

		src += rgba_pitch;
		dst += gray4_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_xrgb8888_y4_dithered2_neon);

void rockchip_ebc_blit_direct_fnum_a2_neon(const struct rockchip_ebc_ctx *ctx,
					u8 *phase, u8 *frame_num, u8 *next,
					u8 *prev, const struct drm_epd_lut *lut,
					u8 a2_shorten_waveform,
					const struct drm_rect *clip)
{
	unsigned int phase_pitch = ctx->phase_pitch;
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;

	// 8 byte alignment for neon
	unsigned int x_start = max(0, min(clip->x1 & ~31, (int) ctx->frame_num_pitch - 32));
	unsigned int x_end = min((int) ctx->frame_num_pitch, ((clip->x2 + 31) & ~31));

	u8 wf_15to0 = ((const u32 *)lut->buf)[0xf] & 0x3;
	u8 wf_0to15 = (((const u32 *)lut->buf)[0] >> 30) & 0x3;
	// Duplicate for masking
	wf_15to0 |= (wf_15to0 << 2) | (wf_15to0 << 4) | wf_15to0 << 6;
	wf_0to15 |= (wf_0to15 << 2) | (wf_0to15 << 4) | wf_0to15 << 6;
	u8 *phase_line = phase + clip->y1 * phase_pitch + x_start / 4;
	u8 *next_line = next + clip->y1 * gray4_pitch + x_start / 2;
	u8 *prev_line = prev + clip->y1 * gray4_pitch + x_start / 2;
	u8 *fnum_line = frame_num + clip->y1 * frame_num_pitch + x_start;

	// pr_debug("%s wf15to0=%d wf_0to15=%d x_start=%d x_end=%d " DRM_RECT_FMT, __func__, wf_15to0, wf_0to15, x_start, x_end, DRM_RECT_ARG(clip));
	uint8x16_t q8_last_phase = vdupq_n_u8(lut->num_phases - 1 - min(lut->num_phases - 1, a2_shorten_waveform));
	uint8x16_t q8_0x04 = vdupq_n_u8(0x04);
	uint8x16_t q8_wf_15to0 = vdupq_n_u8(wf_15to0);
	uint8x16_t q8_wf_0to15 = vdupq_n_u8(wf_0to15);
	uint8x8_t q8s_0x0f = vdup_n_u8(0x0f);
	uint8x8_t q8s_0xf0 = vdup_n_u8(0xf0);
	/* Each byte in the phase buffer [DCBA] maps to a horizontal block of four pixels in Y4 format [BA] [DC] */
	for (y = clip->y1; y < clip->y2; ++y, next_line += gray4_pitch, prev_line += gray4_pitch, phase_line += phase_pitch, fnum_line += frame_num_pitch) {
		u8 *phase_elm = phase_line;
		u16 *fnum_elm = (u16 *) fnum_line;
		u8 *next_elm = next_line;
		u8 *prev_elm = prev_line;
		for (x = x_start; x < x_end; x += 32, phase_elm += 8, next_elm += 16, prev_elm += 16, fnum_elm += 16) {
			uint16x8x2_t q16x2_fnum = vld2q_u16(fnum_elm);
			// 0xff if these need to be blitted to wf_15to0 or wf_0to15, otherwise 0x00
			// BB AA FF EE
			uint8x16_t q8_fnum_mask0 = vcltq_u8(vreinterpretq_u8_u16(q16x2_fnum.val[0]), q8_last_phase);
			// DD CC HH GG
			uint8x16_t q8_fnum_mask1 = vcltq_u8(vreinterpretq_u8_u16(q16x2_fnum.val[1]), q8_last_phase);

			uint8x8x2_t q8sx2_next = vld2_u8(next_elm); // val[0]: BA FE JI NM, val[1]: DC HG LK PO
			uint8x8x2_t q8sx2_prev = vld2_u8(prev_elm);

			// BB AA FF EE
			uint8x16_t q8_cnt_next0 = vcntq_u8(vreinterpretq_u8_u16(vshll_n_u8(q8sx2_next.val[0], 4)));
			// DD CC HH GG
			uint8x16_t q8_cnt_next1 = vcntq_u8(vreinterpretq_u8_u16(vshll_n_u8(q8sx2_next.val[1], 4)));
			uint8x16_t q8_cnt_prev0 = vcntq_u8(vreinterpretq_u8_u16(vshll_n_u8(q8sx2_prev.val[0], 4)));
			uint8x16_t q8_cnt_prev1 = vcntq_u8(vreinterpretq_u8_u16(vshll_n_u8(q8sx2_prev.val[1], 4)));

			// 0xff if equal to zero, otherwise 0x00
			// BB AA FF EE JJ II NN MM
			uint8x16_t q8_mask_next0_0 = vceqzq_u8(q8_cnt_next0);
			// DD CC HH GG LL KK PP OO
			uint8x16_t q8_mask_next1_0 = vceqzq_u8(q8_cnt_next1);
			uint8x16_t q8_mask_prev0_0 = vceqzq_u8(q8_cnt_prev0);
			uint8x16_t q8_mask_prev1_0 = vceqzq_u8(q8_cnt_prev1);

			// 0xff if equal to 4 (0b00001111 has four bits set), otherwise 0x00
			uint8x16_t q8_mask_next0_f = vceqq_u8(q8_cnt_next0, q8_0x04);
			uint8x16_t q8_mask_next1_f = vceqq_u8(q8_cnt_next1, q8_0x04);
			uint8x16_t q8_mask_prev0_f = vceqq_u8(q8_cnt_prev0, q8_0x04);
			uint8x16_t q8_mask_prev1_f = vceqq_u8(q8_cnt_prev1, q8_0x04);

			// bbbbbbbb aaaaaaaa ffffffff eeeeeeee
			uint8x16_t q8_phase0 = vorrq_u8(
				vandq_u8(q8_wf_15to0, vandq_u8(q8_fnum_mask0, vandq_u8(q8_mask_next0_0, q8_mask_prev0_f))),
				vandq_u8(q8_wf_0to15, vandq_u8(q8_fnum_mask0, vandq_u8(q8_mask_next0_f, q8_mask_prev0_0))));
			// dddddddd cccccccc hhhhhhhh gggggggg
			uint8x16_t q8_phase1 = vorrq_u8(
				vandq_u8(q8_wf_15to0, vandq_u8(q8_fnum_mask1, vandq_u8(q8_mask_next1_0, q8_mask_prev1_f))),
				vandq_u8(q8_wf_0to15, vandq_u8(q8_fnum_mask1, vandq_u8(q8_mask_next1_f, q8_mask_prev1_0))));
			// 0000bbaa 0000ffee
			uint8x8_t q8s_phase0 = vand_u8(vshrn_n_u16(vreinterpretq_u16_u8(q8_phase0), 6), q8s_0x0f);
			// ddcc0000 hhgg0000
			uint8x8_t q8s_phase1 = vand_u8(vshrn_n_u16(vreinterpretq_u16_u8(q8_phase1), 2), q8s_0xf0);

			// ddccbbaa hhggffee
			vst1_u8(phase_elm, vorr_u8(q8s_phase0, q8s_phase1));
		}
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_direct_fnum_a2_neon);

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
	if (drm_rect_width(&clip_shrunk) > 0)
		rockchip_ebc_drm_rect_extend(&clip_shrunk, ((clip_shrunk.x2 - 1) & ~31) + 31, clip_shrunk.y2 - 1);
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
	if (drm_rect_width(&conflict) > 0)
		rockchip_ebc_drm_rect_extend(&conflict, ((conflict.x2 - 1) & ~31) + 31, conflict.y2 - 1);
	if (drm_rect_width(&area_started) > 0)
		rockchip_ebc_drm_rect_extend(&area_started, ((area_started.x2 - 1) & ~31) + 31, area_started.y2 - 1);
	area->clip = conflict;
	area->frame_begin = frame_begin;
	rockchip_ebc_drm_rect_extend_rect(clip_ongoing_new_areas, &area_started);
}
EXPORT_SYMBOL(rockchip_ebc_schedule_and_blit_neon);

// Blit final to next within clip where final differs from next. If frame_num = 0xff, blit 0 to frame_num, o.w. last_phase - frame_num + early_cancellation_addition and set prev to ~next
void rockchip_ebc_schedule_cancel_blit_a2_neon(
	const struct rockchip_ebc_ctx *ctx, u8 *frame_num, u8 *prev, u8 *next,
	u8 *final, struct drm_rect *clip_ongoing_new_areas,
	struct rockchip_ebc_area *area, u8 last_phase,
	u8 early_cancellation_addition)
{
	unsigned int gray4_pitch = ctx->gray4_pitch;
	unsigned int frame_num_pitch = ctx->frame_num_pitch;
	unsigned int x, y;
	// 8 Byte alignment for neon
	unsigned int x_start = max(0, min(area->clip.x1 & ~31, (int) ctx->frame_num_pitch - 32));
	unsigned int pixel_count = 0;
	struct drm_rect area_started = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	uint8x16_t q8_0xff = vdupq_n_u8(0xff);
	uint8x16_t q8_0x11 = vdupq_n_u8(0x11);
	uint8x16_t q8_0x00 = vdupq_n_u8(0x00);
	uint8x16_t q8_last_phase = vdupq_n_u8(last_phase);
	uint8x16_t q8_early_cancellation_addition = vdupq_n_u8(early_cancellation_addition);
	for (y = area->clip.y1; y < area->clip.y2; ++y) {
		u8 *prev_line = prev + y * gray4_pitch + x_start / 2;
		u8 *next_line = next + y * gray4_pitch + x_start / 2;
		u8 *final_line = final + y * gray4_pitch + x_start / 2;
		u16 *fnum_line = (u16 *) (frame_num + y * frame_num_pitch + x_start);
		for (x = x_start; x < area->clip.x2; x += 32, prev_line += 16, next_line += 16, final_line += 16, fnum_line += 16) {
			u8 pixel_count_inc = 0;
			uint8x16_t q8_next, q8_final, q8_prev, q8_fnum1, q8_fnum2;
			uint16x8_t q16_fnum1, q16_fnum2;
			uint8x16_t q8_mask1_complete, q8_mask2_complete, q8_y4_mask_difference, q8_y4_mask_cancelled;
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
			q8_y4_mask_cancelled = vandq_u8(q8_y4_mask_difference, vmvnq_u8(q8_y4_mask_complete));

			// New prev: select conflicting pixels from next, otherwise keep prev
			q8_prev = vld1q_u8(prev_line);
			q8_prev = vbslq_u8(q8_y4_mask_cancelled, q8_next, q8_prev);
			vst1q_u8(prev_line, q8_prev);
			vst1q_u8(next_line, q8_final);

			// Next, set the frame number of pixels that were blitted without conflict to 0. 0x00 if blitted, o.w. 0xff
			uint8x16_t q8_mask1_not_blitted = vmvnq_u8(vandq_u8(q8_mask1_complete, q8_mask1_difference));
			uint8x16_t q8_mask2_not_blitted = vmvnq_u8(vandq_u8(q8_mask2_complete, q8_mask2_difference));

			uint8x16_t q8_mask1_cancelled = vandq_u8(vmvnq_u8(q8_mask1_complete), q8_mask1_difference);
			uint8x16_t q8_mask2_cancelled = vandq_u8(vmvnq_u8(q8_mask2_complete), q8_mask2_difference);

			// Compute frame number last_phase - prev_fnum - early_cancellation_addition
			uint8x16_t q8_fnum1_cancelled = vqsubq_u8(q8_last_phase, vreinterpretq_u8_u16(q16_fnum1));
			uint8x16_t q8_fnum2_cancelled = vqsubq_u8(q8_last_phase, vreinterpretq_u8_u16(q16_fnum2));
			q8_fnum1_cancelled = vqsubq_u8(q8_fnum1_cancelled, q8_early_cancellation_addition);
			q8_fnum2_cancelled = vqsubq_u8(q8_fnum2_cancelled, q8_early_cancellation_addition);
			q8_fnum1 = vbslq_u8(q8_mask1_cancelled, q8_fnum1_cancelled, q8_fnum1);
			q8_fnum2 = vbslq_u8(q8_mask2_cancelled, q8_fnum2_cancelled, q8_fnum2);

			// And with fnum_data to zero the frame numbers of pixels that start now
			q16_fnum1 = vandq_u16(vreinterpretq_u16_u8(q8_fnum1), vreinterpretq_u16_u8(q8_mask1_not_blitted));
			q16_fnum2 = vandq_u16(vreinterpretq_u16_u8(q8_fnum2), vreinterpretq_u16_u8(q8_mask2_not_blitted));
			vst1q_u16(fnum_line, q16_fnum1);
			vst1q_u16(fnum_line + 8, q16_fnum2);

			// Update tracked regions
			pixel_count_inc = vaddvq_u8(vcntq_u8(vandq_u8(vorrq_u8(q8_y4_mask_blitted, q8_y4_mask_cancelled), q8_0x11)));
			// At least one pixel from this block was started
			if (pixel_count_inc) {
				rockchip_ebc_drm_rect_extend(&area_started, x, y);
			}
			pixel_count += pixel_count_inc;
		}
	}
	if (drm_rect_width(&area_started) > 0)
		rockchip_ebc_drm_rect_extend(&area_started, ((area_started.x2 - 1) & ~31) + 31, area_started.y2 - 1);
	// Make sure the area gets deleted
	area->clip = (struct drm_rect) { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
	rockchip_ebc_drm_rect_extend_rect(clip_ongoing_new_areas, &area_started);
}
EXPORT_SYMBOL(rockchip_ebc_schedule_cancel_blit_a2_neon);

MODULE_LICENSE("GPL v2");
