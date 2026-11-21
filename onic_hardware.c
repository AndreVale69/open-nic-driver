/*
 * Copyright (c) 2020 Xilinx, Inc.
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
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/pci.h>

#include "onic_hardware.h"
#include "onic_register.h"
#include "onic.h"
#include "qdma_register.h"
#include "qdma_context.h"
#include "qdma_csr.h"
#include "qdma_error_info.h"

#define RX_ALIGN_TIMEOUT_MS			1000
#define CMAC_RESET_TIMEOUT_MS			1000

/* 0 = detect from QDMA_GLBL2_MISC_CAP, 4 = QDMA 4.0 programming,
 * 5 = QDMA 5.0 programming
 */
static int qdma_ip_override;
module_param(qdma_ip_override, int, 0444);
MODULE_PARM_DESC(qdma_ip_override,
		 "QDMA programming: 0=auto-detect (default), 4=force QDMA 4.0, 5=force QDMA 5.0");

u16 onic_ring_count(u8 idx)
{
	return qdma_ring_count(idx);
}

/**
 * qdma_select_hw - detect the QDMA IP generation and pick its programming
 * @qdev: QDMA device with BAR-0 mapped
 * @dev: device used for log messages
 *
 * Returns 0, or -ENODEV if BAR-0 does not look like the QDMA config BAR.
 **/
static int qdma_select_hw(struct qdma_dev *qdev, struct device *dev)
{
	const struct qdma_hw_version *ver = &qdev->ver;
	enum qdma_ip_gen gen;
	int rv;

	rv = qdma_detect_version(qdev);
	if (rv < 0) {
		dev_err(dev, "BAR-0 is not a QDMA config BAR (block id 0x%08x)",
			ver->block_id);
		return rv;
	}

	gen = ver->gen;
	dev_info(dev, "QDMA IP: %s (misc_cap 0x%08x: device type %u, ip version %u, vivado release id %u, rtl version %u)",
		 qdma_ip_gen_name(gen), ver->misc_cap, ver->device_type,
		 ver->ip_version, ver->vivado_release, ver->rtl_version);

	switch (qdma_ip_override) {
	case 0:
		break;
	case 4:
		gen = QDMA_IP_GEN_QDMA4;
		break;
	case 5:
		gen = QDMA_IP_GEN_QDMA5;
		break;
	default:
		dev_warn(dev, "ignoring invalid qdma_ip_override=%d",
			 qdma_ip_override);
		break;
	}
	if (gen != ver->gen)
		dev_warn(dev, "qdma_ip_override=%d: programming as %s, hardware reports %s",
			 qdma_ip_override, qdma_ip_gen_name(gen),
			 qdma_ip_gen_name(ver->gen));
	else if (gen == QDMA_IP_GEN_UNKNOWN || gen == QDMA_IP_GEN_QDMA3)
		dev_warn(dev, "QDMA generation not validated with this driver, using QDMA 4.0 programming");

	qdev->ops = qdma_hw_ops_for_gen(gen);
	dev_info(dev, "QDMA programming: %s", qdev->ops->name);
	return 0;
}

/**
 * onic_wait_shell_status - wait until all @mask bits are set in shell status
 * @hw: pointer to hardware
 * @mask: status bits to wait for
 *
 * Returns 0, or -ETIMEDOUT after CMAC_RESET_TIMEOUT_MS.  The wait used to be
 * unbounded, which hung the probe (busy loop, no rmmod) on an image whose
 * reset never completes.
 **/
static int onic_wait_shell_status(struct onic_hardware *hw, u32 mask)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(CMAC_RESET_TIMEOUT_MS);

	for (;;) {
		if ((onic_read_reg(hw, SYSCFG_OFFSET_SHELL_STATUS) & mask) == mask)
			return 0;
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}
}

