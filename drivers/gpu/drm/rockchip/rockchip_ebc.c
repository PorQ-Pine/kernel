// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
 * Copyright (C) 2025 hrdl <git@hrdl.eu>
 */


#include "linux/spinlock.h"
#include <asm/neon.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/iio/consumer.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/rockchip_ebc_drm.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_framebuffer.h>
#include <drm/clients/drm_client_setup.h>

#include "rockchip_ebc.h"

#define EBC_DSP_START			0x0000
#define EBC_DSP_START_DSP_OUT_LOW		BIT(31)
#define EBC_DSP_START_DSP_SDCE_WIDTH(x)		((x) << 16)
#define EBC_DSP_START_DSP_EINK_MODE		BIT(13)
#define EBC_DSP_START_SW_BURST_CTRL		BIT(12)
#define EBC_DSP_START_DSP_FRM_TOTAL(x)		((x) << 2)
#define EBC_DSP_START_DSP_RST			BIT(1)
#define EBC_DSP_START_DSP_FRM_START		BIT(0)
#define EBC_EPD_CTRL			0x0004
#define EBC_EPD_CTRL_EINK_MODE_SWAP		BIT(31)
#define EBC_EPD_CTRL_DSP_GD_END(x)		((x) << 16)
#define EBC_EPD_CTRL_DSP_GD_ST(x)		((x) << 8)
#define EBC_EPD_CTRL_DSP_THREE_WIN_MODE		BIT(7)
#define EBC_EPD_CTRL_DSP_SDDW_MODE		BIT(6)
#define EBC_EPD_CTRL_EPD_AUO			BIT(5)
#define EBC_EPD_CTRL_EPD_PWR(x)			((x) << 2)
#define EBC_EPD_CTRL_EPD_GDRL			BIT(1)
#define EBC_EPD_CTRL_EPD_SDSHR			BIT(0)
#define EBC_DSP_CTRL			0x0008
#define EBC_DSP_CTRL_DSP_SWAP_MODE(x)		((x) << 30)
#define EBC_DSP_CTRL_DSP_DIFF_MODE		BIT(29)
#define EBC_DSP_CTRL_DSP_LUT_MODE		BIT(28)
#define EBC_DSP_CTRL_DSP_VCOM_MODE		BIT(27)
#define EBC_DSP_CTRL_DSP_GDOE_POL		BIT(26)
#define EBC_DSP_CTRL_DSP_GDSP_POL		BIT(25)
#define EBC_DSP_CTRL_DSP_GDCLK_POL		BIT(24)
#define EBC_DSP_CTRL_DSP_SDCE_POL		BIT(23)
#define EBC_DSP_CTRL_DSP_SDOE_POL		BIT(22)
#define EBC_DSP_CTRL_DSP_SDLE_POL		BIT(21)
#define EBC_DSP_CTRL_DSP_SDCLK_POL		BIT(20)
#define EBC_DSP_CTRL_DSP_SDCLK_DIV(x)		((x) << 16)
#define EBC_DSP_CTRL_DSP_BACKGROUND(x)		((x) << 0)
#define EBC_DSP_HTIMING0		0x000c
#define EBC_DSP_HTIMING0_DSP_HTOTAL(x)		((x) << 16)
#define EBC_DSP_HTIMING0_DSP_HS_END(x)		((x) << 0)
#define EBC_DSP_HTIMING1		0x0010
#define EBC_DSP_HTIMING1_DSP_HACT_END(x)	((x) << 16)
#define EBC_DSP_HTIMING1_DSP_HACT_ST(x)		((x) << 0)
#define EBC_DSP_VTIMING0		0x0014
#define EBC_DSP_VTIMING0_DSP_VTOTAL(x)		((x) << 16)
#define EBC_DSP_VTIMING0_DSP_VS_END(x)		((x) << 0)
#define EBC_DSP_VTIMING1		0x0018
#define EBC_DSP_VTIMING1_DSP_VACT_END(x)	((x) << 16)
#define EBC_DSP_VTIMING1_DSP_VACT_ST(x)		((x) << 0)
#define EBC_DSP_ACT_INFO		0x001c
#define EBC_DSP_ACT_INFO_DSP_HEIGHT(x)		((x) << 16)
#define EBC_DSP_ACT_INFO_DSP_WIDTH(x)		((x) << 0)
#define EBC_WIN_CTRL			0x0020
#define EBC_WIN_CTRL_WIN2_FIFO_THRESHOLD(x)	((x) << 19)
#define EBC_WIN_CTRL_WIN_EN			BIT(18)
#define EBC_WIN_CTRL_AHB_INCR_NUM_REG(x)	((x) << 13)
#define EBC_WIN_CTRL_AHB_BURST_REG(x)		((x) << 10)
#define EBC_WIN_CTRL_WIN_FIFO_THRESHOLD(x)	((x) << 2)
#define EBC_WIN_CTRL_WIN_FMT_Y4			(0x0 << 0)
#define EBC_WIN_CTRL_WIN_FMT_Y8			(0x1 << 0)
#define EBC_WIN_CTRL_WIN_FMT_XRGB8888		(0x2 << 0)
#define EBC_WIN_CTRL_WIN_FMT_RGB565		(0x3 << 0)
#define EBC_WIN_MST0			0x0024
#define EBC_WIN_MST1			0x0028
#define EBC_WIN_VIR			0x002c
#define EBC_WIN_VIR_WIN_VIR_HEIGHT(x)		((x) << 16)
#define EBC_WIN_VIR_WIN_VIR_WIDTH(x)		((x) << 0)
#define EBC_WIN_ACT			0x0030
#define EBC_WIN_ACT_WIN_ACT_HEIGHT(x)		((x) << 16)
#define EBC_WIN_ACT_WIN_ACT_WIDTH(x)		((x) << 0)
#define EBC_WIN_DSP			0x0034
#define EBC_WIN_DSP_WIN_DSP_HEIGHT(x)		((x) << 16)
#define EBC_WIN_DSP_WIN_DSP_WIDTH(x)		((x) << 0)
#define EBC_WIN_DSP_ST			0x0038
#define EBC_WIN_DSP_ST_WIN_DSP_YST(x)		((x) << 16)
#define EBC_WIN_DSP_ST_WIN_DSP_XST(x)		((x) << 0)
#define EBC_INT_STATUS			0x003c
#define EBC_INT_STATUS_DSP_FRM_INT_NUM(x)	((x) << 12)
#define EBC_INT_STATUS_LINE_FLAG_INT_CLR	BIT(11)
#define EBC_INT_STATUS_DSP_FRM_INT_CLR		BIT(10)
#define EBC_INT_STATUS_DSP_END_INT_CLR		BIT(9)
#define EBC_INT_STATUS_FRM_END_INT_CLR		BIT(8)
#define EBC_INT_STATUS_LINE_FLAG_INT_MSK	BIT(7)
#define EBC_INT_STATUS_DSP_FRM_INT_MSK		BIT(6)
#define EBC_INT_STATUS_DSP_END_INT_MSK		BIT(5)
#define EBC_INT_STATUS_FRM_END_INT_MSK		BIT(4)
#define EBC_INT_STATUS_LINE_FLAG_INT_ST		BIT(3)
#define EBC_INT_STATUS_DSP_FRM_INT_ST		BIT(2)
#define EBC_INT_STATUS_DSP_END_INT_ST		BIT(1)
#define EBC_INT_STATUS_FRM_END_INT_ST		BIT(0)
#define EBC_VCOM0			0x0040
#define EBC_VCOM1			0x0044
#define EBC_VCOM2			0x0048
#define EBC_VCOM3			0x004c
#define EBC_CONFIG_DONE			0x0050
#define EBC_CONFIG_DONE_REG_CONFIG_DONE		BIT(0)
#define EBC_VNUM			0x0054
#define EBC_VNUM_DSP_VCNT(x)			((x) << 16)
#define EBC_VNUM_LINE_FLAG_NUM(x)		((x) << 0)
#define EBC_WIN_MST2			0x0058
#define EBC_LUT_DATA			0x1000

#define EBC_FRAME_PENDING		-1

#define EBC_MAX_PHASES			256

#define EBC_NUM_LUT_REGS		0x1000

#define EBC_FRAME_TIMEOUT		msecs_to_jiffies(25)
#define EBC_REFRESH_TIMEOUT		msecs_to_jiffies(3000)
#define EBC_SUSPEND_DELAY_MS		2000

#define EBC_FIRMWARE		"rockchip/ebc.wbf"
MODULE_FIRMWARE(EBC_FIRMWARE);
#define EBC_OFFCONTENT "rockchip/rockchip_ebc_default_screen.bin"
MODULE_FIRMWARE(EBC_OFFCONTENT);
#define EBC_CUSTOM_WF "rockchip/custom_wf.bin"
MODULE_FIRMWARE(EBC_CUSTOM_WF);

static const char *custom_wf_magic_version = "CLUT0002";

#define ROCKCHIP_EBC_WORK_ITEM_CHANGE_LUT	1
#define ROCKCHIP_EBC_WORK_ITEM_GLOBAL_REFRESH	2
#define ROCKCHIP_EBC_WORK_ITEM_INIT		4
#define ROCKCHIP_EBC_WORK_ITEM_SUSPEND		8
#define ROCKCHIP_EBC_WORK_ITEM_RESCHEDULE 16
#define ROCKCHIP_EBC_WORK_ITEM_ENABLE_FAST_MODE 32
#define ROCKCHIP_EBC_WORK_ITEM_DISABLE_FAST_MODE 64

static const u8 dither_bayer_04[] = {
	7, 8, 2, 10, 7, 8, 2, 10, 7, 8, 2, 10, 7, 8, 2, 10,
	12, 4, 14, 6, 12, 4, 14, 6, 12, 4, 14, 6, 12, 4, 14, 6,
	3, 11, 1, 9, 3, 11, 1, 9, 3, 11, 1, 9, 3, 11, 1, 9,
	15, 7, 13, 5, 15, 7, 13, 5, 15, 7, 13, 5, 15, 7, 13, 5,
};

// https://momentsingraphics.de/BlueNoise.html : 16_16/LDR_LLL1_0.png >> 4
static const u8 dither_blue_noise_16[] = {
	6, 3, 8, 10, 7, 12, 4, 11, 12, 3, 9, 5, 4, 2, 5, 15,
	1, 6, 14, 13, 2, 15, 9, 1, 2, 6, 13, 10, 12, 8, 0, 10,
	7, 11, 4, 0, 4, 10, 7, 5, 13, 8, 15, 1, 7, 3, 14, 13,
	2, 12, 9, 8, 11, 6, 3, 14, 10, 3, 0, 11, 4, 15, 9, 4,
	0, 15, 3, 5, 14, 0, 12, 1, 11, 6, 9, 12, 2, 5, 11, 6,
	13, 10, 7, 2, 13, 9, 8, 4, 15, 5, 14, 3, 7, 9, 1, 8,
	5, 12, 1, 15, 4, 2, 11, 7, 0, 2, 10, 6, 15, 11, 13, 3,
	6, 11, 9, 7, 10, 6, 14, 8, 13, 9, 12, 0, 4, 1, 14, 2,
	14, 1, 4, 0, 12, 3, 1, 12, 5, 3, 7, 13, 8, 5, 7, 9,
	13, 8, 15, 10, 14, 6, 2, 15, 10, 1, 14, 11, 3, 12, 10, 0,
	6, 11, 3, 5, 8, 11, 9, 4, 2, 8, 6, 9, 2, 15, 5, 3,
	1, 4, 13, 2, 0, 4, 14, 7, 12, 15, 0, 4, 7, 1, 14, 8,
	15, 10, 7, 12, 15, 6, 9, 0, 13, 10, 6, 13, 12, 5, 12, 10,
	1, 5, 9, 1, 10, 11, 3, 1, 5, 4, 2, 8, 10, 3, 7, 2,
	13, 14, 3, 8, 5, 14, 13, 7, 9, 15, 11, 1, 15, 6, 0, 8,
	4, 11, 0, 13, 2, 6, 0, 8, 14, 5, 0, 7, 14, 12, 9, 11,
};

