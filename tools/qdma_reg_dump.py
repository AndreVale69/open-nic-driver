#!/usr/bin/env python3
"""Read-only dump of the QDMA global CSRs that onic programs, with checks.

Usage (needs root to map the BAR; reads only, never writes):
    sudo python3 -I tools/qdma_reg_dump.py 0000:65:00.0 [--expect auto|v4|v5|none]

Maps BAR-0 of the given PCI function read-only.  The Config_Block_Identifier
is read first: without the QDMA magic nothing else is read (exit status 2).
Then QDMA_GLBL2_MISC_CAP tells the silicon generation, and only registers that
exist on that generation are read.  None of them has read side effects (no
clear-on-read registers).

--expect compares the values with what qdma_init_global_csr_v4()/v5() write.
The expected values are derived like the driver derives them, from the
capability registers measured on this device (0xBE0 prefetch cache depth,
0xBE4 completion coalescing buffer depth, 0x15C PCIe throttle readback), so
the check holds for any build, e.g. the U250 OpenNIC IP generated with
PFCH_CACHE_DEPTH=16 and WRB_COAL_MAX_BUF=16.

--expect auto (default) checks against the programming the driver used: the
onic module parameter qdma_ip_override (read from sysfs; 4 or 5 forces it),
otherwise the generation the silicon reports.  The source is printed.

Error status registers are decoded and reported, never part of the check;
so are the C2H counters (packets and completions accepted from the shell,
packets dropped because no RX descriptor was posted).

Run it on the master PF after loading onic, before and after each traffic
phase, and keep the output with the validation log.

Exit status: 0 all checked values match, 1 mismatches, 2 not a QDMA config
BAR (or not readable).
"""
import argparse
import mmap
import os
import struct
import sys

OVERRIDE_PARAM = "/sys/module/onic/parameters/qdma_ip_override"
QDMA_MAGIC = 0x1FD3
BAR_SPAN = 0x40000


def genmask(hi, lo):
    return ((1 << (hi + 1)) - 1) & ~((1 << lo) - 1)


def bit(n):
    return 1 << n


def field_set(mask, val):
    """FIELD_SET() from onic_common.h: shift into place and mask."""
    shift = (mask & -mask).bit_length() - 1
    return ((val & 0xFFFFFFFFFFFFFFFF) << shift) & mask


def field_get(mask, reg):
    shift = (mask & -mask).bit_length() - 1
    return (reg & mask) >> shift


# ---------------------------------------------------------------------------
# Registers.  (offset, name, exists only in QDMA 5.0)

COMMON = False
V5_ONLY = True

