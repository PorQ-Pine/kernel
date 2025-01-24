/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
 */

#ifndef _ROCKCHIP_EBC_H
#define _ROCKCHIP_EBC_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

/**
 * struct rockchip_ebc_ctx - context for performing display refreshes
 *
 * @kref: Reference count, maintained as part of the CRTC's atomic state
 * @queue: Queue of damaged areas to be refreshed
 * @queue_lock: Lock protecting access to @queue
 * @prev: Display contents (Y4) before this refresh
 * @next: Display contents (Y4) after this refresh
 * @final: Display contents (Y4) after all pending refreshes
 * @phase: Buffers for selecting a phase from the EBC's LUT, 1 byte/pixel
 * @gray4_pitch: Horizontal line length of a Y4 pixel buffer in bytes
 * @gray4_size: Size of a Y4 pixel buffer in bytes
 * @phase_pitch: Horizontal line length of a phase buffer in bytes
 * @phase_size: Size of a phase buffer in bytes
 */
struct rockchip_ebc_ctx {
	struct kref			kref;
	struct list_head		queue;

	// see final_ebc/final_atomic_update for current use
	spinlock_t			queue_lock;
	u8				*prev;
	u8				*next;
	// this now is only pointer to either final_buffer[0] or final_buffer[1]
	u8				*final;
	u8				*final_buffer[2];
	u8              *final_atomic_update;
	u8				*phase[2];
	u8				*frame_num[2];
	// which buffer is in use by the ebc thread (0 or 1)?
	int             ebc_buffer_index;
	// the first time we get data from the atomic update function, both final
	// buffers must be filled
	bool            first_switch;
	// we only want to switch between buffers when the buffer content actually
	// changed
	bool            switch_required;
	u32				gray4_pitch;
	u32				gray4_size;
	u32				phase_pitch;
	u32				phase_size;
	u32				frame_num_size;
	u32				frame_num_pitch;
	// either phase_size or frame_num_size
	u32				mapped_win_size;
	u64 area_count;
};

#endif /* _ROCKCHIP_EBC_H */
