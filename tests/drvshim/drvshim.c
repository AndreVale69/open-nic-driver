/*
 * Implementation of the recording kernel shim in drvshim.h.
 */
#include "drvshim.h"

struct shim_state shim;
unsigned long jiffies;

void shim_reset(void)
{
	memset(&shim, 0, sizeof(shim));
	shim.napi_complete_ret = true;
	shim.xdp_action = XDP_PASS;
}

void shim_record(enum shim_event_type type, int a, int b)
{
	if (shim.nev >= SHIM_MAX_EVENTS) {
		fprintf(stderr, "drvshim: event log overflow\n");
		abort();
	}
	shim.ev[shim.nev].type = type;
	shim.ev[shim.nev].a = a;
	shim.ev[shim.nev].b = b;
	shim.nev++;
}

int shim_count(enum shim_event_type type)
{
	int i, n = 0;

	for (i = 0; i < shim.nev; i++)
		n += shim.ev[i].type == type;
	return n;
}

int shim_index_of_last(enum shim_event_type type)
{
	int i;

	for (i = shim.nev - 1; i >= 0; i--)
		if (shim.ev[i].type == type)
			return i;
	return -1;
}

const struct shim_event *shim_last(enum shim_event_type type)
{
	int i = shim_index_of_last(type);

	return i < 0 ? NULL : &shim.ev[i];
}

void shim_log(const void *dev, const char *level, const char *fmt, ...)
{
	va_list ap;
	size_t used;

	va_start(ap, fmt);
	vsnprintf(shim.last_log, sizeof(shim.last_log), fmt, ap);
	va_end(ap);

	shim.log_count++;
	if (!strcmp(level, "warn"))
		shim.log_warn++;
	if (!strcmp(level, "err"))
		shim.log_err++;

	used = strlen(shim.log_text);
	if (used + strlen(shim.last_log) + 2 < sizeof(shim.log_text)) {
		strcat(shim.log_text, shim.last_log);
		strcat(shim.log_text, "\n");
	}
	if (getenv("SHIM_VERBOSE"))
		fprintf(stderr, "[%s] %s\n", level, shim.last_log);
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!size)
		return 0;
	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0)
		return 0;
	return (size_t)n < size ? n : (int)size - 1;
}

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle,
			 gfp_t gfp)
{
	size_t rounded = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	void *p = aligned_alloc(PAGE_SIZE, rounded ? rounded : PAGE_SIZE);

	if (!p)
		return NULL;
	memset(p, 0, rounded);
	*handle = (dma_addr_t)(uintptr_t)p;
	shim.dma_allocs++;
	return p;
}

void dma_free_coherent(struct device *dev, size_t size, void *cpu,
		       dma_addr_t handle)
{
	free(cpu);
	shim.dma_allocs--;
}

void __iomem *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	if (bar != 0)
		return NULL;
	shim.iomaps++;
	return (void __iomem *)FAKE_BAR_BASE;
}

void __iomem *pci_iomap_range(struct pci_dev *pdev, int bar,
			      unsigned long offset, unsigned long maxlen)
{
	if (bar != 2)
		return NULL;
	shim.iomaps++;
	return (void __iomem *)(FAKE_BAR2_BASE + offset);
}

void pci_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
	if (!addr) {
		fprintf(stderr, "drvshim: pci_iounmap(NULL)\n");
		abort();
	}
	shim.iounmaps++;
}

/* ------------------------------------------------------------------------ */
/* page pool: a free list of page-aligned fake pages                          */

struct page_pool *page_pool_create(const struct page_pool_params *params)
{
	return calloc(1, sizeof(struct page_pool));
}

static struct page *new_page(struct page_pool *pool)
{
	size_t sz = (sizeof(struct page) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	struct page *pg = aligned_alloc(PAGE_SIZE, sz);

	if (!pg)
		return NULL;
	memset(pg, 0, sizeof(*pg));
	pg->pool = pool;
	pg->dma = (dma_addr_t)(uintptr_t)pg->data;
	return pg;
}

struct page *page_pool_dev_alloc_pages(struct page_pool *pool)
{
	struct page *pg = pool->free;