static int onic_enable_cmac(struct onic_hardware *hw, u8 cmac_id)
{
	u32 reset_mask;

	if (cmac_id != 0 && cmac_id != 1)
		return -EINVAL;

	reset_mask = (cmac_id == 0) ? 0x10 : 0x100;
	onic_write_reg(hw, SYSCFG_OFFSET_SHELL_RESET, reset_mask);
	if (onic_wait_shell_status(hw, reset_mask))
		return -ETIMEDOUT;

    if (hw->RS_FEC) {
		/* Enable RS-FEC for CMACs with RS-FEC implemented */
		onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_ENABLE(cmac_id), 0x3);
		onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_IND_CORRECTION(cmac_id), 0x7);
    }

	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_1(cmac_id), 0x1);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_1(cmac_id), 0x10);

	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_1(cmac_id), 0x1);

	/* RX flow control */
	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_FC_CTRL_1(cmac_id), 0x00003DFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_FC_CTRL_2(cmac_id), 0x0001C631);

	/* TX flow control */
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_1(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_2(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_3(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_4(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_5(cmac_id), 0x0000FFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_1(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_2(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_3(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_4(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_5(cmac_id), 0x0000FFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_CTRL_1(cmac_id), 0x000001FF);

	return 0;
}

int onic_init_hardware(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev;
	struct qdma_fmap_ctxt fmap_ctxt;
	u16 qbase, qmax, func_id;
	u32 val;
	u8 master_pf = test_bit(ONIC_FLAG_MASTER_PF, priv->flags);
	int i, rv;

    priv->hw.RS_FEC = priv->RS_FEC;

	/* shell registers uses BAR-2 */
	hw->addr = pci_iomap_range(pdev, 2, SHELL_START, SHELL_MAXLEN);
	if (!hw->addr)
		return -EINVAL;

	/* QDMA IP registers uses BAR-0 */
	qdev = qdma_create_dev(pdev, 0);
	if (!qdev) {
		rv = -ENOMEM;
		goto unmap_shell;
	}

	rv = qdma_select_hw(qdev, &pdev->dev);
	if (rv < 0)
		goto destroy_qdma;

	func_id = PCI_FUNC(pdev->devfn);
	qbase = func_id * ONIC_MAX_QUEUES;
	qmax = max(priv->num_tx_queues, priv->num_rx_queues);

	/* initialize QDMA function map context */
	memset(&fmap_ctxt, 0, sizeof(struct qdma_fmap_ctxt));
	fmap_ctxt.qbase = qbase;
	fmap_ctxt.qmax = qmax;
	rv = qdma_clear_fmap_ctxt(qdev);
	if (rv < 0)
		goto fmap_failed;
	rv = qdma_write_fmap_ctxt(qdev, &fmap_ctxt);
	if (rv < 0)
		goto fmap_failed;

	/* inform shell about the function map */
	val = (FIELD_SET(QDMA_FUNC_QCONF_QBASE_MASK, qbase) |
	       FIELD_SET(QDMA_FUNC_QCONF_NUMQ_MASK, qmax));
	onic_write_reg(hw, QDMA_FUNC_OFFSET_QCONF(func_id), val);

	/* initialize indirection table */
	for (i = 0; i < 128; ++i) {
		u32 val = (i % qmax) & 0x0000FFFF;
		u32 offset = QDMA_FUNC_OFFSET_INDIR_TABLE(func_id, i);
		onic_write_reg(hw, offset, val);
	}

	/* initialize global registers if device is a master PF */
	if (master_pf)
		qdev->ops->init_global_csr(qdev);

    hw->qdma = (unsigned long)qdev;

	/* get the number of CMAC instances */
	for (i = 0; i < ONIC_MAX_CMACS; ++i) {
		val = onic_read_reg(hw, CMAC_OFFSET_CORE_VERSION(i));
		if (val != ONIC_CMAC_CORE_VERSION)
			break;
		if (master_pf && onic_enable_cmac(hw, i) < 0)
			dev_warn(&pdev->dev,
				 "CMAC %d: shell reset not done after %d ms, port not enabled",
				 i, CMAC_RESET_TIMEOUT_MS);
	}
	hw->num_cmacs = i;
	dev_info(&pdev->dev, "Number of CMAC instances = %d", hw->num_cmacs);

	return 0;

	/*
	 * hw->qdma is still unset on these paths, so unwind here instead of
	 * calling onic_clear_hardware(), which would use a NULL QDMA handle.
	 */
fmap_failed:
	dev_err(&pdev->dev, "QDMA function map programming failed, err = %d",
		rv);
destroy_qdma:
	qdma_destroy_dev(qdev);
unmap_shell:
	pci_iounmap(pdev, hw->addr);
	hw->addr = NULL;
	return rv;
}

void onic_clear_hardware(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev = (struct qdma_dev *)hw->qdma;
	u16 func_id = PCI_FUNC(pdev->devfn);

	/* clear the function map in shell */
	if (hw->addr)
		onic_write_reg(hw, QDMA_FUNC_OFFSET_QCONF(func_id), 0);

	if (qdev) {
		qdma_invalidate_fmap_ctxt(qdev);
		qdma_destroy_dev(qdev);
	}

	if (hw->addr)
		pci_iounmap(pdev, hw->addr);

	memset(hw, 0, sizeof(struct onic_hardware));
}

void onic_qdma_init_error_interrupt(unsigned long qdma, u16 vid)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	int i;

	offset = QDMA_OFFSET_GLBL_ERR_INT;
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, vid) |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 0));
	qdma_write_reg(qdev, offset, val);

	for (i = 0; i < NUM_LEAF_ERROR_AGGREGATORS; i++) {
		u32 err_idx = leaf_error_aggregators[i];

		offset = qdma_error_info[err_idx].mask_reg_addr;
		val = qdma_error_info[err_idx].leaf_err_mask;
		qdma_write_reg(qdev, offset, val);

		offset = QDMA_OFFSET_GLBL_ERR_MASK;
		val = qdma_read_reg(qdev, offset);
		val |= FIELD_SET(qdma_error_info[err_idx].glbl_err_mask, 1);
		qdma_write_reg(qdev, offset, val);
	}

	offset = QDMA_OFFSET_GLBL_ERR_INT;
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, vid) |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 1));
	qdma_write_reg(qdev, offset, val);
}

