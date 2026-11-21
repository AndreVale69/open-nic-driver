/*
 * Hardware-free tests for onic_hardware.c: probe-time hardware setup and
 * teardown, and the QDMA error interrupt handling.
 *
 * The real onic_hardware.c and qdma_access/ are compiled against drvshim.h,
 * with the QDMA BAR-0 and the shell BAR-2 backed by fake_mmio.c.  Run the
 * ASan build too: it turns a NULL QDMA handle into a crash and reports a
 * leaked QDMA device.
 */
#include "drvshim.h"
#include "fake_mmio.h"
#include "onic.h"
#include "onic_common.h"
#include "onic_hardware.h"
#include "onic_register.h"
#include "qdma_register.h"

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

#define CONFIG_BLOCK_ID			0x1fd30001u
#define MISC_CAP_QDMA5			0x00200000u	/* soft IP, ip_version 2 */
#define IND_CTXT_CMD			0x844
#define CTXT_SEL_FMAP			12

static struct pci_dev pdev;
static struct net_device ndev;
static struct onic_private priv;

static void setup(unsigned int devfn)
{
	shim_reset();
	fake_mmio_reset();
	jiffies = 0;
	fake_mmio_preset(QDMA_OFFSET_CONFIG_BLOCK_ID, CONFIG_BLOCK_ID);
	fake_mmio_preset(QDMA_OFFSET_GLBL2_MISC_CAP, MISC_CAP_QDMA5);
	fake_mmio_preset(QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH, 0x10);
	fake_mmio_preset(QDMA_OFFSET_C2H_CMPL_COAL_BUF_DEPTH, 0x10);

	memset(&priv, 0, sizeof(priv));
	memset(&ndev, 0, sizeof(ndev));
	pdev.devfn = devfn;
	ndev.priv = &priv;
	priv.pdev = &pdev;
	priv.netdev = &ndev;
	priv.num_tx_queues = 8;
	priv.num_rx_queues = 8;
	if (PCI_FUNC(devfn) == 0)
		set_bit(ONIC_FLAG_MASTER_PF, priv.flags);
}

static void check_unwound(const char *what)
{
	CHECK(priv.hw.qdma == 0, "%s: no QDMA handle left", what);
	CHECK(priv.hw.addr == NULL, "%s: shell BAR unmapped", what);
	CHECK_EQ(shim.iounmaps, shim.iomaps, what);
}

static void test_init_and_clear(void)
{
	u32 cmd;

	setup(0);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	CHECK(priv.hw.qdma != 0, "QDMA handle set");
	CHECK(fake_log_find_last(IND_CTXT_CMD, &cmd) &&
	      ((cmd >> 1) & 0xf) == CTXT_SEL_FMAP, "function map programmed");
	CHECK_EQ(fake_shell_peek(QDMA_FUNC_OFFSET_QCONF(0)),
		 FIELD_SET(QDMA_FUNC_QCONF_QBASE_MASK, 0) |
		 FIELD_SET(QDMA_FUNC_QCONF_NUMQ_MASK, 8), "shell QCONF");
	CHECK_EQ(priv.hw.num_cmacs, 0, "no CMAC in the fake shell");

	onic_clear_hardware(&priv);
	check_unwound("clear");
}

static void test_fmap_timeout_unwinds(void)
{
	setup(0);
	fake_ind_busy_polls = -1;	/* indirect context command never completes */
	CHECK_EQ(onic_init_hardware(&priv), -EBUSY, "init fails");
	check_unwound("FMAP timeout");
	CHECK(strstr(shim.log_text, "function map") != NULL, "reported");

	/* the probe does not call onic_clear_hardware() then, but it must cope */
	onic_clear_hardware(&priv);
	check_unwound("clear after failed init");
}

static void test_fmap_slow_but_completes(void)
{
	setup(1);
	fake_ind_busy_polls = 3;	/* a few busy polls are normal */
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	onic_clear_hardware(&priv);
	check_unwound("slow FMAP");
}

static void test_bad_block_id_unwinds(void)
{
	setup(0);
	fake_mmio_preset(QDMA_OFFSET_CONFIG_BLOCK_ID, 0xffffffff);
	CHECK_EQ(onic_init_hardware(&priv), -ENODEV, "not a QDMA BAR");
	check_unwound("bad block id");
}

static void test_clear_on_empty_hardware(void)
{
	setup(0);
	onic_clear_hardware(&priv);	/* must not touch a NULL handle or BAR */
	CHECK_EQ(shim.iounmaps, 0, "nothing to unmap");
}