	if (pg)
		pool->free = pg->next_free;
	else
		pg = new_page(pool);
	if (pg) {
		pg->next_free = NULL;
		pool->in_use++;
	}
	return pg;
}

static void pool_return(struct page_pool *pool, struct page *page)
{
	if (!page)
		return;
	if (page->pool != pool) {
		fprintf(stderr, "drvshim: page returned to the wrong pool\n");
		abort();
	}
	page->next_free = pool->free;
	pool->free = page;
	pool->in_use--;
}

void page_pool_put_full_page(struct page_pool *pool, struct page *page,
			     bool allow_direct)
{
	pool_return(pool, page);
}

void page_pool_recycle_direct(struct page_pool *pool, struct page *page)
{
	pool_return(pool, page);
}

void page_pool_destroy(struct page_pool *pool)
{
	struct page *pg, *next;

	if (!pool)
		return;
	for (pg = pool->free; pg; pg = next) {
		next = pg->next_free;
		free(pg);
	}
	free(pool);
}

/* ------------------------------------------------------------------------ */
/* XDP                                                                        */

u32 bpf_prog_run_xdp(const struct bpf_prog *prog, struct xdp_buff *xdp)
{
	shim.xdp_runs++;
	return shim.xdp_action;
}

void bpf_warn_invalid_xdp_action(struct net_device *dev, struct bpf_prog *prog,
				 u32 act)
{
}

int xdp_do_redirect(struct net_device *dev, struct xdp_buff *xdp,
		    struct bpf_prog *prog)
{
	struct page *pg = virt_to_page(xdp->data_hard_start);

	/* the redirect target owns the page now; give it back right away */
	shim.redirects++;
	pool_return(pg->pool, pg);
	return 0;
}

void xdp_do_flush(void)
{
	shim_record(SHIM_EV_XDP_FLUSH, 0, 0);
}

struct xdp_frame *xdp_convert_buff_to_frame(struct xdp_buff *xdp)
{
	return NULL;
}

void xdp_return_frame(struct xdp_frame *frame)
{
	shim_record(SHIM_EV_XDP_RETURN, 0, 0);
}

void xdp_return_frame_rx_napi(struct xdp_frame *frame)
{
	shim_record(SHIM_EV_XDP_RETURN, 1, 0);
}

int xdp_rxq_info_reg(struct xdp_rxq_info *rxq, struct net_device *dev,
		     u32 queue_index, unsigned int napi_id)
{
	rxq->registered = 1;
	return 0;
}

int xdp_rxq_info_reg_mem_model(struct xdp_rxq_info *rxq,
			       enum xdp_mem_type type, void *allocator)
{
	return 0;
}

void xdp_rxq_info_unreg(struct xdp_rxq_info *rxq)
{
	rxq->registered = 0;
}

bool xdp_rxq_info_is_reg(struct xdp_rxq_info *rxq)
{
	return rxq->registered;
}

/* ------------------------------------------------------------------------ */
/* skb and NAPI                                                               */

struct sk_buff *napi_build_skb(void *data, unsigned int frag_size)
{
	struct sk_buff *skb;

	shim.build_skb_calls++;
	if (shim.build_skb_fail_at && shim.build_skb_calls == shim.build_skb_fail_at)
		return NULL;
	skb = calloc(1, sizeof(*skb));
	if (!skb)
		return NULL;
	skb->head = data;
	skb->data = data;
	return skb;
}

static void free_skb(struct sk_buff *skb)
{
	struct page *pg = virt_to_page(skb->head);

	if (skb->pp_recycle)
		pool_return(pg->pool, pg);
	free(skb);
}

void dev_kfree_skb_any(struct sk_buff *skb)
{
	shim_record(SHIM_EV_SKB_FREE, 0, 0);
	free_skb(skb);
}

void dev_kfree_skb(struct sk_buff *skb)
{
	shim_record(SHIM_EV_SKB_FREE, 1, 0);
	free_skb(skb);
}

int napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
	shim.gro_packets++;
	shim.gro_bytes += skb->len;
	free_skb(skb);
	return 3;	/* GRO_NORMAL */
}

void netif_napi_add(struct net_device *dev, struct napi_struct *napi,
		    int (*poll)(struct napi_struct *, int))
{
	napi->dev = dev;
	napi->poll = poll;
}

bool napi_complete_done(struct napi_struct *n, int work_done)
{
	shim_record(SHIM_EV_NAPI_COMPLETE_DONE, work_done, 0);
	if (n->missed) {	/* scheduled again meanwhile: poll once more */
		n->missed = false;
		return false;
	}
	if (!shim.napi_complete_ret)	/* e.g. owned by busy polling */
		return false;
	n->sched = false;
	return true;
}

bool napi_complete(struct napi_struct *n)
{
	shim_record(SHIM_EV_NAPI_COMPLETE, 0, 0);
	n->sched = false;
	n->missed = false;
	return true;
}

void shim_napi_irq(struct napi_struct *n)
{
	if (n->sched)
		n->missed = true;
	else
		n->sched = true;
}

void napi_schedule(struct napi_struct *n)
{
	shim_record(SHIM_EV_NAPI_SCHEDULE, 0, 0);
	shim_napi_irq(n);
}

struct netdev_queue *netdev_get_tx_queue(const struct net_device *dev,
					 unsigned int index)
{
	static struct netdev_queue queues[64];

	return &queues[index % 64];
}
