/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 hrdl <git@hrdl.eu>
 */

#include <linux/module.h>
#include <linux/prefetch.h>
#include <linux/types.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_rect.h>
#include "rockchip_ebc.h"

#include <asm/neon-intrinsics.h>

#define NEON_PR_INFO(_q8_name)                                          \
	{                                                               \
		vst1q_u8(u8_tmp, (_q8_name));                           \
		pr_info("%s %s \t=%16ph", __func__, #_q8_name, u8_tmp); \
	}

#define NEON_PR_INFOS(_q8s_name)                                        \
	{                                                               \
		vst1_u8(u8_tmp, (_q8s_name));                           \
		pr_info("%s %s \t=%8ph", __func__, #_q8s_name, u8_tmp); \
	}

// #define ROCKCHIP_EBC_DEBUG_NEON
// #define ROCKCHIP_EBC_DEBUG_NEON_FAST

void rockchip_ebc_schedule_advance_fast_neon(
	const struct rockchip_ebc *ebc, const u8 *prelim_target, u8 *hints,
	u8 *phase_buffer, struct drm_rect *clip_ongoing,
	struct drm_rect *clip_ongoing_or_waiting,
	u8 early_cancellation_addition, u8 force_wf, u8 force_hint,
	u8 force_hint_mask, bool allow_schedule)
{
	unsigned int pixel_pitch = ebc->pixel_pitch;
	unsigned int prefetch_pitch = (pixel_pitch >> 6) << 6;
	unsigned int phases_prefetch_pitch = (pixel_pitch >> 8) << 6;
	// unsigned int packed_pitch = 3 * pixel_pitch;
	unsigned int phase_pitch = ebc->phase_pitch;
	// struct drm_rect clip_ongoing_or_waiting_new = DRM_RECT_EMPTY_EXTANDABLE;

#ifdef ROCKCHIP_EBC_DEBUG_NEON_FAST
	aligned_u64 _tmp[2];
	u8 *u8_tmp = (u8 *)_tmp;
#endif

	// 16 byte block size for NEON
	unsigned int x_start = max(0, min(clip_ongoing_or_waiting->x1 & ~15,
					  (int)ebc->pixel_pitch - 16));

	uint8x16_t q8_0x00 = vdupq_n_u8(0x00);
	uint8x16_t q8_0x01 = vdupq_n_u8(0x01);
	uint8x16_t q8_0x0f = vdupq_n_u8(0x0f);
	uint8x16_t q8_0x1f = vdupq_n_u8(0x1f);
	uint8x16_t q8_inner_0_15 = vdupq_n_u8(ebc->inner_0_15);
	uint8x16_t q8_inner_15_0 = vdupq_n_u8(ebc->inner_15_0);
	uint8x16_t q8_early_cancellation_addition =
		vdupq_n_u8(early_cancellation_addition);
	uint8x16_t q8_allow_schedule =
		vcgtq_u8(vdupq_n_u8(allow_schedule), q8_0x00);

	uint32x2_t q32s_0x10 = vdup_n_u32(0x10);
	uint16x4_t q16s_mins = vdup_n_u16(0xffff);
	uint16x4_t q16s_maxs = vdup_n_u16(0);
	uint64x1_t q64s_0x00 = vdup_n_u64(0);

	for (unsigned int y = clip_ongoing_or_waiting->y1;
	     y < clip_ongoing_or_waiting->y2; ++y) {
		int offset = y * pixel_pitch + x_start;
		u8 *packed_inner_outer_nextprev_line = ebc->packed_inner_outer_nextprev + offset * 3;
		u8 *phases_line =
			phase_buffer + y * phase_pitch + (x_start >> 2);
		const u8 *prelim_target_line = prelim_target + offset;
		uint32x2_t q32s_xy = vdup_n_u32(y << 16 | x_start);

		for (unsigned int x = x_start; x < clip_ongoing_or_waiting->x2;
		     x += 16, packed_inner_outer_nextprev_line += 48,
			     phases_line += 4, prelim_target_line += 16) {
			uint8x16x3_t q8_inner_outer_nextprev =
				vld3q_u8(packed_inner_outer_nextprev_line);
			/* __builtin_prefetch( */
			/* 		   packed_inner_outer_nextprev_line + packed_pitch, 0, 0); */
			uint8x16_t q8_inner = q8_inner_outer_nextprev.val[0];
			uint8x16_t q8_outer = q8_inner_outer_nextprev.val[1];
			uint8x16_t q8_next_prev =
				q8_inner_outer_nextprev.val[2];
			uint8x16_t q8_prelim_target =
				vld1q_u8(prelim_target_line);
			__builtin_prefetch(prelim_target_line + prefetch_pitch, 0, 0);

			// Extract packed Y4 values and hints
			uint8x16_t q8_inner_num = vandq_u8(q8_inner, q8_0x1f);
			uint8x16_t q8_next = vshrq_n_u8(q8_next_prev, 4);
			uint8x16_t q8_prev = vandq_u8(q8_next_prev, q8_0x0f);
			uint8x16_t q8_prelim = vshrq_n_u8(q8_prelim_target, 4);

			// Start transforming
			uint8x16_t q8_inner_num_is_1 =
				vceqq_u8(q8_inner_num, q8_0x01);
			// Saturating subtraction
			uint8x16_t q8_inner_num_new =
				vqsubq_u8(q8_inner_num, q8_0x01);
			// Restore phase and is_last information
			uint8x16_t q8_inner_new =
				vbslq_u8(q8_0x1f, q8_inner_num_new, q8_inner);

			// These can be scheduled, send to WAITING (hint changed in the meantime)
			uint8x16_t q8_idle = vceqzq_u8(q8_inner);
			uint8x16_t q8_idle_finish =
				vorrq_u8(q8_idle, q8_inner_num_is_1);

			// 1. Schedule: start target or prelim
			uint8x16_t q8_start_scheduled = vandq_u8(
				q8_allow_schedule,
				vmvnq_u8(vceqq_u8(q8_prelim, q8_next)));
			uint8x16_t q8_next_new = vbslq_u8(q8_start_scheduled,
							  q8_prelim, q8_next);
			uint8x16_t q8_prev_new =
				vbslq_u8(q8_start_scheduled, q8_next, q8_prev);

			uint8x16_t q8_next_prev_new = vorrq_u8(
				vshlq_n_u8(q8_next_new, 4), q8_prev_new);

			// 3. Move remaining finished ones to IDLE or WAITING
			uint8x16_t q8_no_start_scheduled =
				vmvnq_u8(q8_start_scheduled);
			uint8x16_t q8_finish_no_start_scheduled =
				vandq_u8(q8_inner_num_is_1, q8_no_start_scheduled);
			uint8x16_t q8_outer_eq_0 = vceqzq_u8(q8_outer);
			uint8x16_t q8_outer_gt_0 = vmvnq_u8(q8_outer_eq_0);
			// Start waiting if:
			// We finish, but we don't start schedule/redraw AND next != target, AND allow_schedule
			// Or: We are not running AND we didn't schedule (next=prelim) AND next != target AND allow_schedule

			uint8x16_t q8_start_idle = vandq_u8(
				q8_finish_no_start_scheduled, q8_outer_gt_0);
			uint8x16_t q8_start = q8_start_scheduled;

			// 4. Get outer_new
			uint8x16_t q8_outer_new = vbslq_u8(q8_inner_num_is_1, q8_0x00, q8_outer);
			q8_outer_new =
				vbslq_u8(q8_start, q8_0x01, q8_outer_new);
			// Or: replace q8_wf with WAITING before the previous statement

			// 5. Get inner_new
			uint8x16_t q8_inner_new_from_lut =
				vbslq_u8(vceqzq_u8(q8_next_new), q8_inner_15_0,
					 q8_inner_0_15);
			q8_inner_new =
				vbslq_u8(q8_start,
					 q8_inner_new_from_lut, q8_inner_new);
			q8_inner_new =
				vbslq_u8(q8_start_idle, q8_0x01, q8_inner_new);

			// 6. Early cancellation
			uint8x16_t q8_cancel =
				vandq_u8(q8_start, vmvnq_u8(q8_idle_finish));
			uint8x16_t q8_to_subtract =
				vandq_u8(q8_cancel, q8_inner_num);
			q8_inner_num_new =
				vqsubq_u8(vandq_u8(q8_inner_new, q8_0x1f),
					  q8_to_subtract);
			q8_inner_num_new = vqaddq_u8(
				q8_inner_num_new,
				vandq_u8(q8_cancel,
					 q8_early_cancellation_addition));
			q8_inner_new = vbslq_u8(q8_0x1f, q8_inner_num_new,
						q8_inner_new);
			uint8x16x3_t q8_ionp_new = {{q8_inner_new, q8_outer_new, q8_next_prev_new}};
			vst3q_u8(packed_inner_outer_nextprev_line, q8_ionp_new);

#ifdef CONFIG_DRM_ROCKCHIP_EBC_3WIN_MODE
			if (!direct_mode) {
				vst1q_u8(phases_line, vshrq_n_u8(q8_inner_new, 6);
			} else
#endif
			{
				uint16x8_t q16_phases = vreinterpretq_u16_u8(
									     vshrq_n_u8(q8_inner_new, 6));
				q16_phases = vorrq_u16(q16_phases,
						       vshrq_n_u16(q16_phases, 6));
				uint8x8_t q8s_phases1 = vmovn_u16(q16_phases);
				q16_phases = vreinterpretq_u16_u8(
								  vcombine_u8(q8s_phases1, q8s_phases1));
				uint8x8_t q8s_phases = vmovn_u16(vorrq_u16(
									  q16_phases, vshrq_n_u16(q16_phases, 4)));
				__builtin_prefetch(phases_line + phases_prefetch_pitch, 1, 0);
				vst1_lane_u32((u32 *)phases_line,
					      vreinterpret_u32_u8(q8s_phases),
					      0);
			}

#ifdef ROCKCHIP_EBC_DEBUG_NEON_FAST
			if (x == 928 && y == 702) {
				NEON_PR_INFO(q8_prelim_target);
				NEON_PR_INFO(q8_finish_no_start_scheduled);
				NEON_PR_INFO(q8_no_start_scheduled);
				NEON_PR_INFO(q8_outer_gt_0);
				NEON_PR_INFO(q8_start_idle);
				NEON_PR_INFO(q8_next_prev_new);
				NEON_PR_INFO(q8_next_prev);
				NEON_PR_INFO(q8_inner);
				NEON_PR_INFO(q8_outer);
				NEON_PR_INFO(q8_allow_schedule);
				NEON_PR_INFO(q8_inner_0_15);
				NEON_PR_INFO(q8_inner_15_0);
				NEON_PR_INFO(q8_inner_new);
				NEON_PR_INFO(q8_inner_new_from_lut);
				NEON_PR_INFO(q8_cancel);
				NEON_PR_INFO(q8_to_subtract);
				NEON_PR_INFO(q8_inner_num_new);
				NEON_PR_INFO(q8_outer_new);
				NEON_PR_INFO(q16_phases);
				NEON_PR_INFOS(q8s_phases1);
				NEON_PR_INFOS(q8s_phases);
			}
#endif

			uint64x1_t q64s_any_ongoing = vreinterpret_u64_u8(
				vorr_u8(vget_high_u8(q8_inner_new),
					vget_low_u8(q8_inner_new)));
			uint32x2_t q32s_mask =
				vreinterpret_u32_u64(vcgt_u64(q64s_any_ongoing, q64s_0x00));
			q16s_maxs = vmax_u16(q16s_maxs,
					     vreinterpret_u16_u32(vand_u32(
						     q32s_xy, q32s_mask)));
			q16s_mins = vmin_u16(
				q16s_mins,
				vreinterpret_u16_u32(vorr_u32(
					q32s_xy, vmvn_u32(q32s_mask))));
			q32s_xy = vqadd_u32(q32s_xy, q32s_0x10);
		}
	}

	aligned_u64 u64_mins, u64_maxs;
	u16 *mins = (u16 *)&u64_mins;
	u16 *maxs = (u16 *)&u64_maxs;
	vst1_lane_u64(&u64_mins, vreinterpret_u64_u16(q16s_mins), 0);
	vst1_lane_u64(&u64_maxs, vreinterpret_u64_u16(q16s_maxs), 0);
	clip_ongoing->x1 = mins[0];
	clip_ongoing->y1 = mins[1];
	clip_ongoing->x2 = maxs[0] + 16;
	clip_ongoing->y2 = maxs[1] + 1;
	*clip_ongoing_or_waiting = *clip_ongoing;
}
EXPORT_SYMBOL(rockchip_ebc_schedule_advance_fast_neon);

void rockchip_ebc_schedule_advance_neon(
	const struct rockchip_ebc *ebc, const u8 *prelim_target, u8 *hints,
	u8 *phase_buffer, struct drm_rect *clip_ongoing,
	struct drm_rect *clip_ongoing_or_waiting,
	u8 early_cancellation_addition, u8 force_wf, u8 force_hint,
	u8 force_hint_mask, bool allow_schedule)
{
	bool direct_mode = ebc->direct_mode;
	unsigned int pixel_pitch = ebc->pixel_pitch;
	// unsigned int packed_pitch = 3 * pixel_pitch;
	unsigned int phase_pitch = direct_mode ? ebc->phase_pitch :
		ebc->pixel_pitch;
	unsigned int prefetch_pitch = (pixel_pitch << 6) >> 6;
	unsigned int phases_prefetch_pitch = (pixel_pitch >> 8) << 6;
	u8 *lut = ebc->lut_custom_active->lut;
	prefetch_range(lut, sizeof(struct drm_epd_lut_temp_v2));

	// 16 u16 values used as addresses for non-NEON code
	aligned_u64 _idxs[4];
	// 16 u8 values aligned for LUT using non-NEON code
	aligned_u64 _lookup[2];

	u16 *u16_idxs = (u16 *)_idxs;
	u8 *u8_lookup = (u8 *)_lookup;
	memset(u8_lookup, 0, 16);
	// Initialise for q8_us_y4_binary_table
	u8_lookup[0] = 0xff;
	u8_lookup[15] = 0xff;

#ifdef ROCKCHIP_EBC_DEBUG_NEON
	aligned_u64 _tmp[2];
	u8 *u8_tmp = (u8 *)_tmp;
#endif

	// 16 byte block size for NEON
	unsigned int x_start = max(0, min(clip_ongoing_or_waiting->x1 & ~15,
					  (int)ebc->pixel_pitch - 16));

	u8 offset_waiting =
		ebc->lut_custom_active->offsets[ROCKCHIP_EBC_CUSTOM_WF_WAITING];
	uint8x16_t q8_0x00 = vdupq_n_u8(0x00);
	uint8x16_t q8_0x01 = vdupq_n_u8(0x01);
	uint8x16_t q8_0x03 = vdupq_n_u8(0x03);
	uint8x16_t q8_0x0f = vdupq_n_u8(0x0f);
	uint8x16_t q8_0x1f = vdupq_n_u8(0x1f);
	uint8x16_t q8_0x20 = vdupq_n_u8(0x20);
	uint8x16_t q8_0x21 = vdupq_n_u8(0x21);
	uint8x16_t q8_0x80 = vdupq_n_u8(0x80);
	uint8x16_t q8_offset_waiting = vdupq_n_u8(offset_waiting);
	uint8x16_t q8_force_wf = vdupq_n_u8(force_wf);
	uint8x16_t q8_force_hint = vdupq_n_u8(force_hint);
	uint8x16_t q8_force_hint_mask = vdupq_n_u8(force_hint_mask);
	uint8x16_t q8_offsets_table = vld1q_u8(ebc->lut_custom_active->offsets);
	uint8x16_t q8_is_y4_binary_table = vld1q_u8(u8_lookup);
	uint8x16_t q8_early_cancellation_addition =
		vdupq_n_u8(early_cancellation_addition);
	uint8x16_t q8_force_wf_gt0 = vcgtq_u8(q8_force_wf, q8_0x00);
	uint8x16_t q8_allow_schedule =
		vcgtq_u8(vdupq_n_u8(allow_schedule), q8_0x00);

	uint32x2_t q32s_0x10 = vdup_n_u32(0x10);
	uint16x4_t q16s_mins = vdup_n_u16(0xffff);
	uint16x4_t q16s_maxs = vdup_n_u16(0);
	uint64x1_t q64s_0x00 = vdup_n_u64(0);

	for (unsigned int y = clip_ongoing_or_waiting->y1;
	     y < clip_ongoing_or_waiting->y2; ++y) {
		int offset = y * pixel_pitch + x_start;
		u8 *packed_inner_outer_nextprev_line = ebc->packed_inner_outer_nextprev + offset * 3;
		u8 *phases_line = phase_buffer + y * phase_pitch +
				  (x_start >> (direct_mode ? 2 : 0));
		const u8 *prelim_target_line =
			prelim_target + offset;
		u8 *hints_line = hints + offset;
		uint32x2_t q32s_xy = vdup_n_u32(y << 16 | x_start);

		for (unsigned int x = x_start; x < clip_ongoing_or_waiting->x2;
		     x += 16, packed_inner_outer_nextprev_line += 48,
				  phases_line += direct_mode ? 4 : 16,
					  prelim_target_line += 16, hints_line += 16) {
			uint8x16x3_t q8_inner_outer_nextprev =
				vld3q_u8(packed_inner_outer_nextprev_line);
			uint8x16_t q8_inner = q8_inner_outer_nextprev.val[0];
			uint8x16_t q8_outer = q8_inner_outer_nextprev.val[1];
			uint8x16_t q8_next_prev =
				q8_inner_outer_nextprev.val[2];
			uint8x16_t q8_prelim_target =
				vld1q_u8(prelim_target_line);
			uint8x16_t q8_hints = vld1q_u8(hints_line);
			__builtin_prefetch(packed_inner_outer_nextprev_line + 3 * prefetch_pitch, 0, 0);
			__builtin_prefetch(prelim_target_line + prefetch_pitch, 0, 0);
			__builtin_prefetch(hints_line + prefetch_pitch, 0, 0);

			// Extract packed Y4 values and hints
			uint8x16_t q8_inner_num = vandq_u8(q8_inner, q8_0x1f);
			uint8x16_t q8_inner_is_last =
				vtstq_u8(q8_inner, q8_0x20);
			uint8x16_t q8_next = vshrq_n_u8(q8_next_prev, 4);
			uint8x16_t q8_prev = vandq_u8(q8_next_prev, q8_0x0f);
			uint8x16_t q8_prelim = vshrq_n_u8(q8_prelim_target, 4);
			uint8x16_t q8_target =
				vandq_u8(q8_prelim_target, q8_0x0f);
			q8_hints = vbslq_u8(q8_force_hint_mask, q8_force_hint,
					    q8_hints);
			uint8x16_t q8_wf_target =
				vandq_u8(vshrq_n_u8(q8_hints, 4), q8_0x03);
			q8_wf_target = vbslq_u8(q8_force_wf_gt0, q8_force_wf,
						q8_wf_target);
			uint8x16_t q8_hint_redraw = vtstq_u8(q8_hints, q8_0x80);
			uint8x16_t q8_hint_noredraw = vmvnq_u8(q8_hint_redraw);

			uint8x16_t q8_next_eq_target =
				vceqq_u8(q8_next, q8_target);

			// Start transforming
			uint8x16_t q8_inner_num_is_1 =
				vceqq_u8(q8_inner_num, q8_0x01);
			// Saturating subtraction
			uint8x16_t q8_inner_num_new =
				vqsubq_u8(q8_inner_num, q8_0x01);
			// Restore phase and is_last information
			uint8x16_t q8_inner_new =
				vbslq_u8(q8_0x1f, q8_inner_num_new, q8_inner);

			// Transition these to WAITING or IDLE, allow rescheduling
			uint8x16_t q8_finish =
				vandq_u8(q8_inner_num_is_1, q8_inner_is_last);
			// These can be rescheduled
			uint8x16_t q8_waiting =
				vcgeq_u8(q8_outer, q8_offset_waiting);
			// Transition these to REDRAW, allow rescheduling
			uint8x16_t q8_finish_waiting =
				vandq_u8(q8_finish, q8_waiting);
			// These can be scheduled, send to WAITING (hint changed in the meantime)
			uint8x16_t q8_idle = vceqzq_u8(q8_inner);
			uint8x16_t q8_waiting_idle =
				vorrq_u8(q8_waiting, q8_idle);
			uint8x16_t q8_waiting_idle_finish =
				vorrq_u8(q8_waiting_idle, q8_finish);

			// For scheduling: choose between target/prelim
			uint8x16_t q8_use_target = vorrq_u8(
				vorrq_u8(q8_finish_waiting, q8_hint_noredraw),
				vorrq_u8(q8_next_eq_target, q8_force_wf_gt0));
			uint8x16_t q8_target_or_prelim_new =
				vbslq_u8(q8_use_target, q8_target, q8_prelim);
			// Prelim uses waveform DU=0
			uint8x16_t q8_wf =
				vandq_u8(q8_wf_target, q8_use_target);
			uint8x16_t q8_wf_is_du = vceqzq_u8(q8_wf);
			uint8x16_t q8_target_or_prelim_is_binary = vqtbl1q_u8(
				q8_is_y4_binary_table, q8_target_or_prelim_new);
			uint8x16_t q8_next_is_binary =
				vqtbl1q_u8(q8_is_y4_binary_table, q8_next);
			uint8x16_t q8_prev_is_binary =
				vqtbl1q_u8(q8_is_y4_binary_table, q8_prev);
			uint8x16_t q8_next_and_prev_are_binary =
				vandq_u8(q8_next_is_binary, q8_prev_is_binary);
			uint8x16_t q8_outer_is_du = vceqq_u8(q8_outer, q8_0x01);
			uint8x16_t q8_src_cancellable = vandq_u8(
				q8_next_and_prev_are_binary, q8_outer_is_du);
			uint8x16_t q8_dst_cancellable = vandq_u8(
				q8_wf_is_du, q8_target_or_prelim_is_binary);

			uint8x16_t q8_can_cancel = vandq_u8(q8_src_cancellable,
							    q8_dst_cancellable);
			uint8x16_t q8_can_start_or_cancel =
				vorrq_u8(q8_waiting_idle_finish, q8_can_cancel);

			// 1. Schedule: start target or prelim
			uint8x16_t q8_start_scheduled = vandq_u8(
				q8_allow_schedule,
				vandq_u8(vmvnq_u8(vceqq_u8(
						 q8_target_or_prelim_new,
						 q8_next)),
					 q8_can_start_or_cancel));
			q8_start_scheduled =
				vorrq_u8(q8_start_scheduled, q8_force_wf_gt0);
			uint8x16_t q8_next_new =
				vbslq_u8(q8_start_scheduled,
					 q8_target_or_prelim_new, q8_next);
			uint8x16_t q8_prev_new =
				vbslq_u8(q8_start_scheduled, q8_next, q8_prev);


			// 2. Redraw: start target if still desired an we finished waiting
			uint8x16_t q8_start_redraw = vandq_u8(
				q8_allow_schedule,
				vandq_u8(
					vmvnq_u8(vceqq_u8(q8_next, q8_target)),
					vandq_u8(q8_finish_waiting,
						 vmvnq_u8(q8_start_scheduled))));
			q8_next_new = vbslq_u8(q8_start_redraw, q8_target,
					       q8_next_new);
			q8_prev_new =
				vbslq_u8(q8_start_redraw, q8_next, q8_prev_new);
			uint8x16_t q8_next_prev_new = vorrq_u8(
							       vshlq_n_u8(q8_next_new, 4), q8_prev_new);

			// 3. Move remaining finished ones to IDLE or WAITING
			uint8x16_t q8_start_scheduled_or_redraw =
				vorrq_u8(q8_start_scheduled, q8_start_redraw);
			uint8x16_t q8_no_start_scheduled_or_redraw =
				vmvnq_u8(q8_start_scheduled_or_redraw);
			uint8x16_t q8_finish_no_start_scheduled_or_redraw =
				vandq_u8(q8_finish,
					 q8_no_start_scheduled_or_redraw);
			uint8x16_t q8_outer_eq_0 = vceqzq_u8(q8_outer);
			uint8x16_t q8_outer_gt_0 = vmvnq_u8(q8_outer_eq_0);
			// Start waiting if:
			// We finish, but we don't start schedule/redraw AND next != target, AND allow_schedule
			// Or: We are not running AND we didn't schedule (next=prelim) AND next != target AND allow_schedule

			uint8x16_t q8_start_waiting = vandq_u8(
				vorrq_u8(
					q8_finish_no_start_scheduled_or_redraw,
					vandq_u8(
						q8_outer_eq_0,
						q8_no_start_scheduled_or_redraw)),
				vandq_u8(q8_allow_schedule,
					 vmvnq_u8(q8_next_eq_target)));
			uint8x16_t q8_start_idle =
				vandq_u8(q8_finish_no_start_scheduled_or_redraw,
					 vandq_u8(vmvnq_u8(q8_start_waiting),
						  q8_outer_gt_0));
			uint8x16_t q8_start = vorrq_u8(
						       q8_start_scheduled_or_redraw, q8_start_waiting);

			// 4. Get outer_new
			uint8x16_t q8_outer_new =
				vandq_u8(vqaddq_u8(q8_outer, q8_0x01),
					 vmvnq_u8(q8_inner_is_last));
			q8_outer_new = vbslq_u8(q8_inner_num_is_1, q8_outer_new,
						q8_outer);
			q8_outer_new = vbslq_u8(
				q8_start, vqtbl1q_u8(q8_offsets_table, q8_wf),
				q8_outer_new);
			// Or: replace q8_wf with WAITING before the previous statement
			q8_outer_new = vbslq_u8(q8_start_waiting,
						q8_offset_waiting,
						q8_outer_new);

			// 5. Get inner_new
			uint16x8_t q16_idx_low =
				vmovl_u8(vget_low_u8(q8_outer_new));
			q16_idx_low = vaddq_u16(
				q16_idx_low,
				vqshlq_n_u16(
					vmovl_u8(vget_low_u8(q8_prev_new)),
					4 + ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT));
			q16_idx_low = vaddq_u16(
				q16_idx_low,
				vshll_n_u8(vget_low_u8(q8_next_new),
					   ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT));
			vst1q_u16(u16_idxs, q16_idx_low);
			uint16x8_t q16_idx_high =
				vmovl_u8(vget_high_u8(q8_outer_new));
			q16_idx_high = vaddq_u16(
				q16_idx_high,
				vqshlq_n_u16(
					vmovl_u8(vget_high_u8(q8_prev_new)),
					4 + ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT));
			q16_idx_high = vaddq_u16(
				q16_idx_high,
				vshll_n_u8(vget_high_u8(q8_next_new),
					   ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT));
			vst1q_u16(u16_idxs + 8, q16_idx_high);
			u8_lookup[0] = lut[u16_idxs[0]];
			u8_lookup[1] = lut[u16_idxs[1]];
			u8_lookup[2] = lut[u16_idxs[2]];
			u8_lookup[3] = lut[u16_idxs[3]];
			u8_lookup[4] = lut[u16_idxs[4]];
			u8_lookup[5] = lut[u16_idxs[5]];
			u8_lookup[6] = lut[u16_idxs[6]];
			u8_lookup[7] = lut[u16_idxs[7]];
			u8_lookup[8] = lut[u16_idxs[8]];
			u8_lookup[9] = lut[u16_idxs[9]];
			u8_lookup[10] = lut[u16_idxs[10]];
			u8_lookup[11] = lut[u16_idxs[11]];
			u8_lookup[12] = lut[u16_idxs[12]];
			u8_lookup[13] = lut[u16_idxs[13]];
			u8_lookup[14] = lut[u16_idxs[14]];
			u8_lookup[15] = lut[u16_idxs[15]];
			uint8x16_t q8_inner_new_from_lut = vld1q_u8(u8_lookup);
			q8_inner_new =
				vbslq_u8(vorrq_u8(q8_inner_num_is_1, q8_start),
					 q8_inner_new_from_lut, q8_inner_new);
			q8_inner_new =
				vbslq_u8(q8_start_idle, q8_0x21, q8_inner_new);

			// 6. Early cancellation
			// TODO: DU4 may be cancellable as well for binary src and dst
			uint8x16_t q8_cancel = vandq_u8(
				q8_start,
				vandq_u8(q8_can_cancel,
					 vmvnq_u8(q8_waiting_idle_finish)));
			uint8x16_t q8_to_subtract =
				vandq_u8(q8_cancel, q8_inner_num);
			q8_inner_num_new =
				vqsubq_u8(vandq_u8(q8_inner_new, q8_0x1f),
					  q8_to_subtract);
			q8_inner_num_new = vqaddq_u8(
				q8_inner_num_new,
				vandq_u8(q8_cancel,
					 q8_early_cancellation_addition));
			q8_inner_new = vbslq_u8(q8_0x1f, q8_inner_num_new,
						q8_inner_new);
			uint8x16x3_t q8_ionp_new = {{q8_inner_new, q8_outer_new, q8_next_prev_new}};
			vst3q_u8(packed_inner_outer_nextprev_line, q8_ionp_new);

			uint8x16_t q8_phases = vshrq_n_u8(q8_inner_new, 6);
			uint16x8_t q16_phases;
			uint8x8_t q8s_phases1, q8s_phases;

#ifdef CONFIG_DRM_ROCKCHIP_EBC_3WIN_MODE
			if (!direct_mode) {
				vst1q_u8(phases_line, q8_phases);
			} else
#endif
			{
				q16_phases = vreinterpretq_u16_u8(q8_phases);
				q16_phases = vorrq_u16(
					q16_phases, vshrq_n_u16(q16_phases, 6));
				q8s_phases1 = vmovn_u16(q16_phases);
				q16_phases = vreinterpretq_u16_u8(
					vcombine_u8(q8s_phases1, q8s_phases1));
				q8s_phases = vmovn_u16(
					vorrq_u16(q16_phases,
						 vshrq_n_u16(q16_phases, 4)));
		vst1_lane_u32((u32 *)phases_line,
					      vreinterpret_u32_u8(q8s_phases),
					      0);
				__builtin_prefetch(phases_line + phases_prefetch_pitch, 1, 0);
			}

			#ifdef ROCKCHIP_EBC_DEBUG_NEON
			if (x == 928 && y == 702) {
				NEON_PR_INFO(q8_inner);
				NEON_PR_INFO(q8_outer);
				NEON_PR_INFO(q8_next_prev);
				NEON_PR_INFO(q8_prelim_target);
				NEON_PR_INFO(q8_hints);
				NEON_PR_INFO(q8_inner_num);
				NEON_PR_INFO(q8_inner_is_last);
				NEON_PR_INFO(q8_next);
				NEON_PR_INFO(q8_prev);
				NEON_PR_INFO(q8_prelim);
				NEON_PR_INFO(q8_target);
				NEON_PR_INFO(q8_wf_target);
				NEON_PR_INFO(q8_hint_redraw);
				NEON_PR_INFO(q8_hint_noredraw);
				NEON_PR_INFO(q8_next_eq_target);
				NEON_PR_INFO(q8_use_target);
				NEON_PR_INFO(q8_target_or_prelim_new);
				NEON_PR_INFO(q8_wf);
				NEON_PR_INFO(q8_wf_is_du);
				NEON_PR_INFO(q8_target_or_prelim_is_binary);
				NEON_PR_INFO(q8_next_is_binary);
				NEON_PR_INFO(q8_prev_is_binary);
				NEON_PR_INFO(q8_next_and_prev_are_binary);
				NEON_PR_INFO(q8_src_cancellable);
				NEON_PR_INFO(q8_dst_cancellable);
				NEON_PR_INFO(q8_inner_num_is_1);
				NEON_PR_INFO(q8_inner_num_new);
				NEON_PR_INFO(q8_inner_new);
				NEON_PR_INFO(q8_finish);
				NEON_PR_INFO(q8_waiting);
				NEON_PR_INFO(q8_finish_waiting);
				NEON_PR_INFO(q8_idle);
				NEON_PR_INFO(q8_waiting_idle);
				NEON_PR_INFO(q8_waiting_idle_finish);
				NEON_PR_INFO(q8_can_cancel);
				NEON_PR_INFO(q8_can_start_or_cancel);
				NEON_PR_INFO(q8_start_scheduled);
				NEON_PR_INFO(q8_next_new);
				NEON_PR_INFO(q8_prev_new);
				NEON_PR_INFO(q8_start_redraw);
				NEON_PR_INFO(q8_start_scheduled_or_redraw);
				NEON_PR_INFO(q8_no_start_scheduled_or_redraw);
				NEON_PR_INFO(q8_finish_no_start_scheduled_or_redraw);
				NEON_PR_INFO(q8_start_idle);
				NEON_PR_INFO(q8_start_waiting);
				NEON_PR_INFO(q8_start);
				NEON_PR_INFO(q8_outer_new);
				NEON_PR_INFO(q8_inner_new);
				NEON_PR_INFO(q8_inner_new_from_lut);
				NEON_PR_INFO(q8_cancel);
				NEON_PR_INFO(q8_to_subtract);
				NEON_PR_INFO(q8_inner_num_new);
				NEON_PR_INFO(q8_inner_new);
				NEON_PR_INFO(q8_phases);
				NEON_PR_INFO(q16_phases);
				NEON_PR_INFOS(q8s_phases1);
				NEON_PR_INFOS(q8s_phases);
			}
#endif // ROCKCHIP_EBC_DEBUG_NEON
			uint8x16_t q8_ongoing = vandq_u8(
				q8_inner_new,
				vcltq_u8(q8_outer_new, q8_offset_waiting));
			uint64x1_t q64s_any_ongoing_or_waiting =
				vreinterpret_u64_u8(
					vorr_u8(vget_high_u8(q8_inner_new),
						vget_low_u8(q8_inner_new)));
			uint64x1_t q64s_any_ongoing = vreinterpret_u64_u8(
				vorr_u8(vget_high_u8(q8_ongoing),
					vget_low_u8(q8_ongoing)));
			q64s_any_ongoing_or_waiting = vcgt_u64(
				q64s_any_ongoing_or_waiting, q64s_0x00);
			q64s_any_ongoing =
				vcgt_u64(q64s_any_ongoing, q64s_0x00);
			uint32x2_t q32s_mask = vzip1_u32(
				vreinterpret_u32_u64(q64s_any_ongoing),
				vreinterpret_u32_u64(
					q64s_any_ongoing_or_waiting));
			q16s_maxs = vmax_u16(q16s_maxs,
					     vreinterpret_u16_u32(vand_u32(
						     q32s_xy, q32s_mask)));
			q16s_mins = vmin_u16(
				q16s_mins,
				vreinterpret_u16_u32(vorr_u32(
							      q32s_xy, vmvn_u32(q32s_mask))));

			q32s_xy = vqadd_u32(q32s_xy, q32s_0x10);
#ifdef ROCKCHIP_EBC_DEBUG_NEON
			if (x == 928 && y == 702) {
				NEON_PR_INFO(q8_inner_new);
				NEON_PR_INFO(q8_outer_new);
				NEON_PR_INFO(q8_ongoing);
				NEON_PR_INFOS(q64s_any_ongoing_or_waiting);
				NEON_PR_INFOS(q64s_any_ongoing);
				NEON_PR_INFOS(q32s_mask);
				NEON_PR_INFOS(q32s_maxs);
				NEON_PR_INFOS(q32s_mins);
				NEON_PR_INFOS(q32s_xy);
			}
#endif
		}
	}

	aligned_u64 u64_mins, u64_maxs;
	u16 *mins = (u16 *)&u64_mins;
	u16 *maxs = (u16 *)&u64_maxs;
	vst1_lane_u64(&u64_mins, vreinterpret_u64_u16(q16s_mins), 0);
	vst1_lane_u64(&u64_maxs, vreinterpret_u64_u16(q16s_maxs), 0);
	clip_ongoing->x1 = mins[0];
	clip_ongoing->y1 = mins[1];
	clip_ongoing->x2 = maxs[0] + 16;
	clip_ongoing->y2 = maxs[1] + 1;
	clip_ongoing_or_waiting->x1 = mins[2];
	clip_ongoing_or_waiting->y1 = mins[3];
	clip_ongoing_or_waiting->x2 = maxs[2] + 16;
	clip_ongoing_or_waiting->y2 = maxs[3] + 1;
}
EXPORT_SYMBOL(rockchip_ebc_schedule_advance_neon);

void rockchip_ebc_blit_y4_high_low_neon(u8 *dst, u8 *y4_high_src,
					u8 *y4_low_src, unsigned int size)
{
	for (unsigned int i = 0; i < size;
	     i += 16, dst += 16, y4_high_src += 16, y4_low_src += 16) {
		vst1q_u8(dst,
			 vorrq_u8(vld1q_u8(y4_high_src), vld1q_u8(y4_low_src)));
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_y4_high_low_neon);

/**
 * rockchip_ebc_blit_y421_y4_neon - blit from y421 to y4
 */
void rockchip_ebc_blit_y421_y4_neon(const struct rockchip_ebc *ebc, u8 *y4_dst,
				    const u8 *y421_src, struct drm_rect *clip)
{
	unsigned int y421_pitch = ebc->pixel_pitch;
	unsigned int gray4_pitch = ebc->gray4_pitch;

	// 16 byte block size for NEON
	unsigned int x_start =
		max(0, min(clip->x1 & ~15, (int)ebc->pixel_pitch - 16));
	unsigned int x_end =
		min((int)ebc->pixel_pitch, ((clip->x2 + 15) & ~15));
	unsigned int x, y;

	for (y = clip->y1; y < clip->y2; y++) {
		const u8 *src_line = y421_src + y * y421_pitch + x_start;
		u8 *dst_line = y4_dst + y * gray4_pitch + x_start / 2;
		for (x = x_start; x < x_end;
		     x += 16, src_line += 16, dst_line += 8) {
			// ?A ?B ?C ?D
			uint8x16_t q8_y421 = vld1q_u8(src_line);
			// AA BB CC DD duplicate lower bits by left shifting and insert immediately
			uint8x16_t q8_y4 = vsliq_n_u8(q8_y421, q8_y421, 4);
			// AABB CCDD
			uint16x8_t q16_y4 = vreinterpretq_u16_u8(q8_y4);
			// Insert AB CD
			vst1_u8(dst_line, vshrn_n_u16(q16_y4, 4));
		}
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_y421_y4_neon);

/**
 * rockchip_ebc_blit_fb_rgb565_y4_hints_neon - blit from RGB565 FB
 *
 * Flips output horizontally.
 * HHHH 4444  (hints in higher bits, Y4 in lower bits)
 */
void rockchip_ebc_blit_fb_rgb565_y4_hints_neon(const struct rockchip_ebc *ebc,
					       struct drm_rect *dst_clip,
					       u8 *prelim_target_atomic_update,
					       u8 *hints_atomic_update,
					       const void *vaddr,
					       const struct drm_framebuffer *fb,
					       const struct drm_rect *src_clip)
{
	unsigned int rgb_pitch = fb->pitches[0];
	unsigned int pixel_pitch = ebc->pixel_pitch;
	unsigned int prefetch_pitch = (pixel_pitch << 6) >> 6;

	// 16 byte block size for NEON
	unsigned int src_start_x = src_clip->x1;
	unsigned int src_end_x = src_clip->x2;
	unsigned int dst_start_x = dst_clip->x2 - 16;
	unsigned int x, y;

	// Force horizontal reflection for simplicity
	u8 *dst_prelim_target = prelim_target_atomic_update +
				dst_clip->y1 * pixel_pitch + dst_start_x;
	const u8 *src = vaddr + src_clip->y1 * rgb_pitch +
			src_start_x * fb->format->cpp[0];
	const u8 *ioctl_hints =
		ebc->hints_ioctl + src_clip->y1 * pixel_pitch + src_start_x;
	u8 *hints =
		hints_atomic_update + dst_clip->y1 * pixel_pitch + dst_start_x;

	// Thresholds and LUT for 4-level tresholding and 2-level threshold
	uint8x16_t q8_thresholds_y2_table = vld1q_u8((u8 *)ebc->lut_y2_y4);
	uint8x16x2_t q8_thresholds_y2_dither_table =
		vld2q_u8((u8 *)ebc->lut_y2_y4_dithered);
	uint8x16_t q8_threshold_y1 = vdupq_n_u8(ebc->y4_threshold_y1);

	const u8 *dithering_texture = ebc->dithering_texture;
	u8 dithering_texture_size_hint = ebc->dithering_texture_size_hint;

	// (256 * (np.array([.299, .587, .114])) * [255/31, 255/63, 255/31] / [8, 4, 8]).round()
	// Factors were modified to reduce number of shifting or masking operations required
	uint8x8_t q8_yuv_r = vdup_n_u8(79);
	uint8x8_t q8_yuv_g = vdup_n_u8(152);
	uint8x8_t q8_yuv_b = vdup_n_u8(30);

	uint8x16_t q8_0x02 = vdupq_n_u8(0x02);
	uint8x16_t q8_0x03 = vdupq_n_u8(0x03);
	uint8x16_t q8_0x08 = vdupq_n_u8(0x08);
	uint8x16_t q8_0x0f = vdupq_n_u8(0x0f);
	uint8x16_t q8_0x40 = vdupq_n_u8(0x40);
	uint8x8_t q8s_0xfc = vdup_n_u8(0xfc);
	uint8x8_t q8s_0xf8 = vdup_n_u8(0xf8);

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u16 *fb_line = (const u16 *) src;
		u8 *prelim_target_line = dst_prelim_target;
		const u8 *ioctl_hints_line = ioctl_hints;
		u8 *hints_line = hints;

		uint8x16_t q8_dither_pattern0, q8_dither_pattern1, q8_tmp;
		if (dithering_texture_size_hint == 32) {
			q8_dither_pattern0 =
				vld1q_u8(dithering_texture + 32 * (y & 31));
			q8_dither_pattern1 =
				vld1q_u8(dithering_texture + 32 * (y & 31));
			if (src_start_x & 16) {
				q8_tmp = q8_dither_pattern0;
				q8_dither_pattern0 = q8_dither_pattern1;
				q8_dither_pattern1 = q8_tmp;
			}
		} else {
			q8_dither_pattern0 = vld1q_u8(
				dithering_texture +
				16 * (y & (dithering_texture_size_hint - 1)));
			q8_dither_pattern1 = q8_dither_pattern0;
		}

		for (x = src_start_x; x < src_end_x; x += 16, fb_line += 16,
		    prelim_target_line -= 16, ioctl_hints_line += 16,
		    hints_line -= 16) {
			uint8x8_t q8s_gray1, q8s_gray2;
			uint8x16_t q8_gray;
			uint16x8_t q16_gray;

			// RGB -> Y8 using rounded YUV
			// Load 16 RGB values
			uint16x8_t q16_rgb = vld1q_u16(fb_line);

			q16_gray = vmull_u8(q8_yuv_b,
					    vmovn_u16(vshlq_n_u16(q16_rgb, 3)));
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g,
					    vand_u8(vshrn_n_u16(q16_rgb, 3),
						    q8s_0xfc));
			q16_gray = vmlal_u8(q16_gray, q8_yuv_r,
					    vand_u8(vshrn_n_u16(q16_rgb, 8),
						    q8s_0xf8));
			// Aa Bb Cc Dd lower-case bits still need to be discarded
			q8s_gray1 = vshrn_n_u16(q16_gray, 8);

			// Same for the next 8 RGB pixels
			q16_rgb = vld1q_u16(fb_line + 8);
			/* __builtin_prefetch(fb_line + pixel_pitch, 0, 0); */

			q16_gray = vmull_u8(q8_yuv_b,
					    vmovn_u16(vshlq_n_u16(q16_rgb, 3)));
			q16_gray = vmlal_u8(q16_gray, q8_yuv_g,
					    vand_u8(vshrn_n_u16(q16_rgb, 3),
						    q8s_0xfc));
			q16_gray = vmlal_u8(q16_gray, q8_yuv_r,
					    vand_u8(vshrn_n_u16(q16_rgb, 8),
						    q8s_0xf8));
			q8s_gray2 = vshrn_n_u16(q16_gray, 8);

			// Combine into single vector, discard lower bits
			// 0A 0B 0C 0D
			q8_gray = vshrq_n_u8(vcombine_u8(q8s_gray2, q8s_gray1),
					     4);

			uint8x16_t q8_hint = vld1q_u8(ioctl_hints_line);
			/* __builtin_prefetch(ioctl_hints_line + pixel_pitch, 0, 0); */
			uint8x16_t q8_hint_dither = vtstq_u8(q8_hint, q8_0x40);
			uint8x16_t q8_hint_waveform =
				vandq_u8(vshrq_n_u8(q8_hint, 4), q8_0x03);
			uint8x16_t q8_hint_gray = vorrq_u8(q8_gray, q8_hint);

			uint8x16_t q8_gray_dithered = vminq_u8(
				vqsubq_u8(vqaddq_u8(q8_gray,
						    q8_dither_pattern0),
					  q8_0x08),
				q8_0x0f);
			uint8x16_t q8_gray_y2_dt =
				vqtbl2q_u8(q8_thresholds_y2_dither_table,
					   q8_gray_dithered);
			uint8x16_t q8_gray_y2_th =
				vqtbl1q_u8(q8_thresholds_y2_table, q8_gray);
			uint8x16_t q8_gray_y1_dt = vandq_u8(
				vcgeq_u8(q8_gray_dithered, q8_threshold_y1),
				q8_0x0f);
			uint8x16_t q8_gray_y1_th = vandq_u8(
				vcgeq_u8(q8_gray, q8_threshold_y1), q8_0x0f);
			uint8x16_t q8_gray_y1 = vbslq_u8(
				q8_hint_dither, q8_gray_y1_dt, q8_gray_y1_th);
			uint8x16_t q8_gray_y2 = vbslq_u8(
				q8_hint_dither, q8_gray_y2_dt, q8_gray_y2_th);
			uint8x16_t q8_gray_y12 =
				vbslq_u8(vceqzq_u8(q8_hint_waveform),
					 q8_gray_y1, q8_gray_y2);

			uint8x16_t q8_target =
				vbslq_u8(vceqq_u8(q8_hint_waveform, q8_0x02),
					 q8_gray, q8_gray_y12);
			// TODO: make dithering/thresholding configurable via a hint
			uint8x16_t q8_prelim_target = vorrq_u8(
				vshlq_n_u8(q8_gray_y1_dt, 4), q8_target);

			q8_prelim_target = vrev64q_u8(q8_prelim_target);
			q8_hint_gray = vrev64q_u8(q8_hint_gray);
			__builtin_prefetch(prelim_target_line + prefetch_pitch, 1, 0); // Maybe 1, 1 to keep in L3 cache
			__builtin_prefetch(hints_line + prefetch_pitch, 1, 0);
			vst1q_u8(hints_line, vcombine_u8(vget_high_u8(q8_hint_gray), vget_low_u8(q8_hint_gray)));
			vst1q_u8(prelim_target_line, q8_prelim_target);

			// Swap to support 32x32 dithering textures
			q8_tmp = q8_dither_pattern0;
			q8_dither_pattern0 = q8_dither_pattern1;
			q8_dither_pattern1 = q8_tmp;
		}

		src += rgb_pitch;
		dst_prelim_target += pixel_pitch;
		ioctl_hints += pixel_pitch;
		hints += pixel_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_rgb565_y4_hints_neon);

/**
 * rockchip_ebc_blit_fb_xrgb8888_y4_hints_neon - blit from XRGB8888 FB
 *
 * Flips output horizontally.
 * HHHH 4444  (hints in higher bits, Y4 in lower bits)
 */
void rockchip_ebc_blit_fb_xrgb8888_y4_hints_neon(
	const struct rockchip_ebc *ebc, struct drm_rect *dst_clip,
	u8 *prelim_target_atomic_update, u8 *hints_atomic_update,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip)
{
	unsigned int rgba_pitch = fb->pitches[0];
	unsigned int pixel_pitch = ebc->pixel_pitch;
	unsigned int prefetch_pitch = (pixel_pitch << 6) >> 6;

	// 16 byte block size for NEON
	unsigned int src_start_x = src_clip->x1;
	unsigned int src_end_x = src_clip->x2;
	unsigned int dst_start_x = dst_clip->x2 - 16;
	unsigned int x, y;

	// Force horizontal reflection for simplicity
	u8 *dst_prelim_target = prelim_target_atomic_update +
				dst_clip->y1 * pixel_pitch + dst_start_x;
	const u8 *src = vaddr + src_clip->y1 * rgba_pitch +
			src_start_x * fb->format->cpp[0];
	const u8 *ioctl_hints =
		ebc->hints_ioctl + src_clip->y1 * pixel_pitch + src_start_x;
	u8 *hints =
		hints_atomic_update + dst_clip->y1 * pixel_pitch + dst_start_x;

	uint8x8_t q8_yuv_r = vdup_n_u8(76);
	uint8x8_t q8_yuv_g = vdup_n_u8(150);
	uint8x8_t q8_yuv_b = vdup_n_u8(29);

	// Thresholds and LUT for 4-level tresholding and 2-level threshold
	uint8x16_t q8_thresholds_y2_table = vld1q_u8((u8 *)ebc->lut_y2_y4);
	uint8x16x2_t q8_thresholds_y2_dither_table =
		vld2q_u8((u8 *)ebc->lut_y2_y4_dithered);
	uint8x16_t q8_threshold_y1 = vdupq_n_u8(ebc->y4_threshold_y1);

	const u8 *dithering_texture = ebc->dithering_texture;
	u8 dithering_texture_size_hint = ebc->dithering_texture_size_hint;

	uint8x16_t q8_0x02 = vdupq_n_u8(0x02);
	uint8x16_t q8_0x03 = vdupq_n_u8(0x03);
	uint8x16_t q8_0x08 = vdupq_n_u8(0x08);
	uint8x16_t q8_0x0f = vdupq_n_u8(0x0f);
	uint8x16_t q8_0x40 = vdupq_n_u8(0x40);

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u32 *fb_line = (const u32 *) src;
		u8 *prelim_target_line = dst_prelim_target;
		const u8 *ioctl_hints_line = ioctl_hints;
		u8 *hints_line = hints;

		uint8x16_t q8_dither_pattern0, q8_dither_pattern1, q8_tmp;
		if (dithering_texture_size_hint == 32) {
			q8_dither_pattern0 =
				vld1q_u8(dithering_texture + 32 * (y & 31));
			q8_dither_pattern1 =
				vld1q_u8(dithering_texture + 32 * (y & 31));
			if (src_start_x & 16) {
				q8_tmp = q8_dither_pattern0;
				q8_dither_pattern0 = q8_dither_pattern1;
				q8_dither_pattern1 = q8_tmp;
			}
		} else {
			q8_dither_pattern0 = vld1q_u8(
				dithering_texture +
				16 * (y & (dithering_texture_size_hint - 1)));
			q8_dither_pattern1 = q8_dither_pattern0;
		}

		for (x = src_start_x; x < src_end_x; x += 16, fb_line += 16,
		    prelim_target_line -= 16, ioctl_hints_line += 16,
		    hints_line -= 16) {
			uint8x8_t q8s_gray1, q8s_gray2;
			uint8x16_t q8_gray;
			uint16x8_t q16_gray;

			// RGB -> Y8 using rounded YUV
			// Load 16 RGB values
			uint8x16x4_t q8x4_rgba = vld4q_u8((const u8 *) fb_line);
			q16_gray = vmull_u8(q8_yuv_r, vget_high_u8(q8x4_rgba.val[0]));
			q16_gray =
				vmlal_u8(q16_gray, q8_yuv_g, vget_high_u8(q8x4_rgba.val[1]));
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, vget_high_u8(q8x4_rgba.val[2]));
			// Aa Bb Cc Dd lower-case bits still need to be discarded
			q8s_gray1 = vshrn_n_u16(q16_gray, 8);

			// Same for the next 8 RGB pixels
			__builtin_prefetch(fb_line + pixel_pitch, 0, 0);
			q16_gray = vmull_u8(q8_yuv_r, vget_low_u8(q8x4_rgba.val[0]));
			q16_gray =
				vmlal_u8(q16_gray, q8_yuv_g, vget_low_u8(q8x4_rgba.val[1]));
			q16_gray = vmlal_u8(q16_gray, q8_yuv_b, vget_low_u8(q8x4_rgba.val[2]));
			/* __builtin_prefetch(fb_line + pixel_pitch, 0, 0); */
			q8s_gray2 = vshrn_n_u16(q16_gray, 8);

			// Combine into single vector, discard lower bits
			// 0A 0B 0C 0D
			q8_gray = vshrq_n_u8(vcombine_u8(q8s_gray1, q8s_gray2),
					     4);

			uint8x16_t q8_hint = vld1q_u8(ioctl_hints_line);
			/* __builtin_prefetch(ioctl_hints_line + pixel_pitch, 0, 0); */
			uint8x16_t q8_hint_dither = vtstq_u8(q8_hint, q8_0x40);
			uint8x16_t q8_hint_waveform =
				vandq_u8(vshrq_n_u8(q8_hint, 4), q8_0x03);
			uint8x16_t q8_hint_gray = vorrq_u8(q8_gray, q8_hint);

			uint8x16_t q8_gray_dithered = vminq_u8(
				vqsubq_u8(vqaddq_u8(q8_gray,
						    q8_dither_pattern0),
					  q8_0x08),
				q8_0x0f);
			uint8x16_t q8_gray_y2_dt =
				vqtbl2q_u8(q8_thresholds_y2_dither_table,
					   q8_gray_dithered);
			uint8x16_t q8_gray_y2_th =
				vqtbl1q_u8(q8_thresholds_y2_table, q8_gray);
			uint8x16_t q8_gray_y1_dt = vandq_u8(
				vcgeq_u8(q8_gray_dithered, q8_threshold_y1),
				q8_0x0f);
			uint8x16_t q8_gray_y1_th = vandq_u8(
				vcgeq_u8(q8_gray, q8_threshold_y1), q8_0x0f);
			uint8x16_t q8_gray_y1 = vbslq_u8(
				q8_hint_dither, q8_gray_y1_dt, q8_gray_y1_th);
			uint8x16_t q8_gray_y2 = vbslq_u8(
				q8_hint_dither, q8_gray_y2_dt, q8_gray_y2_th);
			uint8x16_t q8_gray_y12 =
				vbslq_u8(vceqzq_u8(q8_hint_waveform),
					 q8_gray_y1, q8_gray_y2);

			uint8x16_t q8_target =
				vbslq_u8(vceqq_u8(q8_hint_waveform, q8_0x02),
					 q8_gray, q8_gray_y12);
			// TODO: make dithering/thresholding configurable via a hint
			uint8x16_t q8_prelim_target = vorrq_u8(
				vshlq_n_u8(q8_gray_y1_dt, 4), q8_target);

			q8_prelim_target = vrev64q_u8(q8_prelim_target);
			q8_hint_gray = vrev64q_u8(q8_hint_gray);
			__builtin_prefetch(prelim_target_line + prefetch_pitch, 1, 0); // Maybe 1, 1 to keep in L3 cache
			__builtin_prefetch(hints_line + prefetch_pitch, 1, 0);
			vst1q_u8(hints_line, vcombine_u8(vget_high_u8(q8_hint_gray), vget_low_u8(q8_hint_gray)));
			vst1q_u8(prelim_target_line, q8_prelim_target);

			// Swap to support 32x32 dithering textures
			q8_tmp = q8_dither_pattern0;
			q8_dither_pattern0 = q8_dither_pattern1;
			q8_dither_pattern1 = q8_tmp;
		}

		src += rgba_pitch;
		dst_prelim_target += pixel_pitch;
		ioctl_hints += pixel_pitch;
		hints += pixel_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_xrgb8888_y4_hints_neon);

/**
 * rockchip_ebc_blit_fb_r8_y4_hints_neon - blit from R8 FB
 *
 * Flips output horizontally.
 * HHHH 4444  (hints in higher bits, Y4 in lower bits)
 */
void rockchip_ebc_blit_fb_r8_y4_hints_neon(const struct rockchip_ebc *ebc,
					   struct drm_rect *dst_clip,
					   u8 *prelim_target_atomic_update,
					   u8 *hints_atomic_update,
					   const void *vaddr,
					   const struct drm_framebuffer *fb,
					   const struct drm_rect *src_clip)
{
	unsigned int r8_pitch = fb->pitches[0];
	unsigned int pixel_pitch = ebc->pixel_pitch;
	unsigned int prefetch_pitch = (pixel_pitch << 6) >> 6;

	// 16 byte block size for NEON
	unsigned int src_start_x = src_clip->x1;
	unsigned int src_end_x = src_clip->x2;
	unsigned int dst_start_x = dst_clip->x2 - 16;
	unsigned int x, y;

	// Force horizontal reflection for simplicity
	u8 *dst_prelim_target = prelim_target_atomic_update +
				dst_clip->y1 * pixel_pitch + dst_start_x;
	const u8 *src = vaddr + src_clip->y1 * r8_pitch +
			src_start_x * fb->format->cpp[0];
	const u8 *ioctl_hints =
		ebc->hints_ioctl + src_clip->y1 * pixel_pitch + src_start_x;
	u8 *hints =
		hints_atomic_update + dst_clip->y1 * pixel_pitch + dst_start_x;

	// Thresholds and LUT for 4-level tresholding and 2-level threshold
	uint8x16_t q8_thresholds_y2_table = vld1q_u8((u8 *)ebc->lut_y2_y4);
	uint8x16x2_t q8_thresholds_y2_dither_table =
		vld2q_u8((u8 *)ebc->lut_y2_y4_dithered);
	uint8x16_t q8_threshold_y1 = vdupq_n_u8(ebc->y4_threshold_y1);

	const u8 *dithering_texture = ebc->dithering_texture;
	u8 dithering_texture_size_hint = ebc->dithering_texture_size_hint;

	uint8x16_t q8_0x02 = vdupq_n_u8(0x02);
	uint8x16_t q8_0x03 = vdupq_n_u8(0x03);
	uint8x16_t q8_0x08 = vdupq_n_u8(0x08);
	uint8x16_t q8_0x0f = vdupq_n_u8(0x0f);
	uint8x16_t q8_0x40 = vdupq_n_u8(0x40);

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u8 *fb_line = src;
		u8 *prelim_target_line = dst_prelim_target;
		const u8 *ioctl_hints_line = ioctl_hints;
		u8 *hints_line = hints;

		uint8x16_t q8_dither_pattern0, q8_dither_pattern1, q8_tmp;
		if (dithering_texture_size_hint == 32) {
			q8_dither_pattern0 =
				vld1q_u8(dithering_texture + 32 * (y & 31));
			q8_dither_pattern1 =
				vld1q_u8(dithering_texture + 32 * (y & 31));
			if (src_start_x & 16) {
				q8_tmp = q8_dither_pattern0;
				q8_dither_pattern0 = q8_dither_pattern1;
				q8_dither_pattern1 = q8_tmp;
			}
		} else {
			q8_dither_pattern0 = vld1q_u8(
				dithering_texture +
				16 * (y & (dithering_texture_size_hint - 1)));
			q8_dither_pattern1 = q8_dither_pattern0;
		}

		for (x = src_start_x; x < src_end_x; x += 16, fb_line += 16,
		    prelim_target_line -= 16, ioctl_hints_line += 16,
		    hints_line -= 16) {
			// Load 16 RGB values
			uint8x16_t q8_gray = vld1q_u8(fb_line);
			/* __builtin_prefetch(fb_line + pixel_pitch, 0, 0); */

			// Discard lower bits
			// 0A 0B 0C 0D
			q8_gray = vshrq_n_u8(q8_gray, 4);

			uint8x16_t q8_hint = vld1q_u8(ioctl_hints_line);
			/* __builtin_prefetch(ioctl_hints_line + pixel_pitch, 0, 0); */
			uint8x16_t q8_hint_dither = vtstq_u8(q8_hint, q8_0x40);
			uint8x16_t q8_hint_waveform =
				vandq_u8(vshrq_n_u8(q8_hint, 4), q8_0x03);
			uint8x16_t q8_hint_gray = vorrq_u8(q8_gray, q8_hint);

			uint8x16_t q8_gray_dithered = vminq_u8(
				vqsubq_u8(vqaddq_u8(q8_gray,
						    q8_dither_pattern0),
					  q8_0x08),
				q8_0x0f);
			uint8x16_t q8_gray_y2_dt =
				vqtbl2q_u8(q8_thresholds_y2_dither_table,
					   q8_gray_dithered);
			uint8x16_t q8_gray_y2_th =
				vqtbl1q_u8(q8_thresholds_y2_table, q8_gray);
			uint8x16_t q8_gray_y1_dt = vandq_u8(
				vcgeq_u8(q8_gray_dithered, q8_threshold_y1),
				q8_0x0f);
			uint8x16_t q8_gray_y1_th = vandq_u8(
				vcgeq_u8(q8_gray, q8_threshold_y1), q8_0x0f);
			uint8x16_t q8_gray_y1 = vbslq_u8(
				q8_hint_dither, q8_gray_y1_dt, q8_gray_y1_th);
			uint8x16_t q8_gray_y2 = vbslq_u8(
				q8_hint_dither, q8_gray_y2_dt, q8_gray_y2_th);
			uint8x16_t q8_gray_y12 =
				vbslq_u8(vceqzq_u8(q8_hint_waveform),
					 q8_gray_y1, q8_gray_y2);

			uint8x16_t q8_target =
				vbslq_u8(vceqq_u8(q8_hint_waveform, q8_0x02),
					 q8_gray, q8_gray_y12);
			// TODO: make dithering/thresholding configurable via a hint
			uint8x16_t q8_prelim_target = vorrq_u8(
				vshlq_n_u8(q8_gray_y1_dt, 4), q8_target);

			q8_prelim_target = vrev64q_u8(q8_prelim_target);
			q8_hint_gray = vrev64q_u8(q8_hint_gray);
			__builtin_prefetch(prelim_target_line + prefetch_pitch, 1, 0); // Maybe 1, 1 to keep in L3 cache
			__builtin_prefetch(hints_line + prefetch_pitch, 1, 0);
			vst1q_u8(hints_line, vcombine_u8(vget_high_u8(q8_hint_gray), vget_low_u8(q8_hint_gray)));
			vst1q_u8(prelim_target_line,
				  vcombine_u8(vget_high_u8(q8_prelim_target), vget_low_u8(q8_prelim_target)));

			// Swap to support 32x32 dithering textures
			q8_tmp = q8_dither_pattern0;
			q8_dither_pattern0 = q8_dither_pattern1;
			q8_dither_pattern1 = q8_tmp;
		}

		src += r8_pitch;
		dst_prelim_target += pixel_pitch;
		ioctl_hints += pixel_pitch;
		hints += pixel_pitch;
	}
}
EXPORT_SYMBOL(rockchip_ebc_blit_fb_r8_y4_hints_neon);

MODULE_LICENSE("GPL v2");
