/*
 * Hardware-free tests for the OpenNIC QDMA access layer.
 *
 * Compiles the real qdma_access C sources in userspace against a fake
 * BAR-0 (fake_mmio.c).  Expected values are written out by hand from the
 * field layouts in AMD's qdma_v4_0_pf_registers.csv / qdma_v5_0_pf_registers.csv
 * (global CSRs) and the PG302 context structure tables, which are identical
 * for QDMA 4.0 and 5.0 (AMD dma_ip_drivers eqdma_soft_reg.h is unchanged for
 * these between releases 2020.2 and 2025.2).  They are deliberately not
 * computed with the driver's own masks.
 *
 * What these tests can show: the driver encodes what we intend, the QDMA 4.0
 * write sequence is unchanged, and version detection picks the right path.
 * What they cannot show: that QDMA 5.0 hardware behaves as documented.
 */
#include "kshim.h"
#include "fake_mmio.h"

#include "onic_common.h"
#include "qdma_register.h"
#include "qdma_register_v5.h"
#include "qdma_context.h"
#include "qdma_export.h"
#include "qdma_csr.h"
#include "qdma_version.h"

void legacy_onic_qdma_init_csr(struct qdma_dev *qdev);

static int failures, checks;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fputc('\n', stderr);					\
	}								\
} while (0)

#define CHECK_EQ(got, exp, what) \
	CHECK((u64)(got) == (u64)(exp), "%s: got 0x%llx, expected 0x%llx", \
	      what, (unsigned long long)(got), (unsigned long long)(exp))

/* Reset values from the PG302 register maps, as seen on a U250 build */
#define CONFIG_BLOCK_ID_V4V5		0x1fd30001u	/* identifier 0x1fd, 3, version 1 */
#define PFCH_CACHE_DEPTH_RESET		0x001f0080u	/* MAX_STBUF 31, cache_depth 128 */
#define WRB_COAL_BUF_DEPTH_RESET	0x00000080u	/* buffer_depth 128 */

static struct pci_dev test_pdev;
static struct qdma_dev test_qdev;

static struct qdma_dev *new_qdev(u32 misc_cap, u16 func_id, u16 q_base)
{
	fake_mmio_reset();
	fake_mmio_preset(QDMA_OFFSET_CONFIG_BLOCK_ID, CONFIG_BLOCK_ID_V4V5);
	fake_mmio_preset(QDMA_OFFSET_GLBL2_MISC_CAP, misc_cap);
	fake_mmio_preset(QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH, PFCH_CACHE_DEPTH_RESET);
	fake_mmio_preset(QDMA_OFFSET_C2H_CMPL_COAL_BUF_DEPTH,
			 WRB_COAL_BUF_DEPTH_RESET);

	memset(&test_qdev, 0, sizeof(test_qdev));
	test_pdev.devfn = func_id;
	test_qdev.pdev = &test_pdev;
	test_qdev.func_id = func_id;
	test_qdev.q_base = q_base;
	test_qdev.addr = (void *)FAKE_BAR_BASE;
	return &test_qdev;
}

/* ------------------------------------------------------------------------ */
/* Version detection                                                          */