// https://momentsingraphics.de/BlueNoise.html : 32_32/LDR_LLL1_0.png >> 4
static const u8 dither_blue_noise_32[] = {
	9, 10, 13, 15, 9, 12, 13, 14, 8, 15, 2, 3, 15, 9, 6, 0, 15, 7, 3, 5, 4, 11, 14, 3, 7, 1, 4, 6, 9, 12, 5, 4,
	15, 5, 3, 11, 7, 4, 1, 6, 4, 10, 13, 7, 5, 2, 13, 4, 8, 10, 1, 14, 2, 13, 7, 2, 15, 9, 11, 5, 0, 13, 1, 7,
	2, 6, 1, 2, 14, 0, 10, 8, 11, 5, 0, 10, 8, 14, 11, 2, 11, 14, 12, 9, 7, 1, 10, 8, 5, 12, 2, 13, 14, 8, 3, 11,
	9, 14, 13, 8, 6, 9, 13, 2, 15, 1, 9, 14, 1, 4, 10, 6, 5, 0, 6, 4, 15, 11, 5, 0, 13, 3, 7, 10, 7, 2, 10, 14,
	1, 11, 5, 4, 12, 15, 5, 3, 7, 6, 12, 3, 6, 12, 1, 7, 13, 3, 8, 12, 0, 3, 14, 11, 6, 4, 15, 1, 4, 12, 5, 6,
	8, 0, 10, 7, 1, 11, 0, 9, 10, 14, 4, 11, 8, 15, 3, 14, 9, 15, 11, 2, 9, 6, 13, 9, 1, 10, 6, 12, 9, 0, 15, 3,
	4, 15, 9, 14, 3, 7, 4, 14, 13, 0, 8, 2, 5, 0, 10, 7, 2, 1, 5, 7, 13, 4, 7, 2, 11, 8, 14, 2, 8, 11, 13, 9,
	12, 5, 2, 13, 6, 10, 12, 1, 6, 2, 10, 14, 12, 9, 4, 12, 6, 10, 14, 10, 8, 0, 15, 1, 14, 4, 0, 13, 3, 6, 2, 7,
	14, 3, 11, 0, 8, 15, 3, 8, 11, 5, 15, 3, 7, 1, 13, 15, 0, 4, 13, 1, 3, 12, 10, 9, 5, 3, 11, 7, 15, 5, 12, 1,
	10, 6, 8, 5, 1, 12, 5, 2, 10, 9, 1, 13, 11, 5, 3, 8, 11, 8, 7, 5, 15, 6, 4, 13, 7, 15, 9, 6, 0, 10, 3, 8,
	13, 1, 13, 14, 10, 2, 7, 14, 13, 7, 6, 8, 0, 12, 9, 2, 6, 12, 2, 9, 11, 2, 8, 0, 12, 1, 10, 2, 8, 14, 15, 4,
	2, 6, 9, 4, 7, 12, 9, 0, 4, 3, 12, 2, 15, 4, 7, 10, 15, 0, 14, 5, 0, 14, 11, 5, 8, 3, 13, 12, 4, 11, 5, 0,
	12, 15, 11, 3, 1, 15, 5, 9, 13, 1, 14, 10, 6, 9, 14, 1, 4, 3, 10, 13, 8, 6, 12, 3, 15, 6, 4, 9, 1, 6, 9, 7,
	4, 9, 0, 5, 8, 13, 2, 6, 11, 7, 4, 11, 0, 3, 13, 5, 12, 7, 8, 1, 4, 2, 9, 14, 1, 10, 7, 11, 15, 3, 13, 10,
	1, 7, 14, 12, 6, 10, 3, 12, 14, 2, 8, 5, 12, 8, 2, 11, 8, 3, 11, 15, 12, 7, 0, 5, 11, 8, 0, 14, 2, 5, 0, 14,
	6, 4, 10, 2, 15, 1, 8, 5, 0, 10, 15, 4, 1, 15, 10, 0, 14, 13, 0, 5, 6, 11, 15, 9, 4, 2, 12, 5, 11, 9, 8, 12,
	11, 3, 13, 7, 4, 11, 13, 9, 7, 3, 13, 6, 14, 7, 5, 4, 6, 9, 2, 10, 13, 3, 1, 6, 13, 14, 9, 3, 7, 4, 15, 2,
	8, 1, 15, 9, 2, 6, 0, 15, 4, 11, 1, 11, 9, 3, 11, 15, 7, 3, 14, 7, 8, 4, 12, 8, 5, 0, 8, 15, 1, 13, 6, 9,
	13, 11, 5, 0, 14, 10, 8, 3, 12, 6, 8, 4, 0, 12, 1, 10, 0, 12, 5, 11, 1, 15, 13, 2, 10, 3, 6, 11, 1, 12, 3, 0,
	4, 6, 8, 11, 6, 4, 12, 1, 10, 14, 2, 15, 10, 5, 8, 13, 2, 9, 14, 0, 4, 9, 5, 7, 11, 14, 4, 12, 7, 10, 5, 14,
	10, 1, 12, 3, 13, 2, 14, 7, 5, 0, 7, 12, 3, 14, 4, 6, 4, 10, 3, 13, 10, 2, 6, 0, 15, 1, 9, 2, 5, 14, 9, 7,
	2, 15, 7, 1, 10, 8, 0, 11, 9, 13, 4, 7, 11, 0, 9, 12, 15, 1, 6, 7, 8, 14, 12, 10, 3, 7, 8, 15, 0, 3, 1, 13,
	4, 5, 9, 14, 4, 6, 15, 3, 2, 15, 10, 1, 5, 15, 7, 1, 13, 8, 11, 15, 2, 1, 4, 13, 5, 11, 13, 4, 10, 12, 8, 11,
	14, 12, 0, 3, 10, 13, 5, 9, 11, 1, 6, 13, 8, 2, 11, 3, 5, 4, 0, 9, 5, 11, 7, 9, 2, 0, 12, 6, 2, 15, 6, 0,
	8, 10, 13, 7, 11, 2, 0, 12, 6, 8, 14, 9, 3, 14, 6, 9, 14, 12, 14, 3, 10, 13, 3, 6, 15, 8, 3, 10, 7, 5, 9, 3,
	6, 2, 4, 8, 5, 15, 7, 10, 2, 4, 3, 11, 1, 12, 4, 8, 2, 7, 9, 6, 1, 7, 14, 10, 12, 5, 14, 0, 13, 13, 1, 14,
	4, 12, 15, 1, 9, 14, 3, 8, 13, 15, 0, 5, 7, 15, 0, 12, 10, 2, 0, 11, 13, 4, 0, 1, 3, 7, 9, 11, 4, 2, 8, 11,
	14, 7, 10, 0, 6, 11, 4, 0, 6, 12, 10, 9, 13, 10, 6, 3, 15, 5, 14, 7, 15, 9, 8, 11, 13, 2, 15, 1, 6, 7, 12, 0,
	2, 9, 3, 12, 5, 2, 12, 14, 9, 7, 1, 4, 2, 5, 1, 9, 13, 6, 10, 4, 3, 5, 7, 14, 5, 6, 10, 8, 12, 15, 10, 5,
	13, 5, 15, 8, 14, 7, 8, 1, 5, 3, 14, 13, 8, 15, 11, 7, 1, 8, 0, 12, 2, 12, 1, 13, 3, 1, 12, 0, 4, 3, 1, 8,
	11, 1, 4, 11, 0, 10, 15, 11, 9, 12, 7, 0, 10, 6, 3, 14, 4, 11, 14, 6, 8, 15, 4, 9, 10, 8, 5, 14, 7, 9, 13, 6,
	12, 0, 8, 6, 2, 3, 5, 2, 0, 6, 11, 4, 12, 1, 9, 12, 5, 2, 13, 9, 0, 10, 6, 0, 11, 13, 15, 2, 10, 2, 15, 3,
};

static int default_hint = ROCKCHIP_EBC_HINT_BIT_DEPTH_Y4 | ROCKCHIP_EBC_HINT_THRESHOLD | ROCKCHIP_EBC_HINT_REDRAW;
module_param(default_hint, int, 0644);
MODULE_PARM_DESC(default_hint, "hint to use for pixels not covered otherwise");

static int redraw_delay = 0;
module_param(redraw_delay, int, 0644);
MODULE_PARM_DESC(redraw_delay, "number of hardware frames to delay redraws");

static int early_cancellation_addition = 2;
module_param(early_cancellation_addition, int, 0644);
MODULE_PARM_DESC(early_cancellation_addition, "number of additional frames to drive a pixel when cancelling it");

static bool shrink_virtual_window = false;
module_param(shrink_virtual_window, bool, 0644);
MODULE_PARM_DESC(shrink_virtual_window, "shrink virtual window to ongoing clip");

static bool direct_mode = true;
#ifdef CONFIG_DRM_ROCKCHIP_EBC_3WIN_MODE
module_param(direct_mode, bool, 0444);
MODULE_PARM_DESC(direct_mode, "Don't use the controller's 3WIN mode");
#endif

