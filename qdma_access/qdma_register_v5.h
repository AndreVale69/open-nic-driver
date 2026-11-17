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
#ifndef __QDMA_REGISTER_V5_H__
#define __QDMA_REGISTER_V5_H__

#include <linux/bits.h>

/*
 * Global CSRs programmed only on QDMA 5.0.
 *
 * Every address and field below is taken from AMD's register reference file
 * qdma_v5_0_pf_registers.csv (qdma-v5-0-register-map.zip, PG302 v5.0).
 * Registers that exist in QDMA 4.0 with the same layout are noted as such
 * (qdma_v4_0_pf_registers.csv); registers marked "new in 5.0" are absent
 * from the 4.0 map.
 *
 * The legacy definitions in qdma_register.h describe the QDMA 3.x layout for
 * some of these addresses (0xB08, 0xE24) and must not be used on 5.0.
 */

/* Config_Block_Misc_Control (0x4C), same layout in 4.0 */
#define QDMA5_OFFSET_CFG_BLK_MISC_CTL			0x4C
#define     QDMA5_CFG_BLK_MISC_CTL_10B_TAG_EN_MASK	BIT(23)
#define     QDMA5_CFG_BLK_MISC_CTL_AXI_WBK_MASK		BIT(21)
#define     QDMA5_CFG_BLK_MISC_CTL_AXI_DSC_MASK		BIT(20)
#define     QDMA5_CFG_BLK_MISC_CTL_NUM_TAG_MASK		GENMASK(19, 8)
#define     QDMA5_CFG_BLK_MISC_CTL_RQ_METERING_MUL_MASK	GENMASK(4, 0)

/* QDMA_GLBL2_RRQ_{BRG,PCIE,AXIMM}_THROT (0x158/0x15C/0x160), new in 5.0 */
#define QDMA5_OFFSET_GLBL2_RRQ_BRG_THROT		0x158
#define QDMA5_OFFSET_GLBL2_RRQ_PCIE_THROT		0x15C
#define QDMA5_OFFSET_GLBL2_RRQ_AXIMM_THROT		0x160
#define     QDMA5_RRQ_THROT_REQ_EN_MASK			BIT(31)
#define     QDMA5_RRQ_THROT_REQ_MASK			GENMASK(30, 19)
#define     QDMA5_RRQ_THROT_DAT_EN_MASK			BIT(18)
#define     QDMA5_RRQ_THROT_DAT_MASK			GENMASK(17, 0)

/*
 * QDMA_GLBL_DSC_CFG (0x250).  5.0 adds c2h_uodsc_limit and h2c_uodsc_limit
 * in bits that are reserved in 4.0.
 */
#define QDMA5_OFFSET_GLBL_DSC_CFG			0x250
#define     QDMA5_GLBL_DSC_CFG_C2H_UODSC_LIMIT_MASK	GENMASK(29, 20)
#define     QDMA5_GLBL_DSC_CFG_H2C_UODSC_LIMIT_MASK	GENMASK(19, 10)
#define     QDMA5_GLBL_DSC_CFG_UNC_OVR_COR_MASK		BIT(9)
#define     QDMA5_GLBL_DSC_CFG_CTXT_FER_DIS_MASK	BIT(8)
#define     QDMA5_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK	GENMASK(5, 3)
#define     QDMA5_GLBL_DSC_CFG_WB_ACC_INT_MASK		GENMASK(2, 0)

/* QDMA_C2H_PFCH_CFG_1 (0xA80), same layout in 4.0 */
#define QDMA5_OFFSET_C2H_PFCH_CFG_1			0xA80
#define     QDMA5_C2H_PFCH_CFG_1_EVT_QCNT_TH_MASK	GENMASK(31, 16)
#define     QDMA5_C2H_PFCH_CFG_1_QCNT_MASK		GENMASK(15, 0)

/* QDMA_C2H_PFCH_CFG_2 (0xA84), same layout in 4.0, var_desc_num reset differs */
#define QDMA5_OFFSET_C2H_PFCH_CFG_2			0xA84
#define     QDMA5_C2H_PFCH_CFG_2_FENCE_MASK		BIT(31)
#define     QDMA5_C2H_PFCH_CFG_2_VAR_DESC_NO_DROP_MASK	BIT(28)
#define     QDMA5_C2H_PFCH_CFG_2_LL_SZ_TH_MASK		GENMASK(27, 12)
#define     QDMA5_C2H_PFCH_CFG_2_VAR_DESC_NUM_MASK	GENMASK(11, 6)
#define     QDMA5_C2H_PFCH_CFG_2_NUM_MASK		GENMASK(5, 0)

