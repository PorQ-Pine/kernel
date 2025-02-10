// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
 */

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

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/rockchip_ebc_drm.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_framebuffer.h>

#include "rockchip_ebc.h"
#include "rockchip_ebc_blit.h"
#include "rockchip_ebc_blit_neon.h"

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

#define EBC_FRAME_PENDING		(-1U)

#define EBC_MAX_PHASES			256

#define EBC_NUM_LUT_REGS		0x1000
#define EBC_NUM_SUPPLIES		3

#define EBC_FRAME_TIMEOUT		msecs_to_jiffies(25)
#define EBC_REFRESH_TIMEOUT		msecs_to_jiffies(3000)
#define EBC_SUSPEND_DELAY_MS		2000

#define EBC_FIRMWARE		"rockchip/ebc.wbf"
MODULE_FIRMWARE(EBC_FIRMWARE);
#define EBC_OFFCONTENT "rockchip/rockchip_ebc_default_screen.bin"
MODULE_FIRMWARE(EBC_OFFCONTENT);

#define ROCKCHIP_EBC_BLIT_FRAME_NUM_CHECK

struct rockchip_ebc {
	struct clk			*dclk;
	struct clk			*hclk;
	struct clk			*cpll_333m;
	struct completion		display_end;
	struct drm_crtc			crtc;
	struct drm_device		drm;
	struct drm_encoder		encoder;
	struct drm_epd_lut		lut;
	struct drm_epd_lut_file		lut_file;
	struct drm_plane		plane;
	struct iio_channel		*temperature_channel;
	struct regmap			*regmap;
	struct regulator_bulk_data	supplies[EBC_NUM_SUPPLIES];
	struct task_struct		*refresh_thread;
	u32				dsp_start;
	u16				act_width;
	u16				act_height;
	u16				hact_start;
	u16				vact_start;
	bool				lut_changed;
	bool				reset_complete;
	// one screen content: 1872 * 1404 / 2
	// the array size should probably be set dynamically...
	char off_screen[1314144];
	// before suspend we need to save the screen content so we can restore the
	// prev buffer after resuming
	char suspend_prev[1314144];
	char suspend_next[1314144];
	spinlock_t			refresh_once_lock;
	// should this go into the ctx?
	bool do_one_full_refresh;
	// used to detect when we are suspending so we can do different things to
	// the ebc display depending on whether we are sleeping or suspending
	int suspend_was_requested;
};

static int check_blit_frame_num = 0;
#ifdef ROCKCHIP_EBC_BLIT_FRAME_NUM_CHECK
module_param(check_blit_frame_num, int, 0644);
MODULE_PARM_DESC(
	check_blit_frame_num,
	"Check for scheduling errors why blitting to frame number buffer");
#endif

static int default_waveform = DRM_EPD_WF_GC16;
module_param(default_waveform, int, 0644);
MODULE_PARM_DESC(default_waveform, "waveform to use for display updates");

static bool shrink_virtual_window = false;
module_param(shrink_virtual_window, bool, 0644);
MODULE_PARM_DESC(shrink_virtual_window, "shrink virtual window to ongoing clip");

static bool shrink_damage_clip = false;
module_param(shrink_damage_clip, bool, 0644);
MODULE_PARM_DESC(shrink_damage_clip, "shrink damage clip if possible");

static bool diff_mode = true;
module_param(diff_mode, bool, 0644);
MODULE_PARM_DESC(diff_mode, "only compute waveforms for changed pixels");

static bool direct_mode = false;
module_param(direct_mode, bool, 0444);
MODULE_PARM_DESC(direct_mode, "compute waveforms in software (software LUT)");

static bool panel_reflection = true;
module_param(panel_reflection, bool, 0644);
MODULE_PARM_DESC(panel_reflection, "reflect the horizontally, otherwise vertically");

static bool skip_reset = false;
module_param(skip_reset, bool, 0444);
MODULE_PARM_DESC(skip_reset, "skip the initial display reset");