static int limit_fb_blits = -1;
module_param(limit_fb_blits, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(split_area_limit, "how many fb blits to allow. -1 does not limit");

static bool no_off_screen = false;
module_param(no_off_screen, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(no_off_screen, "If set to true, do not apply the off screen on next loop exit");

/* delay parameters used to delay the return of plane_atomic_atomic */
/* see plane_atomic_update function for specific usage of these parameters */
static int delay_a = 200;
module_param(delay_a, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_a, "delay_a");

static int refresh_thread_wait_idle = 2000;
module_param(refresh_thread_wait_idle, int, 0644);
MODULE_PARM_DESC(refresh_thread_wait_idle, "Number of ms to wait and last frame start before stopping the refresh thread");

#define DITHERING_BAYER 0
#define DITHERING_BLUE_NOISE_16 1
#define DITHERING_BLUE_NOISE_32 2

static int dithering_method = 2;
module_param(dithering_method, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(dithering_method, "Dithering method, 0-2");

static int bw_threshold = 7;
module_param(bw_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_threshold, "black and white threshold");

static int y2_dt_thresholds = 0x070f16;
module_param(y2_dt_thresholds, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(y2_dt_thresholds, "int whose lowest three bytes indicate thresholds when dithering");

static int y2_th_thresholds = 0x04080c;
module_param(y2_th_thresholds, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(y2_th_thresholds, "int whose lowest three bytes indicate thresholds");

static int dclk_select = 0;
module_param(dclk_select, int, 0644);
MODULE_PARM_DESC(dclk_select, "-1: use dclk from mode, 0: 200 MHz (default), 1: 250");

static int temp_override = 0;
module_param(temp_override, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(temp_override, "Values > 0 override the temperature");

/* Values for testing should be multiples of 8 and >= 8
 *
 * */
static int hskew_override = 0;
module_param(hskew_override, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(hskew_override, "Override hskew value");

static int testing = 0;

DEFINE_DRM_GEM_FOPS(rockchip_ebc_fops);

static int ioctl_trigger_global_refresh(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_trigger_global_refresh *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);

	if (args->trigger_global_refresh){
		spin_lock(&ebc->work_item_lock);
		ebc->work_item |= ROCKCHIP_EBC_WORK_ITEM_GLOBAL_REFRESH;
		spin_unlock(&ebc->work_item_lock);
		// try to trigger the refresh immediately
		wake_up_process(ebc->refresh_thread);
	}

	return 0;
}

static int ioctl_set_off_screen(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_off_screen *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);
	int copy_result;
	pr_info("rockchip-ebc: ioctl_set_off_screen");

	// TODO: blit
	copy_result = copy_from_user(&ebc->final_off_screen, args->ptr_screen_content, 1313144);
	copy_result = copy_from_user(&ebc->final_off_screen + 1313144, args->ptr_screen_content, 1313144);
	if (copy_result != 0){
		pr_err("Could not copy off screen content from user-supplied data pointer (bytes not copied: %d)", copy_result);
	}

	return 0;
}

struct ebc_crtc_state {
	struct drm_crtc_state		base;
	struct rockchip_ebc_ctx		*ctx;
};

	static inline struct ebc_crtc_state *
to_ebc_crtc_state(struct drm_crtc_state *crtc_state)
{
	return container_of(crtc_state, struct ebc_crtc_state, base);
}
static int ioctl_extract_fbs(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_extract_fbs *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);
	struct rockchip_ebc_ctx *ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;
	int copy_result = 0;

	// todo: use access_ok here
	access_ok(args->ptr_next_prev, ebc->num_pixels);
	// TODO: fix inner/outer size and ioctl
	copy_result |= copy_to_user(args->ptr_next_prev, ebc->packed_inner_outer_nextprev,
				    ebc->num_pixels);
	copy_result |= copy_to_user(args->ptr_hints, ctx->hints_buffer[ctx->refresh_index], ebc->num_pixels);
	copy_result |= copy_to_user(args->ptr_prelim_target, ctx->prelim_target_buffer[ctx->refresh_index], ebc->num_pixels);

	copy_result |= copy_to_user(args->ptr_phase1, ebc->phase[0], ebc->phase_size);
	copy_result |= copy_to_user(args->ptr_phase2, ebc->phase[1], ebc->phase_size);

	return copy_result;
}

static int ioctl_rect_hints(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_rect_hints *rect_hints = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);

	// Alternatively, use separate buffer and only lock when copying final buffer
	spin_lock(&ebc->hints_ioctl_lock);
	ebc->hints_changed = 2;
	if (rect_hints->set_default_hint)
		memset(ebc->hints_ioctl, default_hint & ROCKCHIP_EBC_HINT_MASK, ebc->num_pixels);
	// TODO: verify parameter num_rects, e.g. copy_struct_from_user(data, sizeof(struct TODO), _IOC_SIZE(cmd));
	// TODO: neon blit
	for (int i = 0; i < min(20, rect_hints->num_rects); ++i) {
		struct drm_rockchip_ebc_rect_hint *rect_hint = rect_hints->rect_hints + i;
		struct drm_rect *r = &rect_hint->rect;
		u8 hint = rect_hint->hints & ROCKCHIP_EBC_HINT_MASK;
		for (unsigned int y = max(0, r->y1);
		     y < min(ebc->pixel_pitch, (u32)r->y2); ++y) {
			unsigned int x1 = max(0, r->x1);
			unsigned int x2 = min(ebc->pixel_pitch, (u32)r->x2);
			unsigned int width = min(ebc->pixel_pitch, x2 - x1);
			if (x1 < ebc->pixel_pitch)
				memset(ebc->hints_ioctl + y * ebc->pixel_pitch + x1, hint, width);
		}
	}
	spin_unlock(&ebc->hints_ioctl_lock);

	return 0;
}

static int ioctl_set_fast_mode(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_fast_mode *fast_mode = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);

	spin_lock(&ebc->work_item_lock);
	if (fast_mode->fast_mode) {
		ebc->work_item |= ROCKCHIP_EBC_WORK_ITEM_ENABLE_FAST_MODE;
		ebc->work_item &= ~ROCKCHIP_EBC_WORK_ITEM_DISABLE_FAST_MODE;
	} else {
		ebc->work_item |= ROCKCHIP_EBC_WORK_ITEM_DISABLE_FAST_MODE;
		ebc->work_item &= ~ROCKCHIP_EBC_WORK_ITEM_ENABLE_FAST_MODE;
	}
	spin_unlock(&ebc->work_item_lock);

	return 0;
}

static const struct drm_ioctl_desc ioctls[DRM_COMMAND_END - DRM_COMMAND_BASE] = {
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_GLOBAL_REFRESH,
			  ioctl_trigger_global_refresh, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_OFF_SCREEN, ioctl_set_off_screen,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_EXTRACT_FBS, ioctl_extract_fbs,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_RECT_HINTS, ioctl_rect_hints,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_FAST_MODE, ioctl_set_fast_mode,
			  DRM_RENDER_ALLOW),
};

static const struct drm_driver rockchip_ebc_drm_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	.major			= 0,
	.minor			= 3,
	.name			= "rockchip-ebc",
	.desc			= "Rockchip E-Book Controller",
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &rockchip_ebc_fops,
	.ioctls = ioctls,
	.num_ioctls = DRM_ROCKCHIP_EBC_NUM_IOCTLS,
};

static const struct drm_mode_config_funcs rockchip_ebc_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create_with_dirty,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static void rockchip_ebc_ctx_free(struct rockchip_ebc_ctx *ctx)
{
	pr_info("EBC: rockchip_ebc_ctx_free");

	vfree(ctx->hints_buffer[0]);
	vfree(ctx->hints_buffer[1]);
	vfree(ctx->hints_buffer[2]);
	vfree(ctx->prelim_target_buffer[0]);
	vfree(ctx->prelim_target_buffer[1]);
	vfree(ctx->prelim_target_buffer[2]);

	kfree(ctx);
}

static struct rockchip_ebc_ctx *rockchip_ebc_ctx_alloc(struct rockchip_ebc *ebc)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	struct rockchip_ebc_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->hints_buffer[0] = vmalloc(ebc->num_pixels);
	ctx->hints_buffer[1] = vmalloc(ebc->num_pixels);
	ctx->hints_buffer[2] = vmalloc(ebc->num_pixels);
	ctx->prelim_target_buffer[0] = vmalloc(ebc->num_pixels);
	ctx->prelim_target_buffer[1] = vmalloc(ebc->num_pixels);
	ctx->prelim_target_buffer[2] = vmalloc(ebc->num_pixels);

	if (!(ctx->hints_buffer[0] && ctx->hints_buffer[1] && ctx->hints_buffer[2] && ctx->prelim_target_buffer[0] && ctx->prelim_target_buffer[1] && ctx->prelim_target_buffer[2])) {
		rockchip_ebc_ctx_free(ctx);
		return NULL;
	}

	kref_init(&ctx->kref);
	spin_lock_init(&ctx->buffer_switch_lock);
	for (int i = 0; i < 3; ++i) {
		ctx->dst_clip[i] = DRM_RECT_EMPTY_EXTANDABLE;
		ctx->src_clip_extended[i] = DRM_RECT_EMPTY_EXTANDABLE;
	}

	return ctx;
}

static void rockchip_ebc_ctx_release(struct kref *kref)
{
	struct rockchip_ebc_ctx *ctx =
		container_of(kref, struct rockchip_ebc_ctx, kref);
	pr_info("ebc: %s", __func__);

	return rockchip_ebc_ctx_free(ctx);
}

static void rockchip_ebc_change_lut(struct rockchip_ebc *ebc)
{
	int temp_index;
	struct drm_epd_lut_temp_v2 *lut;
	struct drm_epd_lut_v2 *luts = &ebc->lut_custom;

	for (temp_index = 0; temp_index < luts->num_temp_ranges - 1; ++temp_index) {
		if (ebc->temperature <= luts->luts[temp_index].temp_upper)
			break;
	}
	ebc->lut_custom_active = lut = luts->luts + temp_index;
	int waiting_remaining = redraw_delay;
	for (int i = lut->offsets[ROCKCHIP_EBC_CUSTOM_WF_WAITING]; i < ROCKCHIP_EBC_CUSTOM_WF_SEQ_LENGTH; ++i) {
		int waiting_this = max(0, min(waiting_remaining, 0x1f));
		waiting_remaining -= waiting_this;
		if (!waiting_remaining ||
		    i == ROCKCHIP_EBC_CUSTOM_WF_SEQ_LENGTH - 1)
			waiting_this |= 0x20;
		for (int next = 0; next < 16; ++next) {
			for (int prev = 0; prev < 16; ++prev) {
				lut->lut[(prev << (4 + ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT)) + (next << ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT) + i] = waiting_this;
			}
		}
	}
	// TODO: generalise for temperature ranges for which more than 31 phases are required
	ebc->inner_0_15 = lut->lut[(0xf << ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT) +
		lut->offsets[ROCKCHIP_EBC_CUSTOM_WF_DU]];
	ebc->inner_15_0 =
		lut->lut[(0xf << (ROCKCHIP_EBC_CUSTOM_WF_SEQ_SHIFT + 4)) +
			lut->offsets[ROCKCHIP_EBC_CUSTOM_WF_DU]];
}

static void print_lut(struct rockchip_ebc *ebc)
{
	struct drm_epd_lut_temp_v2 *lut_active = ebc->lut_custom_active;
	u8 *lut = lut_active->lut;
	pr_info("%s temp_lower=%d temp_upper=%d offsets=%16ph", __func__, lut_active->temp_lower, lut_active->temp_upper, lut_active->offsets);
	pr_info("%s lut=%64ph", __func__, lut + 15 * 64);
}

static void rockchip_ebc_partial_refresh(struct rockchip_ebc *ebc,
					 struct rockchip_ebc_ctx *ctx)
{
	struct drm_device *drm = &ebc->drm;
	struct device *dev = drm->dev;
	u32 frame;
	ktime_t time_start_advance;
	ktime_t time_advance_sync;
	ktime_t time_sync_wait;
	ktime_t times_wait_end[2];
	u32 min_frame_delay = 1000000;
	u32 max_frame_delay = 0;
	struct drm_rect clip_incoming = DRM_RECT_EMPTY_EXTANDABLE;
	struct drm_rect clip_ongoing = DRM_RECT_EMPTY_EXTANDABLE;

	struct drm_rect clip_ongoing_or_waiting = clip_ongoing;
	u8 work_item = ebc->work_item;
	ktime_t time_last_start = ktime_get();

	// TODO: move into logic setting these values
	for (int i = 0; i < 16; ++i) {
		u8 _sum = (i >= (y2_th_thresholds & 0xff)) + (i >= ((y2_th_thresholds >> 8) & 0xff)) + (i >= ((y2_th_thresholds >> 16) & 0xff));
		((u8 *) ebc->lut_y2_y4)[i] = (_sum << 2) | _sum;
	}
	for (int i = 0; i < 32; ++i) {
		u8 _sum = (i >= (y2_dt_thresholds & 0xff)) + (i >= ((y2_dt_thresholds >> 8) & 0xff)) + (i >= ((y2_dt_thresholds >> 16) & 0xff));
		((u8 *) ebc->lut_y2_y4_dithered)[i] = (_sum << 2) | _sum;
	}

	spin_lock(&ctx->buffer_switch_lock);
	ctx->refresh_index = ctx->next_refresh_index;
	for (int i = 0; i < 3; ++i) {
		if (ctx->not_after_others[i] & (1 << ctx->refresh_index)) {
			rockchip_ebc_drm_rect_extend_rect(&clip_incoming,
						  ctx->dst_clip + i);
			ctx->dst_clip[i] = DRM_RECT_EMPTY_EXTANDABLE;
		}
	}
	spin_unlock(&ctx->buffer_switch_lock);
	u8 *prelim_target = ctx->prelim_target_buffer[ctx->refresh_index];
	u8 *hints = ctx->hints_buffer[ctx->refresh_index];

	bool awaiting_completion = false;
	bool awaiting_start = false;
	bool no_schedule_until_clip_empty = false;
	bool is_enabling_fast_mode = false, is_disabling_fast_mode = false;
	bool is_suspending = false;
	ktime_t time_last_report = ktime_get();
	int num_frames_since_last_report = 0;
	s64 max_advance_time_since_last_report = 0;

