/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
   Copyright (C) 2025 hrdl <git@hrdl.eu>
 */

#ifndef _ROCKCHIP_EBC_H
#define _ROCKCHIP_EBC_H

#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

#define ROCKCHIP_EBC_CUSTOM_WF_NUM_SEQS 6
#define ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT 6
#define ROCKCHIP_EBC_CUSTOM_WF_SEQ_LENGTH (1 << ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT)
#define ROCKCHIP_EBC_CUSTOM_WF_LUT_SIZE (16 * 16 * ROCKCHIP_EBC_CUSTOM_WF_SEQ_LENGTH)

// The order matters is related to various buffer formats and NEON optimisations

#define ROCKCHIP_EBC_CUSTOM_WF_DU 0
#define ROCKCHIP_EBC_CUSTOM_WF_DU4 1
#define ROCKCHIP_EBC_CUSTOM_WF_GL16 2
#define ROCKCHIP_EBC_CUSTOM_WF_GC16 3
#define ROCKCHIP_EBC_CUSTOM_WF_INIT 4
#define ROCKCHIP_EBC_CUSTOM_WF_WAITING 5

#define EBC_NUM_SUPPLIES		3

/*
 * @lower_temp: Lower temperature limit for this waveform
 * @upper_temp: Upper temperature limit for this waveform
 * @offsets: offsets for DU, DU4, GL16, GC16, INIT
 * @lut: compact LUT containing phase information (00, 01, 10) and inner nums at every outer num. Sequences start at offsets, are terminated by 0x00, and are index by src, dst, outer_num
 */
struct drm_epd_lut_temp_v2 {
	s32	temp_lower;
	s32	temp_upper;
	// 16 bytes for NEON loading
	u8	offsets[16];
	u8	lut[ROCKCHIP_EBC_CUSTOM_WF_LUT_SIZE];
};

struct drm_epd_lut_v2 {
	// A single LUT struct for every temperature range
	unsigned int			num_temp_ranges;
	struct drm_epd_lut_temp_v2	*luts;
};

struct rockchip_ebc {
	struct clk			*dclk;
	struct clk			*hclk;
	struct clk			*cpll_333m;

	// Hardware-related display timings
	u32				dsp_start;
	u16				act_width;
	u16				act_height;
	u16				hact_start;
	u16				vact_start;

	// ebc: indicate frame completion
	struct completion		display_end;
	struct iio_channel		*temperature_channel;
	struct regmap			*regmap;
	struct regulator_bulk_data	supplies[EBC_NUM_SUPPLIES];

	// DRM
	struct drm_crtc			crtc;
	struct drm_device		drm;
	struct drm_encoder		encoder;
	struct drm_plane		plane;

	// LUT holding waveform information
	struct drm_epd_lut		lut;
	struct drm_epd_lut_file		lut_file;
	struct drm_epd_lut_v2		lut_custom;
	struct drm_epd_lut_temp_v2	*lut_custom_active;
	u8				inner_15_0;
	u8				inner_0_15;

	struct task_struct		*temp_upd_thread;
	struct task_struct		*refresh_thread;

	struct drm_rect			screen_rect;
	// final buffer to display before suspending
	u8				*final_off_screen;
	spinlock_t			hints_ioctl_lock;
	// Used to keep next/prev in 3WIN mode at zero. Read by controller
	u8				*zero;
	u8				*hardware_wf;
	// inner: PPLI IIIII, indicating the current phase (0, 1, 2), whether this is the (L)ast entry, and the number of frames left until a phase shift is required
	// outer: indicating the position in the driver-based LUT
	// next_prev: NNNN PPPP, dispaly state after/before the ongoing/previous update finishes
	u8				*packed_inner_outer_nextprev;
	// Hints written by ioctl_rect_hints (for now) and read by rockchip_ebc_plane_atomic_update
	u8				*hints_ioctl;
	// Phase buffer, 1 pixel per byte in 3WIN mode and 4 in direct mode (not implemented yet). Written by refresh thread and read by controller. Flipped after every display frame
	u8				*phase[2];
	dma_addr_t			phase_handles[2];
	dma_addr_t			zero_handle;
	// Pitches and sizes
	u32				gray4_pitch;
	u32				gray4_size;
	u32				phase_pitch;
	u32				phase_size;
	u32				num_pixels;
	u32				pixel_pitch;
	u32				height;
	// Dithering / thresholding information
	u8				y4_threshold_y1;
	aligned_u64			lut_y2_y4[2];
	aligned_u64			lut_y2_y4_dithered[4];
	const u8			*dithering_texture;
	// 4, 16, or 32 rows. Pitch is 16 for 4 and 16, and 32 for 32
	u8				dithering_texture_size_hint;
	// whether we use direct mode or 3WIN mode. Only 3WIN mode is implemented right now
	bool				direct_mode;
	int				driver_mode;
	int				redraw_delay;
	// Used to change the driver LUT due to temperature update, trigger a global refresh, or buffer changes
	spinlock_t			work_item_lock;
	u32				work_item;
	// used to detect when we are suspending so we can do different things to
	// the ebc display depending on whether we are sleeping or suspending
	int				suspend_was_requested;
	// Cached temperature in deg C
	int				temperature;
	struct drm_rockchip_ebc_phase_sequence	*phase_sequence;
	spinlock_t			phase_sequence_lock;
};

