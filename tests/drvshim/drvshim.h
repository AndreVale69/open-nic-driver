/*
 * Userspace stand-ins for the kernel networking, PCI and memory APIs used by
 * onic_netdev.c and onic_hardware.c, so that the real driver code can run in
 * unit tests.  Force-included (-include drvshim.h) in those test builds.
 *
 * Calls that matter for the tests are recorded in struct shim_state; the
 * rest is a minimal model (pages, page pool, skb) or a no-op.  Register
 * accesses go to the fake BARs in fake_mmio.c through kshim.h.
 */
#ifndef __TEST_DRVSHIM_H__
#define __TEST_DRVSHIM_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include "kshim.h"

#define KERNEL_VERSION(a, b, c)		(((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE		KERNEL_VERSION(6, 8, 0)

typedef u64 dma_addr_t;
typedef unsigned int gfp_t;
typedef u16 __be16;
#define GFP_KERNEL			0u

#define likely(x)			__builtin_expect(!!(x), 1)
#define unlikely(x)			__builtin_expect(!!(x), 0)
#define READ_ONCE(x)			(x)
#define xchg(p, v)			({ __typeof__(*(p)) __old = *(p); *(p) = (v); __old; })
#define container_of(ptr, type, member)	((type *)((char *)(ptr) - offsetof(type, member)))
#define ALIGN(x, a)			(((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#define fallthrough			__attribute__((__fallthrough__))
#define min_t(t, a, b)			((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max(a, b)			((a) > (b) ? (a) : (b))
#define wmb()				__sync_synchronize()
#define ARRAY_SIZE(a)			(sizeof(a) / sizeof((a)[0]))

#define PAGE_SIZE			4096UL
#define ETH_HLEN			14
#define ETH_ZLEN			60
#define ETH_ALEN			6
#define IFNAMSIZ			16
#define XDP_PACKET_HEADROOM		256
#define CHECKSUM_NONE			0

#define MAX_ERRNO			4095
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr)
{
	return (unsigned long)ptr >= (unsigned long)-MAX_ERRNO;
}

/* bitmaps (single threaded, so no atomics) */
#define BITS_PER_LONG			64
#define DECLARE_BITMAP(name, bits)	unsigned long name[((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG]
static inline int test_bit(int nr, const unsigned long *addr)
{
	return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1;
}
static inline void set_bit(int nr, unsigned long *addr)
{
	addr[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG);
}
static inline void clear_bit(int nr, unsigned long *addr)
{
	addr[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG));
}
static inline int test_and_set_bit(int nr, unsigned long *addr)
{
	int old = test_bit(nr, addr);

	set_bit(nr, addr);
	return old;
}

typedef struct { int locked; } spinlock_t;
struct list_head { struct list_head *next, *prev; };
struct cpumask { unsigned long bits[1]; };

#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)

/* logging: every message is counted and kept, printed with SHIM_VERBOSE=1 */
void shim_log(const void *dev, const char *level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
#define dev_info(d, fmt, ...)			shim_log((d), "info", fmt, ##__VA_ARGS__)
#define dev_warn(d, fmt, ...)			shim_log((d), "warn", fmt, ##__VA_ARGS__)
#define dev_err(d, fmt, ...)			shim_log((d), "err", fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(d, fmt, ...)	shim_log((d), "err", fmt, ##__VA_ARGS__)
#define netdev_info(d, fmt, ...)		shim_log((d), "info", fmt, ##__VA_ARGS__)
#define netdev_warn(d, fmt, ...)		shim_log((d), "warn", fmt, ##__VA_ARGS__)
#define netdev_err(d, fmt, ...)			shim_log((d), "err", fmt, ##__VA_ARGS__)
#define netdev_dbg(d, fmt, ...)			shim_log((d), "dbg", fmt, ##__VA_ARGS__)
static inline int net_ratelimit(void) { return 1; }
int scnprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* time: sleeping advances a fake jiffies counter (1 jiffy = 1 ms) */
extern unsigned long jiffies;
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
#define time_after(a, b)		((long)((b) - (a)) < 0)
static inline void usleep_range(unsigned long min, unsigned long max) { jiffies++; }
static inline void mdelay(unsigned long ms) { jiffies += ms; }

/* memory */
static inline void *kzalloc(size_t size, gfp_t flags) { return calloc(1, size); }
static inline void *kcalloc(size_t n, size_t size, gfp_t flags) { return calloc(n, size); }
static inline void kfree(const void *p) { free((void *)p); }

/* DMA: identity mapping */
enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
};
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle,
			 gfp_t gfp);