REGS = [
    (0x0000, "Config_Block_Identifier", COMMON),
    (0x004C, "Config_Block_Misc_Control", COMMON),
    (0x0134, "QDMA_GLBL2_MISC_CAP", COMMON),
    (0x0158, "QDMA_GLBL2_RRQ_BRG_THROT", V5_ONLY),
    (0x015C, "QDMA_GLBL2_RRQ_PCIE_THROT", V5_ONLY),
    (0x0160, "QDMA_GLBL2_RRQ_AXIMM_THROT", V5_ONLY),
    (0x0248, "QDMA_GLBL_ERR_STAT", COMMON),
    (0x024C, "QDMA_GLBL_ERR_MASK", COMMON),
    (0x0250, "QDMA_GLBL_DSC_CFG", COMMON),
    (0x0254, "QDMA_GLBL_DSC_ERR_STS", COMMON),
    (0x0264, "QDMA_GLBL_TRQ_ERR_STS", COMMON),
    (0x0A80, "QDMA_C2H_PFCH_CFG_1", COMMON),
    (0x0A84, "QDMA_C2H_PFCH_CFG_2", COMMON),
    (0x0A88, "QDMA_C2H_STAT_S_AXIS_C2H_ACCEPTED", COMMON),
    (0x0A8C, "QDMA_C2H_STAT_S_AXIS_WRB_ACCEPTED", COMMON),
    (0x0A90, "QDMA_C2H_STAT_DESC_RSP_PKT_ACCEPTED", COMMON),
    (0x0AF0, "QDMA_C2H_ERR_STAT", COMMON),
    (0x0AF8, "QDMA_C2H_FATAL_ERR_STAT", COMMON),
    (0x0B04, "QDMA_GLBL_ERR_INT", COMMON),
    (0x0B08, "QDMA_C2H_PFCH_CFG", COMMON),
    (0x0B0C, "QDMA_C2H_INT_TIMER_TICK", COMMON),
    (0x0B10, "QDMA_C2H_STAT_DESC_RSP_DROP_ACCEPTED", COMMON),
    (0x0B14, "QDMA_C2H_STAT_DESC_RSP_ERR_ACCEPTED", COMMON),
    (0x0B30, "QDMA_C2H_FIRST_ERR_QID", COMMON),
    (0x0B50, "QDMA_C2H_WRB_COAL_CFG", COMMON),
    (0x0BC0, "QDMA_C2H_DROP_PAYLOAD_CNT", COMMON),
    (0x0BE0, "QDMA_C2H_PFCH_CACHE_DEPTH", COMMON),
    (0x0BE4, "QDMA_C2H_WRB_COAL_BUF_DEPTH", COMMON),
    (0x0E00, "QDMA_H2C_ERR_STAT", COMMON),
    (0x0E08, "QDMA_H2C_FIRST_ERR_QID", COMMON),
    (0x0E24, "QDMA_H2C_REQ_THROT_PCIE", COMMON),
    (0x0E2C, "QDMA_H2C_REQ_THROT_AXIMM", COMMON),
    (0x12EC, "QDMA_H2C_MM_Data_Throttle", V5_ONLY),
    (0x1400, "QDMA_C2H_CRDT_COAL_CFG_1", COMMON),
    (0x1404, "QDMA_C2H_CRDT_COAL_CFG_2", COMMON),
    (0x147C, "PFCH_CFG_3", V5_ONLY),
    (0x1484, "PFCH_CFG_4", V5_ONLY),
]

# Lookup tables written by qdma_init_csr_tables() (qdma_csr.c), both paths
TABLES = [
    ("QDMA_GLBL_RNG_SZ", 0x0204, [4096, 64, 128, 192, 256, 384, 512, 768,
                                  1024, 1536, 3072, 4096, 6144, 8192, 12288,
                                  16384]),
    ("QDMA_C2H_TIMER_CNT", 0x0A00, [10, 2, 4, 5, 8, 10, 15, 20, 25, 30, 50,
                                    75, 100, 125, 150, 200]),
    ("QDMA_C2H_CNT_TH", 0x0A40, [64, 2, 4, 8, 16, 24, 32, 48, 80, 96, 112,
                                 128, 144, 160, 176, 192]),
    ("QDMA_C2H_BUF_SZ", 0x0AB0, [4096, 256, 512, 1024, 2048, 3968, 4096,
                                 4096, 4096, 4096, 4096, 4096, 4096, 8192,
                                 9018, 16384]),
]