static int use_neon = 0;
module_param(use_neon, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(use_neon, "use neon-based functions for blitting");

static bool auto_refresh = false;
module_param(auto_refresh, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(auto_refresh, "auto refresh the screen based on partial refreshed area");

static int refresh_threshold = 20;
module_param(refresh_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(refresh_threshold, "refresh threshold in screen area multiples");

static int refresh_waveform = DRM_EPD_WF_GC16;
module_param(refresh_waveform, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(refresh_waveform, "refresh waveform to use");

static int split_area_limit = 12;
module_param(split_area_limit, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(split_area_limit, "how many areas to split in each scheduling call");

static int limit_fb_blits = -1;
module_param(limit_fb_blits, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(split_area_limit, "how many fb blits to allow. -1 does not limit");

static bool no_off_screen = false;
module_param(no_off_screen, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(no_off_screen, "If set to true, do not apply the offscreen on next loop exit");

/* delay parameters used to delay the return of plane_atomic_atomic */
/* see plane_atomic_update function for specific usage of these parameters */
static int delay_a = 2000;
module_param(delay_a, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_a, "delay_a");

static int delay_b = 100000;
module_param(delay_b, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_b, "delay_b");

static int delay_c = 1000;
module_param(delay_c, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_c, "delay_c");

// mode = 0: 16-level gray scale
// mode = 1: 2-level black&white with dithering enabled
// mode = 2: 2-level black&white, uses bw_threshold
// mode = 3: 4-level gray scale
static int bw_mode = 0;
module_param(bw_mode, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_mode, "black & white mode");

static int bw_threshold = 7;
module_param(bw_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_threshold, "black and white threshold");

static int fourtone_low_threshold = 4;
module_param(fourtone_low_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(fourtone_low_threshold, "everything below this is white");

static int fourtone_mid_threshold = 7;
module_param(fourtone_mid_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(fourtone_mid_threshold, "from low_threshold to here is light gray; from here to hi_threhsold is dark gray");

static int fourtone_hi_threshold = 12;
module_param(fourtone_hi_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(fourtone_hi_threshold, "everything above this is black");

static int bw_dither_invert = 0;
module_param(bw_dither_invert, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_dither_invert, "invert dither colors in bw mode");

static bool prepare_prev_before_a2 = false;
module_param(prepare_prev_before_a2, bool, 0644);
MODULE_PARM_DESC(prepare_prev_before_a2, "Convert prev buffer to bw when switchting to the A2 waveform");

static bool globre_convert_before = false;
module_param(globre_convert_before, bool, 0644);
MODULE_PARM_DESC(globre_convert_before, "Convert prev buffer to target color space before global refresh");

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


DEFINE_DRM_GEM_FOPS(rockchip_ebc_fops);

static int ioctl_trigger_global_refresh(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_trigger_global_refresh *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);

	if (args->trigger_global_refresh){
		/* printk(KERN_INFO "[rockchip_ebc] ioctl_trigger_global_refresh"); */
		spin_lock(&ebc->refresh_once_lock);
		ebc->do_one_full_refresh = true;
		spin_unlock(&ebc->refresh_once_lock);
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

	copy_result = copy_from_user(&ebc->off_screen, args->ptr_screen_content, 1313144);
	if (copy_result != 0){
		pr_err("Could not copy offscreen content from user-supplied data pointer (bytes not copied: %i)", copy_result);
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
	int copy_result = 0;
	struct rockchip_ebc_ctx * ctx;

	// todo: use access_ok here
	access_ok(args->ptr_next, 1313144);
	ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;
	copy_result |= copy_to_user(args->ptr_prev, ctx->prev, 1313144);
	copy_result |= copy_to_user(args->ptr_next, ctx->next, 1313144);
	copy_result |= copy_to_user(args->ptr_final, ctx->final, 1313144);
	// TODO final_atomic_update ?

	if (direct_mode) {
		copy_result |= copy_to_user(args->ptr_phase1, ctx->phase[0], 2 * 1313144);
		copy_result |= copy_to_user(args->ptr_phase2, ctx->phase[1], 2 * 1313144);
	}

	return copy_result;
}

static const struct drm_ioctl_desc ioctls[DRM_COMMAND_END - DRM_COMMAND_BASE] = {
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_GLOBAL_REFRESH, ioctl_trigger_global_refresh,
			DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_OFF_SCREEN, ioctl_set_off_screen,
			DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_EXTRACT_FBS, ioctl_extract_fbs,
			DRM_RENDER_ALLOW),
};

static const struct drm_driver rockchip_ebc_drm_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	.major			= 0,
	.minor			= 3,
	.name			= "rockchip-ebc",
	.desc			= "Rockchip E-Book Controller",
	.date			= "20220303",
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
	struct rockchip_ebc_area *area;
	pr_info("EBC: rockchip_ebc_ctx_free");

	list_for_each_entry(area, &ctx->queue, list)
		kfree(area);

	kfree(ctx->prev);
	kfree(ctx->next);
	kfree(ctx->final_buffer[0]);
	kfree(ctx->final_buffer[1]);
	kfree(ctx->final_atomic_update);
	kfree(ctx->frame_num[0]);
	kfree(ctx->frame_num[1]);
	kfree(ctx->phase[0]);
	kfree(ctx->phase[1]);
	kfree(ctx);
}

static struct rockchip_ebc_ctx *rockchip_ebc_ctx_alloc(struct rockchip_ebc *ebc, u32 width, u32 height)
{
	u32 gray4_size = width * height / 2;
	u32 phase_size = width * height / 4;
	u32 frame_num_size = width * height;
	struct rockchip_ebc_ctx *ctx;
	/* pr_info("ebc: %s", __func__); */

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	// TODO: ensure 16 byte alignment
	ctx->prev = kmalloc(gray4_size, GFP_KERNEL | (direct_mode ? 0 : GFP_DMA));
	ctx->next = kmalloc(gray4_size, GFP_KERNEL | (direct_mode ? 0 : GFP_DMA));
	ctx->final_buffer[0] = kmalloc(gray4_size, GFP_KERNEL);
	ctx->final_buffer[1] = kmalloc(gray4_size, GFP_KERNEL);
	ctx->final_atomic_update = kmalloc(gray4_size, GFP_KERNEL);
	ctx->frame_num[0] = kmalloc(frame_num_size, GFP_KERNEL | (direct_mode ? 0 : GFP_DMA));
	ctx->frame_num[1] = kmalloc(frame_num_size, GFP_KERNEL | (direct_mode ? 0 : GFP_DMA));
	if (direct_mode) {
		ctx->phase[0] = kmalloc(phase_size, GFP_KERNEL | GFP_DMA);
		ctx->phase[1] = kmalloc(phase_size, GFP_KERNEL | GFP_DMA);
	}
	if (!ctx->prev || !ctx->next || !ctx->final_buffer[0] ||
	    !ctx->final_buffer[1] || !ctx->final_atomic_update ||
	    !ctx->frame_num[0] || !ctx->frame_num[1] ||
	    (direct_mode && (!ctx->phase[0] || !ctx->phase[1]))) {
		rockchip_ebc_ctx_free(ctx);
		return NULL;
	}

	kref_init(&ctx->kref);
	INIT_LIST_HEAD(&ctx->queue);
	spin_lock_init(&ctx->queue_lock);
	ctx->final = ctx->final_buffer[0];
	ctx->ebc_buffer_index = 0;
	ctx->first_switch = true;
	ctx->switch_required = true;
	ctx->gray4_pitch = width / 2;
	ctx->gray4_size  = gray4_size;
	ctx->phase_pitch = width / 4;
	ctx->phase_size  = phase_size;
	ctx->frame_num_size = frame_num_size;
	ctx->frame_num_pitch = width;
	ctx->mapped_win_size = direct_mode ? phase_size : frame_num_size;

	// we keep track of the updated area and use this value to trigger global
	// refreshes if auto_refresh is enabled
	ctx->area_count = 0;

	return ctx;
}

static void rockchip_ebc_ctx_release(struct kref *kref)
{
	struct rockchip_ebc_ctx *ctx =
		container_of(kref, struct rockchip_ebc_ctx, kref);
	pr_info("ebc: %s", __func__);

	return rockchip_ebc_ctx_free(ctx);
}

static void rockchip_ebc_global_refresh_direct(struct rockchip_ebc *ebc,
		struct rockchip_ebc_ctx *ctx
		);

/*
 * CRTC
 */

static void convert_final_buf_to_target(u8 * buffer, u8 * tmp, u32 gray4_size){
	u8 * src;
	u8 * dst;
	u8 pixel1;
	u8 pixel2;
	int x, y;

	int pattern[4][4] = {
		{7, 8, 2, 10},
		{12, 4, 14, 6},
		{3, 11, 1,  9},
		{15, 7, 13, 5},
	};

	u8 dither_low = bw_dither_invert ? 15 : 0;
	u8 dither_high = bw_dither_invert ? 0 : 15;

	if (default_waveform == 1) {
		// A2 waveform
		// apply dithering
		src = buffer;
		dst = tmp;

		for (y=0; y<1404; y++){
			for (x=0; x<1872; x=x+2){
				pixel1 = *src & 0b00001111;
				pixel2 = (*src & 0b11110000) >> 4;

				if (pixel1 >= pattern[x & 3][y & 3]){
					pixel1 = dither_high;
				} else {
					pixel1 = dither_low;
				}

				if (pixel2 >= pattern[(x + 1) & 3][y & 3]){
					pixel2 = dither_high;
				} else {
					pixel2 = dither_low;
				}

				*dst = pixel1 | pixel2 << 4;
				src++;
				dst++;

			}
		}
		// now tmp should contain the dithered image. copy it back
		memcpy(buffer, tmp, gray4_size);
	}

	// DU4 waveform
	if (default_waveform == 3){
		src = buffer;
		dst = tmp;

		for (x=0; x<1872; x=x+2){
			for (y=0; y<1404; y++){
				pixel1 = *src & 0b00001111;
				pixel2 = (*src & 0b11110000) >> 4;
				// downsample to 4 bw values corresponding to the DU4
				// transitions: 0, 5, 10, 15
				if (pixel1 < fourtone_low_threshold){
					pixel1 = 0;
				} else if (pixel1  < fourtone_mid_threshold){
					pixel1 = 5;
				} else if (pixel1  < fourtone_hi_threshold){
					pixel1 = 10;
				} else {
					pixel1 = 15;
				}

				if (pixel2 < fourtone_low_threshold){
					pixel2 = 0;
				} else if (pixel2  < fourtone_mid_threshold){
					pixel2 = 5;
				} else if (pixel2  < fourtone_hi_threshold){
					pixel2 = 10;
				} else {
					pixel2 = 15;
				}

				*dst = (pixel2 << 4) | pixel1;
				src++;
				dst++;
			}
		}
		// now tmp should contain the down-sampled image. copy it back
		memcpy(buffer, tmp, gray4_size);
	}
}

static void rockchip_ebc_global_refresh(struct rockchip_ebc *ebc,
		struct rockchip_ebc_ctx *ctx,
		dma_addr_t next_handle,
		dma_addr_t prev_handle
		)
{
	struct drm_device *drm = &ebc->drm;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;

	struct rockchip_ebc_area *area, *next_area;
	LIST_HEAD(areas);

	if (direct_mode)
		return rockchip_ebc_global_refresh_direct(ebc, ctx);

	spin_lock(&ctx->queue_lock);
	list_splice_tail_init(&ctx->queue, &areas);
	// switch buffers
	if(ctx->switch_required){
		ctx->ebc_buffer_index = !ctx->ebc_buffer_index;
		ctx->switch_required = false;
	}
	ctx->final = ctx->final_buffer[ctx->ebc_buffer_index];
	// we either want to force a conversion using the module parameter
	// globre_convert_before, or we are coming out of suspend - in this case,
	// make sure to convert to the current waveform (A2 or DU4) in this global
	// refresh so subsequent draws will start from a known, reachable state by
	// those waveforms
	if (globre_convert_before || ebc->suspend_was_requested){
		// convert both final buffers to the target colorspace (i.e., the
		// current default waveform
		// note: we use next as a tmp buffer, as it will be overwritten a few
		// lines further down
		convert_final_buf_to_target(ctx->final_buffer[0], ctx->next, gray4_size);
		convert_final_buf_to_target(ctx->final_buffer[1], ctx->next, gray4_size);
	}
	spin_unlock(&ctx->queue_lock);
	memcpy(ctx->next, ctx->final, gray4_size);

	dma_sync_single_for_device(dev, next_handle, gray4_size, DMA_TO_DEVICE);
	dma_sync_single_for_device(dev, prev_handle, gray4_size, DMA_TO_DEVICE);

	reinit_completion(&ebc->display_end);
	regmap_write(ebc->regmap, EBC_WIN_VIR, EBC_WIN_VIR_WIN_VIR_HEIGHT(ebc->act_height) | EBC_WIN_VIR_WIN_VIR_WIDTH(ebc->act_width));
	regmap_write(ebc->regmap, EBC_WIN_ACT, EBC_WIN_ACT_WIN_ACT_HEIGHT(ebc->act_height) | EBC_WIN_ACT_WIN_ACT_WIDTH(ebc->act_width));
	regmap_write(ebc->regmap, EBC_WIN_DSP, EBC_WIN_DSP_WIN_DSP_HEIGHT(ebc->act_height) | EBC_WIN_DSP_WIN_DSP_WIDTH(ebc->act_width));
	regmap_write(ebc->regmap, EBC_WIN_DSP_ST, EBC_WIN_DSP_ST_WIN_DSP_YST(ebc->vact_start) | EBC_WIN_DSP_ST_WIN_DSP_XST(ebc->hact_start));

	regmap_write(ebc->regmap, EBC_CONFIG_DONE,
			EBC_CONFIG_DONE_REG_CONFIG_DONE);
	regmap_write(ebc->regmap, EBC_DSP_START,
			ebc->dsp_start |
			EBC_DSP_START_DSP_FRM_TOTAL(ebc->lut.num_phases - 1) |
			EBC_DSP_START_DSP_FRM_START);
	// while we wait for the refresh, delete all scheduled areas
	list_for_each_entry_safe(area, next_area, &areas, list) {
		list_del(&area->list);
		kfree(area);
	}

	if (!wait_for_completion_timeout(&ebc->display_end,
				EBC_REFRESH_TIMEOUT))
		drm_err(drm, "Refresh timed out!\n");

	memcpy(ctx->prev, ctx->next, gray4_size);
	// this was the first global refresh after resume, reset the variable
	ebc->suspend_was_requested = 0;
}
static void rockchip_ebc_global_refresh_direct(struct rockchip_ebc *ebc,
					struct rockchip_ebc_ctx *ctx)
{
	struct drm_device *drm = &ebc->drm;
	u32 last_phase = ebc->lut.num_phases - 1;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;
	struct drm_rect screen_clip = DRM_RECT_INIT(0, 0, 1872, 1404);

	struct rockchip_ebc_area *area, *next_area;
	LIST_HEAD(areas);

	dma_addr_t phase_handles[2];
	phase_handles[0] = dma_map_single(dev, ctx->phase[0], ctx->phase_size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, phase_handles[0])) {
		drm_err(drm, "phase_handles[0] dma mapping error");
	}
	phase_handles[1] = dma_map_single(dev, ctx->phase[1], ctx->phase_size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, phase_handles[1])) {
		drm_err(drm, "phase_handles[0] dma mapping error");
	}

	spin_lock(&ctx->queue_lock);
	list_splice_tail_init(&ctx->queue, &areas);
	// switch buffers
	if(ctx->switch_required){
		ctx->ebc_buffer_index = !ctx->ebc_buffer_index;
		ctx->switch_required = false;
	}
	ctx->final = ctx->final_buffer[ctx->ebc_buffer_index];
	spin_unlock(&ctx->queue_lock);
	memcpy(ctx->next, ctx->final, gray4_size);

	for (int phase = 0; phase <= last_phase; ++phase)
	{
		u8 *phase_buffer = ctx->phase[phase % 2];
		dma_addr_t phase_handle = phase_handles[phase % 2];

		rockchip_ebc_blit_direct(ctx, phase_buffer, phase >= last_phase ? 0xff : phase, &ebc->lut, &screen_clip);
		dma_sync_single_for_device(dev, phase_handle, ctx->phase_size, DMA_TO_DEVICE);

		if (phase > 0 && !wait_for_completion_timeout(&ebc->display_end,
						 EBC_FRAME_TIMEOUT))
			drm_err(drm, "Frame %d timed out!\n", phase);

		regmap_write(ebc->regmap, EBC_WIN_VIR, EBC_WIN_VIR_WIN_VIR_HEIGHT(ebc->act_height) | EBC_WIN_VIR_WIN_VIR_WIDTH(ebc->act_width));
		regmap_write(ebc->regmap, EBC_WIN_ACT, EBC_WIN_ACT_WIN_ACT_HEIGHT(ebc->act_height) | EBC_WIN_ACT_WIN_ACT_WIDTH(ebc->act_width));
		regmap_write(ebc->regmap, EBC_WIN_DSP, EBC_WIN_DSP_WIN_DSP_HEIGHT(ebc->act_height) | EBC_WIN_DSP_WIN_DSP_WIDTH(ebc->act_width));
		regmap_write(ebc->regmap, EBC_WIN_DSP_ST, EBC_WIN_DSP_ST_WIN_DSP_YST(ebc->vact_start) | EBC_WIN_DSP_ST_WIN_DSP_XST(ebc->hact_start));
		regmap_write(ebc->regmap, EBC_WIN_MST0, phase_handle);
		regmap_write(ebc->regmap, EBC_CONFIG_DONE, EBC_CONFIG_DONE_REG_CONFIG_DONE);
		regmap_write(ebc->regmap, EBC_DSP_START, ebc->dsp_start | EBC_DSP_START_DSP_FRM_START);
		pr_debug("tcon started on frame %d", phase);
	}
	// Ensure both buffers are set to neutral polarity
	memset(ctx->phase[(last_phase + 1) % 2], 0, ctx->phase_size);

	// while we wait for the refresh, delete all scheduled areas
	list_for_each_entry_safe(area, next_area, &areas, list) {
		list_del(&area->list);
		kfree(area);
	}

	if (!wait_for_completion_timeout(&ebc->display_end, EBC_FRAME_TIMEOUT))
		drm_err(drm, "Last frame timed out");

	dma_unmap_single(dev, phase_handles[0], ctx->phase_size, DMA_TO_DEVICE);
	dma_unmap_single(dev, phase_handles[1], ctx->phase_size, DMA_TO_DEVICE);

	memcpy(ctx->prev, ctx->next, gray4_size);
}

static void rockchip_ebc_partial_refresh(struct rockchip_ebc *ebc,
					 struct rockchip_ebc_ctx *ctx,
					 dma_addr_t next_handle,
					 dma_addr_t prev_handle
					 )
{
	struct rockchip_ebc_area *area, *next_area;
	u32 last_phase = ebc->lut.num_phases - 1;
	struct drm_device *drm = &ebc->drm;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;
	LIST_HEAD(areas);
	u32 frame;
	u64 local_area_count = 0;
	ktime_t times[100];
	int time_index = 0;
	s64 duration;
	u32 min_frame_delay = 1000000;
	u32 max_frame_delay = 0;
	struct drm_rect clip_ongoing = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };

	dma_addr_t win_handles[2];
	win_handles[0] = dma_map_single(
		dev, direct_mode ? ctx->phase[0] : ctx->frame_num[0],
		ctx->mapped_win_size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, win_handles[0])) {
		drm_err(drm, "win_handles[0] dma mapping error");
	}
	win_handles[1] = dma_map_single(
		dev, direct_mode ? ctx->phase[1] : ctx->frame_num[1],
		ctx->mapped_win_size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, win_handles[1])) {
		drm_err(drm, "win_handles[1] dma mapping error");
	}

	/* Move the queued damage areas to the local list. */
	spin_lock(&ctx->queue_lock);
	list_splice_tail_init(&ctx->queue, &areas);
	// switch buffers
	if(ctx->switch_required){
		ctx->ebc_buffer_index = !ctx->ebc_buffer_index;
		ctx->switch_required = false;
	}
	ctx->final = ctx->final_buffer[ctx->ebc_buffer_index];
	spin_unlock(&ctx->queue_lock);

	times[time_index++] = ktime_get();
	for (frame = 0;; frame++) {
		u8 *phase_buffer = ctx->phase[frame % 2];
		u8 *frame_num_buffer = ctx->frame_num[frame % 2];
		dma_addr_t win_handle = win_handles[frame % 2];
		bool sync_next = false;
		bool sync_prev = false;
		struct drm_rect clip_ongoing_new_areas = { .x1 = 100000, .x2 = 0, .y1 = 100000, .y2 = 0 };
		// Used to reset the second phase buffer in direct mode after last_phase
		struct drm_rect clip_needs_sync = clip_ongoing;

		if (frame > 0) {
			// Increase frame number of running frames by one
			if (use_neon & 1) {
				kernel_neon_begin();
				rockchip_ebc_update_blit_fnum_prev_neon(
					ctx, ctx->prev, ctx->next,
					frame_num_buffer,
					ctx->frame_num[(frame + 1) % 2],
					&clip_ongoing, last_phase);
				kernel_neon_end();
				pr_debug("%s inc %d " DRM_RECT_FMT " " DRM_RECT_FMT, __func__, frame, DRM_RECT_ARG(&clip_needs_sync), DRM_RECT_ARG(&clip_ongoing));
			} else {
				rockchip_ebc_increment_frame_num(
					ctx, frame_num_buffer,
					ctx->frame_num[(frame + 1) % 2],
					&clip_ongoing, last_phase);
			}
		}

		list_for_each_entry_safe(area, next_area, &areas, list) {
			if (area->frame_begin == EBC_FRAME_PENDING || area->frame_begin == frame) {
				pr_debug("%s schedul1 %d " DRM_RECT_FMT " " DRM_RECT_FMT, __func__, frame, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&clip_ongoing_new_areas));
				if (use_neon & 2) {
					kernel_neon_begin();
					rockchip_ebc_schedule_and_blit_neon(
						ctx, frame_num_buffer,
						ctx->next, ctx->final,
						&clip_ongoing,
						&clip_ongoing_new_areas, area,
						frame, last_phase, next_area);
					kernel_neon_end();
				} else {
					rockchip_ebc_schedule_and_blit(
						ctx, frame_num_buffer,
						ctx->next, ctx->final,
						&clip_ongoing,
						&clip_ongoing_new_areas, area,
						frame, last_phase, next_area);
				}
				pr_debug("%s schedul2 %d " DRM_RECT_FMT " " DRM_RECT_FMT, __func__, frame, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&clip_ongoing_new_areas));
				sync_next = true;
				if (drm_rect_width(&area->clip) <= 0) {
					// All pixels covered by this region have been blitted
					list_del(&area->list);
					kfree(area);
				}
			}
		}

		rockchip_ebc_drm_rect_extend_rect(&clip_ongoing_new_areas, &clip_ongoing);
		rockchip_ebc_drm_rect_extend_rect(&clip_needs_sync, &clip_ongoing_new_areas);

		if (direct_mode) {
			if ((use_neon & 4) && ebc->lut.mode_index == pvi_wbf_get_mode_index(ebc->lut.file, DRM_EPD_WF_A2)) {
				kernel_neon_begin();
				rockchip_ebc_blit_direct_fnum_a2_neon(
					ctx, phase_buffer, frame_num_buffer,
					ctx->next, ctx->prev, &ebc->lut,
					&clip_needs_sync);
				kernel_neon_end();
			} else {
				rockchip_ebc_blit_direct_fnum(
					ctx, phase_buffer, frame_num_buffer,
					ctx->next, ctx->prev, &ebc->lut,
					&clip_needs_sync);
			}
		}

		// Blit pixels of next to prev that are at last_phase (0xff)
		if (use_neon & 1) {
			sync_prev = true;
		} else if (rockchip_ebc_blit_pixels_last(
				   ctx, ctx->prev, ctx->next, frame_num_buffer,
				   &clip_ongoing, last_phase))
			sync_prev = true;

		// TODO: shrink these
		if (sync_next && !direct_mode)
			dma_sync_single_for_device(dev, next_handle, gray4_size,
						   DMA_TO_DEVICE);
		if (sync_prev && !direct_mode)
			dma_sync_single_for_device(dev, prev_handle, gray4_size,
						   DMA_TO_DEVICE);
		dma_sync_single_for_device(dev, win_handle,
					   ctx->mapped_win_size, DMA_TO_DEVICE);

		if (frame > 0 && !wait_for_completion_timeout(
					 &ebc->display_end, EBC_FRAME_TIMEOUT))
			drm_err(drm, "Frame %d timed out!\n", frame);

		// record time after frame completed
		if (frame > 0 && time_index < 100) {
			times[time_index++] = ktime_get();
		}

		if (use_neon & 1) {
			if (drm_rect_width(&clip_needs_sync) <= 0)
				break;
		} else {
			if (drm_rect_width(&clip_ongoing) <= 0)
				break;
		}

		if (shrink_virtual_window) {
			u16 adj_win_width = ((clip_ongoing.x2 + 7) & ~7) - (clip_ongoing.x1 & ~7);
			unsigned int win_start_offset = ebc->act_width * clip_ongoing.y1 + (clip_ongoing.x1 & ~7);
			regmap_write(ebc->regmap, EBC_WIN_VIR, EBC_WIN_VIR_WIN_VIR_HEIGHT(drm_rect_height(&clip_ongoing)) | EBC_WIN_VIR_WIN_VIR_WIDTH(ctx->gray4_pitch * 2));
			regmap_write(ebc->regmap, EBC_WIN_ACT, EBC_WIN_ACT_WIN_ACT_HEIGHT(drm_rect_height(&clip_ongoing)) | EBC_WIN_ACT_WIN_ACT_WIDTH(adj_win_width));
			regmap_write(ebc->regmap, EBC_WIN_DSP, EBC_WIN_DSP_WIN_DSP_HEIGHT(drm_rect_height(&clip_ongoing)) | EBC_WIN_DSP_WIN_DSP_WIDTH(adj_win_width));
			regmap_write(ebc->regmap, EBC_WIN_DSP_ST, EBC_WIN_DSP_ST_WIN_DSP_YST(ebc->vact_start + clip_ongoing.y1) | EBC_WIN_DSP_ST_WIN_DSP_XST(ebc->hact_start + clip_ongoing.x1 / 8));
			if (direct_mode) {
				regmap_write(ebc->regmap, EBC_WIN_MST0, win_handle + win_start_offset / 4);
			} else {
				regmap_write(ebc->regmap, EBC_WIN_MST0, prev_handle + win_start_offset / 2);
				regmap_write(ebc->regmap, EBC_WIN_MST1, next_handle + win_start_offset / 2);
				regmap_write(ebc->regmap, EBC_WIN_MST2, win_handle + win_start_offset);
			}
		} else {
			if (direct_mode) {
				regmap_write(ebc->regmap, EBC_WIN_MST0, win_handle);
			} else {
				regmap_write(ebc->regmap, EBC_WIN_MST0, prev_handle);
				regmap_write(ebc->regmap, EBC_WIN_MST1, next_handle);
				regmap_write(ebc->regmap, EBC_WIN_MST2, win_handle);
			}
		}
		regmap_write(ebc->regmap, EBC_CONFIG_DONE,
			     EBC_CONFIG_DONE_REG_CONFIG_DONE);
		regmap_write(ebc->regmap, EBC_DSP_START,
			     ebc->dsp_start |
			     EBC_DSP_START_DSP_FRM_START);

		// at this point the ebc is working. It does not access the final
		// buffer directly, and therefore we can use the time to update the
		// queue. Well, we probably only wait for the spinlock in case the
		// atomic_update function is currently blitting

		clip_ongoing = clip_ongoing_new_areas;

		if (ctx->switch_required){
			pr_debug("    we could switch now");
		}
		// we have some time until the frame finishes, try a few times to
		// acquire the lock
		for (int i=0; i <= 5; i++){
			if (spin_trylock(&ctx->queue_lock)){
				list_splice_tail_init(&ctx->queue, &areas);
				// switch buffers
				if(ctx->switch_required){
					ctx->ebc_buffer_index = !ctx->ebc_buffer_index;
					ctx->switch_required = false;
				}
				ctx->final = ctx->final_buffer[ctx->ebc_buffer_index];
				spin_unlock(&ctx->queue_lock);
				break;
			}
			else {
				pr_debug("    but did not get the lock");
			}
			// sleep 1 ms
			usleep_range(1 * 1000 - 100, 1 * 1000);
		}

		if (kthread_should_stop()) {
			break;
		};
	}

	dma_unmap_single(dev, win_handles[0], ctx->mapped_win_size, DMA_TO_DEVICE);
	dma_unmap_single(dev, win_handles[1], ctx->mapped_win_size, DMA_TO_DEVICE);

	ctx->area_count += local_area_count;

	// print the min/max execution times from within the first 100 frames
	for (int i=1; i < min(time_index, 100); i++){
		duration = ktime_us_delta(times[i], times[i-1]);
		if (duration > max_frame_delay)
			if (duration <= 100000)
					max_frame_delay = duration;
		if (duration < min_frame_delay)
			if (duration <= 100000)
				min_frame_delay = duration;
		pr_debug("ebc: frame %i took %llu us", i, duration);
	}
	pr_debug("ebc: min/max frame durations: %u/%u [us]", min_frame_delay, max_frame_delay);
}

static void rockchip_ebc_refresh(struct rockchip_ebc *ebc,
				 struct rockchip_ebc_ctx *ctx,
				 bool global_refresh,
				 enum drm_epd_waveform waveform)
{
	struct drm_device *drm = &ebc->drm;
	u32 dsp_ctrl = 0, epd_ctrl = 0;
	struct device *dev = drm->dev;
	int ret, temperature;
	dma_addr_t next_handle;
	dma_addr_t prev_handle;
	int one_screen_area = 1314144;
	/* printk(KERN_INFO "[rockchip_ebc] rockchip_ebc_refresh"); */

	/* Resume asynchronously while preparing to refresh. */
	ret = pm_runtime_get(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to request resume: %d\n", ret);
		return;
	}

	ret = iio_read_channel_processed(ebc->temperature_channel, &temperature);
	if (ret < 0) {
		drm_err(drm, "Failed to get temperature: %d\n", ret);
	} else {
		/* Convert from millicelsius to celsius. */
		temperature /= 1000;

		if (temp_override > 0){
			printk(KERN_INFO "rockchip-ebc: override temperature from %i to %i\n", temp_override, temperature);
			temperature = temp_override;
		}

		ret = drm_epd_lut_set_temperature(&ebc->lut, temperature);
		if (ret < 0)
			drm_err(drm, "Failed to set LUT temperature: %d\n", ret);
		else if (ret)
			ebc->lut_changed = true;
	}

	ret = drm_epd_lut_set_waveform(&ebc->lut, waveform);
	if (ret < 0)
		drm_err(drm, "Failed to set LUT waveform: %d\n", ret);
	else if (ret)
		ebc->lut_changed = true;

	/* if we change to A2 in bw mode, then make sure that the prev-buffer is
	 * converted to bw so the A2 waveform can actually do anything
	 * */
	// todo: make optional
	if (prepare_prev_before_a2){
		if(ebc->lut_changed && waveform == 1){
			u8 pixel1, pixel2;
			void *src = ctx->prev;
			u8 *sbuf = src;
			int index;
			printk(KERN_INFO "Change to A2 waveform detected, converting prev to bw");

			for (index=0; index < ctx->gray4_size; index++){
				pixel1 = *sbuf & 0b00001111;
				pixel2 = (*sbuf & 0b11110000) >> 4;

				// convert to bw
				if (pixel1 > 7)
					pixel1 = 15;
				else
					pixel1 = 0;
				if (pixel2 > 7)
					pixel2 = 15;
				else
					pixel2 = 0;

				*sbuf++ = pixel1 | pixel2 << 4;
			}
		}
	}

	/* Wait for the resume to complete before writing any registers. */
	ret = pm_runtime_resume(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to resume: %d\n", ret);
		pm_runtime_put(dev);
		return;
	}

	/* This flag may have been set above, or by the runtime PM callback. */
	if (ebc->lut_changed) {
		ebc->lut_changed = false;
		if (!direct_mode) {
			regmap_bulk_write(ebc->regmap, EBC_LUT_DATA,
						ebc->lut.buf, EBC_NUM_LUT_REGS);
		}
	}

	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start);

	/*
	 * The hardware has a separate bit for each mode, with some priority
	 * scheme between them. For clarity, only set one bit at a time.
	 *
	 * NOTE: In direct mode, no mode bits are set.
	 */
	if (global_refresh) {
		if (!direct_mode)
			dsp_ctrl |= EBC_DSP_CTRL_DSP_LUT_MODE;
	} else if (!direct_mode) {
		epd_ctrl |= EBC_EPD_CTRL_DSP_THREE_WIN_MODE;
		if (diff_mode)
			dsp_ctrl |= EBC_DSP_CTRL_DSP_DIFF_MODE;
	}
	regmap_update_bits(ebc->regmap, EBC_EPD_CTRL,
			   EBC_EPD_CTRL_DSP_THREE_WIN_MODE,
			   epd_ctrl);
	regmap_update_bits(ebc->regmap, EBC_DSP_CTRL,
			   EBC_DSP_CTRL_DSP_DIFF_MODE |
			   EBC_DSP_CTRL_DSP_LUT_MODE,
			   dsp_ctrl);

	if (!direct_mode)
	{
		next_handle = dma_map_single(dev, ctx->next, ctx->gray4_size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, next_handle)) {
			drm_err(drm, "next_handle dma mapping error");
		}
		prev_handle = dma_map_single(dev, ctx->prev, ctx->gray4_size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, prev_handle)) {
			drm_err(drm, "prev_handle dma mapping error");
		}

		regmap_write(ebc->regmap, EBC_WIN_MST1,
					 next_handle);
		regmap_write(ebc->regmap, EBC_WIN_MST0,
					 prev_handle);
	}

	/* printk(KERN_INFO "[rockchip_ebc] ebc_refresh"); */
	if (global_refresh)
		rockchip_ebc_global_refresh(ebc, ctx, next_handle, prev_handle);
	else
		rockchip_ebc_partial_refresh(ebc, ctx, next_handle, prev_handle);

	if (!direct_mode)
	{
		dma_unmap_single(dev, next_handle, ctx->gray4_size, DMA_TO_DEVICE);
		dma_unmap_single(dev, prev_handle, ctx->gray4_size, DMA_TO_DEVICE);
	}

	/* Drive the output pins low once the refresh is complete. */
	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start |
		     EBC_DSP_START_DSP_OUT_LOW);


	// do we need a full refresh
	if (auto_refresh){
		if (ctx->area_count >= refresh_threshold * one_screen_area){
			spin_lock(&ebc->refresh_once_lock);
			ebc->do_one_full_refresh = true;
			spin_unlock(&ebc->refresh_once_lock);
			ctx->area_count = 0;
		}
	} else {
		ctx->area_count = 0;
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int rockchip_ebc_refresh_thread(void *data)
{
	struct rockchip_ebc *ebc = data;
	struct rockchip_ebc_ctx *ctx;
	bool one_full_refresh;
	/* printk(KERN_INFO "[rockchip_ebc] rockchip_ebc_refresh_thread"); */

	while (!kthread_should_stop()) {
		/* printk(KERN_INFO "[rockchip_ebc] just started"); */
		/* The context will change each time the thread is unparked. */
		ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;

		/*
		 * Initialize the buffers before use. This is deferred to the
		 * kthread to avoid slowing down atomic_check.
		 *
		 * ctx->prev and ctx->next are set to 0xff, all white, because:
		 *  1) the display is set to white by the reset waveform, and
		 *  2) the driver maintains the invariant that the display is
		 *     all white whenever the CRTC is disabled.
		 *
		 * ctx->final is initialized by the first plane update.
		 *
		 * ctx->phase is set to 0xff, the number of the last possible
		 * phase, because the LUT for that phase is known to be all
		 * zeroes. (The last real phase in a waveform is zero in order
		 * to discharge the display, and unused phases in the LUT are
		 * zeroed out.) This prevents undesired driving of the display
		 * in 3-window mode between when the framebuffer is blitted
		 * (and thus prev != next) and when the refresh thread starts
		 * counting phases for that region.
		 */
		memcpy(ctx->prev, ebc->suspend_prev, ctx->gray4_size);
		if(ebc->suspend_was_requested == 1){
			// this means we are coming out from suspend. Reset the buffers to
			// the before-suspend state
			/* memcpy(ctx->prev, ebc->suspend_prev, ctx->gray4_size); */
			memcpy(ctx->final, ebc->suspend_next, ctx->gray4_size);
			/* memset(ctx->prev, 0xff, ctx->gray4_size); */
			memset(ctx->next, 0xff, ctx->gray4_size);
			/* memset(ctx->final, 0xff, ctx->gray4_size); */
			ebc->do_one_full_refresh = 1;
		} else {
			// only on first run
			if (!ebc->reset_complete) {
				/* memset(ctx->prev, 0xff, ctx->gray4_size); */
				memset(ctx->next, 0xff, ctx->gray4_size);
				memset(ctx->final_buffer[0], 0xff, ctx->gray4_size);
				memset(ctx->final_buffer[1], 0xff, ctx->gray4_size);
				memset(ctx->final_atomic_update, 0xff, ctx->gray4_size);
			} else {
				memcpy(ctx->next, ebc->suspend_next, ctx->gray4_size);
				memcpy(ctx->final, ebc->suspend_next, ctx->gray4_size);
				memcpy(ctx->final_atomic_update, ebc->suspend_next, ctx->gray4_size);
			}
		}

		if (direct_mode) {
			memset(ctx->phase[0], 0, ctx->phase_size);
			memset(ctx->phase[1], 0, ctx->phase_size);
		}
		memset(ctx->frame_num[0], 0xff, ctx->frame_num_size);
		memset(ctx->frame_num[1], 0xff, ctx->frame_num_size);

		/*
		 * LUTs use both the old and the new pixel values as inputs.
		 * However, the initial contents of the display are unknown.
		 * The special RESET waveform will initialize the display to
		 * known contents (white) regardless of its current contents.
		 */
		if (!ebc->reset_complete) {
			ebc->reset_complete = true;
			rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_RESET);
		}

		while ((!kthread_should_park()) && (!kthread_should_stop())) {
			/* printk(KERN_INFO "[rockchip_ebc] inner loop"); */
			spin_lock(&ebc->refresh_once_lock);
			one_full_refresh = ebc->do_one_full_refresh;
			spin_unlock(&ebc->refresh_once_lock);

			if (one_full_refresh) {
				/* printk(KERN_INFO "[rockchip_ebc] got one full refresh"); */
				spin_lock(&ebc->refresh_once_lock);
				ebc->do_one_full_refresh = false;
				spin_unlock(&ebc->refresh_once_lock);
/* 				 * @DRM_EPD_WF_A2: Fast transitions between black and white only */
/* 				 * @DRM_EPD_WF_DU: Transitions 16-level grayscale to monochrome */
/* 				 * @DRM_EPD_WF_DU4: Transitions 16-level grayscale to 4-level grayscale */
/* 				 * @DRM_EPD_WF_GC16: High-quality but flashy 16-level grayscale */
/* 				 * @DRM_EPD_WF_GCC16: Less flashy 16-level grayscale */
/* 				 * @DRM_EPD_WF_GL16: Less flashy 16-level grayscale */
/* 				 * @DRM_EPD_WF_GLR16: Less flashy 16-level grayscale, plus anti-ghosting */
/* 				 * @DRM_EPD_WF_GLD16: Less flashy 16-level grayscale, plus anti-ghosting */
				// Not sure why only the GC16 is able to clear the ghosts from A2
				// rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_GC16);
				rockchip_ebc_refresh(ebc, ctx, true, refresh_waveform);
				/* printk(KERN_INFO "[rockchip_ebc] got one full refresh done"); */
			} else {
				rockchip_ebc_refresh(ebc, ctx, false, default_waveform);
			}

			if (ebc->do_one_full_refresh)
				continue;

			set_current_state(TASK_IDLE);
			if (list_empty(&ctx->queue) && (!kthread_should_stop()) && (!kthread_should_park())){
				/* printk(KERN_INFO "[rockchip_ebc] scheduling"); */
				schedule();
				/* printk(KERN_INFO "[rockchip_ebc] scheduling done"); */
			}
			__set_current_state(TASK_RUNNING);
		}

		/*
		 * Clear the display before disabling the CRTC. Use the
		 * highest-quality waveform to minimize visible artifacts.
		 */
		memcpy(ebc->suspend_next, ctx->prev, ctx->gray4_size);

		if (!no_off_screen){
			// WARNING: This check here does not work. if the ebc device was in
			// runtime suspend at the time of suspending, we get the
			// suspend_was_requested == 1 too late ...
			// Therefore, for now do not differ in the way we treat the screen
			// content. Would be nice to improve this in the future
			if(ebc->suspend_was_requested){
				/* printk(KERN_INFO "[rockchip_ebc] we want to suspend, do something"); */
				memcpy(ctx->final, ebc->off_screen, ctx->gray4_size);
			} else {
				// shutdown/module remove
				/* printk(KERN_INFO "[rockchip_ebc] normal shutdown/module unload"); */
				memcpy(ctx->final, ebc->off_screen, ctx->gray4_size);
			}
			/* memcpy(ctx->final, ebc->off_screen, ctx->gray4_size); */
			rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_GC16);
		}
		else{
			// no_off_screen = false;
		}

		// save the prev buffer in case we need it after resuming
		memcpy(ebc->suspend_prev, ctx->prev, ctx->gray4_size);

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
		     EBC_EPD_CTRL_DSP_SDDW_MODE * bus_16bit);

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
	/* pr_info("ebc: %s", __func__); */

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state->mode_changed)
		return 0;

	if (crtc_state->enable) {
		struct drm_display_mode *mode = &crtc_state->adjusted_mode;

		int rate = rockchip_ebc_set_dclk(ebc, mode);
		if (rate < 0)
			return rate;
		mode->clock = rate / 1000;

		ctx = rockchip_ebc_ctx_alloc(ebc, mode->hdisplay, mode->vdisplay);
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
	/* pr_info("ebc: %s", __func__); */
}