void dma_free_coherent(struct device *dev, size_t size, void *cpu,
		       dma_addr_t handle);
static inline dma_addr_t dma_map_single(struct device *d, void *p, size_t s,
					enum dma_data_direction dir)
{
	return (dma_addr_t)(uintptr_t)p;
}
static inline void dma_unmap_single(struct device *d, dma_addr_t a, size_t s,
				    enum dma_data_direction dir) { }
static inline int dma_mapping_error(struct device *d, dma_addr_t a) { return 0; }
static inline void dma_sync_single_for_cpu(struct device *d, dma_addr_t a,
					   size_t s, enum dma_data_direction dir) { }
static inline void dma_sync_single_for_device(struct device *d, dma_addr_t a,
					      size_t s, enum dma_data_direction dir) { }
static inline int dev_to_node(struct device *dev) { return 0; }

/* PCI BAR mapping: BAR-0 -> FAKE_BAR_BASE, BAR-2 -> FAKE_BAR2_BASE */
void __iomem *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void __iomem *pci_iomap_range(struct pci_dev *pdev, int bar,
			      unsigned long offset, unsigned long maxlen);
void pci_iounmap(struct pci_dev *pdev, void __iomem *addr);
static inline unsigned long pci_resource_len(struct pci_dev *pdev, int bar)
{
	return bar == 0 ? 0x40000 : 0x400000;
}

/* pages and page pool */
struct page_pool;
struct page {
	u8 data[PAGE_SIZE];	/* first, so that page_address(pg) == pg */
	struct page_pool *pool;
	dma_addr_t dma;
	struct page *next_free;
};
#define PP_FLAG_DMA_MAP			BIT(0)
#define PP_FLAG_DMA_SYNC_DEV		BIT(1)
struct page_pool_params {
	unsigned int order;
	unsigned int flags;
	unsigned int pool_size;
	int nid;
	struct device *dev;
	enum dma_data_direction dma_dir;
	unsigned int offset;
	unsigned int max_len;
};
struct page_pool {
	struct page *free;
	int in_use;		/* pages handed out and not returned */
};
struct page_pool *page_pool_create(const struct page_pool_params *params);
void page_pool_destroy(struct page_pool *pool);
struct page *page_pool_dev_alloc_pages(struct page_pool *pool);
void page_pool_put_full_page(struct page_pool *pool, struct page *page,
			     bool allow_direct);
void page_pool_recycle_direct(struct page_pool *pool, struct page *page);
static inline dma_addr_t page_pool_get_dma_addr(const struct page *page)
{
	return page->dma;
}
static inline void *page_address(const struct page *page)
{
	return (void *)page->data;
}
static inline struct page *virt_to_page(const void *addr)
{
	return (struct page *)((uintptr_t)addr & ~(PAGE_SIZE - 1));
}