# Fields of the programmed registers, used to explain a mismatch.  The 0xB08
# and 0xE24 layouts of the v4 path are the QDMA 3.x ones it writes on purpose.
FIELDS = {
    "v4": {
        0x0250: [("max_dsc_fetch", genmask(5, 3)), ("wb_acc_int", genmask(2, 0))],
        0x0B08: [("evt_qcnt_th(3.x)", genmask(31, 25)),
                 ("pfch_qcnt(3.x)", genmask(24, 18)),
                 ("num_pfch(3.x)", genmask(17, 9)),
                 ("pfch_fl_th(3.x)", genmask(8, 0))],
        0x0E24: [("req_throt_en_req(3.x)", bit(31)),
                 ("req_thresh(3.x)", genmask(25, 17)),
                 ("req_throt_en_data(3.x)", bit(16)),
                 ("data_thresh(3.x)", genmask(15, 0))],
    },
    "v5": {
        0x004C: [("num_tag", genmask(19, 8)), ("rq_metering_mul", genmask(4, 0))],
        0x0250: [("c2h_uodsc_limit", genmask(29, 20)),
                 ("h2c_uodsc_limit", genmask(19, 10)),
                 ("max_dsc_fetch", genmask(5, 3)), ("wb_acc_int", genmask(2, 0))],
        0x0A80: [("evt_qcnt_th", genmask(31, 16)), ("pfch_qcnt", genmask(15, 0))],
        0x0A84: [("fence", bit(31)), ("pfch_ll_sz_th", genmask(27, 12)),
                 ("var_desc_num_pfch", genmask(11, 6)), ("num_pfch", genmask(5, 0))],
        0x0B08: [("evt_pfch_fl_th", genmask(31, 16)), ("pfch_fl_th", genmask(15, 0))],
        0x1400: [("pld_fifo_th", genmask(17, 10)), ("crdt_timer_th", genmask(9, 0))],
        0x1404: [("crdt_fifo_th", genmask(23, 16)), ("crdt_cnt_th", genmask(10, 0))],
        0x147C: [("var_desc_fl_free_cnt_th", genmask(15, 7))],
        0x1484: [("glb_evt_timer_tick", genmask(31, 17)),
                 ("evt_timer_tick", genmask(15, 1))],
    },
}
THROT = [("req_throt_en", bit(31)), ("req_throt", genmask(30, 19)),
         ("dat_throt_en", bit(18)), ("dat_throt", genmask(17, 0))]
for _off in (0x0158, 0x015C, 0x0160, 0x0E24, 0x0E2C):
    FIELDS["v5"][_off] = THROT
FIELDS["v5"][0x12EC] = [("dat_throt_en", bit(16)), ("dat_throt", genmask(15, 0))]
WRB_COAL = [("max_buf_sz", genmask(31, 26)), ("tick_val", genmask(25, 14)),
            ("tick_cnt", genmask(13, 2))]
FIELDS["v4"][0x0B50] = WRB_COAL
FIELDS["v5"][0x0B50] = WRB_COAL

# Error status decode, identical in the QDMA 4.0 and 5.0 register maps
STATUS_BITS = {
    0x0248: {17: "err_fab", 16: "err_h2c_st", 15: "err_bdg", 8: "err_c2h_st",
             7: "err_c2h_mm_1", 6: "err_c2h_mm_0", 5: "err_h2c_mm_1",
             4: "err_h2c_mm_0", 3: "err_trq", 2: "err_dsc", 1: "err_ram_dbe",
             0: "err_ram_sbe", "14:9": "ind_ctxt_cmd_err"},
    0x0254: {25: "port_id", 24: "sbe", 23: "dbe", 22: "rq_cancel", 21: "dsc",
             20: "dma", 19: "flr_cancel", 16: "dat_poison", 9: "timeout",
             8: "flr", 6: "tag", 5: "addr", 4: "param", 3: "bcnt", 2: "ur_ca",
             1: "poison"},
    0x0264: {7: "tcp_qspc_timeout", 5: "qid_range", 4: "qspc_unmapped",
             3: "tcp_csr_timeout", 1: "vf_access_err", 0: "csr_unmapped"},
    0x0AF0: {20: "wrb_port_id_err", 19: "hdr_par_err", 18: "hdr_ecc_cor_err",
             17: "hdr_ecc_unc_err", 16: "avl_ring_dsc_err", 15: "wrb_prty_err",
             14: "wrb_cidx_err", 13: "wrb_qfull_err", 12: "wrb_inv_q_err",
             10: "port_id_ctxt_mismatch", 9: "err_desc_cnt", 7: "msi_int_fail",
             6: "eng_wpl_data_par_err", 4: "desc_rsp_err", 3: "qid_mismatch",
             2: "sh_cmpt_dsc_err", 1: "len_mismatch", 0: "mty_mismatch"},
    0x0AF8: {20: "hdr_ecc_unc_err", 19: "avl_ring_fifo_ram_rdbe",
             18: "wpl_data_par_err", 17: "payload_fifo_ram_rdbe",
             16: "qid_fifo_ram_rdbe", 15: "cmpt_fifo_ram_rdbe",
             14: "wrb_coal_data_ram_rdbe", 12: "int_ctxt_ram_rdbe",
             11: "desc_req_fifo_ram_rdbe", 10: "pfch_ctxt_ram_rdbe",
             9: "wrb_ctxt_ram_rdbe", 8: "pfch_ll_ram_rdbe", 3: "qid_mismatch",
             1: "len_mismatch", 0: "mty_mismatch", "7:4": "timer_fifo_ram_rdbe"},
    0x0E00: {5: "par_err", 4: "sbe", 3: "dbe", 2: "no_dma_dsc_err",
             1: "sdi_mrkr_req_mop_err", 0: "zero_len_dsc_err"},
}


