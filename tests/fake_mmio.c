/*
 * Fake QDMA BAR-0 for unit tests.
 *
 * A flat register file covering BAR-0 offsets 0x0 to 0x3FFFF.  Every write is
 * appended to a log so tests can compare complete register write sequences.
 * Reads return the last written (or preset) value, except that the indirect
 * context command register never reports busy.
 */
#include "kshim.h"
#include "fake_mmio.h"

#define FAKE_BAR_WORDS		(0x40000 / 4)
#define QDMA_IND_CTXT_CMD	0x844

static u32 regs[FAKE_BAR_WORDS];
struct fake_write fake_log[FAKE_LOG_MAX];
int fake_log_len;
int fake_read_count;

void fake_mmio_reset(void)
{
	memset(regs, 0, sizeof(regs));
	fake_log_len = 0;
	fake_read_count = 0;
}

void fake_mmio_preset(u32 offset, u32 val)
{
	regs[offset / 4] = val;
}

u32 fake_mmio_peek(u32 offset)
{
	return regs[offset / 4];
}

u32 fake_mmio_read(u32 offset)
{
	if (offset % 4 || offset / 4 >= FAKE_BAR_WORDS) {
		fprintf(stderr, "fake_mmio: bad read offset 0x%x\n", offset);
		abort();
	}
	fake_read_count++;
	if (offset == QDMA_IND_CTXT_CMD)
		return regs[offset / 4] & ~1u;	/* busy bit always clear */
	return regs[offset / 4];
}

void fake_mmio_write(u32 offset, u32 val)
{
	if (offset % 4 || offset / 4 >= FAKE_BAR_WORDS) {
		fprintf(stderr, "fake_mmio: bad write offset 0x%x\n", offset);
		abort();
	}
	if (fake_log_len >= FAKE_LOG_MAX) {
		fprintf(stderr, "fake_mmio: write log overflow\n");
		abort();
	}
	fake_log[fake_log_len].offset = offset;
	fake_log[fake_log_len].val = val;
	fake_log_len++;
	regs[offset / 4] = val;
}

int fake_log_find_last(u32 offset, u32 *val)
{
	int i;

	for (i = fake_log_len - 1; i >= 0; --i) {
		if (fake_log[i].offset == offset) {
			*val = fake_log[i].val;
			return 1;
		}
	}
	return 0;
}

int fake_log_count(u32 offset)
{
	int i, n = 0;

	for (i = 0; i < fake_log_len; ++i)
		n += fake_log[i].offset == offset;
	return n;
}