	for (frame = 0;; frame++) {
		u8 *phase_buffer = ebc->phase[frame % 2];
		dma_addr_t phase_handle = ebc->phase_handles[frame % 2];
		// Used to reset the second phase buffer in direct mode after last_phase
		work_item = ebc->work_item;
		bool skip_advance = false;

		time_start_advance = ktime_get();
		// All currently scheduled pixels have finished and we have a work item
		if (drm_rect_width(&clip_ongoing) <= 0 && work_item) {
			// Refresh work item
			spin_lock(&ebc->work_item_lock);
			work_item |= ebc->work_item;
			ebc->work_item = 0;
			spin_unlock(&ebc->work_item_lock);
			if ((work_item &
			     ROCKCHIP_EBC_WORK_ITEM_ENABLE_FAST_MODE) &&
			    !ebc->fast_mode) {
				no_schedule_until_clip_empty = true;
				is_enabling_fast_mode = true;
				work_item |= ROCKCHIP_EBC_WORK_ITEM_GLOBAL_REFRESH;
			} else if ((work_item &
				    ROCKCHIP_EBC_WORK_ITEM_DISABLE_FAST_MODE) &&
				   ebc->fast_mode) {
				no_schedule_until_clip_empty = true;
				is_disabling_fast_mode = true;
			}
			if (work_item & ROCKCHIP_EBC_WORK_ITEM_CHANGE_LUT) {
				rockchip_ebc_change_lut(ebc);
				print_lut(ebc);
			}
			if (work_item & ROCKCHIP_EBC_WORK_ITEM_INIT) {
				clip_ongoing_or_waiting = ebc->screen_rect;
				memset(prelim_target, 0xff, ebc->num_pixels);
				kernel_neon_begin();
				rockchip_ebc_schedule_advance_neon(ebc, prelim_target, hints, phase_buffer, &clip_ongoing, &clip_ongoing_or_waiting, 0, ROCKCHIP_EBC_CUSTOM_WF_INIT, 0, ROCKCHIP_EBC_HINT_REDRAW, true);
				kernel_neon_end();
				skip_advance = true;
				no_schedule_until_clip_empty = true;
			} else if (work_item & ROCKCHIP_EBC_WORK_ITEM_SUSPEND) {
				clip_ongoing_or_waiting = ebc->screen_rect;
				if (!no_off_screen) {
					kernel_neon_begin();
					// Use the highest-quality waveform to minimize visible artifacts
					rockchip_ebc_schedule_advance_neon(ebc, ebc->final_off_screen, hints, phase_buffer, &clip_ongoing, &clip_ongoing_or_waiting, 0, ROCKCHIP_EBC_CUSTOM_WF_GC16, 0, ROCKCHIP_EBC_HINT_REDRAW, true);
					kernel_neon_end();
				}
				no_off_screen = false;
				skip_advance = true;
				no_schedule_until_clip_empty = true;
				is_suspending = true;
				ebc->suspend_was_requested = 1;
			} else if (work_item &
				   ROCKCHIP_EBC_WORK_ITEM_GLOBAL_REFRESH) {
				if (ebc->fast_mode || is_enabling_fast_mode) {
					ebc->fast_mode = false;
					is_enabling_fast_mode = true;
					for (int i = 0; i < ebc->num_pixels; ++i) {
						u8 prelim = prelim_target[i] & 0xf0;
						prelim_target[i] = prelim | prelim >> 4;
					}
				}
				clip_ongoing_or_waiting = ebc->screen_rect;
				kernel_neon_begin();
				rockchip_ebc_schedule_advance_neon(ebc, prelim_target, hints, phase_buffer, &clip_ongoing, &clip_ongoing_or_waiting, 0, ROCKCHIP_EBC_CUSTOM_WF_GC16, 0, ROCKCHIP_EBC_HINT_REDRAW, true);
				kernel_neon_end();
				skip_advance = true;
				no_schedule_until_clip_empty = true;
				ebc->suspend_was_requested = 0;
			}
			work_item = 0;
		} else if (drm_rect_width(&clip_ongoing_or_waiting) <= 0 &&
			   (is_suspending ||
			    ((drm_rect_width(&clip_incoming) <= 0) &&
			     (ktime_ms_delta(ktime_get(), time_last_start) >
			      refresh_thread_wait_idle)))) {
			// Wait before yielding the refresh thread
			is_suspending = false;
			break;
		} else if (!no_schedule_until_clip_empty && !work_item) {
			rockchip_ebc_drm_rect_extend_rect(
							  &clip_ongoing_or_waiting, &clip_incoming);
			clip_incoming = DRM_RECT_EMPTY_EXTANDABLE;
		}
		pr_debug("%s frame=%d clip_ongoing=" DRM_RECT_FMT " clip_ongoing_or_waiting=" DRM_RECT_FMT " work_item=%d no_schedule_until_clip_empty=%d time_elapsed_since_last_start=%llu", __func__, frame, DRM_RECT_ARG(&clip_ongoing), DRM_RECT_ARG(&clip_ongoing_or_waiting), work_item, no_schedule_until_clip_empty, ktime_ms_delta(ktime_get(), time_last_start));
		if (drm_rect_width(&clip_ongoing_or_waiting) > 0 &&
		    !skip_advance) {
			if (ebc->fast_mode) {
				kernel_neon_begin();
				rockchip_ebc_schedule_advance_fast_neon(
					ebc, prelim_target, hints,
					phase_buffer, &clip_ongoing,
					&clip_ongoing_or_waiting,
					early_cancellation_addition, 0, 0, 0,
					!no_schedule_until_clip_empty && !work_item);
				kernel_neon_end();
			} else {
				kernel_neon_begin();
				rockchip_ebc_schedule_advance_neon(
					ebc, prelim_target, hints,
					phase_buffer, &clip_ongoing,
					&clip_ongoing_or_waiting,
					early_cancellation_addition, 0, 0, 0,
					!no_schedule_until_clip_empty && !work_item);
				kernel_neon_end();
			}
		}
		if (drm_rect_width(&clip_ongoing) <= 0 &&
		    no_schedule_until_clip_empty) {
			no_schedule_until_clip_empty = false;
			if (is_enabling_fast_mode) {
				ebc->fast_mode = true;
				is_enabling_fast_mode = false;
			}
			if (is_disabling_fast_mode) {
				ebc->fast_mode = false;
				is_disabling_fast_mode = false;
			}
		}
		pr_debug("%s schedul2 frame=%d clip_ongoing=" DRM_RECT_FMT " clip_ongoing_or_waiting=" DRM_RECT_FMT,
			 __func__, frame, DRM_RECT_ARG(&clip_ongoing), DRM_RECT_ARG(&clip_ongoing_or_waiting));

		time_advance_sync = ktime_get();

		u64 time_since_last_report =
			ktime_ms_delta(time_advance_sync, time_last_report);
		num_frames_since_last_report += 1;
		max_advance_time_since_last_report =
			max(max_advance_time_since_last_report,
			    ktime_us_delta(time_advance_sync,
					   time_start_advance));
		if (time_since_last_report >= 1000) {
			pr_debug("%s rate num_frames=%d max_advance=%llu us",
				 __func__, num_frames_since_last_report,
				 max_advance_time_since_last_report);
			time_last_report = time_advance_sync;
			num_frames_since_last_report = 0;
			max_advance_time_since_last_report = 0;
		}
		awaiting_start = drm_rect_width(&clip_ongoing) > 0;
		if (awaiting_start) {
			// TODO: make sure we've synced all zeros as well
			int win_start = clip_ongoing.y1 * ebc->phase_pitch + (direct_mode ? clip_ongoing.x1 / 4 : clip_ongoing.x1);
			int win_end = clip_ongoing.y2 * ebc->phase_pitch + (direct_mode ? (clip_ongoing.x2 + 3) / 4 : clip_ongoing.x2);
			/*win_start = 0;*/
			/*win_end = ebc->phase_size;*/
			dma_sync_single_for_device(dev, phase_handle + win_start,
						   win_end - win_start, DMA_TO_DEVICE);
		}
		time_sync_wait = ktime_get();

		if (awaiting_completion && !wait_for_completion_timeout(
									&ebc->display_end, EBC_FRAME_TIMEOUT))
			drm_err(drm, "Frame %d timed out!\n", frame);
		pr_debug("%s:%d frame completion event received", __func__, __LINE__);
		times_wait_end[0] = ktime_get();
		awaiting_completion = false;

		if (awaiting_start) {
			if (shrink_virtual_window) {
				u16 adj_win_width = ((clip_ongoing.x2 + 7) & ~7) - (clip_ongoing.x1 & ~7);
				unsigned int win_start_offset = ebc->act_width * clip_ongoing.y1 + (clip_ongoing.x1 & ~7);
				pr_debug("%s clip_ongoing=" DRM_RECT_FMT " adj_win_width=%ud win_start_offset=%ud", __func__, DRM_RECT_ARG(&clip_ongoing), adj_win_width, win_start_offset);
				regmap_write(ebc->regmap, EBC_WIN_VIR, EBC_WIN_VIR_WIN_VIR_HEIGHT(drm_rect_height(&clip_ongoing)) | EBC_WIN_VIR_WIN_VIR_WIDTH(ebc->pixel_pitch));
				regmap_write(ebc->regmap, EBC_WIN_ACT, EBC_WIN_ACT_WIN_ACT_HEIGHT(drm_rect_height(&clip_ongoing)) | EBC_WIN_ACT_WIN_ACT_WIDTH(adj_win_width));
				regmap_write(ebc->regmap, EBC_WIN_DSP, EBC_WIN_DSP_WIN_DSP_HEIGHT(drm_rect_height(&clip_ongoing)) | EBC_WIN_DSP_WIN_DSP_WIDTH(adj_win_width));
				regmap_write(ebc->regmap, EBC_WIN_DSP_ST, EBC_WIN_DSP_ST_WIN_DSP_YST(ebc->vact_start + clip_ongoing.y1) | EBC_WIN_DSP_ST_WIN_DSP_XST(ebc->hact_start + clip_ongoing.x1 / 8));
				regmap_write(ebc->regmap, direct_mode ? EBC_WIN_MST0 : EBC_WIN_MST2, phase_handle + (direct_mode ? win_start_offset / 4 : win_start_offset));
			} else {
				// TODO: restore other registers in case they were changed
				regmap_write(ebc->regmap, direct_mode ? EBC_WIN_MST0 : EBC_WIN_MST2, phase_handle);
			}
			regmap_write(ebc->regmap, EBC_CONFIG_DONE,
				     EBC_CONFIG_DONE_REG_CONFIG_DONE);
			awaiting_completion = true;
			awaiting_start = false;
			if (testing < 2) {
				regmap_write(
					ebc->regmap, EBC_DSP_START,
					ebc->dsp_start |
						EBC_DSP_START_DSP_FRM_START);
				pr_debug("%s:%d frame started", __func__, __LINE__);
			}
			time_last_start = ktime_get();
		}

		// at this point the ebc is working. It does not access the final
		// buffer directly, and therefore we can use the time to switch
		// buffers or wait for a new update.

		u64 delta_advance = ktime_us_delta(time_advance_sync, time_start_advance);
		u64 delta_sync = ktime_us_delta(time_sync_wait, time_advance_sync);
		u64 delta_wait = ktime_us_delta(times_wait_end[0], time_sync_wait);
		u64 delta_frame = frame > 0 ? ktime_us_delta(times_wait_end[0], times_wait_end[1]) : 0;
		times_wait_end[1] = times_wait_end[0];
		u64 work_total = delta_advance + delta_sync + delta_wait;
		if (delta_frame > max_frame_delay && delta_frame <= 100000)
			max_frame_delay = delta_frame;
		if (delta_frame < min_frame_delay && delta_frame > 0 && delta_frame <= 100000)
			min_frame_delay = delta_frame;
		pr_debug(
			 "%s: frame %i [us]: advance=%llu sync=%llu wait=%llu frame=%llu work_total=%llu",
			 __func__, frame, delta_advance, delta_sync,
			 delta_wait, delta_frame, work_total);

		// TODO: don't assume 85 Hz, as non-direct mode uses 80 Hz
		// We have about max(0, 11700 - delta_advance - delta_wait) [us] that we can wait until we start delaying things
		s64 switch_buffer = 11700 - delta_advance - delta_wait - 1000;

		// TODO: consider adding || !work_item
		while(!is_suspending) {
			spin_lock(&ctx->buffer_switch_lock);
			ctx->refresh_index = ctx->next_refresh_index;
			for (int i = 0; i < 3; ++i) {
				// TODO: consider rejecting an update if it would incur a delay
				if (ctx->not_after_others[i] & (1 << ctx->refresh_index)) {
					rockchip_ebc_drm_rect_extend_rect(&clip_incoming,
									  ctx->dst_clip + i);
					ctx->dst_clip[i] = DRM_RECT_EMPTY_EXTANDABLE;
				}
			}
			spin_unlock(&ctx->buffer_switch_lock);
			// TODO: use predicted processing time based on clip_incoming, as clip_incoming can be significantly larger than the previous one
			s64 time_us_buffer =
				switch_buffer -
					ktime_us_delta(ktime_get(), times_wait_end[0]);

			if (time_us_buffer <= 0)
				break;

			fsleep(time_us_buffer);
		}

		prelim_target = ctx->prelim_target_buffer[ctx->refresh_index];
		hints = ctx->hints_buffer[ctx->refresh_index];

		if (kthread_should_stop()) {
			break;
		};
	}
}

