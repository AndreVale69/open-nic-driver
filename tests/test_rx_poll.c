/*
 * Hardware-free tests for the RX NAPI poll, onic_rx_poll() in onic_netdev.c.
 *
 * The real onic_netdev.c is compiled against drvshim.h.  The queue is set up
 * by onic_open_netdev(), so ring sizes and the initial doorbells are the
 * driver's own.  The test then plays the QDMA: it writes C2H completion
 * entries and the completion status descriptor into the coherent ring, calls
 * the poll function the driver registered with netif_napi_add(), and checks
 * the NAPI calls and the completion CIDX doorbells against the NAPI contract
 * (Documentation/networking/napi.rst):
 *
 *   - work == budget: return budget, do not call napi_complete*()
 *   - work <  budget: napi_complete_done(), re-arm the IRQ only if it says so
 *   - budget == 0:    no RX processing, no NAPI state change
 *
 * plus the QDMA side: consumed completions are handed back (CIDX) on every
 * poll.  The fake QDMA honours that credit, so a withheld CIDX shows up as a
 * full completion ring (wrb_qfull_err on hardware).
 *
 * Most tests check the CIDX and the interrupt request (irq_en_wrb) the
 * driver writes.  The event loop tests at the end also model what PG302 says
 * the QDMA does with them: one interrupt outstanding per queue, serviced by
 * any CIDX update, after which the trigger is evaluated again; irq_en_wrb
 * only counts with full_upd set in the completion context (OpenNIC leaves
 * it clear); completions only for packets that found a posted descriptor;
 * and a completion for a full ring is dropped and invalidates the context.
 */
#include "drvshim.h"
#include "onic.h"
#include "onic_common.h"
#include "onic_netdev.h"
#include "onic_hardware.h"
#include "qdma_export.h"
#include "qdma_csr.h"

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
	CHECK((long long)(got) == (long long)(exp), "%s: got %lld, expected %lld", \
	      what, (long long)(got), (long long)(exp))

/* ------------------------------------------------------------------------ */
/* the QDMA side of the queue                                                 */

static struct {
	u16 pidx;	/* next completion entry the QDMA writes */
	u16 cidx;	/* last CIDX the driver handed back */
	u8 color;	/* color of the current pass over the ring */
	u16 pkt_id;
	int qfull;	/* completions that did not fit (wrb_qfull_err) */

	/* PG302 model, used by the event loop tests */
	bool model;		/* evaluate interrupts, invalidate on overflow */
	bool full_upd;		/* irq_en_wrb honoured (sets en_int) */
	bool en_int;		/* completion context en_int */
	bool outstanding;	/* interrupt sent, no CIDX update since */
	bool invalid;		/* context invalidated by a ring overflow */
	bool limit_desc;	/* completions only into posted descriptors */
	u16 desc_pidx;		/* C2H descriptor PIDX written by the driver */
	u16 desc_used;		/* descriptors the QDMA has filled */
	int produced;
	int stalls;		/* offers blocked by descriptor exhaustion */
	int lost;		/* completions dropped by an invalid context */
	int irqs;
} hw;

static struct onic_rx_queue *rxq;

static void hw_eval_irq(void)
{
	if (!hw.model || hw.invalid || hw.outstanding || !hw.en_int)
		return;
	if (hw.pidx == hw.cidx)
		return;			/* nothing unread */
	hw.outstanding = true;
	hw.irqs++;
	shim_napi_irq(&rxq->napi);	/* onic_q_handler() */
}

static int fail_rx_qid = -1;
static struct onic_private priv;
static bool inspect_shutdown;

/* stubs for the onic_hardware.c functions used by onic_netdev.c */
u16 onic_ring_count(u8 idx)
{
	return qdma_ring_count(idx);
}

int onic_qdma_init_tx_queue(unsigned long qdma, u16 qid,
			    const struct onic_qdma_h2c_param *param)
{
	return 0;
}