static void test_version_decode(void)
{
	static const struct {
		u32 block_id;
		u32 misc_cap;
		int rv;
		enum qdma_ip_gen gen;
		const char *what;
	} cases[] = {
		{ CONFIG_BLOCK_ID_V4V5, 0x00000000, 0, QDMA_IP_GEN_QDMA3, "ip_version 0 = QDMA 3.x" },
		{ CONFIG_BLOCK_ID_V4V5, 0x00100000, 0, QDMA_IP_GEN_QDMA4, "ip_version 1 = QDMA 4.0" },
		{ CONFIG_BLOCK_ID_V4V5, 0x02100000, 0, QDMA_IP_GEN_QDMA4, "QDMA 4.0, vivado id 2" },
		{ CONFIG_BLOCK_ID_V4V5, 0x00200000, 0, QDMA_IP_GEN_QDMA5, "ip_version 2 = QDMA 5.0" },
		{ CONFIG_BLOCK_ID_V4V5, 0x00210000, 0, QDMA_IP_GEN_QDMA5, "QDMA 5.0, rtl patch" },
		{ CONFIG_BLOCK_ID_V4V5, 0x0020ffff, 0, QDMA_IP_GEN_QDMA5, "low bits ignored" },
		{ CONFIG_BLOCK_ID_V4V5, 0x00300000, 0, QDMA_IP_GEN_UNKNOWN, "ip_version 3 unknown" },
		{ CONFIG_BLOCK_ID_V4V5, 0x10200000, 0, QDMA_IP_GEN_UNKNOWN, "Versal CPM4 not soft IP" },
		{ CONFIG_BLOCK_ID_V4V5, 0x20200000, 0, QDMA_IP_GEN_UNKNOWN, "Versal CPM5 not soft IP" },
		{ 0x1fd30000, 0x00200000, 0, QDMA_IP_GEN_QDMA5, "magic only in [31:16]" },
		{ 0x00000000, 0x00200000, -ENODEV, QDMA_IP_GEN_UNKNOWN, "bad magic" },
		{ 0xffffffff, 0xffffffff, -ENODEV, QDMA_IP_GEN_UNKNOWN, "surprise removal / all ones" },
	};
	struct qdma_hw_version ver;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		int rv = qdma_decode_version(cases[i].block_id,
					     cases[i].misc_cap, &ver);

		CHECK(rv == cases[i].rv, "%s: rv %d, expected %d",
		      cases[i].what, rv, cases[i].rv);
		CHECK(ver.gen == cases[i].gen, "%s: gen %s, expected %s",
		      cases[i].what, qdma_ip_gen_name(ver.gen),
		      qdma_ip_gen_name(cases[i].gen));
	}

	qdma_decode_version(CONFIG_BLOCK_ID_V4V5, 0x12345678, &ver);
	CHECK_EQ(ver.device_type, 1, "device_type = misc_cap[31:28]");
	CHECK_EQ(ver.vivado_release, 2, "vivado_release = misc_cap[27:24]");
	CHECK_EQ(ver.ip_version, 3, "ip_version = misc_cap[23:20]");
	CHECK_EQ(ver.rtl_version, 4, "rtl_version = misc_cap[19:16]");
}

static void test_version_detect_reads_registers(void)
{
	struct qdma_dev *qdev = new_qdev(0x00200000, 0, 0);

	CHECK(qdma_detect_version(qdev) == 0, "detect returns 0");
	CHECK_EQ(qdev->ver.gen, QDMA_IP_GEN_QDMA5, "detected generation");
	CHECK_EQ(qdev->ver.misc_cap, 0x00200000, "raw misc_cap kept");
	CHECK(fake_log_len == 0, "detection must not write registers");
}

static void test_ops_selection(void)
{
	CHECK(qdma_hw_ops_for_gen(QDMA_IP_GEN_QDMA5)->init_global_csr ==
	      qdma_init_global_csr_v5, "QDMA 5.0 uses v5 programming");
	CHECK(qdma_hw_ops_for_gen(QDMA_IP_GEN_QDMA4)->init_global_csr ==
	      qdma_init_global_csr_v4, "QDMA 4.0 uses legacy programming");
	CHECK(qdma_hw_ops_for_gen(QDMA_IP_GEN_QDMA3)->init_global_csr ==
	      qdma_init_global_csr_v4, "QDMA 3.x keeps legacy programming");
	CHECK(qdma_hw_ops_for_gen(QDMA_IP_GEN_UNKNOWN)->init_global_csr ==
	      qdma_init_global_csr_v4, "unknown keeps legacy programming");
}

/* ------------------------------------------------------------------------ */
/* Global CSR programming                                                      */