/**
 * struct rockchip_ebc_ctx - DRM-related context for performing display refreshes
 *
 * @kref: Reference count, maintained as part of the CRTC's atomic state
 * @buffer_switch_lock: Lock protecting write-access to refresh_buffer_index
 * @refresh_index: which buffer is being read by the refresh thread
 * @update_index: which buffer is the next one to be written by atomic_update
 * @dst_clip: clip i belonging to buffer i. Independent of the the other buffers
 * @src_clip_extended: the union of clips != i blitted after i was blitted
 * @prelim_target_buffer: triple-buffered prelim_target
 * @hints_buffer: triple-buffered hints_buffer
*/
struct rockchip_ebc_ctx {
	struct kref		kref;
	spinlock_t		buffer_switch_lock;
	int			next_refresh_index;
	int			refresh_index;
	int			update_index;
	struct drm_rect		dst_clip[3];
	struct drm_rect		src_clip_extended[3];
	u8			*prelim_target_buffer[3];
	u8			*hints_buffer[3];
	u8			not_after_others[3];
};

/**
 * struct rockchip_ebc_area - describes a damaged area of the display
 *
 * @list: Used to put this area in the state/context/refresh thread list
 * @clip: The rectangular clip of this damage area
 * @frame_begin: The frame number when this damage area starts being refreshed
 */
struct rockchip_ebc_area {
	struct drm_rect			clip;
};

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

void rockchip_ebc_schedule_advance_fast_neon(const struct rockchip_ebc *ebc,
					const u8 *prelim_target, u8 *hints,
					u8 *phase_buffer,
					struct drm_rect *clip_ongoing,
					struct drm_rect *clip_ongoing_or_waiting,
					u8 early_cancellation_addition,
					u8 force_wf, u8 force_hint,
					u8 force_hint_mask, bool allow_schedule);

void rockchip_ebc_schedule_advance_neon(const struct rockchip_ebc *ebc,
					const u8 *prelim_target, u8 *hints,
					u8 *phase_buffer,
					struct drm_rect *clip_ongoing,
					struct drm_rect *clip_ongoing_or_waiting,
					u8 early_cancellation_addition,
					u8 force_wf, u8 force_hint,
					u8 force_hint_mask, bool allow_schedule);

void rockchip_ebc_blit_y4_high_low_neon(u8 *dst, u8 *y4_high_src,
					u8 *y4_low_src, unsigned int size);

void rockchip_ebc_blit_y421_y4_neon(const struct rockchip_ebc *ebc,
				    u8 *y4_dst, const u8 *y421_src,
				    struct drm_rect *clip);

void rockchip_ebc_blit_fb_rgb565_y4_hints_neon(const struct rockchip_ebc *ebc,
					       struct drm_rect *dst_clip,
					       u8 *prelim_target_atomic_update,
					       u8 *hints_atomic_update,
					       const void *vaddr,
					       const struct drm_framebuffer *fb,
					       const struct drm_rect *src_clip);

void rockchip_ebc_blit_fb_xrgb8888_y4_hints_neon(
	const struct rockchip_ebc *ebc, struct drm_rect *dst_clip,
	u8 *prelim_target_atomic_update, u8 *hints_atomic_update,
	const void *vaddr, const struct drm_framebuffer *fb,
	const struct drm_rect *src_clip);

void rockchip_ebc_blit_fb_r8_y4_hints_neon(const struct rockchip_ebc *ebc,
					   struct drm_rect *dst_clip,
					   u8 *prelim_target_atomic_update,
					   u8 *hints_atomic_update,
					   const void *vaddr,
					   const struct drm_framebuffer *fb,
					   const struct drm_rect *src_clip);

void rockchip_ebc_reset_inner_outer_neon(const struct rockchip_ebc *ebc);

#define DRM_RECT_EMPTY_EXTANDABLE DRM_RECT_INIT(100000, 100000, -100000, -100000);

#endif /* _ROCKCHIP_EBC_H */