int onic_qdma_init_rx_queue(unsigned long qdma, u16 qid,
			    const struct onic_qdma_c2h_param *param)
{
	return qid == fail_rx_qid ? -ENOMEM : 0;
}

void onic_qdma_clear_tx_queue(unsigned long qdma, u16 qid)
{
	int i;

	if (inspect_shutdown) {
		CHECK(!priv.queues_ready, "IRQ and XDP entry points gated");
		for (i = 0; i < priv.num_rx_queues; ++i)
			CHECK(!priv.rx_queue[i] || !priv.rx_queue[i]->napi.enabled,
			      "RX poll %d disabled before clearing TX %d", i, qid);
	}
}
void onic_qdma_clear_rx_queue(unsigned long qdma, u16 qid) { }
void onic_set_tx_head(unsigned long qdma, u16 qid, u16 head) { }

void onic_set_rx_head(unsigned long qdma, u16 qid, u16 head)
{
	shim_record(SHIM_EV_RX_PIDX, head, 0);
	hw.desc_pidx = head;
}

void onic_set_completion_tail(unsigned long qdma, u16 qid, u16 tail,
			      u8 irq_arm)
{
	shim_record(SHIM_EV_CMPL_CIDX, tail, irq_arm);
	hw.cidx = tail;
	if (!hw.model)
		return;
	if (hw.full_upd)
		hw.en_int = irq_arm;
	hw.outstanding = false;		/* any CIDX update services it */
	hw_eval_irq();
}