static void rockchip_ebc_upd_temp(struct rockchip_ebc *ebc)
{
	struct drm_device *drm = &ebc->drm;
	int ret, temperature;
	struct drm_epd_lut_temp_v2 *lut_active = ebc->lut_custom_active;
	struct drm_epd_lut_v2 *luts = &ebc->lut_custom;

	ret = iio_read_channel_processed(ebc->temperature_channel,
					 &temperature);
	pr_debug("%s ret=%d temperature=%d", __func__, ret, temperature);

	if (ret < 0) {
		drm_err(drm, "Failed to get temperature: %d\n", ret);
	} else {
		// Convert from millicelsius to celsius.
		temperature /= 1000;
		if (temp_override > 0){
			printk(KERN_INFO "rockchip-ebc: override temperature from %i to %i\n", temperature, temp_override);
			temperature = temp_override;
		}
		ebc->temperature = temperature;

		// TODO: figure out exclusivity/inclusivity and lowest and highest temperature range
		if ((temperature < lut_active->temp_lower &&
		     lut_active->temp_lower != luts->luts[0].temp_lower) ||
		    (temperature > lut_active->temp_upper &&
		     lut_active->temp_upper !=
		     luts->luts[luts->num_temp_ranges - 1].temp_upper)) {
			spin_lock(&ebc->work_item_lock);
			ebc->work_item |= ROCKCHIP_EBC_WORK_ITEM_CHANGE_LUT;
			spin_unlock(&ebc->work_item_lock);
		}
	}
}

static void rockchip_ebc_refresh(struct rockchip_ebc *ebc,
				 struct rockchip_ebc_ctx *ctx)
{
	struct drm_device *drm = &ebc->drm;
	struct device *dev = drm->dev;
	int ret;
	ktime_t time_start_resume = ktime_get();

	// Resume synchronously before writing to any registers
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to resume: %d\n", ret);
		return;
	}
	pr_debug("%s pm_runtime_resume_and_get took %lld ms", __func__, ktime_ms_delta(ktime_get(), time_start_resume));

	// TODO: do only once and restore at resume
	if (!direct_mode) {
		// Another 8-9 ms. Is it okay to do this only once?
		regmap_bulk_write(ebc->regmap, EBC_LUT_DATA, ebc->hardware_wf,
				  EBC_NUM_LUT_REGS);
		// TODO: do this only once
		pr_debug("%s:%d hardware_wf written\n", __func__, __LINE__);
		regmap_write(ebc->regmap, EBC_WIN_MST0, ebc->zero_handle);
		regmap_write(ebc->regmap, EBC_WIN_MST1, ebc->zero_handle);
		pr_debug("%s:%d EBC_WIN_MST? written\n", __func__, __LINE__);
	}

	regmap_write(ebc->regmap, EBC_DSP_START, ebc->dsp_start);

	rockchip_ebc_partial_refresh(ebc, ctx);

	/* Drive the output pins low once the refresh is complete. */
	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start |
		     EBC_DSP_START_DSP_OUT_LOW);

	pr_debug("%s:%d EBC_DSP_START to low\n", __func__, __LINE__);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int rockchip_ebc_temp_upd_thread(void *data)
{
	struct rockchip_ebc *ebc = data;
	while (!kthread_should_stop()) {
		while (!kthread_should_park() && !kthread_should_stop()) {
			set_current_state(TASK_RUNNING);
			rockchip_ebc_upd_temp(ebc);
			msleep_interruptible(10000);
		}
		if (!kthread_should_stop())
			kthread_parkme();
	}
	return 0;
}

static int rockchip_ebc_refresh_thread(void *data)
{
	struct rockchip_ebc *ebc = data;
	struct rockchip_ebc_ctx *ctx;
	rockchip_ebc_change_lut(ebc);

	while (!kthread_should_stop()) {
		pr_debug("%s:%d\n", __func__, __LINE__);
		/* The context will change each time the thread is unparked. */
		ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;

		// this means rockchip_ebc_crtc_atomic_disable does not trigger a global refresh
		// TODO: consider dropping this condition
		if(ebc->suspend_was_requested == 1) {
			// this means we are coming out from suspend. Reset the buffers to
			// the before-suspend state
			spin_lock(&ebc->work_item_lock);
			ebc->work_item |= ROCKCHIP_EBC_WORK_ITEM_GLOBAL_REFRESH;
			spin_unlock(&ebc->work_item_lock);
		}

		// This shouldn't be necessary, but might be safer
		memset(ebc->phase[0], 0, ebc->phase_size);
		memset(ebc->phase[1], 0, ebc->phase_size);

		while ((!kthread_should_park()) && (!kthread_should_stop())) {
			rockchip_ebc_refresh(ebc, ctx);

			set_current_state(TASK_IDLE);
			if (!kthread_should_stop() && !kthread_should_park()) {
				schedule();
			}
			__set_current_state(TASK_RUNNING);
		}

		if (!kthread_should_stop()){
			kthread_parkme();
		}
	}

	return 0;
}

static inline struct rockchip_ebc *crtc_to_ebc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct rockchip_ebc, crtc);
}

static int rockchip_ebc_set_dclk(struct rockchip_ebc *ebc,
					  const struct drm_display_mode *mode)
{
	int rate;

	if (direct_mode) {
		rate = clk_set_rate(ebc->cpll_333m, 33333334);
		if (rate < 0)
			return rate;
		rate = clk_set_rate(ebc->dclk, 34000000);
		return rate;
	}

	switch (dclk_select) {
		case -1:
			// TODO: consider adjusting cpll_333m
			rate = clk_set_rate(ebc->dclk, mode->clock * 1000);
			break;
		case 0:
			rate = clk_set_rate(ebc->dclk, 200000000);
			break;
		case 1:
			rate = clk_set_rate(ebc->cpll_333m, 250000000);
			if (rate < 0)
				return rate;
			rate = clk_set_rate(ebc->dclk, 250000000);
			break;
		default:
			rate = -EINVAL;
	}
	return rate;
}

static void rockchip_ebc_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_display_mode mode = crtc->state->adjusted_mode;
	struct drm_display_mode sdck;
	u16 hsync_width, vsync_width;
	u16 hact_start, vact_start;
	u16 pixels_per_sdck;
	bool bus_16bit;
	/* pr_info("ebc: %s", __func__); */
	/* from drm_modes.h:
	 * * The horizontal and vertical timings are defined per the following diagram.
	 *
	 * ::
	 *
	 *
	 *               Active                 Front           Sync           Back
	 *              Region                 Porch                          Porch
	 *     <-----------------------><----------------><-------------><-------------->
	 *       //////////////////////|
	 *      ////////////////////// |
	 *     //////////////////////  |..................               ................
	 *                                                _______________
	 *     <----- [hv]display ----->
	 *     <------------- [hv]sync_start ------------>
	 *     <--------------------- [hv]sync_end --------------------->
	 *     <-------------------------------- [hv]total ----------------------------->*
	 *
	 * */

	/*
	 * Hardware needs horizontal timings in SDCK (source driver clock)
	 * cycles, not pixels. Bus width is either 8 bits (normal) or 16 bits
	 * (DRM_MODE_FLAG_CLKDIV2), and each pixel uses two data bits.
	 */
	bus_16bit = !!(mode.flags & DRM_MODE_FLAG_CLKDIV2);
	pixels_per_sdck = bus_16bit ? 8 : 4;
	sdck.hdisplay = mode.hdisplay / pixels_per_sdck;
	sdck.hsync_start = mode.hsync_start / pixels_per_sdck;
	sdck.hsync_end = mode.hsync_end / pixels_per_sdck;
	sdck.htotal = mode.htotal / pixels_per_sdck;

	if (hskew_override > 0){
		pr_info(
			"rockchip-ebc: overriding hskew value %i with new value: %i",
				mode.hskew, hskew_override
		);
		sdck.hskew = hskew_override / pixels_per_sdck;
	} else {
		// use the value supplied via the panel mode
		sdck.hskew = mode.hskew / pixels_per_sdck;
	}

	/*
	 * Linux timing order is display/fp/sync/bp. Hardware timing order is
	 * sync/bp/display/fp, aka sync/start/display/end.
	 */
	hact_start = sdck.htotal - sdck.hsync_start;
	// mode.vtotal = 1404 + 12 + 1 + 4
	// mode.vsync_start = 1404 + 12
	// vact_start = 5
	vact_start = mode.vtotal - mode.vsync_start;

	hsync_width = sdck.hsync_end - sdck.hsync_start;
	vsync_width = mode.vsync_end - mode.vsync_start;

	// TODO: error checking
	rockchip_ebc_set_dclk(ebc, &mode);

	/* Display timings in ebc hardware:
	+        * GD_ST
	+        * GD_END
	+        * HTOTAL
	+        * HS_END
	+        * HACT_END
	+        * HACT_ST
	+        * VTOTAL
	+        * VS_END
	+        * VACT_END
	+        * VACT_ST
	+        * DSP_ACT_HEIGHT
	+        * DSP_ACT_WIDTH
	+        *
	+        * */

	ebc->dsp_start = EBC_DSP_START_DSP_SDCE_WIDTH(sdck.hdisplay) |
			 EBC_DSP_START_SW_BURST_CTRL;

	ebc->act_width = mode.hdisplay;
	ebc->act_height = mode.vdisplay;
	ebc->vact_start = vact_start;
	ebc->hact_start = hact_start;

	regmap_write(ebc->regmap, EBC_EPD_CTRL,
		     EBC_EPD_CTRL_DSP_GD_END(sdck.htotal - sdck.hskew) |
			     EBC_EPD_CTRL_DSP_GD_ST(hsync_width + sdck.hskew) |
			     EBC_EPD_CTRL_DSP_SDDW_MODE * bus_16bit |
			     (direct_mode ? 0 :
			      EBC_EPD_CTRL_DSP_THREE_WIN_MODE));

	regmap_write(ebc->regmap, EBC_DSP_CTRL,
		     /* no swap */
		     EBC_DSP_CTRL_DSP_SWAP_MODE(bus_16bit ? 2 : 3) |
		     EBC_DSP_CTRL_DSP_SDCLK_DIV(direct_mode ? 0 : pixels_per_sdck - 1));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING0,
		     EBC_DSP_HTIMING0_DSP_HTOTAL(sdck.htotal) |
		     /* sync end == sync width */
		     EBC_DSP_HTIMING0_DSP_HS_END(hsync_width));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING1,
		     EBC_DSP_HTIMING1_DSP_HACT_END(hact_start + sdck.hdisplay) |
		     /* minus 1 for fixed delay in timing sequence */
		     EBC_DSP_HTIMING1_DSP_HACT_ST(hact_start - 1));

	/* vertical timings */
	regmap_write(ebc->regmap, EBC_DSP_VTIMING0,
		     EBC_DSP_VTIMING0_DSP_VTOTAL(mode.vtotal) |
		     /* sync end == sync width */
			 /* mw: when comparing the data sheets, this is equal to FEL of the display, translating to 12 frames
			  * yet, here vsync_width computes to 1*/
			 // = 1
		     EBC_DSP_VTIMING0_DSP_VS_END(vsync_width));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING1,
		     EBC_DSP_VTIMING1_DSP_VACT_END(vact_start + mode.vdisplay) |
		     EBC_DSP_VTIMING1_DSP_VACT_ST(vact_start));

	/* active region */
	regmap_write(ebc->regmap, EBC_DSP_ACT_INFO,
		     EBC_DSP_ACT_INFO_DSP_HEIGHT(mode.vdisplay) |
		     EBC_DSP_ACT_INFO_DSP_WIDTH(mode.hdisplay));

	/* misc */
	regmap_write(ebc->regmap, EBC_WIN_CTRL,
		     /* FIFO depth - 16 */
		     EBC_WIN_CTRL_WIN2_FIFO_THRESHOLD(496) |
		     EBC_WIN_CTRL_WIN_EN |
		     /* INCR16 */
		     EBC_WIN_CTRL_AHB_BURST_REG(7) |
		     /* FIFO depth - 16 */
		     EBC_WIN_CTRL_WIN_FIFO_THRESHOLD(240) |
		     EBC_WIN_CTRL_WIN_FMT_Y4);

	/* To keep things simple, always use a window size matching the CRTC. */
	regmap_write(ebc->regmap, EBC_WIN_VIR,
		     EBC_WIN_VIR_WIN_VIR_HEIGHT(mode.vdisplay) |
		     EBC_WIN_VIR_WIN_VIR_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_ACT,
		     EBC_WIN_ACT_WIN_ACT_HEIGHT(mode.vdisplay) |
		     EBC_WIN_ACT_WIN_ACT_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_DSP,
		     EBC_WIN_DSP_WIN_DSP_HEIGHT(mode.vdisplay) |
		     EBC_WIN_DSP_WIN_DSP_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_DSP_ST,
		     EBC_WIN_DSP_ST_WIN_DSP_YST(vact_start) |
		     EBC_WIN_DSP_ST_WIN_DSP_XST(hact_start));
}