def decode_status(off, val):
    names = []
    for key, name in STATUS_BITS.get(off, {}).items():
        if isinstance(key, int):
            if val & bit(key):
                names.append(name)
        else:
            hi, lo = (int(x) for x in key.split(":"))
            if val & genmask(hi, lo):
                names.append("%s=0x%x" % (name, field_get(genmask(hi, lo), val)))
    return names


# Read-only C2H counters (same in 4.0 and 5.0): packets and completions the
# QDMA took from the shell, and packets dropped because no descriptor was
# posted.  Compare them before and after a traffic phase.
COUNTERS = {0x0A88, 0x0A8C, 0x0A90, 0x0B10, 0x0B14, 0x0BC0}


def decode_other(off, val):
    if off in COUNTERS:
        return "count %d" % val
    if off == 0x0B04:
        return "vec %d func %d %s" % (field_get(genmask(22, 12), val),
                                      field_get(genmask(11, 0), val),
                                      "armed" if val & bit(24) else "NOT armed")
    if off in (0x0B30, 0x0E08):
        if not val:
            return ""
        hi = 20 if off == 0x0B30 else 19
        return "err_type %d qid %d" % (field_get(genmask(hi, 16), val),
                                       field_get(genmask(12, 0), val))
    if off == 0x0BE0:
        return "cache_depth %d" % field_get(genmask(7, 0), val)
    if off == 0x0BE4:
        return "buffer_depth %d" % val
    return ""


# ---------------------------------------------------------------------------
# Expected values: Python mirror of qdma_init_global_csr_v4()/v5()

def expect_tables():
    exp = {}
    for _name, base, values in TABLES:
        for i, v in enumerate(values):
            exp[base + 4 * i] = v
    exp[0x0B0C] = 25
    return exp


def wrb_coal_cfg(wrb_depth):
    return (field_set(genmask(13, 2), 5) | field_set(genmask(25, 14), 25) |
            field_set(genmask(31, 26), wrb_depth))


def expect_v4(m):
    exp = expect_tables()
    bd = m[0x0BE0]
    exp[0x0250] = field_set(genmask(5, 3), 6) | field_set(genmask(2, 0), 0)
    exp[0x0B08] = (field_set(genmask(8, 0), 256) | field_set(genmask(17, 9), 8) |
                   field_set(genmask(24, 18), bd >> 1) |
                   field_set(genmask(31, 25), ((bd >> 1) - 2) & 0xFFFFFFFF))
    exp[0x0B50] = wrb_coal_cfg(m[0x0BE4])
    exp[0x0E24] = (field_set(genmask(15, 0), 0x4000) | field_set(bit(16), 1) |
                   field_set(genmask(25, 17), 0x60) | field_set(bit(31), 0))
    return exp


def throt(req_en, req, dat_en, dat):
    return (field_set(bit(31), req_en) | field_set(genmask(30, 19), req) |
            field_set(bit(18), dat_en) | field_set(genmask(17, 0), dat))