static void rockchip_ebc_crtc_atomic_enable(struct drm_crtc *crtc,
					    struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;
	/* pr_info("ebc: %s", __func__); */

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed)
			kthread_unpark(ebc->refresh_thread);
}

static void rockchip_ebc_crtc_atomic_disable(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;
	/* pr_info("ebc: %s", __func__); */

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed){
		if (! ((ebc->refresh_thread->__state) & (TASK_DEAD))){
			kthread_park(ebc->refresh_thread);
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
	struct list_head		areas;
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
	struct rockchip_ebc_area *area;
	struct drm_rect clip;
	int ret;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
	if (ret)
		return ret;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		area = kmalloc(sizeof(*area), GFP_KERNEL);
		if (!area)
			return -ENOMEM;

		area->frame_begin = EBC_FRAME_PENDING;
		area->clip = clip;

		drm_dbg(plane->dev, "area %p (" DRM_RECT_FMT ") allocated\n",
			area, DRM_RECT_ARG(&area->clip));

		list_add_tail(&area->list, &ebc_plane_state->areas);
	}

	return 0;
}

static void rockchip_ebc_plane_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = plane_to_ebc(plane);
	struct rockchip_ebc_area *area, *next_area;
	struct ebc_plane_state *ebc_plane_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;
	int translate_x, translate_y;
	struct drm_rect src;
	const void *vaddr;
	u64 blit_area = 0;
	int delay;
	bool reflect_x = panel_reflection;
	bool reflect_y = !reflect_x;;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ctx = to_ebc_crtc_state(crtc_state)->ctx;

	drm_rect_fp_to_int(&src, &plane_state->src);
	translate_x = plane_state->dst.x1 - src.x1;
	translate_y = plane_state->dst.y1 - src.y1;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	vaddr = ebc_plane_state->base.data[0].vaddr;
	/* pr_info("ebc atomic update: vaddr: 0x%px", vaddr); */

	pr_debug(KERN_INFO "[rockchip-ebc] new fb clips\n");
	list_for_each_entry_safe(area, next_area, &ebc_plane_state->areas, list) {
		struct drm_rect *dst_clip = &area->clip;
		struct drm_rect src_clip = area->clip;
		bool clip_changed_fb;
		/* printk(KERN_INFO "[rockchip-ebc]    checking from list: (" DRM_RECT_FMT ") \n", */
		/* 	DRM_RECT_ARG(&area->clip)); */

		/* Convert from plane coordinates to CRTC coordinates. */
		drm_rect_translate(dst_clip, translate_x, translate_y);

		if (reflect_x) {
			int x1 = dst_clip->x1, x2 = dst_clip->x2;

			dst_clip->x1 = plane_state->dst.x2 - x2;
			dst_clip->x2 = plane_state->dst.x2 - x1;
		}
		if (reflect_y) {
			// "normal" mode
			// flip y coordinates
			int y1 = dst_clip->y1, y2 = dst_clip->y2;

			dst_clip->y1 = plane_state->dst.y2 - y2;
			dst_clip->y2 = plane_state->dst.y2 - y1;
		}

		if (limit_fb_blits != 0){
			switch(plane_state->fb->format->format){
			case DRM_FORMAT_XRGB8888:
				if ((use_neon & 8) && reflect_x && !reflect_y && (bw_mode == 3 || bw_mode == 4)) {
					kernel_neon_begin();
					rockchip_ebc_blit_fb_xrgb8888_y4_thresholded4_neon(
						ctx, dst_clip, vaddr,
						plane_state->fb, &src_clip,
						bw_dither_invert,
						fourtone_low_threshold,
						fourtone_mid_threshold,
						fourtone_hi_threshold,
						bw_mode == 4);
					kernel_neon_end();
					clip_changed_fb = true;
				} else if ((use_neon & 8) && reflect_x && !reflect_y && bw_mode == 1) {
					kernel_neon_begin();
					rockchip_ebc_blit_fb_xrgb8888_y4_dithered2_neon(
						ctx, dst_clip, vaddr,
						plane_state->fb, &src_clip,
						bw_dither_invert);
					kernel_neon_end();
					clip_changed_fb = true;
				} else if ((use_neon & 8) && reflect_x &&
					   !reflect_y && bw_mode == 0) {
					kernel_neon_begin();
					rockchip_ebc_blit_fb_xrgb8888_y4_neon(
						ctx, dst_clip, vaddr,
						plane_state->fb, &src_clip);
					kernel_neon_end();
					clip_changed_fb = true;
				} else {
					clip_changed_fb =
						rockchip_ebc_blit_fb_xrgb8888(
							ctx, dst_clip, vaddr,
							plane_state->fb,
							&src_clip, reflect_x,
							reflect_y,
							shrink_damage_clip,
							bw_mode, bw_threshold,
							bw_dither_invert,
							fourtone_low_threshold,
							fourtone_mid_threshold,
							fourtone_hi_threshold);
				}
				break;
			case DRM_FORMAT_R4:
				clip_changed_fb = rockchip_ebc_blit_fb_r4(
					ctx, dst_clip, vaddr, plane_state->fb,
					&src_clip, shrink_damage_clip);
				break;
			}
			// the counter should only reach 0 here, -1 can only be externally set
			limit_fb_blits -= (limit_fb_blits > 0) ? 1 : 0;

			blit_area += (u64) (src_clip.x2 - src_clip.x1) *
				(src_clip.y2 - src_clip.y1);
		} else {
			// we do not want to blit anything
			/* printk(KERN_INFO "[rockchip-ebc] atomic update: not blitting: %i\n", limit_fb_blits); */
			clip_changed_fb = false;
		}

		if (!clip_changed_fb) {
			drm_dbg(plane->dev, "area %p (" DRM_RECT_FMT ") <= (" DRM_RECT_FMT ") skipped\n",
				area, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&src_clip));

			/* printk(KERN_INFO "[rockchip-ebc]       clip skipped"); */
			/* Drop the area if the FB didn't actually change. */
			list_del(&area->list);
			kfree(area);
		} else {
			drm_dbg(plane->dev, "area %p (" DRM_RECT_FMT ") <= (" DRM_RECT_FMT ") blitted\n",
				area, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&src_clip));
			/* printk(KERN_INFO "[rockchip-ebc]        adding to list: (" DRM_RECT_FMT ") <= (" DRM_RECT_FMT ") blitted\n", */
			/* 	DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&src_clip)); */
		}
	}

	/* uncomment to set the delay depending on the updated area, using a
	 * polynomial of second degree */
	/* delay = (int) (blit_area * blit_area * delay_a / 10000000000 + blit_area * delay_b / 10000 + delay_c); */
	/* a simple threshold function: below a certain updated area, delay by
	 * delay_s [mu s], otherwise delay by delay_b [mu s] */
	delay = delay_a;
	if (blit_area > 100000)
		delay = delay_b;
	/* printk(KERN_INFO "area update, for area %llu we compute a delay of: %i (a,b: %i, %i)", */
	/* 	blit_area, */
	/* 	delay, */
	/* 	delay_a, */
	/* 	delay_b */
	/* ); */

	if (list_empty(&ebc_plane_state->areas)){
		// spin_unlock(&ctx->queue_lock);
		// the idea here: give the refresh thread time to acquire the lock
		// before new clips arrive
		usleep_range(delay, delay + 500);
		return;
	}

	spin_lock(&ctx->queue_lock);
	// copy into the buffer that is NOT in use by the ebc thread
	memcpy(
		ctx->final_buffer[!ctx->ebc_buffer_index],
	   	ctx->final_atomic_update,
	   	ctx->gray4_size
	);
	if (ctx->first_switch){
		memcpy(
			ctx->final_buffer[ctx->ebc_buffer_index],
			ctx->final_atomic_update,
			ctx->gray4_size
		);

		ctx->first_switch = false;
	}
	list_splice_tail_init(&ebc_plane_state->areas, &ctx->queue);
	// we actually changed the buffer content
	ctx->switch_required = true;
	spin_unlock(&ctx->queue_lock);

	// the idea here: give the refresh thread time to acquire the lock
	// before new clips arrive
	usleep_range(delay, delay + 100);
	/* usleep_range(2000, 2000 + 100); */

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

	INIT_LIST_HEAD(&ebc_plane_state->areas);
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

	INIT_LIST_HEAD(&ebc_plane_state->areas);

	return &ebc_plane_state->base.base;
}

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state)
{
	struct ebc_plane_state *ebc_plane_state = to_ebc_plane_state(plane_state);
	struct rockchip_ebc_area *area, *next_area;
	/* pr_info("ebc: %s", __func__); */

	list_for_each_entry_safe(area, next_area, &ebc_plane_state->areas, list)
		kfree(area);

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
	DRM_FORMAT_R4,
};