/* QDMA_C2H_PFCH_CFG (0xB08), same layout in 4.0 */
#define QDMA5_OFFSET_C2H_PFCH_CFG			0xB08
#define     QDMA5_C2H_PFCH_CFG_EVT_PFCH_FL_TH_MASK	GENMASK(31, 16)
#define     QDMA5_C2H_PFCH_CFG_PFCH_FL_TH_MASK		GENMASK(15, 0)

/* QDMA_C2H_PFCH_CACHE_DEPTH (0xBE0), same layout in 4.0 */
#define QDMA5_OFFSET_C2H_PFCH_CACHE_DEPTH		0xBE0
#define     QDMA5_C2H_PFCH_CACHE_DEPTH_MAX_STBUF_MASK	GENMASK(23, 16)
#define     QDMA5_C2H_PFCH_CACHE_DEPTH_MASK		GENMASK(7, 0)

/* QDMA_H2C_REQ_THROT_PCIE (0xE24) and _AXIMM (0xE2C), same layout in 4.0 */
#define QDMA5_OFFSET_H2C_REQ_THROT_PCIE			0xE24
#define QDMA5_OFFSET_H2C_REQ_THROT_AXIMM		0xE2C
#define     QDMA5_H2C_REQ_THROT_EN_REQ_MASK		BIT(31)
#define     QDMA5_H2C_REQ_THROT_REQ_MASK		GENMASK(30, 19)
#define     QDMA5_H2C_REQ_THROT_EN_DATA_MASK		BIT(18)
#define     QDMA5_H2C_REQ_THROT_DATA_MASK		GENMASK(17, 0)

/* QDMA_H2C_MM_Data_Throttle (0x12EC), new in 5.0 */
#define QDMA5_OFFSET_H2C_MM_DATA_THROTTLE		0x12EC
#define     QDMA5_H2C_MM_DATA_THROTTLE_EN_MASK		BIT(16)
#define     QDMA5_H2C_MM_DATA_THROTTLE_DAT_MASK		GENMASK(15, 0)

/* QDMA_C2H_CRDT_COAL_CFG_1/2 (0x1400/0x1404), same layout in 4.0 */
#define QDMA5_OFFSET_C2H_CRDT_COAL_CFG_1		0x1400
#define     QDMA5_C2H_CRDT_COAL_CFG_1_PLD_FIFO_TH_MASK	GENMASK(17, 10)
#define     QDMA5_C2H_CRDT_COAL_CFG_1_TIMER_TH_MASK	GENMASK(9, 0)
#define QDMA5_OFFSET_C2H_CRDT_COAL_CFG_2		0x1404
#define     QDMA5_C2H_CRDT_COAL_CFG_2_FIFO_TH_MASK	GENMASK(23, 16)
#define     QDMA5_C2H_CRDT_COAL_CFG_2_CNT_TH_MASK	GENMASK(10, 0)

/* PFCH_CFG_3 (0x147C) and PFCH_CFG_4 (0x1484), new in 5.0 */
#define QDMA5_OFFSET_PFCH_CFG_3				0x147C
#define     QDMA5_PFCH_CFG_3_VAR_DESC_FL_FREE_CNT_TH_MASK GENMASK(15, 7)
#define     QDMA5_PFCH_CFG_3_VAR_DESC_LG_PKT_CAM_CN_TH_MASK GENMASK(6, 0)
#define QDMA5_OFFSET_PFCH_CFG_4				0x1484
#define     QDMA5_PFCH_CFG_4_GLB_EVT_TIMER_TICK_MASK	GENMASK(31, 17)
#define     QDMA5_PFCH_CFG_4_DISABLE_GLB_EVT_TIMER_MASK	BIT(16)
#define     QDMA5_PFCH_CFG_4_EVT_TIMER_TICK_MASK	GENMASK(15, 1)
#define     QDMA5_PFCH_CFG_4_DISABLE_EVT_TIMER_MASK	BIT(0)

#endif
