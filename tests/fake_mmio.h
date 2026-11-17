#ifndef __TEST_FAKE_MMIO_H__
#define __TEST_FAKE_MMIO_H__

#include "kshim.h"

#define FAKE_LOG_MAX	4096

struct fake_write {
	u32 offset;
	u32 val;
};

extern struct fake_write fake_log[FAKE_LOG_MAX];
extern int fake_log_len;
extern int fake_read_count;

void fake_mmio_reset(void);
void fake_mmio_preset(u32 offset, u32 val);
u32 fake_mmio_peek(u32 offset);
int fake_log_find_last(u32 offset, u32 *val);
int fake_log_count(u32 offset);

#endif