static const u64 rockchip_ebc_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int rockchip_ebc_drm_init(struct rockchip_ebc *ebc)
{
	struct drm_device *drm = &ebc->drm;
	struct drm_bridge *bridge;
	int ret;
	const struct firmware * default_offscreen;

	ret = drmm_epd_lut_file_init(drm, &ebc->lut_file, "rockchip/ebc.wbf");
	if (ret)
		return ret;

	ret = drmm_epd_lut_init(&ebc->lut_file, &ebc->lut,
				DRM_EPD_LUT_4BIT_PACKED, EBC_MAX_PHASES);
	if (ret)
		return ret;

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

	drm_fbdev_ttm_setup(drm, 0);

	// check if there is a default off-screen
	if (!request_firmware(&default_offscreen, "rockchip/rockchip_ebc_default_screen.bin", drm->dev))
	{
		if (default_offscreen->size != 1314144)
			drm_err(drm, "Size of default offscreen data file is not 1314144\n");
		else {
			memcpy(ebc->off_screen, default_offscreen->data, 1314144);
		}
	} else {
		// fill the off-screen with some values
		memset(ebc->off_screen, 0xff, 1314144);
		/* memset(ebc->off_screen, 0x00, 556144); */
	}
	release_firmware(default_offscreen);

	return 0;
}

static int __maybe_unused rockchip_ebc_suspend(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;
	pr_info("%s", __func__);

	ebc->suspend_was_requested = 1;

	ret = drm_mode_config_helper_suspend(&ebc->drm);
	if (ret)
		return ret;

	return pm_runtime_force_suspend(dev);
}

static int __maybe_unused rockchip_ebc_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

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
	ebc->lut_changed = true;

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

static const struct dev_pm_ops rockchip_ebc_dev_pm_ops = {
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

	// TODO: Error checking

	ebc->do_one_full_refresh = true;
	ebc->suspend_was_requested = 0;

	spin_lock_init(&ebc->refresh_once_lock);

	if (IS_ERR(ebc))
		return PTR_ERR(ebc);

	platform_set_drvdata(pdev, ebc);
	init_completion(&ebc->display_end);
	ebc->reset_complete = skip_reset;

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
		goto err_stop_kthread;

	return 0;

err_stop_kthread:
	kthread_stop(ebc->refresh_thread);
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
	drm_atomic_helper_shutdown(&ebc->drm);

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

MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>");
MODULE_DESCRIPTION("Rockchip EBC driver");
MODULE_LICENSE("GPL v2");
