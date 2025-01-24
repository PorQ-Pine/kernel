/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) hrdl
 * Author: hrdl <git@hrdl.eu>
 */

#ifndef _ROCKCHIP_EBC_BLIT_NEON_H
#define _ROCKCHIP_EBC_BLIT_NEON_H

void rockchip_ebc_blit_pixels_blocks_neon(u8 *dst_line, const u8 *src_line,
					  int byte_width, int pitch,
					  int clip_height);

#endif /* _ROCKCHIP_EBC_BLIT_NEON_H */
