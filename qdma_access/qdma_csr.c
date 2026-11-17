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
#include <linux/errno.h>

#include "onic_common.h"
#include "qdma_register.h"
#include "qdma_register_v5.h"
#include "qdma_csr.h"

/* default CSR values for QDMA (QDMA 4.0 programming, unchanged) */
#define DEFAULT_MAX_DESC_FETCH			6
#define DEFAULT_WB_INTVL			QDMA_WB_INTVL_4
#define DEFAULT_PFCH_STOP_THRES			256
#define DEFAULT_PFCH_NUM_ENTRIES_PER_Q		8
#define DEFAULT_PFCH_MAX_Q_CNT			16
#define DEFAULT_C2H_INTR_TIMER_TICK		25
#define DEFAULT_CMPL_COAL_TIMER_CNT		5
#define DEFAULT_CMPL_COAL_TIMER_TICK		25
#define DEFAULT_CMPL_COAL_MAX_BUFSZ		32
#define DEFAULT_H2C_THROT_DATA_THRES		0x4000
#define DEFAULT_THROT_EN_DATA			1
#define DEFAULT_THROT_EN_REQ			0
#define DEFAULT_H2C_THROT_REQ_THRES		0x60

/*
 * QDMA 5.0 values.  These are the settings AMD's QDMA reference driver
 * applies to QDMA 5.0 soft IP only (dma_ip_drivers, eqdma_soft_access.c,
 * eqdma_set_perf_opt(), identical in releases 2023.2.1 and 2025.2), validated
 * by AMD on QDMA 5.0 example designs.  Field positions are from
 * qdma_v5_0_pf_registers.csv.
 */
#define QDMA5_DSC_CFG_C2H_UODSC_LIMIT		5
#define QDMA5_DSC_CFG_H2C_UODSC_LIMIT		8
#define QDMA5_DSC_CFG_MAX_DSC_FETCH		2
#define QDMA5_DSC_CFG_WB_ACC_INT		QDMA_WB_INTVL_128
#define QDMA5_MISC_CTL_NUM_TAG			256
#define QDMA5_MISC_CTL_RQ_METERING_MUL		9
#define QDMA5_PFCH_CFG_EVT_PFCH_FL_TH		256
#define QDMA5_PFCH_CFG_PFCH_FL_TH		256
#define QDMA5_PFCH_CFG_1_QCNT_MARGIN		4	/* cache_depth - 4 */
#define QDMA5_PFCH_CFG_2_FENCE			1
#define QDMA5_PFCH_CFG_2_LL_SZ_TH		1024
#define QDMA5_PFCH_CFG_2_VAR_DESC_NUM		15
#define QDMA5_PFCH_CFG_2_NUM			8
#define QDMA5_PFCH_CFG_3_FL_FREE_CNT_TH		256
#define QDMA5_PFCH_CFG_4_GLB_EVT_TIMER_TICK	64
#define QDMA5_PFCH_CFG_4_EVT_TIMER_TICK		400
#define QDMA5_CRDT_COAL_CFG_1_PLD_FIFO_TH	16
#define QDMA5_CRDT_COAL_CFG_1_TIMER_TH		16
#define QDMA5_CRDT_COAL_CFG_2_FIFO_MARGIN	8	/* cache_depth - 8 */
#define QDMA5_CRDT_COAL_CFG_2_CNT_TH		96
#define QDMA5_RRQ_PCIE_THROT_REQ_EN		0
#define QDMA5_RRQ_PCIE_THROT_REQ		192
#define QDMA5_RRQ_PCIE_THROT_DAT_EN		1
#define QDMA5_RRQ_PCIE_THROT_DAT		20480
#define QDMA5_RRQ_BRG_THROT_REQ_EN		1
#define QDMA5_RRQ_BRG_THROT_REQ			QDMA5_RRQ_PCIE_THROT_REQ
#define QDMA5_RRQ_BRG_THROT_DAT_EN		1
#define QDMA5_H2C_THROT_PCIE_EN_REQ		1
#define QDMA5_H2C_THROT_PCIE_REQ		QDMA5_RRQ_PCIE_THROT_REQ
#define QDMA5_H2C_THROT_PCIE_EN_DATA		1
#define QDMA5_H2C_THROT_PCIE_DATA		24576
#define QDMA5_H2C_THROT_AXIMM_EN_REQ		1
#define QDMA5_H2C_THROT_AXIMM_REQ		64
#define QDMA5_H2C_THROT_AXIMM_EN_DATA		1
#define QDMA5_H2C_THROT_AXIMM_DATA		16384
#define QDMA5_H2C_MM_DATA_THROTTLE_EN		QDMA5_RRQ_PCIE_THROT_DAT_EN
#define QDMA5_H2C_MM_DATA_THROTTLE_DAT		QDMA5_RRQ_PCIE_THROT_DAT