/* XDP / BPF */
struct net_device;
struct bpf_prog { int id; };
struct xdp_rxq_info { int registered; };
struct xdp_buff {
	void *data;
	void *data_end;
	void *data_meta;
	void *data_hard_start;
	struct xdp_rxq_info *rxq;
	u32 frame_sz;
};
struct xdp_frame {
	void *data;
	u16 len;
	u16 headroom;
};
enum xdp_action {
	XDP_ABORTED = 0,
	XDP_DROP,
	XDP_PASS,
	XDP_TX,
	XDP_REDIRECT,
};
enum xdp_mem_type { MEM_TYPE_PAGE_POOL = 2 };
#define XDP_XMIT_FLUSH			(1U << 0)
#define XDP_XMIT_FLAGS_MASK		XDP_XMIT_FLUSH
enum bpf_netdev_command { XDP_SETUP_PROG };
struct netdev_bpf {
	enum bpf_netdev_command command;
	struct bpf_prog *prog;
};
static inline void xdp_init_buff(struct xdp_buff *xdp, u32 frame_sz,
				 struct xdp_rxq_info *rxq)
{
	xdp->frame_sz = frame_sz;
	xdp->rxq = rxq;
}
static inline void xdp_prepare_buff(struct xdp_buff *xdp,
				    unsigned char *hard_start, int headroom,
				    int data_len, const bool meta_valid)
{
	unsigned char *data = hard_start + headroom;

	xdp->data_hard_start = hard_start;
	xdp->data = data;
	xdp->data_end = data + data_len;
	xdp->data_meta = meta_valid ? data : data + 1;
}
u32 bpf_prog_run_xdp(const struct bpf_prog *prog, struct xdp_buff *xdp);
void bpf_warn_invalid_xdp_action(struct net_device *dev, struct bpf_prog *prog,
				 u32 act);
static inline void trace_xdp_exception(const struct net_device *dev,
				       const struct bpf_prog *prog, u32 act) { }
int xdp_do_redirect(struct net_device *dev, struct xdp_buff *xdp,
		    struct bpf_prog *prog);
void xdp_do_flush(void);
struct xdp_frame *xdp_convert_buff_to_frame(struct xdp_buff *xdp);
void xdp_return_frame(struct xdp_frame *frame);
void xdp_return_frame_rx_napi(struct xdp_frame *frame);
int xdp_rxq_info_reg(struct xdp_rxq_info *rxq, struct net_device *dev,
		     u32 queue_index, unsigned int napi_id);
int xdp_rxq_info_reg_mem_model(struct xdp_rxq_info *rxq,
			       enum xdp_mem_type type, void *allocator);
void xdp_rxq_info_unreg(struct xdp_rxq_info *rxq);
bool xdp_rxq_info_is_reg(struct xdp_rxq_info *rxq);
static inline void xdp_features_set_redirect_target(struct net_device *dev,
						    bool support_sg) { }
static inline void xdp_features_clear_redirect_target(struct net_device *dev) { }
static inline void bpf_prog_put(struct bpf_prog *prog) { }

/* skb: the head is the page handed over by napi_build_skb() */
struct sk_buff {
	unsigned char *head;
	unsigned char *data;
	unsigned int len;
	u16 queue_mapping;
	__be16 protocol;
	u8 ip_summed;
	bool pp_recycle;
};
struct sk_buff *napi_build_skb(void *data, unsigned int frag_size);
static inline void skb_mark_for_recycle(struct sk_buff *skb) { skb->pp_recycle = true; }
static inline void skb_reserve(struct sk_buff *skb, int len) { skb->data += len; }
static inline void *skb_put(struct sk_buff *skb, unsigned int len)
{
	void *tail = skb->data + skb->len;

	skb->len += len;
	return tail;
}
static inline __be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev) { return 0; }
static inline void skb_record_rx_queue(struct sk_buff *skb, u16 rx_queue)
{
	skb->queue_mapping = rx_queue + 1;
}
static inline int skb_put_padto(struct sk_buff *skb, unsigned int len) { return 0; }
void dev_kfree_skb_any(struct sk_buff *skb);
void dev_kfree_skb(struct sk_buff *skb);

/* net device and NAPI */
/*
 * NAPI state as the core keeps it: sched while queued or polling, missed
 * when scheduled again during that time (napi_complete_done() then keeps it
 * scheduled and returns false).
 */
struct napi_struct {
	int (*poll)(struct napi_struct *napi, int budget);
	struct net_device *dev;
	bool enabled;
	bool sched;
	bool missed;
};
struct net_device {
	char name[IFNAMSIZ];
	unsigned int mtu;
	unsigned int real_num_tx_queues;
	unsigned int real_num_rx_queues;
	unsigned char dev_addr[ETH_ALEN];
	void *priv;
};
static inline void *netdev_priv(const struct net_device *dev) { return dev->priv; }
struct netdev_queue { int locked; };
typedef int netdev_tx_t;
#define NETDEV_TX_OK			0x00
#define NETDEV_TX_BUSY			0x10
struct rtnl_link_stats64 {
	u64 rx_packets;
	u64 tx_packets;
	u64 rx_bytes;
	u64 tx_bytes;
	u64 rx_errors;
	u64 tx_errors;
	u64 rx_dropped;
	u64 tx_dropped;
};
struct sockaddr {
	unsigned short sa_family;
	char sa_data[14];
};
struct ifreq { int unused; };