static void test_v4_csr_sequence_unchanged(void)
{
	static struct fake_write ref[FAKE_LOG_MAX];
	int ref_len, i, n;

	new_qdev(0x00100000, 0, 0);
	legacy_onic_qdma_init_csr(&test_qdev);
	ref_len = fake_log_len;
	memcpy(ref, fake_log, sizeof(ref[0]) * ref_len);

	new_qdev(0x00100000, 0, 0);
	qdma_init_global_csr_v4(&test_qdev);

	CHECK(fake_log_len == ref_len, "v4 write count %d, legacy %d",
	      fake_log_len, ref_len);
	n = fake_log_len < ref_len ? fake_log_len : ref_len;
	for (i = 0; i < n; ++i)
		CHECK(fake_log[i].offset == ref[i].offset &&
		      fake_log[i].val == ref[i].val,
		      "v4 write %d: 0x%05x=0x%08x, legacy 0x%05x=0x%08x", i,
		      fake_log[i].offset, fake_log[i].val, ref[i].offset,
		      ref[i].val);
	/* 16 ring sizes, 16 buffer sizes, tick, 16 timers, 16 counters, 4 CSRs */
	CHECK_EQ(ref_len, 16 + 16 + 1 + 16 + 16 + 4, "legacy write count");
}

/*
 * Document the effect of the legacy encoding on the 4.0/5.0 field layout.
 * These are the values the unchanged QDMA 4.0 path writes today.
 */
static void test_v4_legacy_encoding_decoded(void)
{
	u32 v;

	new_qdev(0x00100000, 0, 0);
	qdma_init_global_csr_v4(&test_qdev);

	CHECK(fake_log_find_last(0xB08, &v), "0xB08 written");
	/* QDMA 3.x fields packed into evt_pfch_fl_th[31:16] / pfch_fl_th[15:0] */
	CHECK_EQ(v, 0x7d001100, "legacy C2H_PFCH_CFG raw");
	CHECK_EQ(v & 0xffff, 4352, "pfch_fl_th as seen by 4.0/5.0");
	CHECK_EQ(v >> 16, 32000, "evt_pfch_fl_th as seen by 4.0/5.0");

	CHECK(fake_log_find_last(0xE24, &v), "0xE24 written");
	CHECK_EQ(v, 0x00c14000, "legacy H2C_REQ_THROT raw");
	CHECK_EQ(v >> 31, 0, "4.0/5.0 req_throt_en_req (bit 31) off");
	CHECK_EQ((v >> 18) & 1, 0, "4.0/5.0 req_throt_en_data (bit 18) off");

	CHECK(fake_log_find_last(0x250, &v), "0x250 written");
	CHECK_EQ(v, 0x30, "legacy GLBL_DSC_CFG: max fetch 6, wb interval 4");
	CHECK_EQ((v >> 10) & 0xfffff, 0, "5.0 uodsc limits left at 0");

	CHECK_EQ(fake_log_count(0xA80) + fake_log_count(0xA84), 0,
		 "legacy never programs C2H_PFCH_CFG_1/2");
}