void onic_qdma_clear_error_interrupt(unsigned long qdma)
{
	shim_record(SHIM_EV_GLBL_ERR_INT_CLEAR, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* fixture                                                                    */

static struct pci_dev pdev;
static struct net_device ndev;
static struct rtnl_link_stats64 stats;
static struct bpf_prog xdp_prog;
static u16 real_cmpl;		/* usable completion entries */
static u16 real_desc;		/* usable C2H descriptors */

static void setup(void)
{
	shim_reset();
	memset(&hw, 0, sizeof(hw));
	memset(&ndev, 0, sizeof(ndev));
	memset(&priv, 0, sizeof(priv));
	memset(&stats, 0, sizeof(stats));

	ndev.priv = &priv;
	ndev.mtu = 1500;
	ndev.real_num_tx_queues = 1;
	ndev.real_num_rx_queues = 1;
	priv.pdev = &pdev;
	priv.netdev = &ndev;
	priv.netdev_stats = &stats;
	priv.num_q_vectors = 1;
	priv.num_tx_queues = 1;
	priv.num_rx_queues = 1;
	priv.hw.qdma = 1;	/* opaque, the stubs ignore it */

	CHECK(onic_open_netdev(&ndev) == 0, "onic_open_netdev");
	rxq = priv.rx_queue[0];
	real_cmpl = rxq->cmpl_ring.count - 1;
	real_desc = rxq->desc_ring.count - 1;
	CHECK(rxq->napi.poll != NULL, "NAPI poll registered");

	/* Run the startup poll that catches interrupts during queue setup. */
	CHECK(rxq->napi.sched, "startup poll scheduled");
	CHECK_EQ(rxq->napi.poll(&rxq->napi, 64), 0, "empty startup poll");
	hw.color = 1;
	shim.nev = 0;		/* forget the doorbells written at open */
}

static void teardown(void)
{
	onic_stop_netdev(&ndev);
	CHECK_EQ(shim.dma_allocs, 0, "coherent buffers freed");
}

static void write_status(void)
{
	u64 st = FIELD_SET(QDMA_C2H_CMPL_STAT_DW_PIDX_MASK, hw.pidx) |
		 FIELD_SET(QDMA_C2H_CMPL_STAT_DW_COLOR_MASK, hw.color);

	memcpy(rxq->cmpl_ring.desc + QDMA_C2H_CMPL_SIZE * real_cmpl, &st,
	       sizeof(st));
}

/* write up to @n completions; returns how many fitted before the ring was full */
static int hw_produce_err(int n, int len, int err)
{
	int i;

	for (i = 0; i < n; i++) {
		u64 dw;

		if (hw.invalid) {
			hw.lost += n - i;	/* silently dropped (PG302) */
			break;
		}
		if (hw.limit_desc && hw.desc_used == hw.desc_pidx) {
			hw.stalls++;	/* source retains the unaccepted packets */
			break;
		}
		if ((hw.pidx + 1) % real_cmpl == hw.cidx) {
			if (hw.model) {
				/* PG302: drop it, invalidate the context */
				hw.qfull++;
				hw.invalid = true;
				hw.lost += n - i - 1;
				break;
			}
			hw.qfull += n - i;
			break;
		}
		dw = FIELD_SET(QDMA_C2H_CMPL_DW_COLOR_MASK, hw.color) |
		     FIELD_SET(QDMA_C2H_CMPL_DW_ERR_MASK, err) |
		     FIELD_SET(QDMA_C2H_CMPL_DW_PKT_LEN_MASK, len) |
		     FIELD_SET(QDMA_C2H_CMPL_DW_PKT_ID_MASK, ++hw.pkt_id);
		memcpy(rxq->cmpl_ring.desc + QDMA_C2H_CMPL_SIZE * hw.pidx, &dw,
		       sizeof(dw));
		if (++hw.pidx == real_cmpl) {
			hw.pidx = 0;
			hw.color ^= 1;
		}
		if (hw.limit_desc)
			hw.desc_used = (hw.desc_used + 1) % real_desc;
		hw.produced++;
	}
	write_status();
	hw_eval_irq();
	return i;
}

static int hw_produce(int n, int len)
{
	return hw_produce_err(n, len, 0);
}

static int poll(int budget)
{
	int rv = rxq->napi.poll(&rxq->napi, budget);

	CHECK(rv >= 0 && rv <= budget, "poll(%d) returned %d", budget, rv);
	return rv;
}

static void check_no_legacy_napi_calls(const char *what)
{
	CHECK(shim_count(SHIM_EV_NAPI_COMPLETE) == 0,
	      "%s: napi_complete() must not be called", what);
	CHECK(shim_count(SHIM_EV_NAPI_SCHEDULE) == 0,
	      "%s: napi_schedule() from poll", what);
}

static void check_last_cidx(int cidx, int arm, const char *what)
{
	const struct shim_event *e = shim_last(SHIM_EV_CMPL_CIDX);

	CHECK(e != NULL, "%s: CIDX written", what);
	if (!e)
		return;
	CHECK(e->a == cidx && e->b == arm, "%s: CIDX %d arm %d, expected %d arm %d",
	      what, e->a, e->b, cidx, arm);
}

/* ------------------------------------------------------------------------ */

static void test_empty_poll_rearms(void)
{
	setup();
	CHECK_EQ(poll(64), 0, "empty poll");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 1, "completes");
	CHECK_EQ(shim_last(SHIM_EV_NAPI_COMPLETE_DONE)->a, 0, "work_done 0");
	check_last_cidx(0, 1, "empty poll re-arms");
	check_no_legacy_napi_calls("empty");
	teardown();
}

static void test_partial_batch(void)
{
	setup();
	hw_produce(10, 100);
	CHECK_EQ(poll(64), 10, "10 completions");
	CHECK_EQ(stats.rx_packets, 10, "rx_packets");
	CHECK_EQ(shim.gro_packets, 10, "delivered");
	CHECK_EQ(shim.gro_bytes, 1000, "delivered bytes");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 1, "completes once");
	CHECK_EQ(shim_last(SHIM_EV_NAPI_COMPLETE_DONE)->a, 10, "work_done");
	CHECK_EQ(shim_count(SHIM_EV_CMPL_CIDX), 1, "one CIDX write");
	check_last_cidx(10, 1, "partial");
	CHECK(shim_index_of_last(SHIM_EV_NAPI_COMPLETE_DONE) <
	      shim_index_of_last(SHIM_EV_CMPL_CIDX),
	      "complete NAPI before arming the interrupt");
	check_no_legacy_napi_calls("partial");
	teardown();
}