void netif_napi_add(struct net_device *dev, struct napi_struct *napi,
		    int (*poll)(struct napi_struct *, int));
static inline void napi_enable(struct napi_struct *n) { n->enabled = true; }
static inline void napi_disable(struct napi_struct *n) { n->enabled = false; }
static inline void netif_napi_del(struct napi_struct *n) { }
bool napi_complete_done(struct napi_struct *n, int work_done);
bool napi_complete(struct napi_struct *n);
void napi_schedule(struct napi_struct *n);
/* what the queue IRQ handler does (napi_schedule_irqoff()), not recorded */
void shim_napi_irq(struct napi_struct *n);
int napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb);
static inline void netif_tx_start_all_queues(struct net_device *dev) { }
static inline void netif_tx_stop_all_queues(struct net_device *dev) { }
static inline void netif_carrier_on(struct net_device *dev) { }
static inline void netif_carrier_off(struct net_device *dev) { }
static inline bool netif_running(const struct net_device *dev) { return true; }
struct netdev_queue *netdev_get_tx_queue(const struct net_device *dev,
					 unsigned int index);
static inline void __netif_tx_lock(struct netdev_queue *q, int cpu) { }
static inline void __netif_tx_unlock(struct netdev_queue *q) { }
static inline bool netdev_xmit_more(void) { return false; }
static inline int smp_processor_id(void) { return 0; }
#define this_cpu_ptr(p)			(p)
#define per_cpu_ptr(p, cpu)		(p)
#define for_each_possible_cpu(cpu)	for ((cpu) = 0; (cpu) < 1; (cpu)++)
static inline bool is_valid_ether_addr(const u8 *addr) { return true; }
static inline void eth_hw_addr_set(struct net_device *dev, const u8 *addr)
{
	memcpy(dev->dev_addr, addr, ETH_ALEN);
}

/* recorded calls and knobs */
#define SHIM_MAX_EVENTS			4096

enum shim_event_type {
	SHIM_EV_NAPI_COMPLETE_DONE = 1,
	SHIM_EV_NAPI_COMPLETE,
	SHIM_EV_NAPI_SCHEDULE,
	SHIM_EV_XDP_FLUSH,
	SHIM_EV_CMPL_CIDX,		/* recorded by the test's stub */
	SHIM_EV_RX_PIDX,		/* recorded by the test's stub */
	SHIM_EV_GLBL_ERR_INT_CLEAR,	/* recorded by the test's stub */
	SHIM_EV_XDP_RETURN,		/* xdp_return_frame*() */
	SHIM_EV_SKB_FREE,		/* dev_kfree_skb*() */
};

struct shim_event {
	enum shim_event_type type;
	int a;
	int b;
};

struct shim_state {
	struct shim_event ev[SHIM_MAX_EVENTS];
	int nev;

	/* knobs */
	bool napi_complete_ret;		/* napi_complete_done() return value */
	u32 xdp_action;			/* bpf_prog_run_xdp() result */
	int build_skb_fail_at;		/* fail the Nth napi_build_skb() call */

	/* counters */
	int build_skb_calls;
	int xdp_runs;
	int gro_packets;
	u64 gro_bytes;
	int redirects;
	int iomaps;
	int iounmaps;
	int dma_allocs;
	int log_count;
	int log_warn;
	int log_err;
	char last_log[512];
	char log_text[16384];		/* concatenated messages */
};
extern struct shim_state shim;

void shim_reset(void);
void shim_record(enum shim_event_type type, int a, int b);
int shim_count(enum shim_event_type type);
const struct shim_event *shim_last(enum shim_event_type type);
int shim_index_of_last(enum shim_event_type type);

#endif