static const u16 rngcnt_pool[QDMA_NUM_DESC_RNGCNT] = {
	4096, 64, 128, 192, 256, 384, 512, 768,
	1024, 1536, 3072, 4096, 6144, 8192, 12288, 16384
};

static const u16 c2h_bufsz_pool[QDMA_NUM_C2H_BUFSZ] = {
	4096, 256, 512, 1024, 2048, 3968, 4096, 4096,
	4096, 4096, 4096, 4096, 4096, 8192, 9018, 16384
};

static const u16 c2h_timer_pool[QDMA_NUM_C2H_TIMERS] = {
	10, 2, 4, 5, 8, 10, 15, 20, 25,
	30, 50, 75, 100, 125, 150, 200
};

static const u16 c2h_thres_pool[QDMA_NUM_C2H_COUNTERS] = {
	64, 2, 4, 8, 16, 24, 32, 48,
	80, 96, 112, 128, 144, 160, 176, 192
};

u16 qdma_ring_count(u8 idx)
{
	return (idx < QDMA_NUM_DESC_RNGCNT) ? rngcnt_pool[idx] : 0;
}

int qdma_detect_version(struct qdma_dev *qdev)
{
	u32 block_id = qdma_read_reg(qdev, QDMA_OFFSET_CONFIG_BLOCK_ID);
	u32 misc_cap = qdma_read_reg(qdev, QDMA_OFFSET_GLBL2_MISC_CAP);

	return qdma_decode_version(block_id, misc_cap, &qdev->ver);
}

/**
 * qdma_init_csr_tables - program the lookup tables shared by all generations
 *
 * Ring sizes, C2H buffer sizes, C2H interrupt timer tick, C2H timers and C2H
 * counter thresholds.  These registers have the same address and layout in
 * QDMA 4.0 and 5.0; the values are OpenNIC's (the netdev code indexes these
 * tables), not AMD's reference values.
 **/