static void test_full_budget_hands_back_cidx(void)
{
	setup();
	hw_produce(100, 64);

	CHECK_EQ(poll(64), 64, "full budget returns budget");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 0,
		 "no napi_complete_done() when the budget is used");
	check_last_cidx(64, 0, "full budget: CIDX published, IRQ not armed");

	CHECK_EQ(poll(64), 36, "rest");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 1, "now completes");
	check_last_cidx(100, 1, "drained: armed");
	CHECK_EQ(stats.rx_packets, 100, "rx_packets");
	check_no_legacy_napi_calls("full");
	teardown();
}

static void test_exactly_full_then_empty(void)
{
	setup();
	hw_produce(64, 64);

	CHECK_EQ(poll(64), 64, "exactly budget");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 0, "still owned by core");
	check_last_cidx(64, 0, "exactly full");

	CHECK_EQ(poll(64), 0, "then empty");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 1, "completes");
	check_last_cidx(64, 1, "empty after full: armed");
	check_no_legacy_napi_calls("exactly full");
	teardown();
}

static void test_zero_budget(void)
{
	setup();
	hw_produce(5, 64);
	CHECK_EQ(poll(0), 0, "zero budget");
	CHECK_EQ(stats.rx_packets, 0, "no RX processing");
	CHECK_EQ(shim.gro_packets, 0, "nothing delivered");
	CHECK_EQ(shim_count(SHIM_EV_CMPL_CIDX), 0, "no CIDX write");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 0, "no NAPI change");
	check_no_legacy_napi_calls("zero budget");

	CHECK_EQ(poll(64), 5, "normal poll picks them up");
	check_last_cidx(5, 1, "after zero budget");
	teardown();
}

static void test_wraparound(void)
{
	int total = 0, rv, guard = 0;

	setup();
	hw_produce(1000, 64);
	do {
		rv = poll(64);
		total += rv;
	} while (rv == 64 && ++guard < 100);
	CHECK_EQ(total, 1000, "first pass");
	check_last_cidx(1000, 1, "first pass drained");

	/* 50 more cross the end of the 1023 entry ring */
	CHECK_EQ(hw_produce(50, 128), 50, "fits");
	CHECK_EQ(poll(64), 50, "across the wrap");
	check_last_cidx((1000 + 50) % real_cmpl, 1, "wrapped CIDX");
	CHECK_EQ(rxq->cmpl_ring.next_to_clean, (1000 + 50) % real_cmpl, "sw tail");
	CHECK_EQ(rxq->cmpl_ring.color, 0, "ring color flipped on the second pass");
	CHECK_EQ(stats.rx_bytes, 1000 * 64 + 50 * 128, "rx_bytes");
	check_no_legacy_napi_calls("wrap");
	teardown();
}

static void test_sustained_full_budget_never_fills_ring(void)
{
	int round, last = 0;

	setup();
	/*
	 * Line rate: 64 new completions arrive before every poll, so every
	 * poll uses its full budget.  The driver used to hand back no CIDX
	 * at all in this pattern; the 1023 entry ring then filled after 16
	 * rounds and the QDMA flagged wrb_qfull_err.
	 */
	for (round = 0; round < 200; round++) {
		hw_produce(64, 64);
		if (poll(64) != 64)
			break;
		last = shim_last(SHIM_EV_CMPL_CIDX) ?
		       shim_last(SHIM_EV_CMPL_CIDX)->a : -1;
	}
	CHECK_EQ(round, 200, "every poll used its budget");
	CHECK_EQ(hw.qfull, 0, "completion ring never full");
	CHECK_EQ(last, (200 * 64) % real_cmpl, "CIDX follows the consumer");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 0, "never completed");
	CHECK_EQ(stats.rx_packets, 200 * 64, "nothing lost");
	check_no_legacy_napi_calls("sustained");
	teardown();
}

