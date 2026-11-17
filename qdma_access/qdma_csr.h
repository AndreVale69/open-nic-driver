/*
 * Copyright (c) 2020 Xilinx, Inc.
 * Copyright (c) 2026 DREAM-NIC contributors
 * All rights reserved.
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
#ifndef __QDMA_CSR_H__
#define __QDMA_CSR_H__

#include "qdma_device.h"
#include "qdma_export.h"
#include "qdma_version.h"

/**
 * struct qdma_hw_ops - per QDMA generation hardware programming
 * @name: name used in log messages
 * @init_global_csr: program the global (non per-queue) CSRs; called once by
 *                   the master PF before any queue is started
 *
 * QDMA 4.0 and 5.0 share the register map, the indirect context layouts, the
 * descriptor formats and the queue (DMAP) space.  What differs is the global
 * CSR programming, so that is the only operation here.  Everything else stays
 * in the common qdma_context.c / onic_hardware.c code.
 **/
struct qdma_hw_ops {
	const char *name;
	void (*init_global_csr)(struct qdma_dev *qdev);
};

/**
 * qdma_hw_ops_for_gen - Select the hardware programming for a generation
 *
 * QDMA_IP_GEN_QDMA5 gets the QDMA 5.0 programming.  Every other value,
 * including QDMA_IP_GEN_UNKNOWN, gets the programming the driver has always
 * used (validated on QDMA 4.0), so behaviour on existing designs is unchanged.
 **/
const struct qdma_hw_ops *qdma_hw_ops_for_gen(enum qdma_ip_gen gen);

/**
 * qdma_detect_version - Read and decode the QDMA identification registers
 * @qdev: QDMA device, BAR already mapped
 *
 * Fills qdev->ver.  Returns 0 or -ENODEV (see qdma_decode_version()).
 **/
int qdma_detect_version(struct qdma_dev *qdev);

/**
 * qdma_ring_count - Number of descriptors for a global ring size index
 **/
u16 qdma_ring_count(u8 idx);

/* Exposed for the unit tests only */
void qdma_init_global_csr_v4(struct qdma_dev *qdev);
void qdma_init_global_csr_v5(struct qdma_dev *qdev);

#endif