static void test_v5_csr_values(void)
{
	static const struct {
		u32 offset;
		u32 val;
		const char *what;
	} exp[] = {
		{ 0x0B50, 0x00064014, "C2H_WRB_COAL_CFG tick_cnt 5, tick_val 25, max_buf_sz 128&0x3f" },
		{ 0x0250, 0x00502015, "GLBL_DSC_CFG c2h_uodsc 5, h2c_uodsc 8, max fetch 2, wb 128" },
		{ 0x004C, 0x00010009, "CFG_BLK_MISC_CTL num_tag 256, rq_metering 9" },
		{ 0x0B08, 0x01000100, "C2H_PFCH_CFG evt fl 256, fl 256" },
		{ 0x0A80, 0x007c007c, "C2H_PFCH_CFG_1 qcnt = evt = 128 - 4" },
		{ 0x0A84, 0x804003c8, "C2H_PFCH_CFG_2 fence, ll 1024, var 15, num 8" },
		{ 0x147C, 0x00008000, "PFCH_CFG_3 var_desc_fl_free_cnt_th 256" },
		{ 0x1484, 0x00800320, "PFCH_CFG_4 glb tick 64, evt tick 400" },
		{ 0x1400, 0x00004010, "C2H_CRDT_COAL_CFG_1 pld 16, timer 16" },
		{ 0x1404, 0x00780060, "C2H_CRDT_COAL_CFG_2 fifo 128-8, cnt 96" },
		{ 0x015C, 0x06045000, "RRQ_PCIE_THROT req 192, dat_en, dat 20480" },
		{ 0x0160, 0x00000000, "RRQ_AXIMM_THROT off" },
		{ 0x0158, 0x86045000, "RRQ_BRG_THROT req_en, req 192, dat_en, dat 20480" },
		{ 0x0E24, 0x86046000, "H2C_REQ_THROT_PCIE en_req, 192, en_data, 24576" },
		{ 0x0E2C, 0x82044000, "H2C_REQ_THROT_AXIMM en_req, 64, en_data, 16384" },
		{ 0x12EC, 0x00015000, "H2C_MM_DATA_THROTTLE en, 20480" },
	};
	size_t i;
	u32 v;
	int ring_writes = 0;

	new_qdev(0x00200000, 0, 0);
	qdma_init_global_csr_v5(&test_qdev);

	for (i = 0; i < sizeof(exp) / sizeof(exp[0]); ++i) {
		CHECK(fake_log_count(exp[i].offset) == 1,
		      "%s: 0x%04x written %d times", exp[i].what,
		      exp[i].offset, fake_log_count(exp[i].offset));
		if (fake_log_find_last(exp[i].offset, &v))
			CHECK_EQ(v, exp[i].val, exp[i].what);
	}

	/* shared OpenNIC tables: same values as the legacy path */
	for (i = 0; i < 16; ++i) {
		ring_writes += fake_log_count(0x204 + 4 * i);
		ring_writes += fake_log_count(0xAB0 + 4 * i);
		ring_writes += fake_log_count(0xA00 + 4 * i);
		ring_writes += fake_log_count(0xA40 + 4 * i);
	}
	CHECK_EQ(ring_writes, 64, "v5 writes all 4 x 16 table entries");
	CHECK(fake_log_find_last(0x204, &v) && v == 4096, "ring size 0 = 4096");
	CHECK(fake_log_find_last(0xAB0 + 4 * 8, &v) && v == 4096,
	      "C2H buffer size 8 = 4096");
	CHECK(fake_log_find_last(0xB0C, &v) && v == 25, "C2H timer tick 25");

	CHECK_EQ(fake_log_len, 64 + 1 + sizeof(exp) / sizeof(exp[0]),
		 "v5 writes nothing else");
}

static void test_v5_small_prefetch_cache(void)
{
	u32 v;

	new_qdev(0x00200000, 0, 0);
	fake_mmio_preset(QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH, 0x001f0010); /* 16 */
	qdma_init_global_csr_v5(&test_qdev);
	CHECK(fake_log_find_last(0xA80, &v) && v == 0x000c000c,
	      "PFCH_CFG_1 for 16 tags: got 0x%08x", v);
	CHECK(fake_log_find_last(0x1404, &v) && v == 0x00080060,
	      "CRDT_COAL_CFG_2 for 16 tags: got 0x%08x", v);

	new_qdev(0x00200000, 0, 0);
	fake_mmio_preset(QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH, 0); /* bogus */
	qdma_init_global_csr_v5(&test_qdev);
	CHECK(fake_log_find_last(0xA80, &v) && v == 0x00040004,
	      "PFCH_CFG_1 clamps a zero depth: got 0x%08x", v);
	CHECK(fake_log_find_last(0x1404, &v) && (v >> 16) == 0,
	      "CRDT fifo_th does not wrap: got 0x%08x", v);
}

/* ------------------------------------------------------------------------ */
/* Indirect context programming (same layout in QDMA 4.0 and 5.0)              */

#define IND_DATA	0x804
#define IND_MASK	0x824
#define IND_CMD		0x844