static void test_cmac_reset_timeout_is_bounded(void)
{
	setup(0);
	fake_shell_preset(CMAC_OFFSET_CORE_VERSION(0), ONIC_CMAC_CORE_VERSION);
	/* SYSCFG_OFFSET_SHELL_STATUS stays 0: the CMAC reset never completes */
	CHECK_EQ(onic_init_hardware(&priv), 0, "probe continues");
	CHECK_EQ(priv.hw.num_cmacs, 1, "CMAC found");
	CHECK(jiffies >= 1000 && jiffies < 1100, "gave up after ~1 s (%lu ms)",
	      jiffies);
	CHECK(strstr(shim.log_text, "shell reset not done") != NULL, "warned");
	CHECK_EQ(fake_shell_peek(CMAC_OFFSET_CONF_RX_1(0)), 0, "port not enabled");
	onic_clear_hardware(&priv);
	check_unwound("CMAC timeout");
}

static void test_cmac_reset_completes(void)
{
	setup(0);
	fake_shell_preset(CMAC_OFFSET_CORE_VERSION(0), ONIC_CMAC_CORE_VERSION);
	fake_shell_preset(SYSCFG_OFFSET_SHELL_STATUS, 0x10);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	CHECK_EQ(priv.hw.num_cmacs, 1, "CMAC found");
	CHECK_EQ(shim.log_warn, 0, "no warning");
	CHECK_EQ(fake_shell_peek(CMAC_OFFSET_CONF_RX_1(0)), 1, "RX enabled");
	CHECK(jiffies < 10, "no waiting");
	onic_clear_hardware(&priv);
}

static void test_error_handler_decodes_clears_rearms(void)
{
	u32 v, glbl, err_int = FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, 9);

	setup(0);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");

	/* state after the QDMA fired the error interrupt: disarmed */
	fake_mmio_preset(QDMA_OFFSET_GLBL_ERR_INT, err_int);
	fake_mmio_preset(QDMA_OFFSET_GLBL_ERR_STAT, QDMA_GLBL_ERR_ST_C2H_MASK);
	fake_mmio_preset(QDMA_OFFSET_C2H_ERR_STAT,
			 QDMA_C2H_ERR_CMPL_QFULL_ERR_MASK |
			 QDMA_C2H_ERR_CMPL_CIDX_ERR_MASK);
	fake_mmio_preset(QDMA_OFFSET_C2H_FIRST_ERR_QID, 0x000d0003);
	fake_log_len = 0;

	glbl = onic_qdma_handle_errors(priv.hw.qdma);
	CHECK_EQ(glbl, QDMA_GLBL_ERR_ST_C2H_MASK, "global status returned");
	CHECK(strstr(shim.log_text, "err_c2h_st") != NULL, "global bit named");
	CHECK(strstr(shim.log_text, "wrb_qfull_err") != NULL, "wrb_qfull_err named");
	CHECK(strstr(shim.log_text, "wrb_cidx_err") != NULL, "wrb_cidx_err named");
	CHECK(strstr(shim.log_text, "0x000d0003") != NULL, "first error QID logged");
	CHECK(fake_log_find_last(QDMA_OFFSET_C2H_ERR_STAT, &v) &&
	      v == (QDMA_C2H_ERR_CMPL_QFULL_ERR_MASK |
		    QDMA_C2H_ERR_CMPL_CIDX_ERR_MASK), "leaf status cleared (W1C)");
	CHECK(fake_log_find_last(QDMA_OFFSET_GLBL_ERR_STAT, &v) &&
	      v == QDMA_GLBL_ERR_ST_C2H_MASK, "global status cleared (W1C)");
	CHECK_EQ(fake_log_count(QDMA_OFFSET_C2H_FATAL_ERR_STAT), 0,
		 "clean leaf left alone");
	CHECK(fake_log_find_last(QDMA_OFFSET_GLBL_ERR_INT, &v) &&
	      v == (err_int | QDMA_GLBL_ERR_ARM_MASK), "re-armed, vector kept");
	CHECK(fake_log[fake_log_len - 1].offset == QDMA_OFFSET_GLBL_ERR_INT,
	      "arm is the last write");
	onic_clear_hardware(&priv);
}

