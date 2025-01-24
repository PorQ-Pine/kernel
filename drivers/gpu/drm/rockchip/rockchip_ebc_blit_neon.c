/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#include <linux/module.h>
#include <linux/types.h>
#include "rockchip_ebc.h"

#include "rockchip_ebc_blit_neon.h"

void rockchip_ebc_blit_pixels_blocks_neon(u8 *dst_line, const u8 *src_line,
					  int byte_width, int pitch,
					  int clip_height)
{
}
EXPORT_SYMBOL(rockchip_ebc_blit_pixels_blocks_neon);

MODULE_LICENSE("GPL v2");