static void check_ctxt_write(const u32 *exp_data, u32 exp_cmd, const char *what)
{
	int i;
	u32 v;

	/* data registers, all 8 written, all mask bits set */
	for (i = 0; i < 8; ++i) {
		CHECK(fake_log_find_last(IND_DATA + 4 * i, &v) &&
		      v == exp_data[i], "%s: data[%d] = 0x%08x, expected 0x%08x",
		      what, i, v, exp_data[i]);
		CHECK(fake_log_find_last(IND_MASK + 4 * i, &v) &&
		      v == 0xffffffff, "%s: mask[%d]", what, i);
	}
	CHECK(fake_log_find_last(IND_CMD, &v) && v == exp_cmd,
	      "%s: cmd 0x%08x, expected 0x%08x", what, v, exp_cmd);
	/* command must be the last write */
	CHECK(fake_log[fake_log_len - 1].offset == IND_CMD,
	      "%s: command written last", what);
}

/* IND_CTXT_CMD: qid[19:7], op[6:5], sel[4:1], busy[0] */
#define CMD(sel, op, qid)	(((u32)(qid) << 7) | ((u32)(op) << 5) | ((u32)(sel) << 1))
#define OP_CLR 0
#define OP_WR  1
#define OP_INV 3
#define SEL_SW_C2H 0
#define SEL_SW_H2C 1
#define SEL_HW_H2C 3
#define SEL_CR_H2C 5
#define SEL_CMPL   6
#define SEL_PFCH   7
#define SEL_FMAP   12

static void test_sw_ctxt_h2c(void)
{
	struct qdma_sw_ctxt c;
	/*
	 * SW context, PG302 4.0/5.0: W0 pidx[15:0] irq_arm[16] fnc_id[28:17];
	 * W1 qen[0] fcrd_en[1] wbi_chk[2] wbi_intvl_en[3] at[4] fetch_max[8:5]
	 * rng_sz[15:12] dsc_sz[17:16] bypass[18] mm_chn[19] wbk_en[20]
	 * irq_en[21] port_id[24:22] ... is_mm[31]; W2/W3 dsc_base; W4 vec[10:0]
	 * int_aggr[11].
	 */
	const u32 exp[8] = {
		0x00020000,			/* fnc_id 1 */
		0x0011000d,			/* qen, wbi_chk, wbi_intvl, dsc_sz 1, wbk */
		0x56789000, 0x00001234,		/* desc base 0x123456789000 */
		0x00000003,			/* vec 3 */
		0, 0, 0,
	};

	new_qdev(0x00200000, 1, 64);
	memset(&c, 0, sizeof(c));
	/* as onic_qdma_init_tx_queue() */
	c.func_id = 1;
	c.qen = 1;
	c.wbk_en = 1;
	c.desc_sz = 1;
	c.wbi_chk = 1;
	c.wbi_intvl_en = 1;
	c.rngsz_idx = 0;
	c.desc_base = 0x123456789000ULL;
	c.vec = 3;
	CHECK(qdma_write_sw_ctxt(&test_qdev, 5, QDMA_H2C, &c) == 0, "sw write");
	check_ctxt_write(exp, CMD(SEL_SW_H2C, OP_WR, 64 + 5), "H2C sw ctxt");
}

static void test_sw_ctxt_c2h(void)
{
	struct qdma_sw_ctxt c;
	const u32 exp[8] = {
		0x00000000,
		0x00108003,	/* qen, fcrd_en, rng_sz 8, wbk_en (desc_sz 0) */
		0xcafe0000, 0x00000000,
		0, 0, 0, 0,
	};

	new_qdev(0x00200000, 0, 0);
	memset(&c, 0, sizeof(c));
	/* as onic_qdma_init_rx_queue() */
	c.qen = 1;
	c.wbk_en = 1;
	c.fcrd_en = 1;
	c.rngsz_idx = 8;
	c.desc_base = 0xcafe0000ULL;
	CHECK(qdma_write_sw_ctxt(&test_qdev, 7, QDMA_C2H, &c) == 0, "sw write");
	check_ctxt_write(exp, CMD(SEL_SW_C2H, OP_WR, 7), "C2H sw ctxt");
}

