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

/**
 * qdma_ring_count - Number of descriptors for a global ring size index
 **/
u16 qdma_ring_count(u8 idx);

/**
 * qdma_init_global_csr_v4 - program the QDMA global CSRs (QDMA 4.0)
 **/
void qdma_init_global_csr_v4(struct qdma_dev *qdev);

#endif
