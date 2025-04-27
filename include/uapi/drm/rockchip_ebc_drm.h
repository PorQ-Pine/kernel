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

struct drm_rockchip_ebc_trigger_global_refresh {
	bool trigger_global_refresh;
};

struct drm_rockchip_ebc_off_screen {
	__u64 info1;
	char * ptr_screen_content;
};

struct drm_rockchip_ebc_extract_fbs {
	char *ptr_next_prev;
	char *ptr_hints;
	char * ptr_prelim_target;
	char * ptr_phase1;
	char * ptr_phase2;
	char * ptr_fnum_inner;
	char * ptr_fnum_outer;
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

struct drm_rockchip_ebc_fast_mode {
	u8					fast_mode;
};

#define DRM_ROCKCHIP_EBC_NUM_IOCTLS		0x05

#define DRM_IOCTL_ROCKCHIP_EBC_GLOBAL_REFRESH	DRM_IOWR(DRM_COMMAND_BASE + 0x00, struct drm_rockchip_ebc_trigger_global_refresh)
#define DRM_IOCTL_ROCKCHIP_EBC_OFF_SCREEN	DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct drm_rockchip_ebc_off_screen)
#define DRM_IOCTL_ROCKCHIP_EBC_EXTRACT_FBS	DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct drm_rockchip_ebc_extract_fbs)
#define DRM_IOCTL_ROCKCHIP_EBC_RECT_HINTS	DRM_IOW(DRM_COMMAND_BASE + 0x03, struct drm_rockchip_ebc_rect_hints)
#define DRM_IOCTL_ROCKCHIP_EBC_FAST_MODE	DRM_IOWR(DRM_COMMAND_BASE + 0x04, struct drm_rockchip_ebc_fast_mode)


#endif /* __ROCKCHIP_EBC_DRM_H__*/
