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
#include "onic_common.h"
#include "qdma_register.h"
#include "qdma_csr.h"

/* default CSR values for QDMA */
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
 * This is the programming OpenNIC has always done (formerly
 * onic_qdma_init_csr() in onic_hardware.c), moved here unchanged.
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