static void test_pfch_ctxt(void)
{
	struct qdma_pfch_ctxt c;
	/* W0 bypass[0] buf_sz_idx[4:1] port_id[7:5] ... pfch_en[27]; W1 valid[13] */
	const u32 exp[8] = { 0x08000010, 0x00002000, 0, 0, 0, 0, 0, 0 };

	new_qdev(0x00200000, 0, 0);
	memset(&c, 0, sizeof(c));
	c.bufsz_idx = 8;
	c.pfch_en = 1;
	c.valid = 1;
	CHECK(qdma_write_pfch_ctxt(&test_qdev, 2, &c) == 0, "pfch write");
	check_ctxt_write(exp, CMD(SEL_PFCH, OP_WR, 2), "prefetch ctxt");
}

static void test_cmpl_ctxt(void)
{
	struct qdma_cmpl_ctxt c;
	/*
	 * Completion context, PG302 4.0/5.0 (AMD eqdma_soft_reg.h CMPL_CTXT_DATA):
	 * W0 en_stat_desc[0] en_int[1] trig_mode[4:2] fnc_id[16:5]
	 *    cnter_ix[20:17] timer_ix[24:21] int_st[26:25] color[27] qsize_ix[31:28]
	 * W1 baddr4_high_l[31:0] = baddr[37:6]
	 * W2 baddr4_high_h[25:0] = baddr[63:38], desc_size[27:26], pidx_l[31:28]
	 * W3 pidx_h[11:0] cidx[27:12] valid[28] err[30:29] user_trig_pend[31]
	 * W4 timer_running[0] full_upd[1] ovf_chk_dis[2] at[3] vec[14:4] int_aggr[15]
	 * W5 baddr4_low[17:14] = baddr[5:2] (left 0: ring is page aligned)
	 */
	const u32 exp[8] = {
		0x88000037,	/* stat, int, trig 5, fnc 1, color, qsize 8 */
		0x8d159e00,	/* 0x412345678000 >> 6 */
		0x00000104,	/* 0x412345678000 >> 38 */
		0x10000000,	/* valid */
		0x00000020,	/* vec 2 */
		0, 0, 0,
	};

	new_qdev(0x00200000, 1, 64);
	memset(&c, 0, sizeof(c));
	/* as onic_qdma_init_rx_queue() */
	c.stat_en = 1;
	c.intr_en = 1;
	c.trig_mode = 5;
	c.func_id = 1;
	c.color = 1;
	c.rngsz_idx = 8;
	c.baddr = 0x412345678000ULL;
	c.desc_sz = 0;
	c.valid = 1;
	c.vec = 2;
	CHECK(qdma_write_cmpl_ctxt(&test_qdev, 0, &c) == 0, "cmpl write");
	check_ctxt_write(exp, CMD(SEL_CMPL, OP_WR, 64), "completion ctxt");
}

/*
 * The legacy encoding drops baddr[11:6]; QDMA 4.0/5.0 accept 64-byte aligned
 * rings.  OpenNIC allocates rings with dma_alloc_coherent() (page aligned),
 * so this is a documented constraint, not a defect on either generation.
 */
static void test_cmpl_ctxt_requires_4k_alignment(void)
{
	struct qdma_cmpl_ctxt c;
	u32 v;

	new_qdev(0x00200000, 0, 0);
	memset(&c, 0, sizeof(c));
	c.baddr = 0x1000ULL | 0x40;
	qdma_write_cmpl_ctxt(&test_qdev, 0, &c);
	CHECK(fake_log_find_last(IND_DATA + 4, &v) && v == 0x40,
	      "baddr bit 6 is dropped by the legacy encoding (got 0x%08x)", v);
}

static void test_fmap_ctxt(void)
{
	struct qdma_fmap_ctxt c;
	/* FMAP: W0 qid_base[10:0], W1 qid_max[11:0]; cmd qid = function id */
	const u32 exp[8] = { 64, 2, 0, 0, 0, 0, 0, 0 };

	new_qdev(0x00200000, 1, 0);
	memset(&c, 0, sizeof(c));
	c.qbase = 64;
	c.qmax = 2;
	CHECK(qdma_write_fmap_ctxt(&test_qdev, &c) == 0, "fmap write");
	check_ctxt_write(exp, CMD(SEL_FMAP, OP_WR, 1), "fmap ctxt");
	CHECK_EQ(test_qdev.q_base, 64, "q_base updated");
	CHECK_EQ(test_qdev.num_queues, 2, "num_queues updated");
}

