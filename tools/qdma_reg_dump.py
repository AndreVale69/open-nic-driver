#!/usr/bin/env python3
"""Read-only dump of the QDMA global CSRs that onic programs.

Usage (needs root to map the BAR; reads only, never writes):
    sudo python3 -I tools/qdma_reg_dump.py 0000:65:00.0 [--expect v4|v5|auto]

Maps BAR-0 of the given PCI function read-only and reads an explicit list of
QDMA configuration registers.  None of them has read side effects (no RC /
clear-on-read registers are touched).  With --expect, the values are compared
with what qdma_init_global_csr_v4/v5() program on a QDMA with 128 prefetch
tags and a 128 entry completion coalescing buffer (the U250 OpenNIC build);
other tag counts change only the cache-depth derived fields.

Run it after `modprobe onic` (or insmod) on the master PF, before and after
traffic, and keep the output with the validation log.
"""
import argparse
import mmap
import os
import struct
import sys

REGS = [
    (0x0000, "Config_Block_Identifier"),
    (0x004C, "Config_Block_Misc_Control"),
    (0x0134, "QDMA_GLBL2_MISC_CAP"),
    (0x0158, "QDMA_GLBL2_RRQ_BRG_THROT"),
    (0x015C, "QDMA_GLBL2_RRQ_PCIE_THROT"),
    (0x0160, "QDMA_GLBL2_RRQ_AXIMM_THROT"),
    (0x0204, "QDMA_GLBL_RNG_SZ_1"),
    (0x0248, "QDMA_GLBL_ERR_STAT"),
    (0x024C, "QDMA_GLBL_ERR_MASK"),
    (0x0250, "QDMA_GLBL_DSC_CFG"),
    (0x0254, "QDMA_GLBL_DSC_ERR_STS"),
    (0x0264, "QDMA_GLBL_TRQ_ERR_STS"),
    (0x0A80, "QDMA_C2H_PFCH_CFG_1"),
    (0x0A84, "QDMA_C2H_PFCH_CFG_2"),
    (0x0AF0, "QDMA_C2H_ERR_STAT"),
    (0x0AF8, "QDMA_C2H_FATAL_ERR_STAT"),
    (0x0B04, "QDMA_GLBL_ERR_INT"),
    (0x0B08, "QDMA_C2H_PFCH_CFG"),
    (0x0B0C, "QDMA_C2H_INT_TIMER_TICK"),
    (0x0B50, "QDMA_C2H_WRB_COAL_CFG"),
    (0x0BE0, "QDMA_C2H_PFCH_CACHE_DEPTH"),
    (0x0BE4, "QDMA_C2H_WRB_COAL_BUF_DEPTH"),
    (0x0E00, "QDMA_H2C_ERR_STAT"),
    (0x0E24, "QDMA_H2C_REQ_THROT_PCIE"),
    (0x0E2C, "QDMA_H2C_REQ_THROT_AXIMM"),
    (0x12EC, "QDMA_H2C_MM_Data_Throttle"),
    (0x1400, "QDMA_C2H_CRDT_COAL_CFG_1"),
    (0x1404, "QDMA_C2H_CRDT_COAL_CFG_2"),
    (0x147C, "PFCH_CFG_3"),
    (0x1484, "PFCH_CFG_4"),
]

# Values written by the driver (same numbers as tests/test_qdma.c)
EXPECT = {
    "v4": {0x0250: 0x00000030, 0x0B08: 0x7D001100, 0x0B50: 0x00064014,
           0x0E24: 0x00C14000, 0x0B0C: 25, 0x0204: 4096},
    "v5": {0x0250: 0x00502015, 0x004C: 0x00010009, 0x0B08: 0x01000100,
           0x0A80: 0x007C007C, 0x0A84: 0x804003C8, 0x147C: 0x00008000,
           0x1484: 0x00800320, 0x1400: 0x00004010, 0x1404: 0x00780060,
           0x015C: 0x06045000, 0x0160: 0x00000000, 0x0158: 0x86045000,
           0x0E24: 0x86046000, 0x0E2C: 0x82044000, 0x12EC: 0x00015000,
           0x0B50: 0x00064014, 0x0B0C: 25, 0x0204: 4096},
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bdf", help="PCI address of the QDMA PF, e.g. 0000:65:00.0")
    ap.add_argument("--expect", choices=["v4", "v5", "auto", "none"], default="auto")
    ap.add_argument("--resource", help="read this file instead of the BAR "
                    "(offline testing with a captured or synthetic image)")
    args = ap.parse_args()

    path = args.resource or "/sys/bus/pci/devices/%s/resource0" % args.bdf
    fd = os.open(path, os.O_RDONLY | os.O_SYNC)
    try:
        size = os.fstat(fd).st_size or 0x40000
        mm = mmap.mmap(fd, min(size, 0x40000), mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd)

    def rd(off):
        return struct.unpack_from("<I", mm, off)[0]

    vals = {off: rd(off) for off, _ in REGS}
    mm.close()

    misc = vals[0x0134]
    gen = {0: "QDMA 3.x", 1: "QDMA 4.0", 2: "QDMA 5.0"}.get((misc >> 20) & 0xF, "unknown")
    if (misc >> 28) & 0xF:
        gen = "non-soft (Versal?)"
    print("device %s  block_id 0x%08X  misc_cap 0x%08X  -> %s"
          % (args.bdf, vals[0], misc, gen))
    if (vals[0] >> 16) != 0x1FD3:
        print("WARNING: block id has no 0x1fd3 magic; is this the QDMA config BAR?")

    exp_key = args.expect
    if exp_key == "auto":
        exp_key = "v5" if gen == "QDMA 5.0" else "v4"
    exp = EXPECT.get(exp_key, {})

    bad = 0
    for off, name in REGS:
        v = vals[off]
        note = ""
        if off in exp:
            ok = v == exp[off]
            bad += not ok
            note = "ok" if ok else "EXPECTED 0x%08X" % exp[off]
        print("0x%05X %-30s 0x%08X  %s" % (off, name, v, note))
    if exp:
        print("%s expectation: %d mismatches" % (exp_key, bad))
        print("note: values depend on the master PF having loaded onic;"
              " status registers are informational only")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
