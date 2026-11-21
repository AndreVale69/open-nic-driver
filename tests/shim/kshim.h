/*
 * Minimal userspace stand-ins for the kernel APIs used by qdma_access/.
 *
 * Only what the QDMA access layer needs is provided.  Register accesses go
 * to the fake MMIO model in fake_mmio.c, which records every write.
 */
#ifndef __TEST_KSHIM_H__
#define __TEST_KSHIM_H__

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;	/* as in the kernel (int-ll64.h) */

#define __iomem
#define __percpu

#define BIT(n)			(1UL << (n))
#define GENMASK(h, l)		(((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define GENMASK_ULL(h, l)	(((~0ULL) << (l)) & (~0ULL >> (63 - (h))))

#define BUG_ON(c) do { if (c) { fprintf(stderr, "BUG_ON(%s) %s:%d\n", #c, __FILE__, __LINE__); abort(); } } while (0)

struct mutex { int locked; };
#define DEFINE_MUTEX(name)	struct mutex name = { 0 }
static inline void mutex_lock(struct mutex *m) { m->locked++; }
static inline void mutex_unlock(struct mutex *m) { m->locked--; }

static inline void udelay(unsigned long us) { (void)us; }

struct device { int unused; };
struct pci_dev { struct device dev; unsigned int devfn; };
#define PCI_FUNC(devfn)		((devfn) & 0x07)

/*
 * fake MMIO: QDMA BAR-0 addresses are FAKE_BAR_BASE + offset, OpenNIC shell
 * BAR-2 addresses are FAKE_BAR2_BASE + offset (fake_mmio.c)
 */
#define FAKE_BAR_BASE		((uintptr_t)0x10000000u)
#define FAKE_BAR2_BASE		((uintptr_t)0x20000000u)
u32 fake_mmio_read(u32 offset);
void fake_mmio_write(u32 offset, u32 val);
u32 fake_shell_read(u32 offset);
void fake_shell_write(u32 offset, u32 val);

static inline u32 ioread32(const void __iomem *addr)
{
	if ((uintptr_t)addr >= FAKE_BAR2_BASE)
		return fake_shell_read((u32)((uintptr_t)addr - FAKE_BAR2_BASE));
	return fake_mmio_read((u32)((uintptr_t)addr - FAKE_BAR_BASE));
}

static inline void iowrite32(u32 val, void __iomem *addr)
{
	if ((uintptr_t)addr >= FAKE_BAR2_BASE) {
		fake_shell_write((u32)((uintptr_t)addr - FAKE_BAR2_BASE), val);
		return;
	}
	fake_mmio_write((u32)((uintptr_t)addr - FAKE_BAR_BASE), val);
}

struct sk_buff;

#endif