static int rockchip_ebc_crtc_atomic_check(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct ebc_crtc_state *ebc_crtc_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;
	pr_debug("ebc: %s", __func__);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state->mode_changed)
		return 0;

	if (crtc_state->enable) {
		struct drm_display_mode *mode = &crtc_state->adjusted_mode;

		int rate = rockchip_ebc_set_dclk(ebc, mode);
		if (rate < 0)
			return rate;
		mode->clock = rate / 1000;

		ctx = rockchip_ebc_ctx_alloc(ebc);
		if (!ctx)
			return -ENOMEM;
	} else {
		ctx = NULL;
	}

	ebc_crtc_state = to_ebc_crtc_state(crtc_state);
	if (ebc_crtc_state->ctx)
		kref_put(&ebc_crtc_state->ctx->kref, rockchip_ebc_ctx_release);
	ebc_crtc_state->ctx = ctx;

	return 0;
}

static void rockchip_ebc_crtc_atomic_flush(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
	// TODO: consider moving copying to final_buffer, moving of areas, setting switch_required, waking up process here
	// TODO: also look at atomic_commit
	pr_debug("ebc: %s", __func__);
}

static void rockchip_ebc_crtc_atomic_enable(struct drm_crtc *crtc,
					    struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;
	pr_debug("ebc: %s", __func__);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed) {
		kthread_unpark(ebc->temp_upd_thread);
		kthread_unpark(ebc->refresh_thread);
	}
}

static void rockchip_ebc_crtc_atomic_disable(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;
	pr_debug("ebc: %s", __func__);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed){
		if (! ((ebc->refresh_thread->__state) & (TASK_DEAD))){
			kthread_park(ebc->refresh_thread);
		}
		if (!(ebc->temp_upd_thread->__state & TASK_DEAD)) {
			kthread_park(ebc->temp_upd_thread);
		}
	}
}

static const struct drm_crtc_helper_funcs rockchip_ebc_crtc_helper_funcs = {
	.mode_set_nofb		= rockchip_ebc_crtc_mode_set_nofb,
	.atomic_check		= rockchip_ebc_crtc_atomic_check,
	.atomic_flush		= rockchip_ebc_crtc_atomic_flush,
	.atomic_enable		= rockchip_ebc_crtc_atomic_enable,
	.atomic_disable		= rockchip_ebc_crtc_atomic_disable,
};

static void rockchip_ebc_crtc_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state);

static void rockchip_ebc_crtc_reset(struct drm_crtc *crtc)
{
	struct ebc_crtc_state *ebc_crtc_state;
	/* pr_info("ebc: %s", __func__); */

	if (crtc->state)
		rockchip_ebc_crtc_destroy_state(crtc, crtc->state);

	ebc_crtc_state = kzalloc(sizeof(*ebc_crtc_state), GFP_KERNEL);
	if (!ebc_crtc_state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, &ebc_crtc_state->base);
}

static struct drm_crtc_state *
rockchip_ebc_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct ebc_crtc_state *ebc_crtc_state;
	/* pr_info("ebc: %s", __func__); */

	if (!crtc->state)
		return NULL;

	ebc_crtc_state = kzalloc(sizeof(*ebc_crtc_state), GFP_KERNEL);
	if (!ebc_crtc_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &ebc_crtc_state->base);

	ebc_crtc_state->ctx = to_ebc_crtc_state(crtc->state)->ctx;
	if (ebc_crtc_state->ctx)
		kref_get(&ebc_crtc_state->ctx->kref);

	return &ebc_crtc_state->base;
}

static void rockchip_ebc_crtc_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state)
{
	struct ebc_crtc_state *ebc_crtc_state = to_ebc_crtc_state(crtc_state);

	if (ebc_crtc_state->ctx)
		kref_put(&ebc_crtc_state->ctx->kref, rockchip_ebc_ctx_release);

	__drm_atomic_helper_crtc_destroy_state(&ebc_crtc_state->base);

	kfree(ebc_crtc_state);
}

static const struct drm_crtc_funcs rockchip_ebc_crtc_funcs = {
	.reset			= rockchip_ebc_crtc_reset,
	.destroy		= drm_crtc_cleanup,
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.atomic_duplicate_state	= rockchip_ebc_crtc_duplicate_state,
	.atomic_destroy_state	= rockchip_ebc_crtc_destroy_state,
};

/*
 * Plane
 */

struct ebc_plane_state {
	struct drm_shadow_plane_state	base;
	struct drm_rect			clip;
};

static inline struct ebc_plane_state *
to_ebc_plane_state(struct drm_plane_state *plane_state)
{
	return container_of(plane_state, struct ebc_plane_state, base.base);
}

static inline struct rockchip_ebc *plane_to_ebc(struct drm_plane *plane)
{
	return container_of(plane, struct rockchip_ebc, plane);
}

static int rockchip_ebc_plane_atomic_check(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_atomic_helper_damage_iter iter;
	struct ebc_plane_state *ebc_plane_state;
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_rect clip;
	int ret;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, true);
	if (ret)
		return ret;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		rockchip_ebc_drm_rect_extend_rect(&ebc_plane_state->clip, &clip);
	}

	return 0;
}

static void rockchip_ebc_plane_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = plane_to_ebc(plane);
	struct ebc_plane_state *ebc_plane_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;
	struct drm_rect src;
	const void *vaddr;
	pr_debug("ebc %s", __func__);
	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ctx = to_ebc_crtc_state(crtc_state)->ctx;

	drm_rect_fp_to_int(&src, &plane_state->src);

	ebc_plane_state = to_ebc_plane_state(plane_state);
	vaddr = ebc_plane_state->base.data[0].vaddr;

	struct drm_rect src_clip = ebc_plane_state->clip;
	ebc_plane_state->clip = DRM_RECT_EMPTY_EXTANDABLE;
	if (drm_rect_width(&src_clip) <= 0)
		return;
	// NEON 16 byte alignment
	src_clip.x1 =
		max(0, min(src_clip.x1 & ~15, (int)ebc->pixel_pitch - 16));
	src_clip.x2 = min((src_clip.x2 + 15) & ~15, (int)ebc->pixel_pitch);

	// This is the buffer we are allowed to modify, as it's not being read by the refresh thread
	int idx_update = ctx->update_index;

	// Also blit areas blitted to the other two buffers since the last time this one was modified
	struct drm_rect src_clip_extended = src_clip;
	rockchip_ebc_drm_rect_extend_rect(&src_clip_extended, ctx->src_clip_extended + idx_update);

	rockchip_ebc_drm_rect_extend_rect(ctx->src_clip_extended + 0,
					  &src_clip);
	rockchip_ebc_drm_rect_extend_rect(ctx->src_clip_extended + 1,
					  &src_clip);
	rockchip_ebc_drm_rect_extend_rect(ctx->src_clip_extended + 2,
					  &src_clip);
	ctx->src_clip_extended[idx_update] = DRM_RECT_EMPTY_EXTANDABLE;

	struct drm_rect dst_clip = src_clip;
	struct drm_rect dst_clip_extended = src_clip_extended;

	// Horizontal flip
	dst_clip.x1 = plane_state->dst.x2 - src_clip.x2;
	dst_clip.x2 = plane_state->dst.x2 - src_clip.x1;
	dst_clip_extended.x1 = plane_state->dst.x2 - src_clip_extended.x2;
	dst_clip_extended.x2 = plane_state->dst.x2 - src_clip_extended.x1;

	// TODO: consider measuring required time and indicate to refresh thread when the next update is going to be available

	pr_debug("%s dst_clip=" DRM_RECT_FMT, __func__, DRM_RECT_ARG(&dst_clip));
	if (limit_fb_blits != 0) {
		// the counter should only reach 0 here, -1 can only be externally set
		limit_fb_blits -= (limit_fb_blits > 0) ? 1 : 0;

		switch (plane_state->fb->format->format) {
		case DRM_FORMAT_RGB565:
			kernel_neon_begin();
			rockchip_ebc_blit_fb_rgb565_y4_hints_neon(
				ebc, &dst_clip_extended,
				ctx->prelim_target_buffer[idx_update],
				ctx->hints_buffer[idx_update], vaddr,
				plane_state->fb, &src_clip_extended);
			kernel_neon_end();
			break;
		case DRM_FORMAT_XRGB8888:
			kernel_neon_begin();
			rockchip_ebc_blit_fb_xrgb8888_y4_hints_neon(
				ebc, &dst_clip_extended,
				ctx->prelim_target_buffer[idx_update],
				ctx->hints_buffer[idx_update], vaddr,
				plane_state->fb, &src_clip_extended);
			kernel_neon_end();
			break;
		case DRM_FORMAT_R8:
			kernel_neon_begin();
			rockchip_ebc_blit_fb_r8_y4_hints_neon(
				ebc, &dst_clip_extended,
				ctx->prelim_target_buffer[idx_update],
				ctx->hints_buffer[idx_update], vaddr,
				plane_state->fb, &src_clip_extended);
			kernel_neon_end();
			break;
		}
	}

	// Don't extend until now to avoid out-of-order updates and allowing the refresh thread to clear this area in the meantime
	spin_lock(&ctx->buffer_switch_lock);
	for (int i = 0; i < 3; ++i) {
		ctx->not_after_others[i] |= (1 << idx_update);
	}
	ctx->not_after_others[idx_update] = (1 << idx_update);
	rockchip_ebc_drm_rect_extend_rect(ctx->dst_clip + idx_update,
					  &dst_clip);
	ctx->next_refresh_index = idx_update;
	if ((ctx->update_index = (ctx->update_index + 1) % 3) ==
	    ctx->refresh_index) {
		ctx->update_index = (ctx->update_index + 1) % 3;
	}
	spin_unlock(&ctx->buffer_switch_lock);

	wake_up_process(ebc->refresh_thread);
}