static void test_xdp_redirect_flushed_before_complete(void)
{
	setup();
	rxq->xdp_prog = &xdp_prog;
	shim.xdp_action = XDP_REDIRECT;

	hw_produce(10, 64);
	CHECK_EQ(poll(64), 10, "redirected");
	CHECK_EQ(shim.redirects, 10, "redirect calls");
	CHECK_EQ(shim_count(SHIM_EV_XDP_FLUSH), 1, "one flush");
	CHECK(shim_index_of_last(SHIM_EV_XDP_FLUSH) <
	      shim_index_of_last(SHIM_EV_NAPI_COMPLETE_DONE),
	      "flush before napi_complete_done()");

	shim.nev = 0;
	hw_produce(100, 64);
	CHECK_EQ(poll(64), 64, "full budget of redirects");
	CHECK_EQ(shim_count(SHIM_EV_XDP_FLUSH), 1, "flushed on a full budget too");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 0, "not completed");
	CHECK_EQ(poll(64), 36, "rest");
	CHECK_EQ(shim.gro_packets, 0, "nothing passed to the stack");
	rxq->xdp_prog = NULL;
	teardown();
}

static void test_xdp_drop_recycles(void)
{
	setup();
	rxq->xdp_prog = &xdp_prog;
	shim.xdp_action = XDP_DROP;
	hw_produce(20, 64);
	CHECK_EQ(poll(64), 20, "dropped by XDP");
	CHECK_EQ(shim.xdp_runs, 20, "program ran per packet");
	CHECK_EQ(rxq->xdp_rx_stats.xdp_drop, 20, "xdp_drop");
	check_last_cidx(20, 1, "drop");
	rxq->xdp_prog = NULL;
	teardown();
}

static void test_skb_failure_retries_without_hang(void)
{
	setup();
	shim.build_skb_fail_at = 5;
	hw_produce(10, 64);
	CHECK_EQ(poll(64), 4, "stops at the failed packet");
	CHECK_EQ(shim_count(SHIM_EV_NAPI_COMPLETE_DONE), 1,
		 "NAPI completes (it used to stay scheduled forever)");
	check_last_cidx(4, 1, "consumed part handed back, IRQ armed");

	CHECK_EQ(poll(64), 6, "retried on the next poll");
	check_last_cidx(10, 1, "rest");
	CHECK_EQ(shim.gro_packets, 10, "all delivered once");
	teardown();
}

static void test_no_rearm_when_napi_not_completed(void)
{
	setup();
	shim.napi_complete_ret = false;	/* busy poll / deferred IRQ */
	hw_produce(10, 64);
	CHECK_EQ(poll(64), 10, "polled");
	check_last_cidx(10, 0, "CIDX handed back, IRQ left disarmed");

	shim.napi_complete_ret = true;
	CHECK_EQ(poll(64), 0, "next poll");
	check_last_cidx(10, 1, "armed by the completing poll");
	teardown();
}

static void test_completion_error_counted_not_disarming(void)
{
	setup();
	hw_produce(2, 64);
	hw_produce_err(1, 64, 1);
	CHECK_EQ(poll(64), 3, "polled");
	CHECK_EQ(stats.rx_errors, 1, "rx_errors");
	CHECK_EQ(shim_count(SHIM_EV_GLBL_ERR_INT_CLEAR), 0,
		 "the global error interrupt is left alone");
	CHECK_EQ(shim.gro_packets, 3, "delivered as before");
	teardown();
}

/* ------------------------------------------------------------------------ */
/* zero budget with TX completions pending                                    */

static unsigned char tx_payload[256];

static struct sk_buff *new_tx_skb(void)
{
	struct sk_buff *skb = calloc(1, sizeof(*skb));

	skb->data = tx_payload;
	skb->len = 100;
	return skb;
}