void onic_qdma_clear_error_interrupt(unsigned long qdma)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_INT, 0);
}

/*
 * Error status fields of the QDMA 4.0 and 5.0 register maps, which are
 * identical for these registers (qdma_v4_0/v5_0_pf_registers.csv).  The
 * per-error entries of qdma_error_info.h follow the QDMA 3.x layout, which
 * differs for the descriptor, TRQ, H2C and RAM registers, so only their
 * aggregate masks are used (to enable errors), not their names.  The RAM
 * double bit error register (0xFC) has the layout of 0xF4.
 */
struct onic_err_field {
	u16 reg;
	u8 hi;
	u8 lo;
	const char *name;
};

static const struct onic_err_field onic_err_fields[] = {
	{ 0x248, 17, 17, "err_fab" }, { 0x248, 16, 16, "err_h2c_st" },
	{ 0x248, 15, 15, "err_bdg" }, { 0x248, 14, 9, "ind_ctxt_cmd_err" },
	{ 0x248, 8, 8, "err_c2h_st" }, { 0x248, 7, 7, "err_c2h_mm_1" },
	{ 0x248, 6, 6, "err_c2h_mm_0" }, { 0x248, 5, 5, "err_h2c_mm_1" },
	{ 0x248, 4, 4, "err_h2c_mm_0" }, { 0x248, 3, 3, "err_trq" },
	{ 0x248, 2, 2, "err_dsc" }, { 0x248, 1, 1, "err_ram_dbe" },
	{ 0x248, 0, 0, "err_ram_sbe" },

	{ 0x254, 25, 25, "port_id" }, { 0x254, 24, 24, "sbe" },
	{ 0x254, 23, 23, "dbe" }, { 0x254, 22, 22, "rq_cancel" },
	{ 0x254, 21, 21, "dsc" }, { 0x254, 20, 20, "dma" },
	{ 0x254, 19, 19, "flr_cancel" }, { 0x254, 16, 16, "dat_poison" },
	{ 0x254, 9, 9, "timeout" }, { 0x254, 8, 8, "flr" },
	{ 0x254, 6, 6, "tag" }, { 0x254, 5, 5, "addr" },
	{ 0x254, 4, 4, "param" }, { 0x254, 3, 3, "bcnt" },
	{ 0x254, 2, 2, "ur_ca" }, { 0x254, 1, 1, "poison" },

	{ 0x264, 7, 7, "tcp_qspc_timeout" }, { 0x264, 5, 5, "qid_range" },
	{ 0x264, 4, 4, "qspc_unmapped" }, { 0x264, 3, 3, "tcp_csr_timeout" },
	{ 0x264, 1, 1, "vf_access_err" }, { 0x264, 0, 0, "csr_unmapped" },

	{ 0xAF0, 20, 20, "wrb_port_id_err" }, { 0xAF0, 19, 19, "hdr_par_err" },
	{ 0xAF0, 18, 18, "hdr_ecc_cor_err" }, { 0xAF0, 17, 17, "hdr_ecc_unc_err" },
	{ 0xAF0, 16, 16, "avl_ring_dsc_err" }, { 0xAF0, 15, 15, "wrb_prty_err" },
	{ 0xAF0, 14, 14, "wrb_cidx_err" }, { 0xAF0, 13, 13, "wrb_qfull_err" },
	{ 0xAF0, 12, 12, "wrb_inv_q_err" },
	{ 0xAF0, 10, 10, "port_id_ctxt_mismatch" },
	{ 0xAF0, 9, 9, "err_desc_cnt" }, { 0xAF0, 7, 7, "msi_int_fail" },
	{ 0xAF0, 6, 6, "eng_wpl_data_par_err" }, { 0xAF0, 4, 4, "desc_rsp_err" },
	{ 0xAF0, 3, 3, "qid_mismatch" }, { 0xAF0, 2, 2, "sh_cmpt_dsc_err" },
	{ 0xAF0, 1, 1, "len_mismatch" }, { 0xAF0, 0, 0, "mty_mismatch" },

	{ 0xAF8, 20, 20, "hdr_ecc_unc_err" },
	{ 0xAF8, 19, 19, "avl_ring_fifo_ram_rdbe" },
	{ 0xAF8, 18, 18, "wpl_data_par_err" },
	{ 0xAF8, 17, 17, "payload_fifo_ram_rdbe" },
	{ 0xAF8, 16, 16, "qid_fifo_ram_rdbe" },
	{ 0xAF8, 15, 15, "cmpt_fifo_ram_rdbe" },
	{ 0xAF8, 14, 14, "wrb_coal_data_ram_rdbe" },
	{ 0xAF8, 12, 12, "int_ctxt_ram_rdbe" },
	{ 0xAF8, 11, 11, "desc_req_fifo_ram_rdbe" },
	{ 0xAF8, 10, 10, "pfch_ctxt_ram_rdbe" },
	{ 0xAF8, 9, 9, "wrb_ctxt_ram_rdbe" }, { 0xAF8, 8, 8, "pfch_ll_ram_rdbe" },
	{ 0xAF8, 7, 4, "timer_fifo_ram_rdbe" }, { 0xAF8, 3, 3, "qid_mismatch" },
	{ 0xAF8, 1, 1, "len_mismatch" }, { 0xAF8, 0, 0, "mty_mismatch" },

	{ 0xE00, 5, 5, "par_err" }, { 0xE00, 4, 4, "sbe" },
	{ 0xE00, 3, 3, "dbe" }, { 0xE00, 2, 2, "no_dma_dsc_err" },
	{ 0xE00, 1, 1, "sdi_mrkr_req_mop_err" },
	{ 0xE00, 0, 0, "zero_len_dsc_err" },

	{ 0x0F4, 31, 31, "rc_rrq_odd_ram" }, { 0x0F4, 30, 30, "pend_fifo_ram" },
	{ 0x0F4, 29, 29, "pfch_ll_ram" }, { 0x0F4, 28, 28, "wrb_ctxt_ram" },
	{ 0x0F4, 27, 27, "pfch_ctxt_ram" }, { 0x0F4, 26, 26, "desc_req_fifo_ram" },
	{ 0x0F4, 25, 25, "int_ctxt_ram" }, { 0x0F4, 24, 24, "wrb_coal_data_ram" },
	{ 0x0F4, 23, 23, "qid_fifo_ram" }, { 0x0F4, 22, 19, "timer_fifo_ram" },
	{ 0x0F4, 18, 18, "mi_tl_slv_fifo_ram" }, { 0x0F4, 17, 17, "dsc_cpld" },
	{ 0x0F4, 16, 16, "dsc_cpli" }, { 0x0F4, 15, 15, "dsc_sw_ctxt" },
	{ 0x0F4, 14, 14, "dsc_crd_rcv" }, { 0x0F4, 13, 13, "dsc_hw_ctxt" },
	{ 0x0F4, 12, 12, "func_map" }, { 0x0F4, 11, 11, "c2h_wr_brg_dat" },
	{ 0x0F4, 10, 10, "c2h_rd_brg_dat" }, { 0x0F4, 9, 9, "h2c_wr_brg_dat" },
	{ 0x0F4, 8, 8, "h2c_rd_brg_dat" }, { 0x0F4, 7, 4, "mi_c2h_dat" },
	{ 0x0F4, 3, 0, "mi_h2c_dat" },
};

