/*
 * Fake QDMA BAR-0 and OpenNIC shell BAR-2 for unit tests.
 *
 * BAR-0 is a flat register file covering offsets 0x0 to 0x3FFFF.  Every
 * write is appended to a log so tests can compare complete register write
 * sequences.  Reads return the last written (or preset) value, except for the
 * indirect context command register, whose busy bit is clear unless a test
 * sets fake_ind_busy_polls.
 *
 * BAR-2 (shell) is a flat 4 MiB register file without a log.
 */
#include "kshim.h"
#include "fake_mmio.h"

#define FAKE_BAR_WORDS		(0x40000 / 4)
#define FAKE_SHELL_WORDS	(0x400000 / 4)
#define QDMA_IND_CTXT_CMD	0x844

static u32 regs[FAKE_BAR_WORDS];
static u32 shell_regs[FAKE_SHELL_WORDS];
struct fake_write fake_log[FAKE_LOG_MAX];
int fake_log_len;
int fake_read_count;
int fake_ind_busy_polls;

void fake_mmio_reset(void)
{
	memset(regs, 0, sizeof(regs));
	memset(shell_regs, 0, sizeof(shell_regs));
	fake_log_len = 0;
	fake_read_count = 0;
	fake_ind_busy_polls = 0;
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
	if (offset == QDMA_IND_CTXT_CMD) {
		u32 busy = 0;

		if (fake_ind_busy_polls < 0) {
			busy = 1;		/* stuck: never completes */
		} else if (fake_ind_busy_polls > 0) {
			fake_ind_busy_polls--;
			busy = 1;
		}
		return (regs[offset / 4] & ~1u) | busy;
	}
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

int fake_mmio_save(const char *path)
{
	FILE *f = fopen(path, "wb");
	size_t n;

	if (!f)
		return -1;
	n = fwrite(regs, sizeof(regs), 1, f);
	if (fclose(f) != 0 || n != 1)
		return -1;
	return 0;
}

u32 fake_shell_read(u32 offset)
{
	if (offset % 4 || offset / 4 >= FAKE_SHELL_WORDS) {
		fprintf(stderr, "fake_mmio: bad shell read offset 0x%x\n", offset);
		abort();
	}
	return shell_regs[offset / 4];
}

void fake_shell_write(u32 offset, u32 val)
{
	if (offset % 4 || offset / 4 >= FAKE_SHELL_WORDS) {
		fprintf(stderr, "fake_mmio: bad shell write offset 0x%x\n", offset);
		abort();
	}
	shell_regs[offset / 4] = val;
}

void fake_shell_preset(u32 offset, u32 val)
{
	shell_regs[offset / 4] = val;
}

u32 fake_shell_peek(u32 offset)
{
	return shell_regs[offset / 4];
}