/* the QDMA's H2C writeback: everything before @cidx has been sent */
static void hw_tx_done(struct onic_tx_queue *q, u16 cidx)
{
	u64 wb = FIELD_SET(QDMA_WB_STAT_DW_CIDX_MASK, cidx);

	memcpy(q->ring.wb, &wb, sizeof(wb));
}

static void test_zero_budget_completes_skbs_not_xdp(void)
{
	struct xdp_frame frame = { .data = tx_payload, .len = 64 };
	struct xdp_frame *frames[1] = { &frame };
	struct onic_tx_queue *txq;

	setup();
	txq = priv.tx_queue[0];
	CHECK_EQ(onic_xmit_frame(new_tx_skb(), &ndev), NETDEV_TX_OK, "skb queued");
	CHECK_EQ(onic_xdp_xmit(&ndev, 1, frames, XDP_XMIT_FLUSH), 1,
		 "XDP frame queued");
	CHECK_EQ(onic_xmit_frame(new_tx_skb(), &ndev), NETDEV_TX_OK, "skb queued");
	hw_tx_done(txq, 3);

	CHECK_EQ(poll(0), 0, "zero budget");
	CHECK_EQ(shim_count(SHIM_EV_SKB_FREE), 1, "skb before the XDP frame freed");
	CHECK_EQ(shim_count(SHIM_EV_XDP_RETURN), 0, "no XDP API with budget 0");
	CHECK_EQ(txq->ring.next_to_clean, 1, "stopped at the XDP frame");
	check_no_legacy_napi_calls("zero budget TX");

	CHECK_EQ(poll(64), 0, "normal poll, no RX");
	CHECK_EQ(shim_count(SHIM_EV_XDP_RETURN), 1, "XDP frame returned now");
	CHECK_EQ(shim_count(SHIM_EV_SKB_FREE), 2, "and the skb after it");
	CHECK_EQ(txq->ring.next_to_clean, 3, "TX ring clean");
	teardown();
}

/* ------------------------------------------------------------------------ */
/* event loop with the PG302 interrupt model                                  */

/* one softirq pass: poll while NAPI is scheduled, at most @max_polls times */
static void napi_run(int max_polls)
{
	while (rxq->napi.sched && max_polls--)
		poll(64);
}

static void run_model(bool full_upd, int per_round, int polls_per_round,
		      int rounds, int buffer_capacity, const char *what)
{
	int r, offered = 0, pending = 0, ingress_drops = 0;

	setup();
	hw.model = true;
	hw.full_upd = full_upd;
	hw.en_int = true;	/* set in the context by onic_qdma_init_rx_queue() */
	hw.limit_desc = true;	/* hw.desc_pidx: written by the driver at open */

	for (r = 0; r < rounds; r++) {
		offered += per_round;
		pending += per_round;
		pending -= hw_produce(pending, 64);
		/* Capacity is the source's waiting space after this offer. */
		if (pending > buffer_capacity) {
			ingress_drops += pending - buffer_capacity;
			pending = buffer_capacity;
		}
		napi_run(polls_per_round);
	}
	/* Stop new arrivals. Retry queued packets as the driver posts buffers.
	 * Bound the loop so a missed interrupt or refill fails the test.
	 */
	for (r = 0; r <= offered && (pending || rxq->napi.sched); r++) {
		int produced = pending ? hw_produce(pending, 64) : 0;

		pending -= produced;
		if (!produced && !rxq->napi.sched)
			break;
		napi_run(polls_per_round);
	}
	CHECK_EQ(pending, 0, what);

	CHECK(!hw.invalid && hw.qfull == 0,
	      "%s: completion ring overflowed (%d), context invalidated", what,
	      hw.qfull);
	CHECK_EQ(hw.lost, 0, what);
	CHECK_EQ(hw.produced + ingress_drops, offered, what);
	if (buffer_capacity >= offered)
		CHECK_EQ(hw.produced, offered, what);
	else
		CHECK(ingress_drops > 0, "%s: finite source buffer did not fill", what);
	CHECK_EQ(stats.rx_packets, hw.produced, what);
	CHECK(!rxq->napi.sched, "%s: NAPI still scheduled after traffic", what);
	CHECK(hw.cidx == hw.pidx, "%s: completions not handed back (%u/%u)", what,
	      hw.cidx, hw.pidx);
	CHECK(hw.en_int, "%s: interrupt left disabled while idle", what);
	CHECK(hw.irqs > 0, "%s: no interrupt", what);
	check_no_legacy_napi_calls(what);
	if (getenv("TEST_VERBOSE"))
		printf("  %-40s polls %4d irqs %5d stalls %5d ingress drops %5d\n", what,
		       shim_count(SHIM_EV_CMPL_CIDX), hw.irqs, hw.stalls, ingress_drops);

	/* and the queue still works afterwards */
	CHECK_EQ(hw_produce(10, 64), 10, "fresh packets accepted");
	napi_run(4);
	CHECK_EQ(stats.rx_packets, hw.produced, what);
	CHECK(!rxq->napi.sched && hw.cidx == hw.pidx, "%s: not idle again", what);
	teardown();
}