static void test_clear_and_invalidate_cmds(void)
{
	u32 v;

	new_qdev(0x00200000, 0, 64);
	qdma_clear_hw_ctxt(&test_qdev, 3, QDMA_H2C);
	CHECK(fake_log_find_last(IND_CMD, &v) && v == CMD(SEL_HW_H2C, OP_CLR, 67),
	      "clear hw H2C cmd 0x%08x", v);
	CHECK_EQ(fake_log_len, 1, "clear writes only the command");

	qdma_invalidate_cr_ctxt(&test_qdev, 3, QDMA_H2C);
	CHECK(fake_log_find_last(IND_CMD, &v) && v == CMD(SEL_CR_H2C, OP_INV, 67),
	      "invalidate credit H2C cmd 0x%08x", v);

	/* highest queue id OpenNIC can use with 2048 queues still fits */
	new_qdev(0x00200000, 0, 0);
	qdma_invalidate_sw_ctxt(&test_qdev, 2047, QDMA_C2H);
	CHECK(fake_log_find_last(IND_CMD, &v) &&
	      v == CMD(SEL_SW_C2H, OP_INV, 2047), "qid 2047 cmd 0x%08x", v);
}

/* ------------------------------------------------------------------------ */
/* Queue pointer (DMAP) and interrupt register encodings                       */

static void test_dmap_and_interrupt_encodings(void)
{
	u32 v;

	/* PG302 v5.0 QDMA_DMAP_SEL_H2C_DSC_PIDX: irq_arm[16] h2c_pidx[15:0] */
	v = FIELD_SET(QDMA_DMAP_SEL_DESC_PIDX_MASK, 0xbeef) |
	    FIELD_SET(QDMA_DMAP_SEL_DESC_IRQ_ARM_MASK, 1);
	CHECK_EQ(v, 0x1beef, "DMAP desc PIDX");

	/*
	 * PG302 v5.0 QDMA_DMAP_SEL_CMPT_CIDX: irq_en_wrb[28] en_sts_desc_wrb[27]
	 * trigger_mode[26:24] c2h_timer_cnt_index[23:20]
	 * c2h_count_threshhold[19:16] wrb_cidx[15:0]
	 */
	v = FIELD_SET(QDMA_DMAP_SEL_CMPL_CIDX_MASK, 0x1234) |
	    FIELD_SET(QDMA_DMAP_SEL_CMPL_COUNTER_IDX_MASK, 0xa) |
	    FIELD_SET(QDMA_DMAP_SEL_CMPL_TIMER_IDX_MASK, 0xb) |
	    FIELD_SET(QDMA_DMAP_SEL_CMPL_TRIG_MODE_MASK, 5) |
	    FIELD_SET(QDMA_DMAP_SEL_CMPL_STAT_EN_MASK, 1) |
	    FIELD_SET(QDMA_DMAP_SEL_CMPL_IRQ_ARM_MASK, 1);
	CHECK_EQ(v, 0x1dba1234, "DMAP completion CIDX");

	/*
	 * Queue space: [2048] arrays at 0x18000..0x1800C (4.0 CSV), address =
	 * base + qid * 16 (PG302 v5.0 QDMA_TRQ_SEL_QUEUE_PF formula).
	 */
	CHECK_EQ(QDMA_OFFSET_DMAP_SEL_INTR_CIDX, 0x18000, "INT CIDX base");
	CHECK_EQ(QDMA_OFFSET_DMAP_SEL_H2C_DESC_PIDX, 0x18004, "H2C PIDX base");
	CHECK_EQ(QDMA_OFFSET_DMAP_SEL_C2H_DESC_PIDX, 0x18008, "C2H PIDX base");
	CHECK_EQ(QDMA_OFFSET_DMAP_SEL_CMPL_CIDX, 0x1800C, "CMPT CIDX base");
	CHECK_EQ(QDMA_OFFSET_DMAP_SEL_CMPL_CIDX + 2047 * 16, 0x1FFFC,
		 "queue 2047 CMPT CIDX stays below the 0x20000 region");

	/*
	 * QDMA_GLBL_ERR_INT (0xB04), 4.0 and 5.0 CSV: Func[11:0] Vec[22:12]
	 * En_coal[23] Err_int_arm[24].  OpenNIC uses an 8 bit function field,
	 * which is a subset (PF numbers are 0..3).
	 */
	v = FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, 3) |
	    FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, 0x7ff) |
	    FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 1);
	CHECK_EQ(v, 0x017ff003, "GLBL_ERR_INT");
	CHECK_EQ(QDMA_OFFSET_GLBL_ERR_INT, 0xB04, "GLBL_ERR_INT offset");
}