static void qdma_init_csr_tables(struct qdma_dev *qdev)
{
	u32 offset, val;
	int i;

	/* initialize descriptor ring size registers */
	for (i = 0; i < QDMA_NUM_DESC_RNGCNT; ++i) {
		offset = QDMA_OFFSET_GLBL_RNG_SZ + (i * 4);
		val = rngcnt_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* initialize C2H buffer size registers */
	for (i = 0; i < QDMA_NUM_C2H_BUFSZ; ++i) {
		offset = QDMA_OFFSET_C2H_BUF_SZ + (i * 4);
		val = c2h_bufsz_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* set QDMA_C2H_INT_TIMER_TICK (0xB0C) register to 25, which corresponds
	 * to 100ns (1 tick = 4ns for 250MHz user clock)
	 */
	offset = QDMA_OFFSET_C2H_INT_TIMER_TICK;
	val = DEFAULT_C2H_INTR_TIMER_TICK;
	qdma_write_reg(qdev, offset, val);

	/* initialize C2H timer counter registers. */
	for (i = 0; i < QDMA_NUM_C2H_TIMERS; ++i) {
		offset = QDMA_OFFSET_C2H_TIMER_CNT + (i * 4);
		val = c2h_timer_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* initialize C2H counter threshold registers */
	for (i = 0; i < QDMA_NUM_C2H_COUNTERS; ++i) {
		offset = QDMA_OFFSET_C2H_CNT_TH + (i * 4);
		val = c2h_thres_pool[i];
		qdma_write_reg(qdev, offset, val);
	}
}

/**
 * qdma_init_global_csr_v4 - QDMA 4.0 (legacy) global CSR programming
 *
 * This is the programming OpenNIC has always done, moved here unchanged so
 * that QDMA 4.0 designs see the exact same register write sequence.
 *
 * Note: QDMA_C2H_PFCH_CFG (0xB08) and QDMA_H2C_REQ_THROT (0xE24) are encoded
 * with the QDMA 3.x field layout from qdma_register.h, which does not match
 * qdma_v4_0_pf_registers.csv.  This is intentionally preserved: it is the
 * configuration that has been used on QDMA 4.0 hardware.  See the DREAM-NIC
 * QDMA compatibility report for the decoded effect.
 **/
void qdma_init_global_csr_v4(struct qdma_dev *qdev)
{
	u32 offset, val;

	qdma_init_csr_tables(qdev);

	/* set QDMA_GLBL_DSC_CFG (0x250) register for max descriptor fetch and
	 * writeback interval
	 */
	offset = QDMA_OFFSET_GLBL_DSC_CFG;
	val = (FIELD_SET(QDMA_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK,
			 DEFAULT_MAX_DESC_FETCH) |
	       FIELD_SET(QDMA_GLBL_DSC_CFG_WB_ACC_INT_MASK,
			 DEFAULT_WB_INTVL));
	qdma_write_reg(qdev, offset, val);

	/* read QDMA_C2H_PFCH_CACHE_DEPTH (0xBE0) register and set
	 * QDMA_C2H_PFCH_CFG (0xB08) register accordingly
	 */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH);
	offset = QDMA_OFFSET_C2H_PFCH_CFG;
	val = (FIELD_SET(QDMA_C2H_PFCH_FL_TH_MASK,
			 DEFAULT_PFCH_STOP_THRES) |
	       FIELD_SET(QDMA_C2H_NUM_PFCH_MASK,
			 DEFAULT_PFCH_NUM_ENTRIES_PER_Q) |
	       FIELD_SET(QDMA_C2H_PFCH_QCNT_MASK,
			 (val >> 1)) |
	       FIELD_SET(QDMA_C2H_EVT_QCNT_TH_MASK,
			 ((val >> 1) - 2)));
	qdma_write_reg(qdev, offset, val);

	/* read QDMA_C2H_CMPL_COAL_BUF_DEPTH (0xBE4) register and set
	 * QDMA_C2H_WB_COAL_CFG (0xB50) register accordingly
	 *
	 * Note that the tick field is set to 25, which corresponds to 100ns (1
	 * tick = 4ns for 250MHz user clock).
	 *
	 * TODO: verify the value for C2H_MAX_BUF_SZ.  QDMA document says that
	 * this should be set to QDMA_C2H_CMPL_COAL_BUF_DEPTH.buf_depth - 2; but
	 * the libqdma code ignores the minus 2 part.
	 */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_CMPL_COAL_BUF_DEPTH);
	offset = QDMA_OFFSET_C2H_WB_COAL_CFG;
	val = (FIELD_SET(QDMA_C2H_TICK_CNT_MASK,
			 DEFAULT_CMPL_COAL_TIMER_CNT) |
	       FIELD_SET(QDMA_C2H_TICK_VAL_MASK,
			 DEFAULT_CMPL_COAL_TIMER_TICK) |
	       FIELD_SET(QDMA_C2H_MAX_BUF_SZ_MASK, val));
	qdma_write_reg(qdev, offset, val);

	/* set QDMA_H2C_REQ_THROT (0xE24) register.
	 *
	 * Data and request-based throttle are enabled only if the respective
	 * threshold is set to a nonzero value.
	 */
	offset = QDMA_OFFSET_H2C_REQ_THROT;
	val = (FIELD_SET(QDMA_H2C_DATA_THRESH_MASK,
			 DEFAULT_H2C_THROT_DATA_THRES) |
	       FIELD_SET(QDMA_H2C_REQ_THROT_EN_DATA_MASK,
			 DEFAULT_THROT_EN_DATA) |
	       FIELD_SET(QDMA_H2C_REQ_THRESH_MASK,
			 DEFAULT_H2C_THROT_REQ_THRES) |
	       FIELD_SET(QDMA_H2C_REQ_THROT_EN_REQ_MASK,
			 DEFAULT_THROT_EN_REQ));
	qdma_write_reg(qdev, offset, val);
}

/**
 * qdma_init_global_csr_v5 - QDMA 5.0 global CSR programming
 *
 * The OpenNIC lookup tables and completion coalescing setup are the same as
 * for 4.0 (identical register layout).  The descriptor engine, prefetch,
 * credit coalescing and read request throttle registers are programmed with
 * the QDMA 5.0 field layout and AMD's QDMA 5.0 values, instead of the QDMA
 * 3.x encoded values used by the legacy path.
 **/
void qdma_init_global_csr_v5(struct qdma_dev *qdev)
{
	u32 val, depth, rrq_dat;

	qdma_init_csr_tables(qdev);

	/* QDMA_C2H_WRB_COAL_CFG (0xB50): same layout as 4.0, OpenNIC values */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_CMPL_COAL_BUF_DEPTH);
	val = (FIELD_SET(QDMA_C2H_TICK_CNT_MASK,
			 DEFAULT_CMPL_COAL_TIMER_CNT) |
	       FIELD_SET(QDMA_C2H_TICK_VAL_MASK,
			 DEFAULT_CMPL_COAL_TIMER_TICK) |
	       FIELD_SET(QDMA_C2H_MAX_BUF_SZ_MASK, val));
	qdma_write_reg(qdev, QDMA_OFFSET_C2H_WB_COAL_CFG, val);

	/* QDMA_GLBL_DSC_CFG (0x250): uodsc limits are new in 5.0 */
	val = FIELD_SET(QDMA5_GLBL_DSC_CFG_C2H_UODSC_LIMIT_MASK,
			QDMA5_DSC_CFG_C2H_UODSC_LIMIT) |
	      FIELD_SET(QDMA5_GLBL_DSC_CFG_H2C_UODSC_LIMIT_MASK,
			QDMA5_DSC_CFG_H2C_UODSC_LIMIT) |
	      FIELD_SET(QDMA5_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK,
			QDMA5_DSC_CFG_MAX_DSC_FETCH) |
	      FIELD_SET(QDMA5_GLBL_DSC_CFG_WB_ACC_INT_MASK,
			QDMA5_DSC_CFG_WB_ACC_INT);
	qdma_write_reg(qdev, QDMA5_OFFSET_GLBL_DSC_CFG, val);

	/* Config_Block_Misc_Control (0x4C) */
	val = FIELD_SET(QDMA5_CFG_BLK_MISC_CTL_NUM_TAG_MASK,
			QDMA5_MISC_CTL_NUM_TAG) |
	      FIELD_SET(QDMA5_CFG_BLK_MISC_CTL_RQ_METERING_MUL_MASK,
			QDMA5_MISC_CTL_RQ_METERING_MUL);
	qdma_write_reg(qdev, QDMA5_OFFSET_CFG_BLK_MISC_CTL, val);

	/* QDMA_C2H_PFCH_CFG (0xB08) */
	val = FIELD_SET(QDMA5_C2H_PFCH_CFG_EVT_PFCH_FL_TH_MASK,
			QDMA5_PFCH_CFG_EVT_PFCH_FL_TH) |
	      FIELD_SET(QDMA5_C2H_PFCH_CFG_PFCH_FL_TH_MASK,
			QDMA5_PFCH_CFG_PFCH_FL_TH);
	qdma_write_reg(qdev, QDMA5_OFFSET_C2H_PFCH_CFG, val);

	/* QDMA_C2H_PFCH_CFG_1 (0xA80), sized from the prefetch tag count.
	 * The CSV lists 8, 16, 32, 64 or 128 tags; clamp so the subtractions
	 * below can never wrap on an unexpected value.
	 */
	depth = BITFIELD_GET(QDMA5_C2H_PFCH_CACHE_DEPTH_MASK,
			     qdma_read_reg(qdev, QDMA5_OFFSET_C2H_PFCH_CACHE_DEPTH));
	if (depth < QDMA5_CRDT_COAL_CFG_2_FIFO_MARGIN)
		depth = QDMA5_CRDT_COAL_CFG_2_FIFO_MARGIN;
	val = FIELD_SET(QDMA5_C2H_PFCH_CFG_1_EVT_QCNT_TH_MASK,
			depth - QDMA5_PFCH_CFG_1_QCNT_MARGIN) |
	      FIELD_SET(QDMA5_C2H_PFCH_CFG_1_QCNT_MASK,
			depth - QDMA5_PFCH_CFG_1_QCNT_MARGIN);
	qdma_write_reg(qdev, QDMA5_OFFSET_C2H_PFCH_CFG_1, val);

	/* QDMA_C2H_PFCH_CFG_2 (0xA84) */
	val = FIELD_SET(QDMA5_C2H_PFCH_CFG_2_FENCE_MASK,
			QDMA5_PFCH_CFG_2_FENCE) |
	      FIELD_SET(QDMA5_C2H_PFCH_CFG_2_LL_SZ_TH_MASK,
			QDMA5_PFCH_CFG_2_LL_SZ_TH) |
	      FIELD_SET(QDMA5_C2H_PFCH_CFG_2_VAR_DESC_NUM_MASK,
			QDMA5_PFCH_CFG_2_VAR_DESC_NUM) |
	      FIELD_SET(QDMA5_C2H_PFCH_CFG_2_NUM_MASK,
			QDMA5_PFCH_CFG_2_NUM);
	qdma_write_reg(qdev, QDMA5_OFFSET_C2H_PFCH_CFG_2, val);

	/* PFCH_CFG_3 (0x147C) and PFCH_CFG_4 (0x1484), new in 5.0 */
	val = FIELD_SET(QDMA5_PFCH_CFG_3_VAR_DESC_FL_FREE_CNT_TH_MASK,
			QDMA5_PFCH_CFG_3_FL_FREE_CNT_TH);
	qdma_write_reg(qdev, QDMA5_OFFSET_PFCH_CFG_3, val);

	val = FIELD_SET(QDMA5_PFCH_CFG_4_GLB_EVT_TIMER_TICK_MASK,
			QDMA5_PFCH_CFG_4_GLB_EVT_TIMER_TICK) |
	      FIELD_SET(QDMA5_PFCH_CFG_4_EVT_TIMER_TICK_MASK,
			QDMA5_PFCH_CFG_4_EVT_TIMER_TICK);
	qdma_write_reg(qdev, QDMA5_OFFSET_PFCH_CFG_4, val);

	/* QDMA_C2H_CRDT_COAL_CFG_1/2 (0x1400/0x1404) */
	val = FIELD_SET(QDMA5_C2H_CRDT_COAL_CFG_1_PLD_FIFO_TH_MASK,
			QDMA5_CRDT_COAL_CFG_1_PLD_FIFO_TH) |
	      FIELD_SET(QDMA5_C2H_CRDT_COAL_CFG_1_TIMER_TH_MASK,
			QDMA5_CRDT_COAL_CFG_1_TIMER_TH);
	qdma_write_reg(qdev, QDMA5_OFFSET_C2H_CRDT_COAL_CFG_1, val);

	val = FIELD_SET(QDMA5_C2H_CRDT_COAL_CFG_2_FIFO_TH_MASK,
			depth - QDMA5_CRDT_COAL_CFG_2_FIFO_MARGIN) |
	      FIELD_SET(QDMA5_C2H_CRDT_COAL_CFG_2_CNT_TH_MASK,
			QDMA5_CRDT_COAL_CFG_2_CNT_TH);
	qdma_write_reg(qdev, QDMA5_OFFSET_C2H_CRDT_COAL_CFG_2, val);

	/* QDMA_GLBL2_RRQ_*_THROT (0x15C, 0x160, 0x158), new in 5.0 */
	val = FIELD_SET(QDMA5_RRQ_THROT_REQ_EN_MASK,
			QDMA5_RRQ_PCIE_THROT_REQ_EN) |
	      FIELD_SET(QDMA5_RRQ_THROT_REQ_MASK, QDMA5_RRQ_PCIE_THROT_REQ) |
	      FIELD_SET(QDMA5_RRQ_THROT_DAT_EN_MASK,
			QDMA5_RRQ_PCIE_THROT_DAT_EN) |
	      FIELD_SET(QDMA5_RRQ_THROT_DAT_MASK, QDMA5_RRQ_PCIE_THROT_DAT);
	qdma_write_reg(qdev, QDMA5_OFFSET_GLBL2_RRQ_PCIE_THROT, val);

	qdma_write_reg(qdev, QDMA5_OFFSET_GLBL2_RRQ_AXIMM_THROT, 0);

	/* the bridge data threshold follows what the PCIe throttle accepted */
	rrq_dat = BITFIELD_GET(QDMA5_RRQ_THROT_DAT_MASK,
			       qdma_read_reg(qdev,
					     QDMA5_OFFSET_GLBL2_RRQ_PCIE_THROT));
	val = FIELD_SET(QDMA5_RRQ_THROT_REQ_EN_MASK,
			QDMA5_RRQ_BRG_THROT_REQ_EN) |
	      FIELD_SET(QDMA5_RRQ_THROT_REQ_MASK, QDMA5_RRQ_BRG_THROT_REQ) |
	      FIELD_SET(QDMA5_RRQ_THROT_DAT_EN_MASK,
			QDMA5_RRQ_BRG_THROT_DAT_EN) |
	      FIELD_SET(QDMA5_RRQ_THROT_DAT_MASK, rrq_dat);
	qdma_write_reg(qdev, QDMA5_OFFSET_GLBL2_RRQ_BRG_THROT, val);

	/* QDMA_H2C_REQ_THROT_PCIE (0xE24) and _AXIMM (0xE2C) */
	val = FIELD_SET(QDMA5_H2C_REQ_THROT_EN_REQ_MASK,
			QDMA5_H2C_THROT_PCIE_EN_REQ) |
	      FIELD_SET(QDMA5_H2C_REQ_THROT_REQ_MASK,
			QDMA5_H2C_THROT_PCIE_REQ) |
	      FIELD_SET(QDMA5_H2C_REQ_THROT_EN_DATA_MASK,
			QDMA5_H2C_THROT_PCIE_EN_DATA) |
	      FIELD_SET(QDMA5_H2C_REQ_THROT_DATA_MASK,
			QDMA5_H2C_THROT_PCIE_DATA);
	qdma_write_reg(qdev, QDMA5_OFFSET_H2C_REQ_THROT_PCIE, val);

	val = FIELD_SET(QDMA5_H2C_REQ_THROT_EN_REQ_MASK,
			QDMA5_H2C_THROT_AXIMM_EN_REQ) |
	      FIELD_SET(QDMA5_H2C_REQ_THROT_REQ_MASK,
			QDMA5_H2C_THROT_AXIMM_REQ) |
	      FIELD_SET(QDMA5_H2C_REQ_THROT_EN_DATA_MASK,
			QDMA5_H2C_THROT_AXIMM_EN_DATA) |
	      FIELD_SET(QDMA5_H2C_REQ_THROT_DATA_MASK,
			QDMA5_H2C_THROT_AXIMM_DATA);
	qdma_write_reg(qdev, QDMA5_OFFSET_H2C_REQ_THROT_AXIMM, val);

	/* QDMA_H2C_MM_Data_Throttle (0x12EC), new in 5.0 */
	val = FIELD_SET(QDMA5_H2C_MM_DATA_THROTTLE_EN_MASK,
			QDMA5_H2C_MM_DATA_THROTTLE_EN) |
	      FIELD_SET(QDMA5_H2C_MM_DATA_THROTTLE_DAT_MASK,
			QDMA5_H2C_MM_DATA_THROTTLE_DAT);
	qdma_write_reg(qdev, QDMA5_OFFSET_H2C_MM_DATA_THROTTLE, val);
}

static const struct qdma_hw_ops qdma_hw_ops_v4 = {
	.name = "QDMA 4.0",
	.init_global_csr = qdma_init_global_csr_v4,
};

static const struct qdma_hw_ops qdma_hw_ops_v5 = {
	.name = "QDMA 5.0",
	.init_global_csr = qdma_init_global_csr_v5,
};

const struct qdma_hw_ops *qdma_hw_ops_for_gen(enum qdma_ip_gen gen)
{
	if (gen == QDMA_IP_GEN_QDMA5)
		return &qdma_hw_ops_v5;
	return &qdma_hw_ops_v4;
}