static void test_event_loop(void)
{
	static const struct {
		int per_round;
		int polls;
		bool stalls;
	} cases[] = {
		{ 48, 1, false },	/* below one poll per round */
		{ 64, 1, false },	/* exactly one budget: the old code overflowed */
		{ 100, 1, true },	/* more than one queue can take */
		{ 200, 4, true },	/* bursts beyond the posted descriptors */
	};
	char what[96];
	int c, f;

	for (f = 0; f < 2; f++) {
		for (c = 0; c < ARRAY_SIZE(cases); c++) {
			snprintf(what, sizeof(what), "full_upd=%d, %d per round, %d poll(s)",
				 f, cases[c].per_round, cases[c].polls);
			run_model(f, cases[c].per_round, cases[c].polls, 300, 300 * cases[c].per_round, what);
			if (cases[c].stalls)
				CHECK(hw.stalls > 0, "%s: descriptor starvation expected", what);
			else
				CHECK_EQ(hw.stalls, 0, what);
		}
		snprintf(what, sizeof(what), "full_upd=%d, finite 64-packet source buffer", f);
		run_model(f, 100, 1, 300, 64, what);
	}
}

static void test_queue_lifecycle(void)
{
	setup();
	teardown();
	inspect_shutdown = true;
	priv.num_rx_queues = 2;
	priv.num_tx_queues = 2;
	priv.num_q_vectors = 2;
	fail_rx_qid = 1;
	CHECK_EQ(onic_open_netdev(&ndev), -ENOMEM, "second RX queue fails");
	CHECK_EQ(shim.dma_allocs, 0, "partial open frees DMA allocations");
	CHECK(!priv.queues_ready, "failed open leaves queue IRQs gated");
	CHECK_EQ(onic_xdp_xmit(&ndev, 0, NULL, 0), -ENETDOWN, "XDP while down");
	fail_rx_qid = -1;
	CHECK_EQ(onic_open_netdev(&ndev), 0, "retry open");
	CHECK(priv.rx_queue[0]->napi.sched && priv.rx_queue[1]->napi.sched,
	      "startup schedules both RX queues");
	teardown();
	inspect_shutdown = false;
}

int main(void)
{
	test_queue_lifecycle();
	test_empty_poll_rearms();
	test_partial_batch();
	test_full_budget_hands_back_cidx();
	test_exactly_full_then_empty();
	test_zero_budget();
	test_wraparound();
	test_sustained_full_budget_never_fills_ring();
	test_xdp_redirect_flushed_before_complete();
	test_xdp_drop_recycles();
	test_skb_failure_retries_without_hang();
	test_no_rearm_when_napi_not_completed();
	test_completion_error_counted_not_disarming();
	test_zero_budget_completes_skbs_not_xdp();
	test_event_loop();

	printf("test_rx_poll: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