def expect_v5(m):
    exp = expect_tables()
    depth = max(field_get(genmask(7, 0), m[0x0BE0]), 8)
    exp[0x0B50] = wrb_coal_cfg(m[0x0BE4])
    exp[0x0250] = (field_set(genmask(29, 20), 5) | field_set(genmask(19, 10), 8) |
                   field_set(genmask(5, 3), 2) | field_set(genmask(2, 0), 5))
    exp[0x004C] = field_set(genmask(19, 8), 256) | field_set(genmask(4, 0), 9)
    exp[0x0B08] = field_set(genmask(31, 16), 256) | field_set(genmask(15, 0), 256)
    exp[0x0A80] = (field_set(genmask(31, 16), depth - 4) |
                   field_set(genmask(15, 0), depth - 4))
    exp[0x0A84] = (field_set(bit(31), 1) | field_set(genmask(27, 12), 1024) |
                   field_set(genmask(11, 6), 15) | field_set(genmask(5, 0), 8))
    exp[0x147C] = field_set(genmask(15, 7), 256)
    exp[0x1484] = field_set(genmask(31, 17), 64) | field_set(genmask(15, 1), 400)
    exp[0x1400] = field_set(genmask(17, 10), 16) | field_set(genmask(9, 0), 16)
    exp[0x1404] = (field_set(genmask(23, 16), depth - 8) |
                   field_set(genmask(10, 0), 96))
    exp[0x015C] = throt(0, 192, 1, 20480)
    exp[0x0160] = 0
    if 0x015C in m:
        # the driver derives the bridge threshold from the PCIe readback
        exp[0x0158] = throt(1, 192, 1, field_get(genmask(17, 0), m[0x015C]))
    exp[0x0E24] = throt(1, 192, 1, 24576)
    exp[0x0E2C] = throt(1, 64, 1, 16384)
    exp[0x12EC] = field_set(bit(16), 1) | field_set(genmask(15, 0), 20480)
    return exp


def explain(mode, off, got, exp):
    parts = []
    for name, mask in FIELDS.get(mode, {}).get(off, []):
        g, e = field_get(mask, got), field_get(mask, exp)
        if g != e:
            parts.append("%s %d (expected %d)" % (name, g, e))
    covered = 0
    for _name, mask in FIELDS.get(mode, {}).get(off, []):
        covered |= mask
    if (got ^ exp) & ~covered & 0xFFFFFFFF:
        parts.append("other bits 0x%08X" % ((got ^ exp) & ~covered & 0xFFFFFFFF))
    return ", ".join(parts)


# ---------------------------------------------------------------------------

def silicon_generation(misc):
    if (misc >> 28) & 0xF:
        return None, "non-soft IP (Versal CPM?)"
    ip = (misc >> 20) & 0xF
    return {0: ("3", "QDMA 3.x"), 1: ("4", "QDMA 4.0"),
            2: ("5", "QDMA 5.0")}.get(ip, (None, "unknown (ip_version %d)" % ip))


