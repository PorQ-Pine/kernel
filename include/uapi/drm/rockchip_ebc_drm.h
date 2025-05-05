#ifndef __ROCKCHIP_EBC_DRM_H__
#define __ROCKCHIP_EBC_DRM_H__

#include "drm.h"
#include "drm/drm_mode.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define ROCKCHIP_EBC_HINT_BIT_DEPTH_Y1 0 << 4
#define ROCKCHIP_EBC_HINT_BIT_DEPTH_Y2 1 << 4
#define ROCKCHIP_EBC_HINT_BIT_DEPTH_Y4 2 << 4
#define ROCKCHIP_EBC_HINT_BIT_DEPTH_MASK 3 << 4
#define ROCKCHIP_EBC_HINT_THRESHOLD 0 << 6
#define ROCKCHIP_EBC_HINT_DITHER 1 << 6
#define ROCKCHIP_EBC_HINT_REDRAW 1 << 7
#define ROCKCHIP_EBC_HINT_MASK 0xf0

#define ROCKCHIP_EBC_DRIVER_MODE_NORMAL			0
#define ROCKCHIP_EBC_DRIVER_MODE_FAST			1
#define ROCKCHIP_EBC_DRIVER_MODE_ZERO_WAVEFORM		8

#define ROCKCHIP_EBC_DITHER_MODE_BAYER		0
#define ROCKCHIP_EBC_DITHER_MODE_BLUE_NOISE_16	1
#define ROCKCHIP_EBC_DITHER_MODE_BLUE_NOISE_32	2

struct drm_rockchip_ebc_trigger_global_refresh {
	bool trigger_global_refresh;
};

/**
 * struct drm_rockchip_ebc_off_screen - Pointer to userspace buffer.
 * @info1: unused.
 * @ptr_screen_content: pointer to width * height * buffer containing
 *   the horizontally flipped off screen. The highest four bits of each
 *   byte are ignored.
 */
struct drm_rockchip_ebc_off_screen {
	__u64 info1;
	__u64 ptr_screen_content;
};

/**
 * struct drm_rockchip_ebc_extract_fbs - Pointers to userspace buffers. Pointers
   that are NULL are skipped without warning.
 * @ptr_packed_inner_outer_nextprev: pointer to 3 * width * height buffer
 *   containing packed tuples of inner counters, outer counters, and a
 *   packed Y4 (next, prev) pairs.
 * @ptr_hints: pointer to width * height buffer for hints.
 * @ptr_prelim_target: pointer to width * height buffer containing packed Y4
     (prelim, target) pairs.
 * @ptr_phase1: pointer to width * height / 4 buffer containing the first of
     the double-buffered phase buffers.
 * @ptr_phase2: pointer to width * height / 4 buffer containing the second of
     the double-buffered phase buffers.
 */
struct drm_rockchip_ebc_extract_fbs {
	char *ptr_packed_inner_outer_nextprev;
	char *ptr_hints;
	char *ptr_prelim_target;
	char *ptr_phase1;
	char *ptr_phase2;
};

/**
 * struct drm_rockchip_ebc_rect_hint - Pixels hint definition
 * @hints: Pixel hints for all pixel in the rectangle.
 * @padding: Padding for 64bit alignment.
 * @rect: Rectangular region on which the hints should be applied.
 */
struct drm_rockchip_ebc_rect_hint {
	__u8		hints;
	__u8		padding[7];
	struct drm_mode_rect	rect;
};

/**
  * struct drm_mode_rect_hints
  * @set_default_hint: Reset hints to their default before applying new ones.
  * @default_hint: Hint to use for uncovered pixels.
  * @padding: 64bit alignment padding.
  * @num_rects: Number of rectangles contained in rect_hints
  * @rect_hints: Pointer to an array of struct drm_rockchip_ebc_rect_hint.
  */
struct drm_rockchip_ebc_rect_hints {
	__u8	set_default_hint;
	__u8	default_hint;
	__u8	padding[2];
	__u32	num_rects;
	__u64	rect_hints;
};

/**
 * struct drm_rockchip_ebc_mode - Query and set driver/dither  modes and
 *   redraw delay
 * @set_driver_mode: apply driver_mode instead of reading it to the same
 *   field.
 * @driver_mode: one of ROCKCHIP_EBC_DRIVER_MODE_NORMAL or
 *   ROCKCHIP_EBC_DRIVER_MODE_FAST.
 * @set_dither_mode: apply dither_mode instead of reading it to the same
 *   field.
 * @dither_mode: one of ROCKCHIP_EBC_DITHER_MODE_BAYER,
 *   ROCKCHIP_EBC_DITHER_MODE_BLUE_NOISE_16, or
 *   ROCKCHIP_EBC_DITHER_MODE_BLUE_NOISE_32.
 * @redraw_delay: number of hardware frames to delay redraws.
 * @set_redraw_delay: apply redraw_delay instead of reading it to the same
 *  field.
 */
struct drm_rockchip_ebc_mode {
	__u8	set_driver_mode;
	__u8	driver_mode;
	__u8	set_dither_mode;
	__u8	dither_mode;
	__u16	redraw_delay;
	__u8	set_redraw_delay;
	__u8	_pad;
};

/**
 * struct drm_rockchip_ebc_zero_waveform- Query and enable/disable zero
 *   waveform mode
 * @set_zero_waveform_mode: apply zero_waveform_mode instead of reading it to
 *   the same field.
 * @zero_waveform_mode: 0 for disable(d), 1 for enable(d)
 */
struct drm_rockchip_ebc_zero_waveform {
	__u8	set_zero_waveform_mode;
	__u8	zero_waveform_mode;
	__u8	_pad[6];
};

#define DRM_ROCKCHIP_EBC_NUM_IOCTLS		0x06

#define DRM_IOCTL_ROCKCHIP_EBC_GLOBAL_REFRESH	DRM_IOWR(DRM_COMMAND_BASE + 0x00, struct drm_rockchip_ebc_trigger_global_refresh)
#define DRM_IOCTL_ROCKCHIP_EBC_OFF_SCREEN	DRM_IOW(DRM_COMMAND_BASE + 0x01, struct drm_rockchip_ebc_off_screen)
#define DRM_IOCTL_ROCKCHIP_EBC_EXTRACT_FBS	DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct drm_rockchip_ebc_extract_fbs)
#define DRM_IOCTL_ROCKCHIP_EBC_RECT_HINTS	DRM_IOW(DRM_COMMAND_BASE + 0x03, struct drm_rockchip_ebc_rect_hints)
#define DRM_IOCTL_ROCKCHIP_EBC_MODE		DRM_IOWR(DRM_COMMAND_BASE + 0x04, struct drm_rockchip_ebc_mode)
#define DRM_IOCTL_ROCKCHIP_EBC_ZERO_WAVEFORM	DRM_IOWR(DRM_COMMAND_BASE + 0x05, struct drm_rockchip_ebc_zero_waveform)

#if defined(__cplusplus)
}
#endif

#endif /* __ROCKCHIP_EBC_DRM_H__*/