static const struct drm_plane_helper_funcs rockchip_ebc_plane_helper_funcs = {
	/* .prepare_fb		= drm_gem_prepare_shadow_fb, */
	/* .cleanup_fb		= drm_gem_cleanup_shadow_fb, */
	.begin_fb_access = drm_gem_begin_shadow_fb_access,
	.end_fb_access = drm_gem_end_shadow_fb_access,
	.atomic_check		= rockchip_ebc_plane_atomic_check,
	.atomic_update		= rockchip_ebc_plane_atomic_update,
};

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state);

static void rockchip_ebc_plane_reset(struct drm_plane *plane)
{
	struct ebc_plane_state *ebc_plane_state;
	pr_info("ebc: %s", __func__);

	if (plane->state)
		rockchip_ebc_plane_destroy_state(plane, plane->state);

	ebc_plane_state = kzalloc(sizeof(*ebc_plane_state), GFP_KERNEL);
	if (!ebc_plane_state)
		return;

	__drm_gem_reset_shadow_plane(plane, &ebc_plane_state->base);
	ebc_plane_state->clip = DRM_RECT_EMPTY_EXTANDABLE;
}

static struct drm_plane_state *
rockchip_ebc_plane_duplicate_state(struct drm_plane *plane)
{
	struct ebc_plane_state *ebc_plane_state;
	/* pr_info("ebc: %s", __func__); */

	if (!plane->state)
		return NULL;

	ebc_plane_state = kzalloc(sizeof(*ebc_plane_state), GFP_KERNEL);
	if (!ebc_plane_state)
		return NULL;

	__drm_gem_duplicate_shadow_plane_state(plane, &ebc_plane_state->base);

	ebc_plane_state->clip = DRM_RECT_EMPTY_EXTANDABLE;

	return &ebc_plane_state->base.base;
}

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state)
{
	struct ebc_plane_state *ebc_plane_state = to_ebc_plane_state(plane_state);
	/* pr_info("ebc: %s", __func__); */

	__drm_gem_destroy_shadow_plane_state(&ebc_plane_state->base);

	kfree(ebc_plane_state);
}

static const struct drm_plane_funcs rockchip_ebc_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= rockchip_ebc_plane_reset,
	.atomic_duplicate_state	= rockchip_ebc_plane_duplicate_state,
	.atomic_destroy_state	= rockchip_ebc_plane_destroy_state,
};

static const u32 rockchip_ebc_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_R8,
};

static const u64 rockchip_ebc_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int rockchip_ebc_waveform_init(struct rockchip_ebc *ebc)
{
	int ret;
	struct drm_device *drm = &ebc->drm;

	const struct firmware * default_off_screen;
	const struct firmware *custom_wf;

	ret = drmm_epd_lut_file_init(drm, &ebc->lut_file, "rockchip/ebc.wbf");
	if (ret)
		return ret;

	ret = drmm_epd_lut_init(&ebc->lut_file, &ebc->lut,
				DRM_EPD_LUT_4BIT_PACKED, EBC_MAX_PHASES);
	if (ret)
		return ret;

	if (!request_firmware(&custom_wf, EBC_CUSTOM_WF, drm->dev)) {
		pr_debug("%s:%d\n", __func__, __LINE__);
		size_t temp_range_size = 8 + ROCKCHIP_EBC_CUSTOM_WF_NUM_SEQS + ROCKCHIP_EBC_CUSTOM_WF_LUT_SIZE;
		if ((custom_wf->size - 12) % temp_range_size) {
			drm_err(drm, "Length error when loading custom_wf.bin\n");
			ret = -EINVAL;
		} else if (memcmp(custom_wf_magic_version, custom_wf->data, 8)) {
			drm_err(drm, "Versioned magic comparison failed. Got %8ph, expected %8ph\n", custom_wf->data, custom_wf_magic_version);
			ret = -EINVAL;
		} else {
			pr_debug("%s:%d\n", __func__, __LINE__);
			unsigned int num_temp_ranges = (custom_wf->size - 12) / temp_range_size;
			ebc->lut_custom.num_temp_ranges = num_temp_ranges;
			ebc->lut_custom.luts = vzalloc(num_temp_ranges * sizeof(struct drm_epd_lut_temp_v2));
			if (!ebc->lut_custom.luts) {
				drm_err(drm, "Failed to allocate lut_custom.luts\n");
				ret = -ENOMEM;
			} else {
				pr_debug("%s:%d\n", __func__, __LINE__);
				const u8 *fw_temp = custom_wf->data + 12;
				for (int i = 0; i < num_temp_ranges; ++i) {
					struct drm_epd_lut_temp_v2 *lut_temp = ebc->lut_custom.luts + i;
					lut_temp->temp_lower = *fw_temp;
					lut_temp->temp_upper = *(fw_temp + 4);
					for (int wf = 0; wf < ROCKCHIP_EBC_CUSTOM_WF_NUM_SEQS; ++wf)
						lut_temp->offsets[wf] = fw_temp[8+wf];
					memcpy(lut_temp->lut, fw_temp + 8 + ROCKCHIP_EBC_CUSTOM_WF_NUM_SEQS, ROCKCHIP_EBC_CUSTOM_WF_LUT_SIZE);
					fw_temp += temp_range_size;
				}
			}
		}
	} else {
		drm_err(drm, "Unable to load custom_wf.bin\n");
		ret = -EINVAL;
	}
	pr_debug("%s:%d\n", __func__, __LINE__);
	release_firmware(custom_wf);
	if (ret)
		return ret;

	// check if there is a default off-screen. Only the lowest four bits will be used per pixel
	if (!request_firmware(&default_off_screen, "rockchip/rockchip_ebc_default_screen.bin", drm->dev))
	{
		if (default_off_screen->size != 1314144)
			drm_err(drm, "Size of default off_screen data file is not 1314144\n");
		else {
			memcpy(ebc->final_off_screen, default_off_screen->data, 1314144);
			memcpy(ebc->final_off_screen + 1314144, default_off_screen->data, 1314144);
		}
	} else {
		// fill the off-screen with some values
		memset(ebc->final_off_screen, 0xff, ebc->num_pixels);
	}
	release_firmware(default_off_screen);
	pr_debug("%s:%d\n", __func__, __LINE__);

	return 0;
}

static int rockchip_ebc_drm_init(struct rockchip_ebc *ebc)
{
	struct drm_device *drm = &ebc->drm;
	struct drm_bridge *bridge;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.max_width = DRM_SHADOW_PLANE_MAX_WIDTH;
	drm->mode_config.max_height = DRM_SHADOW_PLANE_MAX_HEIGHT;
	drm->mode_config.funcs = &rockchip_ebc_mode_config_funcs;
	drm->mode_config.quirk_addfb_prefer_host_byte_order = true;

	drm_plane_helper_add(&ebc->plane, &rockchip_ebc_plane_helper_funcs);
	ret = drm_universal_plane_init(drm, &ebc->plane, 0,
				       &rockchip_ebc_plane_funcs,
				       rockchip_ebc_plane_formats,
				       ARRAY_SIZE(rockchip_ebc_plane_formats),
				       rockchip_ebc_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&ebc->plane);

	drm_crtc_helper_add(&ebc->crtc, &rockchip_ebc_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(drm, &ebc->crtc, &ebc->plane, NULL,
					&rockchip_ebc_crtc_funcs, NULL);
	if (ret)
		return ret;

	ebc->encoder.possible_crtcs = drm_crtc_mask(&ebc->crtc);
	// todo: consider using drmm_simple_encoder_alloc()
	// see: https://www.kernel.org/doc/html/latest/gpu/drm-kms-helpers.html?highlight=drm_simple_encoder_init#c.drm_simple_encoder_init
	ret = drm_simple_encoder_init(drm, &ebc->encoder, DRM_MODE_ENCODER_NONE);
	if (ret)
		return ret;

	bridge = devm_drm_of_get_bridge(drm->dev, drm->dev->of_node, 0, 0);
	if (IS_ERR(bridge))
		return PTR_ERR(bridge);

	ret = drm_bridge_attach(&ebc->encoder, bridge, NULL, 0);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);

	return 0;
}

static int __maybe_unused rockchip_ebc_suspend(struct device *dev)
{
	int ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	return 0;
}

static int __maybe_unused rockchip_ebc_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

	pr_info("%s %lld", __func__, ktime_get());

	pm_runtime_force_resume(dev);

	return drm_mode_config_helper_resume(&ebc->drm);
}

static int rockchip_ebc_runtime_suspend(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

	regcache_cache_only(ebc->regmap, true);

	clk_disable_unprepare(ebc->dclk);
	clk_disable_unprepare(ebc->hclk);
	regulator_bulk_disable(EBC_NUM_SUPPLIES, ebc->supplies);

	return 0;
}

static int rockchip_ebc_runtime_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;

	ret = regulator_bulk_enable(EBC_NUM_SUPPLIES, ebc->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ebc->hclk);
	if (ret)
		goto err_disable_supplies;

	ret = clk_prepare_enable(ebc->dclk);
	if (ret)
		goto err_disable_hclk;

	/*
	 * Do not restore the LUT registers here, because the temperature or
	 * waveform may have changed since the last refresh. Instead, have the
	 * refresh thread program the LUT during the next refresh.
	 */
	regcache_cache_only(ebc->regmap, false);
	regcache_mark_dirty(ebc->regmap);
	regcache_sync(ebc->regmap);

	regmap_write(ebc->regmap, EBC_INT_STATUS,
		     EBC_INT_STATUS_DSP_END_INT_CLR |
		     EBC_INT_STATUS_LINE_FLAG_INT_MSK |
		     EBC_INT_STATUS_DSP_FRM_INT_MSK |
		     EBC_INT_STATUS_FRM_END_INT_MSK);

	return 0;

err_disable_hclk:
	clk_disable_unprepare(ebc->hclk);
err_disable_supplies:
	regulator_bulk_disable(EBC_NUM_SUPPLIES, ebc->supplies);

	return ret;
}

static int rockchip_ebc_prepare(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;

	spin_lock(&ebc->work_item_lock);
	ebc->work_item |= ROCKCHIP_EBC_WORK_ITEM_SUSPEND;
	spin_unlock(&ebc->work_item_lock);

	ret = drm_mode_config_helper_suspend(&ebc->drm);
	if (ret)
		return ret;

	return 0;
}

static const struct dev_pm_ops rockchip_ebc_dev_pm_ops = {
	.prepare = rockchip_ebc_prepare,
	SET_SYSTEM_SLEEP_PM_OPS(rockchip_ebc_suspend, rockchip_ebc_resume)
	SET_RUNTIME_PM_OPS(rockchip_ebc_runtime_suspend,
			   rockchip_ebc_runtime_resume, NULL)
};