/* append " name" for every set field of status register @reg to @buf */
static void onic_qdma_err_names(u32 reg, u32 val, char *buf, size_t len)
{
	u32 key = (reg == QDMA_OFFSET_RAM_DBE_STAT) ? QDMA_OFFSET_RAM_SBE_STAT : reg;
	u32 known = 0;
	size_t used = 0;
	unsigned int i;

	buf[0] = '\0';
	for (i = 0; i < ARRAY_SIZE(onic_err_fields); i++) {
		const struct onic_err_field *f = &onic_err_fields[i];
		u32 mask = GENMASK(f->hi, f->lo);

		if (f->reg != key)
			continue;
		known |= mask;
		if (val & mask)
			used += scnprintf(buf + used, len - used, " %s", f->name);
	}
	if (val & ~known)
		scnprintf(buf + used, len - used, " reserved:0x%x", val & ~known);
}

u32 onic_qdma_handle_errors(unsigned long qdma)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct device *dev = &qdev->pdev->dev;
	char names[320];
	u32 glbl, stat, val;
	int i;

	glbl = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT);
	if (glbl) {
		onic_qdma_err_names(QDMA_OFFSET_GLBL_ERR_STAT, glbl, names,
				    sizeof(names));
		dev_err_ratelimited(dev, "QDMA error: QDMA_GLBL_ERR_STAT 0x%08x:%s",
				    glbl, names);
	}

	for (i = 0; i < NUM_LEAF_ERROR_AGGREGATORS; i++) {
		const struct qdma_error_info *agg =
			&qdma_error_info[leaf_error_aggregators[i]];

		if (!(glbl & agg->glbl_err_mask))
			continue;

		stat = qdma_read_reg(qdev, agg->stat_reg_addr);
		if (!stat)
			continue;

		onic_qdma_err_names(agg->stat_reg_addr, stat, names,
				    sizeof(names));
		dev_err_ratelimited(dev, "QDMA error: status 0x%03x = 0x%08x:%s",
				    agg->stat_reg_addr, stat, names);

		/* status registers are write-1-to-clear */
		qdma_write_reg(qdev, agg->stat_reg_addr, stat);
	}

	if (glbl & QDMA_GLBL_ERR_ST_C2H_MASK)
		dev_err_ratelimited(dev, "QDMA error: QDMA_C2H_FIRST_ERR_QID 0x%08x",
				    qdma_read_reg(qdev, QDMA_OFFSET_C2H_FIRST_ERR_QID));
	if (glbl & QDMA_GLBL_ERR_ST_H2C_MASK)
		dev_err_ratelimited(dev, "QDMA error: QDMA_H2C_FIRST_ERR_QID 0x%08x",
				    qdma_read_reg(qdev, QDMA_OFFSET_H2C_FIRST_ERR_QID));

	if (glbl)
		qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT, glbl);

	/* the QDMA disarms the error interrupt when it fires */
	val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_ERR_INT);
	val |= FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 1);
	qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_INT, val);

	return glbl;
}

