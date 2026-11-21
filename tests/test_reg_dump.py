#!/usr/bin/env python3
"""Check tools/qdma_reg_dump.py against the driver's own CSR programming.

gen_bar_image runs the real qdma_init_global_csr_v4()/v5() on the fake BAR
for a given generation and cache depths and saves the register file; the dump
must then report no mismatch with the matching expectation and must flag the
wrong one.  This is what caught the dump assuming 128-entry caches while the
U250 OpenNIC IP is generated with 16.

Run from tests/ after `make gen_bar_image` (make check does both).
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_bar_image")
DUMP = os.path.join(HERE, "..", "tools", "qdma_reg_dump.py")

MISC_V4 = 0x00100000
MISC_V5 = 0x00200000


class RegDumpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.override = os.path.join(self.tmp.name, "qdma_ip_override")
        self.set_override(None)

    def set_override(self, value):
        if value is None:
            if os.path.exists(self.override):
                os.unlink(self.override)
            return
        with open(self.override, "w", encoding="ascii") as f:
            f.write("%d\n" % value)

    def image(self, prog, misc, be0, be4, name="bar.bin"):
        path = os.path.join(self.tmp.name, name)
        subprocess.run([GEN, prog, hex(misc), hex(be0), hex(be4), path],
                       check=True)
        return path

    def dump(self, img, expect="auto"):
        p = subprocess.run([sys.executable, "-I", DUMP, "synthetic",
                            "--resource", img, "--expect", expect,
                            "--override-param", self.override],
                           capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr

    def patch(self, img, off, val):
        with open(img, "r+b") as f:
            f.seek(off)
            f.write(struct.pack("<I", val))

    def read(self, img, off):
        with open(img, "rb") as f:
            f.seek(off)
            return struct.unpack("<I", f.read(4))[0]

    # ---------------------------------------------------------------------

    def test_matches_driver_for_all_cache_depths(self):
        for misc, prog in ((MISC_V5, "v5"), (MISC_V4, "v4")):
            for be0 in (8, 16, 32, 64, 128, 0x001F0010, 0x001F0080):
                for be4 in (16, 32, 128):
                    with self.subTest(prog=prog, be0=hex(be0), be4=be4):
                        img = self.image("auto", misc, be0, be4)
                        rc, out = self.dump(img, prog)
                        self.assertEqual(rc, 0, out)
                        self.assertIn("0 mismatches", out)

    def test_u250_depth16_regression(self):
        # the review's case: the old dump flagged 0xA80 and 0x1404 here
        img = self.image("auto", MISC_V5, 16, 128)
        rc, out = self.dump(img, "v5")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.read(img, 0x0A80), 0x000C000C)
        self.assertEqual(self.read(img, 0x1404), 0x00080060)
        # and the real U250 IP also has a 16-entry writeback buffer
        img = self.image("auto", MISC_V5, 16, 16, "u250.bin")
        rc, out = self.dump(img, "v5")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.read(img, 0x0B50), 0x40064014)

    def test_wrong_expectation_is_flagged(self):
        img = self.image("v5", MISC_V5, 16, 16)
        rc, out = self.dump(img, "v4")
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPECTED", out)
        img = self.image("v4", MISC_V5, 16, 16, "v4onv5.bin")
        rc, out = self.dump(img, "v5")
        self.assertEqual(rc, 1, out)

    def test_auto_follows_module_parameter(self):
        img = self.image("v4", MISC_V5, 16, 16)   # qdma_ip_override=4 on 5.0
        self.set_override(None)
        rc, out = self.dump(img)
        self.assertEqual(rc, 1, out)               # by silicon: v5, wrong
        self.assertIn("not readable", out)
        self.set_override(4)
        rc, out = self.dump(img)
        self.assertEqual(rc, 0, out)
        self.assertIn("qdma_ip_override=4", out)
        self.set_override(0)
        rc, out = self.dump(self.image("auto", MISC_V5, 16, 16, "auto.bin"))
        self.assertEqual(rc, 0, out)
        self.assertIn("by silicon", out)

    def test_no_magic_reads_nothing_else(self):
        img = os.path.join(self.tmp.name, "zero.bin")
        with open(img, "wb") as f:
            f.write(b"\0" * 0x40000)
        rc, out = self.dump(img)
        self.assertEqual(rc, 2, out)
        self.assertIn("not a QDMA config BAR", out)

    def test_v5_only_registers_not_read_on_v4(self):
        rc, out = self.dump(self.image("auto", MISC_V4, 16, 16), "v4")
        self.assertEqual(rc, 0, out)
        for name in ("PFCH_CFG_3", "QDMA_GLBL2_RRQ_PCIE_THROT",
                     "QDMA_H2C_MM_Data_Throttle"):
            self.assertNotIn(name, out)

    def test_bridge_throttle_follows_pcie_readback(self):
        img = self.image("auto", MISC_V5, 16, 16)
        pcie = self.read(img, 0x015C)
        brg = self.read(img, 0x0158)
        clamped = 0x4000                            # hardware kept fewer bits
        self.patch(img, 0x015C, (pcie & ~0x3FFFF) | clamped)
        rc, out = self.dump(img, "v5")
        self.assertEqual(rc, 1, out)                # 0x15C itself now differs
        self.assertIn("dat_throt 16384 (expected 20480)", out)
        self.patch(img, 0x0158, (brg & ~0x3FFFF) | clamped)
        rc, out = self.dump(img, "v5")
        line = [l for l in out.splitlines() if l.startswith("0x00158 ")][0]
        self.assertTrue(line.endswith("ok"), line)  # derived from the readback
        self.assertIn("1 mismatches", out)          # only 0x15C

    def test_error_status_decoded_not_checked(self):
        img = self.image("auto", MISC_V5, 16, 16)
        self.patch(img, 0x0248, 1 << 8)
        self.patch(img, 0x0AF0, (1 << 13) | (1 << 14))
        rc, out = self.dump(img, "v5")
        self.assertEqual(rc, 0, out)
        self.assertIn("wrb_qfull_err", out)
        self.assertIn("wrb_cidx_err", out)
        self.assertIn("err_c2h_st", out)

    def test_c2h_counters_reported_not_checked(self):
        img = self.image("auto", MISC_V4, 16, 16)
        self.patch(img, 0x0B10, 1234)
        rc, out = self.dump(img, "v4")
        self.assertEqual(rc, 0, out)
        line = [l for l in out.splitlines() if l.startswith("0x00B10 ")][0]
        self.assertIn("QDMA_C2H_STAT_DESC_RSP_DROP_ACCEPTED", line)
        self.assertIn("count 1234 (debug-dependent, drop-marked descriptor responses)", line)


if __name__ == "__main__":
    unittest.main(verbosity=1)