static bool rockchip_ebc_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EBC_DSP_START:
	case EBC_INT_STATUS:
	case EBC_CONFIG_DONE:
	case EBC_VNUM:
		return true;
	default:
		/* Do not cache the LUT registers. */
		return reg > EBC_WIN_MST2;
	}
}

static const struct regmap_config rockchip_ebc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.volatile_reg	= rockchip_ebc_volatile_reg,
	.max_register	= 0x4ffc, /* end of EBC_LUT_DATA */
	.cache_type	= REGCACHE_FLAT,
};

static const char *const rockchip_ebc_supplies[EBC_NUM_SUPPLIES] = {
	"panel",
	"vcom",
	"vdrive",
};

static irqreturn_t rockchip_ebc_irq(int irq, void *dev_id)
{
	struct rockchip_ebc *ebc = dev_id;
	unsigned int status;

	regmap_read(ebc->regmap, EBC_INT_STATUS, &status);

	pr_debug("%s status=%d", __func__, status);
	if (status & EBC_INT_STATUS_DSP_END_INT_ST) {
		status |= EBC_INT_STATUS_DSP_END_INT_CLR;
		complete(&ebc->display_end);
	}

	regmap_write(ebc->regmap, EBC_INT_STATUS, status);

	return IRQ_HANDLED;
}

static int rockchip_ebc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_ebc *ebc;
	void __iomem *base;
	int i, ret;
	pr_info("%s start", __func__);

	if (dma_set_mask(dev, DMA_BIT_MASK(32))){
		dev_warn(dev, "rockchip-ebc: No suitable DMA available\n");
	}

	ebc = devm_drm_dev_alloc(dev, &rockchip_ebc_drm_driver,
				 struct rockchip_ebc, drm);
	if (IS_ERR(ebc))
		return PTR_ERR(ebc);

	unsigned int width = 1872;
	unsigned int height = 1404;
	ebc->direct_mode = direct_mode;
	ebc->gray4_pitch = width / 2;
	ebc->gray4_size = width * height / 2;
	ebc->phase_pitch = ebc->direct_mode ? width / 4 : width;
	ebc->phase_size = ebc->phase_pitch * height;
	ebc->num_pixels = width * height;
	ebc->pixel_pitch = width;
	ebc->screen_rect = DRM_RECT_INIT(0, 0, width, height);

	ebc->y4_threshold_y1 = bw_threshold;

	switch (dithering_method) {
	case DITHERING_BAYER:
		ebc->dithering_texture = dither_bayer_04;
		ebc->dithering_texture_size_hint = 4;
		break;
	case DITHERING_BLUE_NOISE_16:
		ebc->dithering_texture = dither_blue_noise_16;
		ebc->dithering_texture_size_hint = 16;
		break;
	case DITHERING_BLUE_NOISE_32:
		fallthrough;
	default:
		ebc->dithering_texture = dither_blue_noise_32;
		ebc->dithering_texture_size_hint = 32;
		break;
	}

	ebc->final_off_screen = drmm_kzalloc(&ebc->drm, ebc->num_pixels, GFP_KERNEL);
	ebc->packed_inner_outer_nextprev = vmalloc(3 *  ebc->num_pixels); // drmm_kmalloc(&ebc->drm, ebc->num_pixels, GFP_KERNEL);
	if (!direct_mode) {
		ebc->hardware_wf = drmm_kzalloc(&ebc->drm, 4 * EBC_NUM_LUT_REGS,
						GFP_KERNEL);
		ebc->zero = drmm_kzalloc(&ebc->drm, ebc->num_pixels, GFP_KERNEL | GFP_DMA);
	}
	ebc->hints_ioctl = vmalloc(ebc->num_pixels); // drmm_kmalloc(&ebc->drm, ebc->num_pixels, GFP_KERNEL);
	ebc->phase[0] = drmm_kzalloc(&ebc->drm, ebc->phase_size, GFP_KERNEL | GFP_DMA);
	ebc->phase[1] = drmm_kzalloc(&ebc->drm, ebc->phase_size, GFP_KERNEL | GFP_DMA);
	if (!(ebc->final_off_screen && ebc->packed_inner_outer_nextprev &&
	      (direct_mode || (ebc->hardware_wf && ebc->zero)) &&
	      ebc->hints_ioctl && ebc->phase[0] &&
	      ebc->phase[1])) {
		return dev_err_probe(dev, -ENOMEM, "Failed to allocate buffers\n");
	}
	ebc->phase_handles[0] = dma_map_single(dev, ebc->phase[0], ebc->phase_size,
					       DMA_TO_DEVICE);
	if (dma_mapping_error(dev, ebc->phase_handles[0])) {
		return dev_err_probe(dev, -ENOMEM, "phase_handles[0] dma mapping error");
	}
	ebc->phase_handles[1] = dma_map_single(dev, ebc->phase[1], ebc->phase_size,
					       DMA_TO_DEVICE);
	if (dma_mapping_error(dev, ebc->phase_handles[1])) {
		dma_unmap_single(dev, ebc->phase_handles[0], ebc->phase_size, DMA_TO_DEVICE);
		return dev_err_probe(dev, -ENOMEM, "phase_handles[1] dma mapping error");
	}
	if (!direct_mode) {
		ebc->zero_handle = dma_map_single(
						  dev, ebc->zero, ebc->gray4_size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, ebc->zero_handle)) {
			dma_unmap_single(dev, ebc->phase_handles[0], ebc->phase_size, DMA_TO_DEVICE);
			dma_unmap_single(dev, ebc->phase_handles[1], ebc->phase_size, DMA_TO_DEVICE);
			return dev_err_probe(dev, -ENOMEM,
					     "zero_handle dma mapping error");
		}
		dma_sync_single_for_device(dev, ebc->zero_handle, ebc->gray4_size, DMA_TO_DEVICE);
	}
	dma_sync_single_for_device(dev, ebc->phase_handles[0], ebc->phase_size, DMA_TO_DEVICE);
	dma_sync_single_for_device(dev, ebc->phase_handles[1], ebc->phase_size,
				   DMA_TO_DEVICE);

	memset(ebc->hints_ioctl, default_hint & ROCKCHIP_EBC_HINT_MASK,
	       ebc->num_pixels);

	// Custom waveform
	if (!direct_mode) {
		*((u32 *) ebc->hardware_wf + 16) = 0x55555555;
		*((u32 *) ebc->hardware_wf + 32) = 0xaaaaaaaa;
	}

	ebc->fast_mode = false;

	// Sensible temperature default
	ebc->temperature = temp_override > 0 ? temp_override : 25;
	ebc->work_item = ROCKCHIP_EBC_WORK_ITEM_CHANGE_LUT |
		ROCKCHIP_EBC_WORK_ITEM_INIT;

	spin_lock_init(&ebc->work_item_lock);
	spin_lock_init(&ebc->hints_ioctl_lock);
	ebc->suspend_was_requested = 0;

	platform_set_drvdata(pdev, ebc);
	init_completion(&ebc->display_end);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ebc->regmap = devm_regmap_init_mmio(dev, base,
					    &rockchip_ebc_regmap_config);
	if (IS_ERR(ebc->regmap))
		return PTR_ERR(ebc->regmap);

	regcache_cache_only(ebc->regmap, true);

	ebc->dclk = devm_clk_get(dev, "dclk");
	if (IS_ERR(ebc->dclk))
		return dev_err_probe(dev, PTR_ERR(ebc->dclk),
				     "Failed to get dclk\n");

	ebc->hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(ebc->hclk))
		return dev_err_probe(dev, PTR_ERR(ebc->hclk),
				     "Failed to get hclk\n");

	ebc->cpll_333m = devm_clk_get(dev, "cpll_333m");
	if (IS_ERR(ebc->cpll_333m))
		return dev_err_probe(dev, PTR_ERR(ebc->cpll_333m),
				     "Failed to get cpll_333m\n");

	ebc->temperature_channel = devm_iio_channel_get(dev, NULL);
	if (IS_ERR(ebc->temperature_channel))
		return dev_err_probe(dev, PTR_ERR(ebc->temperature_channel),
				     "Failed to get temperature I/O channel\n");

	for (i = 0; i < EBC_NUM_SUPPLIES; i++)
		ebc->supplies[i].supply = rockchip_ebc_supplies[i];

	ret = devm_regulator_bulk_get(dev, EBC_NUM_SUPPLIES, ebc->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       rockchip_ebc_irq, 0, dev_name(dev), ebc);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	pm_runtime_set_autosuspend_delay(dev, EBC_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		ret = rockchip_ebc_runtime_resume(&pdev->dev);
		if (ret)
			return ret;
	}

	ret = rockchip_ebc_waveform_init(ebc);
	if (ret)
		return ret;

	// Make sure ebc->lut_custom_active is initialised
	rockchip_ebc_change_lut(ebc);

	ebc->temp_upd_thread =
		kthread_create(rockchip_ebc_temp_upd_thread, ebc,
			       "ebc-tempupd/%s", dev_name(dev));
	if (IS_ERR(ebc->temp_upd_thread)) {
		ret = dev_err_probe(
			dev, PTR_ERR(ebc->temp_upd_thread),
			"Failed to start temperature update thread");
		goto err_disable_pm;
	}
	kthread_park(ebc->temp_upd_thread);

	ebc->refresh_thread = kthread_create(rockchip_ebc_refresh_thread,
					     ebc, "ebc-refresh/%s",
					     dev_name(dev));
	if (IS_ERR(ebc->refresh_thread)) {
		ret = dev_err_probe(dev, PTR_ERR(ebc->refresh_thread),
				    "Failed to start refresh thread\n");
		goto err_disable_pm;
	}

	kthread_park(ebc->refresh_thread);
	sched_set_fifo(ebc->refresh_thread);

	ret = rockchip_ebc_drm_init(ebc);
	if (ret)
		return ret;

	return 0;

err_disable_pm:
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);

	return ret;
}

static void rockchip_ebc_remove(struct platform_device *pdev)
{
	struct rockchip_ebc *ebc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	// pr_info("%s executing", __func__);

	drm_dev_unregister(&ebc->drm);
	kthread_stop(ebc->refresh_thread);
	kthread_stop(ebc->temp_upd_thread);
	drm_atomic_helper_shutdown(&ebc->drm);

	dma_unmap_single(dev, ebc->phase_handles[0], ebc->phase_size, DMA_TO_DEVICE);
	dma_unmap_single(dev, ebc->phase_handles[1], ebc->phase_size, DMA_TO_DEVICE);

	if (!direct_mode) {
		dma_unmap_single(dev, ebc->zero_handle, ebc->gray4_size,
				 DMA_TO_DEVICE);
	}

	vfree(ebc->hints_ioctl);
	vfree(ebc->packed_inner_outer_nextprev);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);
}

static void rockchip_ebc_shutdown(struct platform_device *pdev)
{
	struct rockchip_ebc *ebc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	// pr_info("%s executing", __func__);

	kthread_stop(ebc->refresh_thread);
	kthread_stop(ebc->temp_upd_thread);
	drm_atomic_helper_shutdown(&ebc->drm);

	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);
}

static const struct of_device_id rockchip_ebc_of_match[] = {
	{ .compatible = "rockchip,rk3568-ebc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rockchip_ebc_of_match);

static struct platform_driver rockchip_ebc_driver = {
	.probe		= rockchip_ebc_probe,
	.remove		= rockchip_ebc_remove,
	.shutdown	= rockchip_ebc_shutdown,
	.driver		= {
		.name		= "rockchip-ebc",
		.of_match_table	= rockchip_ebc_of_match,
		.pm		= &rockchip_ebc_dev_pm_ops,
	},
};
module_platform_driver(rockchip_ebc_driver);

MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>, Maximilian Weigand, hrdl <git@hrdl.eu>");
MODULE_DESCRIPTION("Rockchip EBC driver");
MODULE_LICENSE("GPL v2");