static void test_error_init_enables_h2c_at_bit16(void)
{
	u32 mask, v;

	setup(0);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	onic_qdma_init_error_interrupt(priv.hw.qdma, 9);
	mask = fake_mmio_peek(QDMA_OFFSET_GLBL_ERR_MASK);
	/* err_ram_sbe, err_ram_dbe, err_dsc, err_trq, err_c2h_st, err_h2c_st */
	CHECK_EQ(mask, BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(8) | BIT(16),
		 "QDMA_GLBL_ERR_MASK (CSV4/CSV5 bit positions)");
	CHECK(!(mask & BIT(11)), "bit 11 is ind_ctxt_cmd_err on 4.0/5.0");
	CHECK(fake_log_find_last(QDMA_OFFSET_GLBL_ERR_INT, &v) &&
	      v == (FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, 9) | QDMA_GLBL_ERR_ARM_MASK),
	      "armed on vector 9");
	onic_clear_hardware(&priv);
}

static void test_error_handler_h2c(void)
{
	u32 v, h2c = BIT(4) | BIT(0);	/* sbe, zero_len_dsc_err */

	setup(0);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	fake_mmio_preset(QDMA_OFFSET_GLBL_ERR_INT, FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, 9));
	fake_mmio_preset(QDMA_OFFSET_GLBL_ERR_STAT, BIT(16));
	fake_mmio_preset(QDMA_OFFSET_H2C_ERR_STAT, h2c);
	fake_mmio_preset(QDMA_OFFSET_H2C_FIRST_ERR_QID, 0x00010005);
	fake_log_len = 0;

	CHECK_EQ(onic_qdma_handle_errors(priv.hw.qdma), BIT(16), "H2C status");
	CHECK(strstr(shim.log_text, "err_h2c_st") != NULL, "global bit named");
	CHECK(strstr(shim.log_text, " sbe") != NULL, "sbe named (bit 4)");
	CHECK(strstr(shim.log_text, "zero_len_dsc_err") != NULL, "bit 0 named");
	CHECK(strstr(shim.log_text, "0x00010005") != NULL, "first error QID logged");
	CHECK(fake_log_find_last(QDMA_OFFSET_H2C_ERR_STAT, &v) && v == h2c,
	      "H2C leaf status read and cleared (W1C)");
	CHECK(fake_log_find_last(QDMA_OFFSET_GLBL_ERR_STAT, &v) && v == BIT(16),
	      "global status cleared");
	onic_clear_hardware(&priv);
}

static void test_error_names_follow_v4_v5_maps(void)
{
	setup(0);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	fake_mmio_preset(QDMA_OFFSET_GLBL_ERR_STAT, BIT(2) | BIT(1));
	fake_mmio_preset(QDMA_OFFSET_GLBL_DSC_ERR_STAT, BIT(1) | BIT(0));
	fake_mmio_preset(QDMA_OFFSET_RAM_DBE_STAT, BIT(12));
	fake_log_len = 0;

	onic_qdma_handle_errors(priv.hw.qdma);
	/* the QDMA 3.x table calls DSC bit 1 "Unsupported request..." */
	CHECK(strstr(shim.log_text, "0x00000003: poison reserved:0x1") != NULL,
	      "DSC bit 1 is poison, bit 0 reserved");
	CHECK(strstr(shim.log_text, "0x00001000: func_map") != NULL,
	      "RAM DBE uses the RAM SBE layout");
	onic_clear_hardware(&priv);
}

static void test_error_handler_spurious(void)
{
	u32 v;

	setup(0);
	CHECK_EQ(onic_init_hardware(&priv), 0, "init");
	fake_mmio_preset(QDMA_OFFSET_GLBL_ERR_INT, FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, 9));
	fake_log_len = 0;
	CHECK_EQ(onic_qdma_handle_errors(priv.hw.qdma), 0, "nothing pending");
	CHECK_EQ(fake_log_count(QDMA_OFFSET_GLBL_ERR_STAT), 0, "no status write");
	CHECK(fake_log_find_last(QDMA_OFFSET_GLBL_ERR_INT, &v) &&
	      (v & QDMA_GLBL_ERR_ARM_MASK), "still re-armed");
	onic_clear_hardware(&priv);
}

int main(void)
{
	test_init_and_clear();
	test_fmap_timeout_unwinds();
	test_fmap_slow_but_completes();
	test_bad_block_id_unwinds();
	test_clear_on_empty_hardware();
	test_cmac_reset_timeout_is_bounded();
	test_cmac_reset_completes();
	test_error_handler_decodes_clears_rearms();
	test_error_init_enables_h2c_at_bit16();
	test_error_handler_h2c();
	test_error_names_follow_v4_v5_maps();
	test_error_handler_spurious();

	printf("test_hw: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