int onic_qdma_init_tx_queue(unsigned long qdma, u16 qid,
			    const struct onic_qdma_h2c_param *param)
{
	const enum qdma_dir dir = QDMA_H2C;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct qdma_sw_ctxt sw_ctxt;
	int rv;

	if (qid < 0)
		return qid;

	/* initialize software context */
	memset(&sw_ctxt, 0, sizeof(struct qdma_sw_ctxt));
	sw_ctxt.func_id = qdev->func_id;
	sw_ctxt.qen = 1;
	sw_ctxt.wbk_en = 1;
	sw_ctxt.is_mm = 0;
	sw_ctxt.irq_arm = 0;
	sw_ctxt.irq_en = 0;
	sw_ctxt.desc_sz = 1; /* 1: 16B for H2C stream */
	sw_ctxt.fcrd_en = 0;
	sw_ctxt.wbi_chk = 1;
	sw_ctxt.wbi_intvl_en = 1;
	sw_ctxt.at = 0;
	sw_ctxt.rngsz_idx = param->rngcnt_idx;
	sw_ctxt.desc_base = param->dma_addr;
	sw_ctxt.vec = param->vid;
	sw_ctxt.intr_aggr = 0;

	rv = qdma_clear_sw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_tx_queue;

	/* initialize hardware and credit context */
	rv = qdma_clear_hw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;
	rv = qdma_clear_cr_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;

	return 0;

clear_tx_queue:
	onic_qdma_clear_tx_queue(qdma, qid);
	return rv;
}

