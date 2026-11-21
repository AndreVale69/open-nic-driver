/*
 * Write the QDMA BAR-0 image that the driver's global CSR programming leaves
 * behind, so that tools/qdma_reg_dump.py can be checked against the C code
 * (tests/test_reg_dump.py).
 *
 *   gen_bar_image PROG MISC_CAP PFCH_CACHE_DEPTH WRB_BUF_DEPTH OUT
 *
 * PROG is "auto" (generation from MISC_CAP, as the driver selects it), "v4" or
 * "v5" (as with qdma_ip_override=4/5).  The other numbers are the values the
 * capability registers 0x134, 0xBE0 and 0xBE4 report.
 */
#include "kshim.h"
#include "fake_mmio.h"
#include "qdma_register.h"
#include "qdma_csr.h"

int main(int argc, char **argv)
{
	struct pci_dev pdev = { 0 };
	struct qdma_dev qdev = { 0 };
	enum qdma_ip_gen gen;

	if (argc != 6) {
		fprintf(stderr, "usage: %s auto|v4|v5 MISC_CAP BE0 BE4 OUT\n",
			argv[0]);
		return 2;
	}

	fake_mmio_reset();
	fake_mmio_preset(QDMA_OFFSET_CONFIG_BLOCK_ID, 0x1fd30001u);
	fake_mmio_preset(QDMA_OFFSET_GLBL2_MISC_CAP, strtoul(argv[2], NULL, 0));
	fake_mmio_preset(QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH, strtoul(argv[3], NULL, 0));
	fake_mmio_preset(QDMA_OFFSET_C2H_CMPL_COAL_BUF_DEPTH,
			 strtoul(argv[4], NULL, 0));

	qdev.pdev = &pdev;
	qdev.addr = (void *)FAKE_BAR_BASE;
	if (qdma_detect_version(&qdev) < 0) {
		fprintf(stderr, "no QDMA magic\n");
		return 2;
	}

	if (!strcmp(argv[1], "v4"))
		gen = QDMA_IP_GEN_QDMA4;
	else if (!strcmp(argv[1], "v5"))
		gen = QDMA_IP_GEN_QDMA5;
	else
		gen = qdev.ver.gen;

	qdma_hw_ops_for_gen(gen)->init_global_csr(&qdev);

	if (fake_mmio_save(argv[5]) < 0) {
		perror(argv[5]);
		return 1;
	}
	return 0;
}
