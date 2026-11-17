/*
 * Copyright (c) 2026 DREAM-NIC contributors
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */
#ifndef __QDMA_VERSION_H__
#define __QDMA_VERSION_H__

#include <linux/types.h>
#include <linux/bits.h>

/*
 * Config_Block_Identifier (0x0), PG302 v4.0 and v5.0 register maps:
 * identifier[31:20] = 0x1fd, config_block_identifier[19:16] = 3.
 */
#define QDMA_CONFIG_BLOCK_ID_MAGIC		0x1fd3
#define QDMA_CONFIG_BLOCK_ID_MAGIC_MASK		GENMASK(31, 16)

/*
 * QDMA_GLBL2_MISC_CAP (0x134).  The PG302 register maps list this register
 * as one opaque misc_cap[31:0] field; the decode below follows AMD's QDMA
 * reference driver (dma_ip_drivers, qdma_soft_reg.h and eqdma_soft_access.h):
 *
 *   [31:28] device type: 0 = soft IP (UltraScale+), 1 = Versal CPM4,
 *           2 = Versal CPM5
 *   [27:24] Vivado release id (meaning depends on the IP generation)
 *   [23:20] IP version for soft IP: 0 = QDMA 3.x, 1 = QDMA 4.0, 2 = QDMA 5.0
 *   [19:16] RTL version: 0 = base, 1 = patch
 */
#define QDMA_MISC_CAP_DEVICE_TYPE_MASK		GENMASK(31, 28)
#define QDMA_MISC_CAP_VIVADO_RELEASE_MASK	GENMASK(27, 24)
#define QDMA_MISC_CAP_IP_VERSION_MASK		GENMASK(23, 20)
#define QDMA_MISC_CAP_RTL_VERSION_MASK		GENMASK(19, 16)

#define QDMA_MISC_CAP_DEVICE_SOFT		0
#define QDMA_MISC_CAP_IP_VERSION_QDMA3		0
#define QDMA_MISC_CAP_IP_VERSION_QDMA4		1
#define QDMA_MISC_CAP_IP_VERSION_QDMA5		2

/**
 * enum qdma_ip_gen - QDMA IP generation, as far as the driver cares
 * @QDMA_IP_GEN_UNKNOWN: unrecognised (e.g. Versal CPM or a newer soft IP);
 *                       programmed like QDMA 4.0, which is what the driver
 *                       always did before version detection existed
 * @QDMA_IP_GEN_QDMA3: QDMA 3.x soft IP (Vivado 2019.2 and earlier)
 * @QDMA_IP_GEN_QDMA4: QDMA 4.0 soft IP (Vivado 2020.1 to 2022.1)
 * @QDMA_IP_GEN_QDMA5: QDMA 5.0 soft IP (Vivado 2022.2 and later)
 **/
enum qdma_ip_gen {
	QDMA_IP_GEN_UNKNOWN = 0,
	QDMA_IP_GEN_QDMA3,
	QDMA_IP_GEN_QDMA4,
	QDMA_IP_GEN_QDMA5,
	QDMA_IP_GEN_MAX
};

/**
 * struct qdma_hw_version - decoded QDMA hardware identification
 * @block_id: raw Config_Block_Identifier (0x0)
 * @misc_cap: raw QDMA_GLBL2_MISC_CAP (0x134)
 * @device_type: MISC_CAP[31:28]
 * @vivado_release: MISC_CAP[27:24]
 * @ip_version: MISC_CAP[23:20]
 * @rtl_version: MISC_CAP[19:16]
 * @gen: generation derived from the fields above
 **/
struct qdma_hw_version {
	u32 block_id;
	u32 misc_cap;
	u8 device_type;
	u8 vivado_release;
	u8 ip_version;
	u8 rtl_version;
	enum qdma_ip_gen gen;
};

/**
 * qdma_decode_version - Decode the QDMA identification registers
 * @block_id: value read from Config_Block_Identifier (0x0)
 * @misc_cap: value read from QDMA_GLBL2_MISC_CAP (0x134)
 * @ver: output
 *
 * Pure function, no register access.  Returns 0 on success, or -ENODEV if
 * @block_id does not carry the QDMA magic number (BAR is not the QDMA
 * configuration BAR, or the device is not responding).  An unrecognised
 * @misc_cap is not an error: @ver->gen is then QDMA_IP_GEN_UNKNOWN.
 **/
int qdma_decode_version(u32 block_id, u32 misc_cap,
			struct qdma_hw_version *ver);

/**
 * qdma_ip_gen_name - Human readable name of a QDMA IP generation
 **/
const char *qdma_ip_gen_name(enum qdma_ip_gen gen);

#endif