def programming_mode(expect, silicon, override_path):
    """Return (mode, reason).  mode is 'v4', 'v5' or None (no check)."""
    if expect == "none":
        return None, "no check requested"
    if expect in ("v4", "v5"):
        return expect, "--expect %s" % expect
    try:
        with open(override_path, encoding="ascii") as f:
            override = int(f.read().strip() or "0")
    except (OSError, ValueError):
        override = None
    if override in (4, 5):
        return "v%d" % override, "onic qdma_ip_override=%d" % override
    reason = ("onic qdma_ip_override=0" if override == 0 else
              "qdma_ip_override not readable (onic loaded?)")
    # same rule as qdma_hw_ops_for_gen(): only QDMA 5.0 gets the 5.0 path
    return ("v5" if silicon == "5" else "v4"), reason + ", by silicon"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bdf", help="PCI address of the QDMA PF, e.g. 0000:65:00.0")
    ap.add_argument("--expect", choices=["auto", "v4", "v5", "none"],
                    default="auto")
    ap.add_argument("--resource", help="read this file instead of the BAR "
                    "(offline checks of a captured or synthetic image)")
    ap.add_argument("--override-param", default=OVERRIDE_PARAM,
                    help="where --expect auto reads qdma_ip_override "
                    "(default: %(default)s)")
    ap.add_argument("--force", action="store_true",
                    help="read the registers even without the QDMA magic")
    args = ap.parse_args(argv)

    path = args.resource or "/sys/bus/pci/devices/%s/resource0" % args.bdf
    try:
        fd = os.open(path, os.O_RDONLY | os.O_SYNC)
    except OSError as e:
        print("ERROR: cannot open %s: %s" % (path, e))
        return 2
    try:
        size = min(os.fstat(fd).st_size or BAR_SPAN, BAR_SPAN)
        mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
    except (OSError, ValueError) as e:
        print("ERROR: cannot map %s: %s" % (path, e))
        return 2
    finally:
        os.close(fd)

    def rd(off):
        return struct.unpack_from("<I", mm, off)[0]

    try:
        block_id = rd(0x0000)
        if (block_id >> 16) != QDMA_MAGIC and not args.force:
            print("device %s  block_id 0x%08X: no 0x%04X magic, this is not a "
                  "QDMA config BAR; nothing else read (--force to override)"
                  % (args.bdf, block_id, QDMA_MAGIC))
            return 2
        misc = rd(0x0134)
        silicon, gen_name = silicon_generation(misc)
        regs = [(off, name, v5_only) for off, name, v5_only in REGS
                if silicon == "5" or not v5_only]
        vals = {off: rd(off) for off, _name, _where in regs}
        for _name, base, values in TABLES:
            for i in range(len(values)):
                vals[base + 4 * i] = rd(base + 4 * i)
    finally:
        mm.close()

    mode, reason = programming_mode(args.expect, silicon, args.override_param)
    print("device %s  block_id 0x%08X  misc_cap 0x%08X -> silicon %s"
          % (args.bdf, block_id, misc, gen_name))
    print("checking against: %s (%s)" % (mode or "nothing", reason))
    if mode == "v5" and silicon != "5":
        print("WARNING: QDMA 5.0 programming on %s silicon" % gen_name)

    exp = {}
    if mode == "v4":
        exp = expect_v4(vals)
    elif mode == "v5":
        exp = expect_v5(vals)

    bad = 0
    shown = {off for off, _n, _w in regs}
    for off, name, _where in regs:
        v = vals[off]
        note = ""
        if off in exp:
            if v == exp[off]:
                note = "ok"
            else:
                bad += 1
                note = "EXPECTED 0x%08X  %s" % (exp[off], explain(mode, off, v, exp[off]))
        elif off in STATUS_BITS:
            names = decode_status(off, v)
            note = ("ERROR STATUS: " + ", ".join(names)) if names else ""
        else:
            note = decode_other(off, v)
        print("0x%05X %-36s 0x%08X  %s" % (off, name, v, note))

    for name, base, values in TABLES:
        offs = [base + 4 * i for i in range(len(values))]
        wrong = [o for o in offs if o in exp and vals[o] != exp[o]]
        bad += len(wrong)
        if not exp:
            print("0x%05X %-36s %s" % (base, name + "[0..15]",
                                       " ".join(str(vals[o]) for o in offs)))
        elif wrong:
            for o in wrong:
                print("0x%05X %-36s 0x%08X  EXPECTED 0x%08X"
                      % (o, "%s[%d]" % (name, (o - base) // 4), vals[o], exp[o]))
        else:
            print("0x%05X %-36s %d/%d ok" % (base, name + "[0..15]",
                                             len(offs), len(offs)))

    missing = sorted(o for o in exp if o not in vals and o not in shown)
    for o in missing:
        print("0x%05X not read on this silicon (expected 0x%08X)" % (o, exp[o]))

    if exp:
        print("%s expectation (cache_depth %d, wrb buffer_depth %d): %d mismatches"
              % (mode, field_get(genmask(7, 0), vals[0x0BE0]), vals[0x0BE4], bad))
        print("note: values depend on the master PF having loaded onic; "
              "error status registers are informational only")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