/* ------------------------------------------------------------------------ */
/* Descriptor and writeback formats (PG302, unchanged between 4.0 and 5.0)     */

static void test_descriptor_formats(void)
{
	u8 buf[16];
	u64 dw[2];
	struct qdma_h2c_st_desc h2c = {
		.metadata = 0xdeadbeef, .len = 1514, .src_addr = 0x1122334455667788ULL,
	};
	struct qdma_c2h_st_desc c2h = { .dst_addr = 0x0123456789abcdefULL };
	struct qdma_c2h_cmpl cmpl;
	struct qdma_c2h_cmpl_stat st;
	struct qdma_wb_stat wb;

	qdma_pack_h2c_st_desc(buf, &h2c);
	memcpy(dw, buf, 16);
	CHECK_EQ(dw[0], 0x000005eadeadbeefULL, "H2C ST desc dw0 len|metadata");
	CHECK_EQ(dw[1], 0x1122334455667788ULL, "H2C ST desc dw1 src");

	qdma_pack_c2h_st_desc(buf, &c2h);
	memcpy(dw, buf, 8);
	CHECK_EQ(dw[0], 0x0123456789abcdefULL, "C2H ST desc dst");

	dw[0] = 0xabcd05ea00000006ULL;	/* pkt_id, len 1514, err, color */
	qdma_unpack_c2h_cmpl(&cmpl, (u8 *)dw);
	CHECK_EQ(cmpl.color, 1, "cmpl color");
	CHECK_EQ(cmpl.err, 1, "cmpl err");
	CHECK_EQ(cmpl.pkt_len, 1514, "cmpl len");
	CHECK_EQ(cmpl.pkt_id, 0xabcd, "cmpl pkt id");

	dw[0] = 0x0000000500170042ULL;	/* intr_state 2, color 1, cidx, pidx */
	qdma_unpack_c2h_cmpl_stat(&st, (u8 *)dw);
	CHECK_EQ(st.pidx, 0x42, "cmpl stat pidx");
	CHECK_EQ(st.cidx, 0x17, "cmpl stat cidx");
	CHECK_EQ(st.color, 1, "cmpl stat color");
	CHECK_EQ(st.intr_state, 2, "cmpl stat intr state");

	dw[0] = 0x0000abcd00001234ULL;
	qdma_unpack_wb_stat(&wb, (u8 *)dw);
	CHECK_EQ(wb.pidx, 0x1234, "wb pidx");
	CHECK_EQ(wb.cidx, 0x0000, "wb cidx");
}

int main(void)
{
	test_version_decode();
	test_version_detect_reads_registers();
	test_ops_selection();
	test_v4_csr_sequence_unchanged();
	test_v4_legacy_encoding_decoded();
	test_v5_csr_values();
	test_v5_small_prefetch_cache();
	test_sw_ctxt_h2c();
	test_sw_ctxt_c2h();
	test_pfch_ctxt();
	test_cmpl_ctxt();
	test_cmpl_ctxt_requires_4k_alignment();
	test_fmap_ctxt();
	test_clear_and_invalidate_cmds();
	test_dmap_and_interrupt_encodings();
	test_descriptor_formats();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