int onic_qdma_init_rx_queue(unsigned long qdma, u16 qid,
			    const struct onic_qdma_c2h_param *param)
{
	const enum qdma_dir dir = QDMA_C2H;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct qdma_sw_ctxt sw_ctxt;
	struct qdma_pfch_ctxt pfch_ctxt;
	struct qdma_cmpl_ctxt cmpl_ctxt;
	int rv;

	if (qid < 0)
		return qid;

	/* initialize software context */
	memset(&sw_ctxt, 0, sizeof(struct qdma_sw_ctxt));
	sw_ctxt.func_id = qdev->func_id;
	sw_ctxt.qen = 1;
	sw_ctxt.wbk_en = 1;
	sw_ctxt.is_mm = 0;
	sw_ctxt.desc_sz = 0; /* 0: 8B for C2H stream */
	sw_ctxt.fcrd_en = 1;
	sw_ctxt.rngsz_idx = param->desc_rngcnt_idx;
	sw_ctxt.desc_base = param->desc_dma_addr;

	rv = qdma_clear_sw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	/* initialize hardware and credit context */
	rv = qdma_clear_hw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_clear_cr_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;

	/* initialize prefetch and completion contexts */
	memset(&pfch_ctxt, 0, sizeof(struct qdma_pfch_ctxt));
	pfch_ctxt.bufsz_idx = param->bufsz_idx;
	pfch_ctxt.pfch_en = 1;
	pfch_ctxt.valid = 1;

	rv = qdma_clear_pfch_ctxt(qdev, qid);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_pfch_ctxt(qdev, qid, &pfch_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	memset(&cmpl_ctxt, 0, sizeof(struct qdma_cmpl_ctxt));
	cmpl_ctxt.stat_en = 1;
	//cmpl_ctxt.stat_en = 0;
	cmpl_ctxt.intr_en = 1;
	cmpl_ctxt.trig_mode = 0x5;
	cmpl_ctxt.func_id = qdev->func_id;
	cmpl_ctxt.counter_idx = 0;
	cmpl_ctxt.timer_idx = 0;
	cmpl_ctxt.color = 1;
	cmpl_ctxt.rngsz_idx = param->cmpl_rngcnt_idx;
	cmpl_ctxt.baddr = param->cmpl_dma_addr;
	cmpl_ctxt.desc_sz = param->cmpl_desc_sz;
	cmpl_ctxt.valid = 1;
	cmpl_ctxt.full_upd = 0;
	cmpl_ctxt.ovf_chk_dis = 0;
	cmpl_ctxt.vec = param->vid;
	cmpl_ctxt.intr_aggr = 0;

	rv = qdma_clear_cmpl_ctxt(qdev, qid);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_cmpl_ctxt(qdev, qid, &cmpl_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	return 0;

clear_rx_queue:
	onic_qdma_clear_rx_queue(qdma, qid);
	return rv;
}

void onic_qdma_clear_tx_queue(unsigned long qdma, u16 qid)
{
	const enum qdma_dir dir = QDMA_H2C;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	if (qid < 0)
		return;

	qdma_invalidate_sw_ctxt(qdev, qid, dir);
	qdma_invalidate_hw_ctxt(qdev, qid, dir);
	qdma_invalidate_cr_ctxt(qdev, qid, dir);
}

void onic_qdma_clear_rx_queue(unsigned long qdma, u16 qid)
{
	const enum qdma_dir dir = QDMA_C2H;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	if (qid < 0)
		return;

	qdma_invalidate_sw_ctxt(qdev, qid, dir);
	qdma_invalidate_hw_ctxt(qdev, qid, dir);
	qdma_invalidate_cr_ctxt(qdev, qid, dir);
	qdma_invalidate_pfch_ctxt(qdev, qid);
	qdma_invalidate_cmpl_ctxt(qdev, qid);
}

/**
 * onic_qdma_set_q_pidx - set QDMA queue producer index
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @dir: queue direction
 * @pidx: producer index
 * @irq_arm: interrupt arm bit for next interrupt generation
 **/
static void onic_qdma_set_q_pidx(unsigned long qdma, u16 qid,
				 enum qdma_dir dir, u16 pidx, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	bool debug = 0;
	if (debug) dev_info(&qdev->pdev->dev, "onic_qdma_set_q_pidx(qid:%u, dir:%x, pidx:%u, irq_qrm:%u)", qid, dir, pidx, irq_arm);

	if (qid < 0)
		return;

	if (dir == QDMA_C2H)
		offset = QDMA_OFFSET_DMAP_SEL_C2H_DESC_PIDX + (qid * 16);
	else
		offset = QDMA_OFFSET_DMAP_SEL_H2C_DESC_PIDX + (qid * 16);

	val = (FIELD_SET(QDMA_DMAP_SEL_DESC_PIDX_MASK, pidx) |
	       FIELD_SET(QDMA_DMAP_SEL_DESC_IRQ_ARM_MASK, irq_arm));
	qdma_write_reg(qdev, offset, val);
}

void onic_set_tx_head(unsigned long qdma, u16 qid, u16 head)
{
	onic_qdma_set_q_pidx(qdma, qid, QDMA_H2C, head, 0);
}

void onic_set_rx_head(unsigned long qdma, u16 qid, u16 head)
{
	onic_qdma_set_q_pidx(qdma, qid, QDMA_C2H, head, 0);
}

/**
 * onic_qdma_set_cmpl_cidx - set QDMA completion queue consumer index
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @cidx: consumer index
 * @counter_idx: index to C2H counter threshold registers
 * @timer_idx: index to C2H timer registers
 * @trig_mode: interrupt and status descriptor trigger mode
 * @stat_en: enable completion status writeback
 * @irq_arm: interrupt arm bit for next interrupt generation
 **/
static void onic_qdma_set_cmpl_cidx(unsigned long qdma, u16 qid, u16 cidx,
				       u8 counter_idx, u8 timer_idx,
				       u8 trig_mode, u8 stat_en, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	bool debug = 0;
	if (debug) dev_info(&qdev->pdev->dev, "onic_qdma_set_cmpl_cidx(qid:%u, cidx:%u, idx:%u, timser_idx:%u, trig_mode:%u irq_arm:%u)", qid, cidx, counter_idx, timer_idx, trig_mode, irq_arm);

	if (qid < 0)
		return;

	offset = QDMA_OFFSET_DMAP_SEL_CMPL_CIDX + (qid * 16);

	val = (FIELD_SET(QDMA_DMAP_SEL_CMPL_CIDX_MASK, cidx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_COUNTER_IDX_MASK, counter_idx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_TIMER_IDX_MASK, timer_idx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_TRIG_MODE_MASK, trig_mode) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_STAT_EN_MASK, stat_en) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_IRQ_ARM_MASK, irq_arm));
	qdma_write_reg(qdev, offset, val);
}

void onic_set_completion_tail(unsigned long qdma, u16 qid, u16 tail, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u8 trig_mode = 5; // trigger from: user, count, or timer
	u8 stat_en = 1;  // enabled is necessary for getting proper completion_status, e.g. for knowing pidx
	bool debug = 0;
	if (debug) dev_info(&qdev->pdev->dev, "onic_set_completion_tail (qid:%u, tail:%u, irq_arm:%u)", qid, tail, irq_arm);
	onic_qdma_set_cmpl_cidx(qdma, qid, tail, 0, 0, trig_mode, stat_en, irq_arm);
}
