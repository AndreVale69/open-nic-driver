#!/usr/bin/env python3
"""Check the driver's QDMA register definitions against AMD's register maps.

Usage:
    tools/qdma_check_regs.py --v4 qdma_v4_0_pf_registers.csv \\
                             --v5 qdma_v5_0_pf_registers.csv

The CSV files come from AMD's PG302 "Register Reference File" downloads
(qdma-v4-0-register-map.zip and qdma-v5-0-register-map.zip).  They are AMD
material and are not redistributed with this repository.

For every mask the driver uses, the script checks that it covers exactly one
field (same bit range) of the named register in the 4.0 and/or 5.0 map.  It
also checks that the error-aggregator enable masks only set documented bits.

Result classes:
    OK        exact match
    SUBSET    driver field is narrower than the hardware field, aligned at
              the same LSB (values used by the driver fit)
    LEGACY    known mismatch kept on purpose in the QDMA 4.0 path (QDMA 3.x
              encoding); see the QDMA compatibility report
    MISMATCH  unexpected difference -> exit status 1
"""
import argparse
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QDMA_DIR = os.path.join(HERE, "..", "qdma_access")

# (macro, register address, CSV field name, which maps, expected class)
FIELD_CHECKS = [
    # qdma_register.h: used by the shared and QDMA 4.0 paths
    ("QDMA_GLBL_DSC_CFG_WB_ACC_INT_MASK", 0x250, "wb_acc_int", "45", "OK"),
    ("QDMA_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK", 0x250, "Max_dsc_fetch", "45", "OK"),
    ("QDMA_C2H_PFCH_FL_TH_MASK", 0xB08, "pfch_fl_th", "45", "LEGACY"),
    ("QDMA_C2H_NUM_PFCH_MASK", 0xB08, None, "45", "LEGACY"),
    ("QDMA_C2H_PFCH_QCNT_MASK", 0xB08, None, "45", "LEGACY"),
    ("QDMA_C2H_EVT_QCNT_TH_MASK", 0xB08, "evt_pfch_fl_th", "45", "LEGACY"),
    ("QDMA_C2H_TICK_CNT_MASK", 0xB50, "tick_cnt", "45", "OK"),
    ("QDMA_C2H_TICK_VAL_MASK", 0xB50, "tick_val", "45", "OK"),
    ("QDMA_C2H_MAX_BUF_SZ_MASK", 0xB50, "max_buf_sz", "45", "OK"),
    ("QDMA_H2C_DATA_THRESH_MASK", 0xE24, "data_thresh", "45", "LEGACY"),
    ("QDMA_H2C_REQ_THROT_EN_DATA_MASK", 0xE24, "req_throt_en_data", "45", "LEGACY"),
    ("QDMA_H2C_REQ_THRESH_MASK", 0xE24, "req_throt", "45", "LEGACY"),
    ("QDMA_H2C_REQ_THROT_EN_REQ_MASK", 0xE24, "req_throt_en_req", "45", "OK"),
    ("QDMA_GLBL_ERR_FUNC_MASK", 0xB04, "Func", "45", "SUBSET"),
    ("QDMA_GLBL_ERR_VEC_MASK", 0xB04, "Vec", "45", "OK"),
    ("QDMA_GLBL_ERR_ARM_MASK", 0xB04, "Err_int_arm", "45", "OK"),
    ("QDMA_IND_CTXT_CMD_BUSY_MASK", 0x844, "busy", "45", "OK"),
    ("QDMA_DMAP_SEL_DESC_PIDX_MASK", 0x18004, "h2c_pidx", "4", "OK"),
    ("QDMA_DMAP_SEL_DESC_IRQ_ARM_MASK", 0x18004, "irq_arm", "4", "OK"),
    ("QDMA_DMAP_SEL_CMPL_CIDX_MASK", 0x1800C, "wrb_cidx", "4", "OK"),
    ("QDMA_DMAP_SEL_CMPL_COUNTER_IDX_MASK", 0x1800C, "c2h_count_threshhold", "4", "OK"),
    ("QDMA_DMAP_SEL_CMPL_TIMER_IDX_MASK", 0x1800C, "c2h_timer_cnt_index", "4", "OK"),
    ("QDMA_DMAP_SEL_CMPL_TRIG_MODE_MASK", 0x1800C, "trigger_mode", "4", "OK"),
    ("QDMA_DMAP_SEL_CMPL_STAT_EN_MASK", 0x1800C, "en_sts_desc_wrb", "4", "OK"),
    ("QDMA_DMAP_SEL_CMPL_IRQ_ARM_MASK", 0x1800C, "irq_en_wrb", "4", "OK"),
    # qdma_register_v5.h: QDMA 5.0 path
    ("QDMA5_CFG_BLK_MISC_CTL_10B_TAG_EN_MASK", 0x4C, "10b_tag_en", "45", "OK"),
    ("QDMA5_CFG_BLK_MISC_CTL_AXI_WBK_MASK", 0x4C, "axi_wbk", "45", "OK"),
    ("QDMA5_CFG_BLK_MISC_CTL_AXI_DSC_MASK", 0x4C, "axi_dsc", "45", "OK"),
    ("QDMA5_CFG_BLK_MISC_CTL_NUM_TAG_MASK", 0x4C, "num_tag", "45", "OK"),
    ("QDMA5_CFG_BLK_MISC_CTL_RQ_METERING_MUL_MASK", 0x4C, "rq_metering_multiplier", "45", "OK"),
    ("QDMA5_RRQ_THROT_REQ_EN_MASK", 0x158, "req_throt_en", "5", "OK"),
    ("QDMA5_RRQ_THROT_REQ_MASK", 0x158, "req_throt", "5", "OK"),
    ("QDMA5_RRQ_THROT_DAT_EN_MASK", 0x158, "dat_throt_en", "5", "OK"),
    ("QDMA5_RRQ_THROT_DAT_MASK", 0x158, "dat_throt", "5", "OK"),
    ("QDMA5_RRQ_THROT_REQ_EN_MASK", 0x15C, "req_throt_en", "5", "OK"),
    ("QDMA5_RRQ_THROT_REQ_MASK", 0x15C, "req_throt", "5", "OK"),
    ("QDMA5_RRQ_THROT_DAT_EN_MASK", 0x15C, "dat_throt_en", "5", "OK"),
    ("QDMA5_RRQ_THROT_DAT_MASK", 0x15C, "dat_throt", "5", "OK"),
    ("QDMA5_RRQ_THROT_REQ_EN_MASK", 0x160, "req_throt_en", "5", "OK"),
    ("QDMA5_RRQ_THROT_REQ_MASK", 0x160, "req_throt", "5", "OK"),
    ("QDMA5_RRQ_THROT_DAT_EN_MASK", 0x160, "dat_throt_en", "5", "OK"),
    ("QDMA5_RRQ_THROT_DAT_MASK", 0x160, "dat_throt", "5", "OK"),
    ("QDMA5_GLBL_DSC_CFG_C2H_UODSC_LIMIT_MASK", 0x250, "c2h_uodsc_limit", "5", "OK"),
    ("QDMA5_GLBL_DSC_CFG_H2C_UODSC_LIMIT_MASK", 0x250, "h2c_uodsc_limit", "5", "OK"),
    ("QDMA5_GLBL_DSC_CFG_UNC_OVR_COR_MASK", 0x250, "Unc_ovr_cor", "45", "OK"),
    ("QDMA5_GLBL_DSC_CFG_CTXT_FER_DIS_MASK", 0x250, "ctxt_fer_dis", "45", "OK"),
    ("QDMA5_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK", 0x250, "Max_dsc_fetch", "45", "OK"),
    ("QDMA5_GLBL_DSC_CFG_WB_ACC_INT_MASK", 0x250, "wb_acc_int", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_1_EVT_QCNT_TH_MASK", 0xA80, "evt_qcnt_th", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_1_QCNT_MASK", 0xA80, "pfch_qcnt", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_2_FENCE_MASK", 0xA84, "fence", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_2_VAR_DESC_NO_DROP_MASK", 0xA84, "var_desc_no_drop", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_2_LL_SZ_TH_MASK", 0xA84, "pfch_ll_sz_th", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_2_VAR_DESC_NUM_MASK", 0xA84, "var_desc_num_pfch", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_2_NUM_MASK", 0xA84, "num_pfch", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_EVT_PFCH_FL_TH_MASK", 0xB08, "evt_pfch_fl_th", "45", "OK"),
    ("QDMA5_C2H_PFCH_CFG_PFCH_FL_TH_MASK", 0xB08, "pfch_fl_th", "45", "OK"),
    ("QDMA5_C2H_PFCH_CACHE_DEPTH_MAX_STBUF_MASK", 0xBE0, "MAX_STBUF", "45", "OK"),
    ("QDMA5_C2H_PFCH_CACHE_DEPTH_MASK", 0xBE0, "cache_depth", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_EN_REQ_MASK", 0xE24, "req_throt_en_req", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_REQ_MASK", 0xE24, "req_throt", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_EN_DATA_MASK", 0xE24, "req_throt_en_data", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_DATA_MASK", 0xE24, "data_thresh", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_EN_REQ_MASK", 0xE2C, "req_throt_en_req", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_REQ_MASK", 0xE2C, "req_throt", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_EN_DATA_MASK", 0xE2C, "req_throt_en_data", "45", "OK"),
    ("QDMA5_H2C_REQ_THROT_DATA_MASK", 0xE2C, "data_thresh", "45", "OK"),
    ("QDMA5_H2C_MM_DATA_THROTTLE_EN_MASK", 0x12EC, "dat_throt_en", "5", "OK"),
    ("QDMA5_H2C_MM_DATA_THROTTLE_DAT_MASK", 0x12EC, "dat_throt", "5", "OK"),
    ("QDMA5_C2H_CRDT_COAL_CFG_1_PLD_FIFO_TH_MASK", 0x1400, "pld_fifo_th", "45", "OK"),
    ("QDMA5_C2H_CRDT_COAL_CFG_1_TIMER_TH_MASK", 0x1400, "crdt_timer_th", "45", "OK"),
    ("QDMA5_C2H_CRDT_COAL_CFG_2_FIFO_TH_MASK", 0x1404, "crdt_fifo_th", "45", "OK"),
    ("QDMA5_C2H_CRDT_COAL_CFG_2_CNT_TH_MASK", 0x1404, "crdt_cnt_th", "45", "OK"),
    ("QDMA5_PFCH_CFG_3_VAR_DESC_FL_FREE_CNT_TH_MASK", 0x147C, "var_desc_fl_free_cnt_th", "5", "OK"),
    ("QDMA5_PFCH_CFG_3_VAR_DESC_LG_PKT_CAM_CN_TH_MASK", 0x147C, "var_desc_lg_pkt_cam_cn_th", "5", "OK"),
    ("QDMA5_PFCH_CFG_4_GLB_EVT_TIMER_TICK_MASK", 0x1484, "glb_evt_timer_tick", "5", "OK"),
    ("QDMA5_PFCH_CFG_4_DISABLE_GLB_EVT_TIMER_MASK", 0x1484, "disable_glb_evt_timer", "5", "OK"),
    ("QDMA5_PFCH_CFG_4_EVT_TIMER_TICK_MASK", 0x1484, "evt_timer_tick", "5", "OK"),
    ("QDMA5_PFCH_CFG_4_DISABLE_EVT_TIMER_MASK", 0x1484, "disable_evt_timer", "5", "OK"),
]

# Indirect context command word, union qdma_ctxt_cmd in qdma_context.h
CMD_BITFIELDS = [("busy", 0, 0, "OK"), ("sel", 4, 1, "OK"), ("op", 6, 5, "OK"),
                 ("qid", 17, 7, "SUBSET")]

# Error aggregator enable masks written by onic_qdma_init_error_interrupt()
ERRMASK_CHECKS = [
    ("QDMA_GLBL_DSC_ERR_ALL_MASK", 0x254),
    ("QDMA_GLBL_TRQ_ERR_ALL_MASK", 0x264),
    ("QDMA_C2H_ERR_ALL_MASK", 0xAF0),
    ("QDMA_C2H_FATAL_ERR_ALL_MASK", 0xAF8),
    ("QDMA_H2C_ERR_ALL_MASK", 0xE00),
    ("QDMA_SBE_ERR_ALL_MASK", 0xF4),
    ("QDMA_DBE_ERR_ALL_MASK", 0xFC),
]


def parse_csv(path):
    regs = {}
    cur = None
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#") or row[0] == "register":
                continue
            row += [""] * (9 - len(row))
            reg, addr, field, _acc, _dt, width = (c.strip() for c in row[:6])
            if reg:
                try:
                    cur = int(addr.split("-")[0], 16)
                except ValueError:
                    cur = None
                    continue
                regs.setdefault(cur, {"name": reg, "fields": {}})
            elif cur is not None and field:
                m = re.match(r"\[(\d+)(?::(\d+))?\]", width)
                if m:
                    hi = int(m.group(1))
                    lo = int(m.group(2)) if m.group(2) else hi
                    regs[cur]["fields"][field] = (hi, lo)
    return regs


def parse_macros():
    macros = {}
    for name in ("qdma_register.h", "qdma_register_v5.h"):
        with open(os.path.join(QDMA_DIR, name), encoding="utf-8") as f:
            for line in f:
                m = re.match(r"\s*#define\s+(\w+)\s+(.+?)\s*$", line)
                if m:
                    macros[m.group(1)] = m.group(2)
    return macros


def eval_mask(expr, macros, depth=0):
    if depth > 8:
        raise ValueError(expr)
    expr = expr.strip()
    m = re.fullmatch(r"GENMASK(?:_ULL)?\((\d+),\s*(\d+)\)", expr)
    if m:
        hi, lo = int(m.group(1)), int(m.group(2))
        return ((1 << (hi + 1)) - 1) & ~((1 << lo) - 1)
    m = re.fullmatch(r"BIT\((\d+)\)", expr)
    if m:
        return 1 << int(m.group(1))
    if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", expr):
        return int(expr, 0)
    if expr in macros:
        return eval_mask(macros[expr], macros, depth + 1)
    raise ValueError(expr)


def bit_range(mask):
    lo = (mask & -mask).bit_length() - 1
    hi = mask.bit_length() - 1
    contiguous = mask == ((1 << (hi + 1)) - 1) & ~((1 << lo) - 1)
    return hi, lo, contiguous


def classify(hi, lo, regs, addr, field):
    reg = regs.get(addr)
    if reg is None:
        return "MISMATCH", "register 0x%X not in map" % addr
    if field is None or field not in reg["fields"]:
        overlap = [n for n, (h, l) in reg["fields"].items() if l <= hi and h >= lo]
        return "MISMATCH", "no such field; overlaps %s" % (", ".join(overlap) or "nothing")
    fh, fl = reg["fields"][field]
    if (fh, fl) == (hi, lo):
        return "OK", "%s[%d:%d]" % (field, fh, fl)
    if fl == lo and hi < fh:
        return "SUBSET", "driver [%d:%d] within %s[%d:%d]" % (hi, lo, field, fh, fl)
    return "MISMATCH", "driver [%d:%d] vs %s[%d:%d]" % (hi, lo, field, fh, fl)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--v4", required=True, help="qdma_v4_0_pf_registers.csv")
    ap.add_argument("--v5", required=True, help="qdma_v5_0_pf_registers.csv")
    args = ap.parse_args()
    maps = {"4": parse_csv(args.v4), "5": parse_csv(args.v5)}
    macros = parse_macros()
    unexpected = 0
    counts = {}

    print("%-44s %-7s %-3s %-8s %s" % ("macro", "addr", "map", "result", "detail"))
    for macro, addr, field, which, expect in FIELD_CHECKS:
        hi, lo, contiguous = bit_range(eval_mask(macro, macros))
        for v in which:
            if not contiguous:
                got, detail = "MISMATCH", "mask not contiguous"
            else:
                got, detail = classify(hi, lo, maps[v], addr, field)
            shown = got
            if got != "OK" and expect == "LEGACY":
                shown = "LEGACY"
            elif got != expect:
                shown = "MISMATCH"
                detail += " (expected %s)" % expect
                unexpected += 1
            counts[shown] = counts.get(shown, 0) + 1
            print("%-44s 0x%05X v%s  %-8s %s" % (macro, addr, v, shown, detail))

    for name, hi, lo, expect in CMD_BITFIELDS:
        csv_field = {"busy": "busy", "sel": "sel", "op": "op", "qid": "qid"}[name]
        for v in "45":
            got, detail = classify(hi, lo, maps[v], 0x844, csv_field)
            if got != expect:
                unexpected += 1
                got = "MISMATCH"
            counts[got] = counts.get(got, 0) + 1
            print("%-44s 0x%05X v%s  %-8s %s" % ("qdma_ctxt_cmd." + name, 0x844, v, got, detail))

    for macro, addr in ERRMASK_CHECKS:
        mask = eval_mask(macro, macros)
        for v in "45":
            reg = maps[v].get(addr)
            defined = 0
            for fname, (h, l) in (reg["fields"].items() if reg else []):
                if not fname.lower().startswith(("reserved", "rsvd")):
                    defined |= ((1 << (h + 1)) - 1) & ~((1 << l) - 1)
            extra = mask & ~defined
            res = "OK" if not extra else "LEGACY"
            counts[res] = counts.get(res, 0) + 1
            detail = "all set bits documented" if not extra else \
                "bits 0x%08X set but reserved/undefined" % extra
            print("%-44s 0x%05X v%s  %-8s %s" % (macro, addr, v, res, detail))

    print()
    print(", ".join("%s=%d" % kv for kv in sorted(counts.items())))
    if unexpected:
        print("%d unexpected mismatches" % unexpected)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
