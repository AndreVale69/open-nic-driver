/*
 * Test oracle: verbatim copy of onic_qdma_init_csr() and its constants from
 * upstream open-nic-driver commit 3de2269 (onic_hardware.c), i.e. the QDMA
 * global CSR programming before QDMA 5.0 support was added.  Only renamed to
 * legacy_onic_qdma_init_csr() and made non-static; onic_ring_count() removed.
 *
 * The test suite checks that the QDMA 4.0 path in qdma_access/qdma_csr.c
 * produces exactly the same register write sequence.  Do not edit.
 *
 * Copyright (c) 2020 Xilinx, Inc.  GPL-2.0, see COPYING.
 */
#include "onic_common.h"
#include "qdma_register.h"
#include "qdma_export.h"

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

#define RX_ALIGN_TIMEOUT_MS			1000
#define CMAC_RESET_WAIT_MS			1

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

/**
 * onic_qdma_init_csr - initialize QDMA config/status registers
 * @qdev: pointer to QDMA device
 *
 * This function writes to various H2C and C2H registers, getting QDMA ready for
 * queue operations.  Values of these registers are hard-coded.
 **/
void legacy_onic_qdma_init_csr(struct qdma_dev *qdev)
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
