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
#include <linux/errno.h>

#include "onic_common.h"
#include "qdma_version.h"

int qdma_decode_version(u32 block_id, u32 misc_cap,
			struct qdma_hw_version *ver)
{
	ver->block_id = block_id;
	ver->misc_cap = misc_cap;
	ver->device_type = BITFIELD_GET(QDMA_MISC_CAP_DEVICE_TYPE_MASK, misc_cap);
	ver->vivado_release =
		BITFIELD_GET(QDMA_MISC_CAP_VIVADO_RELEASE_MASK, misc_cap);
	ver->ip_version = BITFIELD_GET(QDMA_MISC_CAP_IP_VERSION_MASK, misc_cap);
	ver->rtl_version = BITFIELD_GET(QDMA_MISC_CAP_RTL_VERSION_MASK, misc_cap);
	ver->gen = QDMA_IP_GEN_UNKNOWN;

	if (BITFIELD_GET(QDMA_CONFIG_BLOCK_ID_MAGIC_MASK, block_id) !=
	    QDMA_CONFIG_BLOCK_ID_MAGIC)
		return -ENODEV;

	/* Versal CPM4/CPM5 use different register maps; not handled here */
	if (ver->device_type != QDMA_MISC_CAP_DEVICE_SOFT)
		return 0;

	switch (ver->ip_version) {
	case QDMA_MISC_CAP_IP_VERSION_QDMA3:
		ver->gen = QDMA_IP_GEN_QDMA3;
		break;
	case QDMA_MISC_CAP_IP_VERSION_QDMA4:
		ver->gen = QDMA_IP_GEN_QDMA4;
		break;
	case QDMA_MISC_CAP_IP_VERSION_QDMA5:
		ver->gen = QDMA_IP_GEN_QDMA5;
		break;
	default:
		break;
	}
	return 0;
}

const char *qdma_ip_gen_name(enum qdma_ip_gen gen)
{
	switch (gen) {
	case QDMA_IP_GEN_QDMA3:
		return "QDMA 3.x";
	case QDMA_IP_GEN_QDMA4:
		return "QDMA 4.0";
	case QDMA_IP_GEN_QDMA5:
		return "QDMA 5.0";
	default:
		return "unknown";
	}
}
