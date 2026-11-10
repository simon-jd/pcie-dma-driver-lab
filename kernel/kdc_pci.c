// SPDX-License-Identifier: GPL-2.0
/*
 * PCI driver for the emulated KDC DMA engine: probe/remove, queue rings,
 * MSI-X handling, dispatch and device aborts. Timeouts and resets are in
 * kdc_recover.c.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "kdc_drv.h"

static unsigned int queue_depth = 64;
module_param(queue_depth, uint, 0444);
MODULE_PARM_DESC(queue_depth, "Ring entries per queue (power of two, 2..4096)");
static int bounce_max = -1;
module_param(bounce_max, int, 0644);
MODULE_PARM_DESC(bounce_max, "Largest payload copied through a bounce buffer, at most 65536; "
		 "-1: 2048 with direct DMA, 65536 behind a translating IOMMU. Read at probe");
static unsigned int max_queues = KDC_MAX_QUEUES;
module_param(max_queues, uint, 0444);
MODULE_PARM_DESC(max_queues, "Upper bound on queue pairs used per device");
static unsigned int coalesce_count;
module_param(coalesce_count, uint, 0444);
MODULE_PARM_DESC(coalesce_count, "Completions per interrupt (0 or 1: one interrupt per completion); read at probe");
static unsigned int coalesce_usecs;
module_param(coalesce_usecs, uint, 0444);
MODULE_PARM_DESC(coalesce_usecs, "Longest an interrupt is held back while coalescing, us; read at probe");
static bool shadow_doorbells = true;
module_param(shadow_doorbells, bool, 0444);
MODULE_PARM_DESC(shadow_doorbells, "Use the device's shadow doorbells (SQ tail in host memory, event index) "
		 "when it offers them; read at probe");
static unsigned int cq_head_batch;
module_param(cq_head_batch, uint, 0444);
MODULE_PARM_DESC(cq_head_batch, "Write the completion-queue head to the device after this many reaped entries "
		 "(1: after every reap; 0: a quarter of the ring); read at probe, debugfs cq_head_batch per device");
unsigned int cmd_timeout_ms = 5000;
module_param(cmd_timeout_ms, uint, 0644);
MODULE_PARM_DESC(cmd_timeout_ms, "Default command timeout from the doorbell, ms");
unsigned int abort_timeout_ms = 500;
module_param(abort_timeout_ms, uint, 0644);
MODULE_PARM_DESC(abort_timeout_ms, "How long a device abort may go unanswered before a reset, ms");
unsigned int reset_timeout_ms = 1000;
module_param(reset_timeout_ms, uint, 0644);
MODULE_PARM_DESC(reset_timeout_ms, "How long the controller may take to stop or become ready, ms");

static const struct class kdc_class = { .name = "kdc" };
static dev_t kdc_devt;
static DEFINE_IDA(kdc_minors);
static struct dentry *kdc_debugfs_root;
struct workqueue_struct *kdc_wq;

/*
 * Default bounce threshold, from the KVM measurements in results/round3/perf/:
 * with direct DMA the bounce copy costs the same as pinning and mapping at
 * 2 KiB and more above it; behind QEMU's emulated IOMMU every zero-copy unmap
 * is an IOTLB invalidation (one more VM exit per buffer), while the bounce
 * path maps nothing per command and stays faster up to 64 KiB.
 */
static unsigned int kdc_bounce_default(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	return domain && iommu_is_dma_domain(domain) ? 65536 : 2048;
}

static u32 kdc_rd(struct kdc_dev *kdev, u32 off)
{
	return readl(kdev->regs + off);
}

static void kdc_wr(struct kdc_dev *kdev, u32 off, u32 val)
{
	writel(val, kdev->regs + off);
}

/* ---- dispatch, abort, completion ---- */

void kdc_queue_dispatch_locked(struct kdc_queue *q)
{
	struct kdc_cmd *cmd, *tmp;
	u16 old = q->sq_tail;
	bool rung = false;

	lockdep_assert_held(&q->lock);
	if (q->stopped)
		return;
	list_for_each_entry_safe(cmd, tmp, &q->pending, node) {
		unsigned int tag;

		if (q->outstanding >= q->depth - 1)
			break;
		tag = find_first_zero_bit(q->tag_map, q->depth);
		if (WARN_ON_ONCE(tag >= q->depth))
			break;
		__set_bit(tag, q->tag_map);
		q->gen[tag] = kdc_gen_next(q->gen[tag]);
		cmd->tag = tag;
		cmd->cid = kdc_cid_make(tag, q->gen[tag]);
		cmd->sqe.cid = cpu_to_le16(cmd->cid);
		cmd->deadline = jiffies + cmd->timeout;
		q->sq[q->sq_tail] = cmd->sqe;
		q->sq_tail = kdc_ring_next(q->sq_tail, q->depth);
		q->slots[tag] = cmd;
		list_del_init(&cmd->node);
		WRITE_ONCE(cmd->phase, KDC_DEVICE);
		q->outstanding++;
		q->submitted++;
		rung = true;
	}
	if (rung) {
		bool ring = true;

		if (q->db) {
			/* the entries before the shadow tail, the tail before the event index */
			wmb();
			WRITE_ONCE(q->db->sq_tail, cpu_to_le32(q->sq_tail));
			mb();
			ring = kdc_need_event(le32_to_cpu(READ_ONCE(q->db->sq_event)),
					      q->sq_tail, old, q->depth);
		}
		/* writel orders the ring stores above before the doorbell. */
		if (ring)
			writel(q->sq_tail, q->sq_db);
		kdc_arm_watchdog(q->kdev);
	}
}

void kdc_send_abort_locked(struct kdc_queue *q, struct kdc_cmd *cmd,
			   enum kdc_abort_reason reason)
{
	lockdep_assert_held(&q->lock);
	/*
	 * The cid is only reissued after its completion has been reaped under
	 * this lock, so while slots[tag] still points here the abort can only
	 * hit this command.
	 */
	if (READ_ONCE(cmd->phase) != KDC_DEVICE || cmd->abort_sent ||
	    q->slots[cmd->tag] != cmd)
		return;
	cmd->abort_reason = reason;
	cmd->abort_sent = true;
	/* An unanswered abort escalates to a reset; see kdc_watchdog_fn(). */
	cmd->abort_deadline = jiffies + msecs_to_jiffies(READ_ONCE(abort_timeout_ms));
	atomic64_inc(&q->kdev->aborts_sent);
	if (!q->stopped)
		writel(cmd->cid, q->abort_reg);
}

void kdc_queue_reap_locked(struct kdc_queue *q, struct list_head *done)
{
	struct kdc_dev *kdev = q->kdev;
	bool reaped = false;

	lockdep_assert_held(&q->lock);
	for (;;) {
		struct kdc_hw_cqe *e = &q->cq[q->cq_head];
		u16 status = le16_to_cpu(READ_ONCE(e->status));
		struct kdc_cmd *cmd;
		u16 cid, tag;

		if (!kdc_cqe_is_new(status, q->phase))
			break;
		dma_rmb(); /* read the rest of the entry after its phase bit */
		cid = le16_to_cpu(e->cid);
		tag = kdc_cid_tag(cid);
		cmd = tag < q->depth ? q->slots[tag] : NULL;
		kdc_cq_advance(&q->cq_head, &q->phase, q->depth);
		reaped = true;
		if (!cmd || cmd->cid != cid) {
			atomic64_inc(&kdev->bad_completions);
			dev_warn_ratelimited(&kdev->pdev->dev,
					     "queue %u: completion for unknown cid %#x\n",
					     q->qid, cid);
			continue;
		}
		q->slots[tag] = NULL;
		__clear_bit(tag, q->tag_map);
		q->outstanding--;
		q->completed++;
		cmd->hw_status = status >> KDC_CQE_CODE_SHIFT;
		cmd->crc = le32_to_cpu(e->result);
		cmd->done_bytes = le32_to_cpu(e->bytes);
		WRITE_ONCE(cmd->phase, KDC_COMPLETING);
		list_add_tail(&cmd->node, done);
	}
	/*
	 * The device needs the head only to see free completion entries, so it
	 * is written once cq_head_batch entries have been reaped since the last
	 * write (one MMIO exit per batch instead of per interrupt). Progress: the
	 * device stalls only when every entry it can post is in use from its
	 * point of view; all of those have been posted with an interrupt, and
	 * reaping them makes the unreported count depth - 1 >= cq_head_batch.
	 * A stopped queue's ring is reinitialised before it runs again.
	 */
	if (reaped && !q->stopped &&
	    kdc_ring_used(q->cq_head, q->cq_head_db, q->depth) >= READ_ONCE(kdev->cq_head_batch)) {
		writel(q->cq_head, q->cq_db);
		q->cq_head_db = q->cq_head;
	}
}

/* Commands the device completed: buffers are ours again. */
void kdc_finish_list(struct kdc_dev *kdev, struct list_head *list)
{
	struct kdc_cmd *cmd, *tmp;

	list_for_each_entry_safe(cmd, tmp, list, node) {
		bool ok = cmd->hw_status == KDC_SC_SUCCESS;

		list_del_init(&cmd->node);
		kdc_cmd_release(kdev, cmd, ok);
		atomic64_inc(&kdev->completed);
		kdc_cmd_publish(cmd, kdc_status_errno(cmd->hw_status,
						      cmd->abort_reason));
	}
}

/*
 * Commands ended without a device completion (reset, failure, removal), with
 * cmd->status already chosen. Callers guarantee the device can no longer DMA.
 */
void kdc_end_list(struct kdc_dev *kdev, struct list_head *list)
{
	struct kdc_cmd *cmd, *tmp;

	list_for_each_entry_safe(cmd, tmp, list, node) {
		list_del_init(&cmd->node);
		kdc_cmd_release(kdev, cmd, false);
		kdc_cmd_publish(cmd, cmd->status);
	}
}

void kdc_cmd_end(struct kdc_cmd *cmd, int base, struct list_head *list)
{
	cmd->status = kdc_final_status(cmd->abort_sent, cmd->abort_reason, base);
	cmd->hw_status = 0;
	cmd->ended_by_reset = true;
	WRITE_ONCE(cmd->phase, KDC_COMPLETING);
	list_add_tail(&cmd->node, list);
}

/*
 * Take every command the driver or the device still holds and stop all
 * queues. The caller must already have made the device unable to DMA
 * (disabled, reset, or bus mastering off).
 */
void kdc_collect_all(struct kdc_dev *kdev, struct list_head *list, int base,
		     enum kdc_abort_reason reason)
{
	struct kdc_cmd *cmd, *tmp;
	struct kdc_ctx *ctx;
	unsigned int i, tag;

	lockdep_assert_held(&kdev->files_mutex);
	list_for_each_entry(ctx, &kdev->files, dev_node) {
		spin_lock(&ctx->lock);
		list_for_each_entry_safe(cmd, tmp, &ctx->staged, node) {
			list_del_init(&cmd->node);
			cmd->abort_reason = reason;
			kdc_cmd_end(cmd, base, list);
		}
		ctx->nr_staged = 0;
		spin_unlock(&ctx->lock);
	}
	for (i = 0; i < kdev->nr_queues; i++) {
		struct kdc_queue *q = &kdev->queues[i];

		spin_lock(&q->lock);
		q->stopped = true;
		list_for_each_entry_safe(cmd, tmp, &q->pending, node) {
			list_del_init(&cmd->node);
			cmd->abort_reason = reason;
			kdc_cmd_end(cmd, base, list);
		}
		for_each_set_bit(tag, q->tag_map, q->depth) {
			cmd = q->slots[tag];
			q->slots[tag] = NULL;
			kdc_cmd_end(cmd, base, list);
		}
		bitmap_zero(q->tag_map, q->depth);
		q->outstanding = 0;
		spin_unlock(&q->lock);
	}
}

void kdc_wake_files(struct kdc_dev *kdev)
{
	struct kdc_ctx *ctx;

	mutex_lock(&kdev->files_mutex);
	list_for_each_entry(ctx, &kdev->files, dev_node)
		wake_up_all(&ctx->wq);
	mutex_unlock(&kdev->files_mutex);
}

static irqreturn_t kdc_queue_irq(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

/* Threaded: unpin_user_pages_dirty_lock() may sleep. */
static irqreturn_t kdc_queue_thread(int irq, void *data)
{
	struct kdc_queue *q = data;
	LIST_HEAD(done);

	spin_lock(&q->lock);
	kdc_queue_reap_locked(q, &done);
	kdc_queue_dispatch_locked(q);
	spin_unlock(&q->lock);
	kdc_finish_list(q->kdev, &done);
	return IRQ_HANDLED;
}

static irqreturn_t kdc_error_irq(int irq, void *data)
{
	struct kdc_dev *kdev = data;
	u32 status = kdc_rd(kdev, KDC_REG_ERR_STATUS);

	if (!status)
		return IRQ_NONE;
	if (status == ~0u) { /* not responding: let recovery find out why */
		queue_work(kdc_wq, &kdev->recover_work);
		return IRQ_HANDLED;
	}
	kdc_wr(kdev, KDC_REG_ERR_STATUS, status);
	atomic64_inc(&kdev->errors);
	dev_warn_ratelimited(&kdev->pdev->dev,
			     "device error %#x (queue %u, status %#x)\n", status,
			     kdc_rd(kdev, KDC_REG_ERR_INFO),
			     kdc_rd(kdev, KDC_REG_STATUS));
	/* A fatal device or a halted queue makes no progress until reset. */
	if (status & (KDC_ERR_FATAL | KDC_ERR_RING_DMA))
		queue_work(kdc_wq, &kdev->recover_work);
	return IRQ_HANDLED;
}

/* ---- hardware bring-up ---- */

bool kdc_device_gone(struct kdc_dev *kdev)
{
	return kdc_rd(kdev, KDC_REG_STATUS) == ~0u &&
	       !pci_device_is_present(kdev->pdev);
}

/* Disable the controller; 0 once READY has cleared, i.e. DMA has stopped. */
int kdc_hw_stop(struct kdc_dev *kdev)
{
	u32 status;
	int err;

	kdc_wr(kdev, KDC_REG_CTRL, 0);
	err = readl_poll_timeout(kdev->regs + KDC_REG_STATUS, status,
				 status == ~0u || !(status & KDC_STATUS_READY),
				 10, READ_ONCE(reset_timeout_ms) * 1000ul);
	if (!err && status == ~0u)
		err = -ENODEV;
	return err;
}

/* Controller reset, enable, wait for READY. */
int kdc_hw_start(struct kdc_dev *kdev)
{
	u32 status;
	int err;

	kdc_wr(kdev, KDC_REG_RESET, KDC_RESET_MAGIC);
	kdc_wr(kdev, KDC_REG_CTRL, KDC_CTRL_ENABLE);
	err = readl_poll_timeout(kdev->regs + KDC_REG_STATUS, status,
				 status == ~0u || (status & KDC_STATUS_READY),
				 10, READ_ONCE(reset_timeout_ms) * 1000ul);
	if (!err && status == ~0u)
		err = -ENODEV;
	return err;
}

static int kdc_queue_alloc(struct kdc_dev *kdev, struct kdc_queue *q, u16 qid)
{
	struct device *dev = &kdev->pdev->dev;
	u16 depth = kdev->depth;

	q->kdev = kdev;
	q->qid = qid;
	q->depth = depth;
	spin_lock_init(&q->lock);
	INIT_LIST_HEAD(&q->pending);
	q->slots = kcalloc(depth, sizeof(*q->slots), GFP_KERNEL);
	q->gen = kcalloc(depth, sizeof(*q->gen), GFP_KERNEL);
	q->tag_map = bitmap_zalloc(depth, GFP_KERNEL);
	q->sq = dma_alloc_coherent(dev, depth * sizeof(*q->sq), &q->sq_dma,
				   GFP_KERNEL);
	q->cq = dma_alloc_coherent(dev, depth * sizeof(*q->cq), &q->cq_dma,
				   GFP_KERNEL);
	q->sq_db = kdev->regs + KDC_QREG(qid, KDC_Q_SQ_TAIL);
	q->cq_db = kdev->regs + KDC_QREG(qid, KDC_Q_CQ_HEAD);
	q->abort_reg = kdev->regs + KDC_QREG(qid, KDC_Q_ABORT);
	return q->slots && q->gen && q->tag_map && q->sq && q->cq ? 0 : -ENOMEM;
}

/*
 * Program an empty queue into an enabled controller. At probe and after every
 * reset; the caller holds q->lock or the queue is not yet visible. The ring
 * memory is only rewritten while the device cannot DMA into it.
 */
static int kdc_queue_hw_setup(struct kdc_dev *kdev, struct kdc_queue *q)
{
	u16 qid = q->qid;

	memset(q->sq, 0, q->depth * sizeof(*q->sq));
	memset(q->cq, 0, q->depth * sizeof(*q->cq));
	q->sq_tail = 0;
	q->cq_head = 0;
	q->cq_head_db = 0;
	q->phase = 1;
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_SQ_BASE_LO), lower_32_bits(q->sq_dma));
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_SQ_BASE_HI), upper_32_bits(q->sq_dma));
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_CQ_BASE_LO), lower_32_bits(q->cq_dma));
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_CQ_BASE_HI), upper_32_bits(q->cq_dma));
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_SIZE_LOG2), ilog2(q->depth));
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_IRQ_VECTOR), qid);
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_COALESCE), kdev->coalesce);
	kdc_wr(kdev, KDC_QREG(qid, KDC_Q_CTRL), KDC_QCTRL_ENABLE);
	if (kdc_rd(kdev, KDC_QREG(qid, KDC_Q_STATUS)) != KDC_QSTATUS_ENABLED)
		return -EIO;
	return 0;
}

/*
 * The shadow doorbell page, zeroed and programmed after a controller reset and
 * before any queue is enabled, so the device cannot be writing it.
 */
static void kdc_dbbuf_setup(struct kdc_dev *kdev)
{
	if (!kdev->dbbuf)
		return;
	memset(kdev->dbbuf, 0, PAGE_SIZE);
	kdc_wr(kdev, KDC_REG_DBBUF_HI, upper_32_bits(kdev->dbbuf_dma));
	kdc_wr(kdev, KDC_REG_DBBUF_LO, lower_32_bits(kdev->dbbuf_dma) | KDC_DBBUF_ENABLE);
}

/* Bring a stopped controller back: called by recovery with reset_lock held. */
int kdc_restart_hw(struct kdc_dev *kdev)
{
	unsigned int i;
	int err;

	pci_set_master(kdev->pdev);
	err = kdc_hw_start(kdev);
	if (err)
		return err;
	kdc_dbbuf_setup(kdev);
	for (i = 0; i < kdev->nr_queues; i++) {
		struct kdc_queue *q = &kdev->queues[i];

		spin_lock(&q->lock);
		err = kdc_queue_hw_setup(kdev, q);
		spin_unlock(&q->lock);
		if (err)
			return err;
	}
	kdc_wr(kdev, KDC_REG_ERR_VECTOR, kdev->nr_queues);
	return 0;
}

static void kdc_queue_free(struct kdc_dev *kdev, struct kdc_queue *q)
{
	struct device *dev = &kdev->pdev->dev;

	if (q->sq)
		dma_free_coherent(dev, q->depth * sizeof(*q->sq), q->sq, q->sq_dma);
	if (q->cq)
		dma_free_coherent(dev, q->depth * sizeof(*q->cq), q->cq, q->cq_dma);
	q->sq = NULL;
	q->cq = NULL;
	kfree(q->slots);
	kfree(q->gen);
	bitmap_free(q->tag_map);
	q->slots = NULL;
	q->gen = NULL;
	q->tag_map = NULL;
}

static void kdc_map_cpus(struct kdc_dev *kdev)
{
	unsigned int q, cpu;

	for_each_possible_cpu(cpu)
		kdev->cpu_queue[cpu] = cpu % kdev->nr_queues;
	for (q = 0; q < kdev->nr_queues; q++) {
		const struct cpumask *mask = pci_irq_get_affinity(kdev->pdev, q);

		if (!mask)
			continue;
		for_each_cpu(cpu, mask)
			kdev->cpu_queue[cpu] = q;
	}
}

static void kdc_free_irqs(struct kdc_dev *kdev)
{
	unsigned int i;

	if (!kdev->irqs_requested)
		return;
	for (i = 0; i < kdev->nr_queues; i++)
		pci_free_irq(kdev->pdev, i, &kdev->queues[i]);
	pci_free_irq(kdev->pdev, kdev->nr_queues, kdev);
	kdev->irqs_requested = false;
}

static int kdc_request_irqs(struct kdc_dev *kdev)
{
	unsigned int i;
	int err;

	for (i = 0; i < kdev->nr_queues; i++) {
		err = pci_request_irq(kdev->pdev, i, kdc_queue_irq,
				      kdc_queue_thread, &kdev->queues[i],
				      "kdc%d-q%u", kdev->minor, i);
		if (err)
			goto unwind;
	}
	err = pci_request_irq(kdev->pdev, kdev->nr_queues, kdc_error_irq, NULL,
			      kdev, "kdc%d-err", kdev->minor);
	if (err)
		goto unwind;
	kdc_wr(kdev, KDC_REG_ERR_VECTOR, kdev->nr_queues);
	kdev->irqs_requested = true;
	return 0;
unwind:
	while (i--)
		pci_free_irq(kdev->pdev, i, &kdev->queues[i]);
	return err;
}

/* ---- debugfs: device timing and fault knobs, register-backed ---- */

static int kdc_dbg_get(void *data, u64 *val)
{
	struct kdc_dbg_reg *r = data;

	*val = readl(r->kdev->regs + r->offset);
	return 0;
}

static int kdc_dbg_set(void *data, u64 val)
{
	struct kdc_dbg_reg *r = data;

	if (val > U32_MAX)
		return -EINVAL;
	writel(val, r->kdev->regs + r->offset);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(kdc_dbg_fops, kdc_dbg_get, kdc_dbg_set, "%llu\n");

/* Commands the device itself holds (fetched, not yet completed), all queues. */
static int kdc_inflight_get(void *data, u64 *val)
{
	struct kdc_dev *kdev = data;
	unsigned int i;

	*val = 0;
	for (i = 0; i < kdev->nr_queues; i++)
		*val += kdc_rd(kdev, KDC_QREG(i, KDC_Q_INFLIGHT));
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(kdc_inflight_fops, kdc_inflight_get, NULL, "%llu\n");

/* Interrupt coalescing of every queue, in the register format usecs << 16 | count. */
static int kdc_coalesce_get(void *data, u64 *val)
{
	struct kdc_dev *kdev = data;

	*val = READ_ONCE(kdev->coalesce);
	return 0;
}

static int kdc_coalesce_set(void *data, u64 val)
{
	struct kdc_dev *kdev = data;
	unsigned int i;

	if (val > U32_MAX)
		return -EINVAL;
	/* reset_lock: a reset reprograms the queues from kdev->coalesce. */
	mutex_lock(&kdev->reset_lock);
	WRITE_ONCE(kdev->coalesce, val);
	if (kdev->state == KDC_STATE_LIVE && !kdev->recovering)
		for (i = 0; i < kdev->nr_queues; i++)
			kdc_wr(kdev, KDC_QREG(i, KDC_Q_COALESCE), val);
	mutex_unlock(&kdev->reset_lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(kdc_coalesce_fops, kdc_coalesce_get, kdc_coalesce_set,
			 "%llu\n");

static int kdc_cq_head_batch_get(void *data, u64 *val)
{
	struct kdc_dev *kdev = data;

	*val = READ_ONCE(kdev->cq_head_batch);
	return 0;
}

/* Clamped to 1..depth - 1: a larger batch would never be reached. */
static int kdc_cq_head_batch_set(void *data, u64 val)
{
	struct kdc_dev *kdev = data;

	WRITE_ONCE(kdev->cq_head_batch, clamp_t(u64, val, 1, kdev->depth - 1));
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(kdc_cq_head_batch_fops, kdc_cq_head_batch_get,
			 kdc_cq_head_batch_set, "%llu\n");

static int kdc_shadow_get(void *data, u64 *val)
{
	*val = !!((struct kdc_dev *)data)->dbbuf;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(kdc_shadow_fops, kdc_shadow_get, NULL, "%llu\n");

static void kdc_debugfs_init(struct kdc_dev *kdev)
{
	static const struct {
		const char *name;
		u32 offset;
		umode_t mode;
	} regs[] = {
		{ "service_ns", KDC_REG_SERVICE_NS, 0600 },
		{ "fault", KDC_REG_FAULT, 0600 },
		{ "fault_drop", KDC_REG_FAULT_DROP, 0600 },
		{ "fault_bad_cid", KDC_REG_FAULT_BAD_CID, 0600 },
		{ "fault_bad_dma", KDC_REG_FAULT_BAD_DMA, 0600 },
		{ "fault_hold", KDC_REG_FAULT_HOLD, 0600 },
		{ "scratch", KDC_REG_SCRATCH, 0600 },
		{ "device_aborted", KDC_REG_STAT_ABORTED, 0400 },
		{ "device_doorbells", KDC_REG_STAT_DOORBELLS, 0400 },
		{ "device_cq_doorbells", KDC_REG_STAT_CQ_DOORBELLS, 0400 },
		{ "device_irqs", KDC_REG_STAT_IRQS, 0400 },
	};
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(regs) != ARRAY_SIZE(kdev->dbg_regs));
	kdev->debugfs = debugfs_create_dir(dev_name(&kdev->dev), kdc_debugfs_root);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		kdev->dbg_regs[i].kdev = kdev;
		kdev->dbg_regs[i].offset = regs[i].offset;
		debugfs_create_file_unsafe(regs[i].name, regs[i].mode, kdev->debugfs,
					   &kdev->dbg_regs[i], &kdc_dbg_fops);
	}
	debugfs_create_file_unsafe("device_inflight", 0400, kdev->debugfs, kdev,
				   &kdc_inflight_fops);
	debugfs_create_file_unsafe("coalesce", 0600, kdev->debugfs, kdev,
				   &kdc_coalesce_fops);
	debugfs_create_file_unsafe("cq_head_batch", 0600, kdev->debugfs, kdev,
				   &kdc_cq_head_batch_fops);
	debugfs_create_file_unsafe("shadow_doorbells", 0400, kdev->debugfs, kdev,
				   &kdc_shadow_fops);
}

/* ---- probe / remove ---- */

static void kdc_dev_release(struct device *dev)
{
	struct kdc_dev *kdev = container_of(dev, struct kdc_dev, dev);

	if (kdev->minor >= 0)
		ida_free(&kdc_minors, kdev->minor);
	kfree(kdev->queues);
	kfree(kdev->cpu_queue);
	kfree(kdev);
}

static void kdc_teardown(struct kdc_dev *kdev)
{
	unsigned int i;

	kdc_free_irqs(kdev);
	for (i = 0; kdev->queues && i < kdev->nr_queues; i++)
		kdc_queue_free(kdev, &kdev->queues[i]);
	dma_pool_destroy(kdev->sgl_pool);
	dma_pool_destroy(kdev->bounce_pool);
	kdev->sgl_pool = kdev->bounce_pool = NULL;
	if (kdev->dbbuf)
		dma_free_coherent(&kdev->pdev->dev, PAGE_SIZE, kdev->dbbuf, kdev->dbbuf_dma);
	kdev->dbbuf = NULL;
	pci_free_irq_vectors(kdev->pdev);
}

static int kdc_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct irq_affinity affd = { .post_vectors = 1 };
	struct device *dev = &pdev->dev;
	struct kdc_dev *kdev;
	unsigned int i, nr, max_depth_log2, segs;
	int nvec, err;
	u32 cap;

	BUILD_BUG_ON(L1_CACHE_BYTES > KDC_CACHELINE ||
		     ARCH_DMA_MINALIGN > KDC_CACHELINE);
	BUILD_BUG_ON(KDC_SGL_BLOCK > PAGE_SIZE);
	BUILD_BUG_ON(KDC_MAX_QUEUES * KDC_DBBUF_SIZE > PAGE_SIZE || PAGE_SIZE % KDC_RING_ALIGN);
	kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return -ENOMEM;
	kdev->minor = ida_alloc_max(&kdc_minors, KDC_MINORS - 1, GFP_KERNEL);
	if (kdev->minor < 0) {
		err = kdev->minor;
		kfree(kdev);
		return err;
	}
	device_initialize(&kdev->dev);
	kdev->dev.release = kdc_dev_release;
	kdev->dev.class = &kdc_class;
	kdev->dev.parent = dev;
	kdev->dev.devt = MKDEV(MAJOR(kdc_devt), kdev->minor);
	err = dev_set_name(&kdev->dev, "kdc%d", kdev->minor);
	if (err)
		goto put;
	kdev->pdev = pdev;
	init_rwsem(&kdev->gone_lock);
	mutex_init(&kdev->files_mutex);
	mutex_init(&kdev->reset_lock);
	INIT_LIST_HEAD(&kdev->files);
	INIT_DELAYED_WORK(&kdev->watchdog, kdc_watchdog_fn);
	INIT_WORK(&kdev->recover_work, kdc_recover_fn);
	kdev->state = KDC_STATE_LIVE;

	err = pcim_enable_device(pdev);
	if (err)
		goto put;
	pci_set_master(pdev);
	kdev->regs = pcim_iomap_region(pdev, KDC_BAR_REGS, "kdc");
	if (IS_ERR(kdev->regs)) {
		err = PTR_ERR(kdev->regs);
		goto put;
	}
	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (err)
		goto put;
	dma_set_max_seg_size(dev, KDC_MAX_LENGTH);

	cap = kdc_rd(kdev, KDC_REG_CAP);
	segs = cap >> KDC_CAP_SEGS_SHIFT;
	max_depth_log2 = (cap >> KDC_CAP_DEPTH_SHIFT) & KDC_CAP_DEPTH_MASK;
	if (kdc_rd(kdev, KDC_REG_ID) != KDC_ID_VALUE ||
	    kdc_rd(kdev, KDC_REG_VERSION) >> 16 != KDC_VERSION_MAJOR ||
	    segs < KDC_DRV_SRC_SEGS || kdc_rd(kdev, KDC_REG_MAX_XFER) < KDC_MAX_LENGTH ||
	    !(cap & KDC_CAP_QUEUES_MASK) || max_depth_log2 < KDC_MIN_DEPTH_LOG2) {
		dev_err(dev, "unsupported device (cap %#x)\n", cap);
		err = -ENODEV;
		goto put;
	}
	nr = min3(cap & KDC_CAP_QUEUES_MASK, (u32)KDC_MAX_QUEUES,
		  max(max_queues, 1U));
	kdev->depth = clamp_t(unsigned int, rounddown_pow_of_two(max(queue_depth, 2U)),
			      2, 1U << max_depth_log2);
	kdev->bounce_max = bounce_max < 0 ? kdc_bounce_default(dev) :
			   min_t(unsigned int, bounce_max, 65536);
	kdev->coalesce = min(coalesce_usecs, 0xffffU) << 16 | min(coalesce_count, 0xffffU);
	kdev->cq_head_batch = clamp(cq_head_batch ?: kdev->depth / 4, 1U, kdev->depth - 1);

	err = kdc_hw_start(kdev);
	if (err) {
		dev_err(dev, "device did not become ready\n");
		goto put;
	}
	nvec = pci_alloc_irq_vectors_affinity(pdev, 2, nr + 1,
					      PCI_IRQ_MSIX | PCI_IRQ_AFFINITY,
					      &affd);
	if (nvec < 0) {
		err = nvec;
		goto disable;
	}
	kdev->nr_queues = nvec - 1;
	kdev->queues = kcalloc(kdev->nr_queues, sizeof(*kdev->queues), GFP_KERNEL);
	kdev->cpu_queue = kcalloc(nr_cpu_ids, sizeof(*kdev->cpu_queue), GFP_KERNEL);
	if (!kdev->queues || !kdev->cpu_queue) {
		err = -ENOMEM;
		goto teardown;
	}
	if (shadow_doorbells && (kdc_rd(kdev, KDC_REG_FEATURES) & KDC_FEAT_SHADOW_DB)) {
		kdev->dbbuf = dma_alloc_coherent(dev, PAGE_SIZE, &kdev->dbbuf_dma, GFP_KERNEL);
		if (!kdev->dbbuf) {
			err = -ENOMEM;
			goto teardown;
		}
		kdc_dbbuf_setup(kdev);
	}
	for (i = 0; i < kdev->nr_queues; i++) {
		err = kdc_queue_alloc(kdev, &kdev->queues[i], i);
		if (err)
			goto teardown;
		kdev->queues[i].db = kdev->dbbuf ? &kdev->dbbuf[i] : NULL;
		err = kdc_queue_hw_setup(kdev, &kdev->queues[i]);
		if (err)
			goto teardown;
	}
	kdc_map_cpus(kdev);
	kdev->sgl_pool = dma_pool_create("kdc-sgl", dev, KDC_SGL_BLOCK,
					 KDC_CACHELINE, 0);
	if (kdev->bounce_max)
		kdev->bounce_pool = dma_pool_create("kdc-bounce", dev,
						    2 * kdev->bounce_max, 64, 0);
	if (!kdev->sgl_pool || (kdev->bounce_max && !kdev->bounce_pool)) {
		err = -ENOMEM;
		goto teardown;
	}
	err = kdc_request_irqs(kdev);
	if (err)
		goto teardown;
	cdev_init(&kdev->cdev, &kdc_fops);
	kdev->cdev.owner = THIS_MODULE;
	pci_set_drvdata(pdev, kdev);
	err = cdev_device_add(&kdev->cdev, &kdev->dev);
	if (err)
		goto teardown;
	kdc_debugfs_init(kdev);
	dev_info(dev, "%s: %u queues x %u entries, bounce <= %u bytes%s\n",
		 dev_name(&kdev->dev), kdev->nr_queues, kdev->depth,
		 kdev->bounce_max, kdev->dbbuf ? ", shadow doorbells" : "");
	return 0;

teardown:
	kdc_hw_stop(kdev);
	kdc_free_irqs(kdev);
	/* The error IRQ may have queued recovery before probe gave up. */
	WRITE_ONCE(kdev->removing, true);
	disable_work_sync(&kdev->recover_work);
	disable_delayed_work_sync(&kdev->watchdog);
	kdc_teardown(kdev);
	goto put;
disable:
	kdc_hw_stop(kdev);
put:
	put_device(&kdev->dev);
	return err;
}

static void kdc_remove(struct pci_dev *pdev)
{
	struct kdc_dev *kdev = pci_get_drvdata(pdev);
	LIST_HEAD(list);

	/*
	 * Recovery may be waiting in pci_try_reset_function(), which fails
	 * while we hold the device lock, so these waits are bounded.
	 */
	WRITE_ONCE(kdev->removing, true);
	disable_delayed_work_sync(&kdev->watchdog);
	disable_work_sync(&kdev->recover_work);
	debugfs_remove_recursive(kdev->debugfs);
	cdev_device_del(&kdev->cdev, &kdev->dev);
	down_write(&kdev->gone_lock);
	WRITE_ONCE(kdev->dead, true);
	up_write(&kdev->gone_lock);

	mutex_lock(&kdev->reset_lock);
	if (kdev->state == KDC_STATE_LIVE && !kdc_device_gone(kdev) &&
	    kdc_hw_stop(kdev))
		dev_warn(&pdev->dev, "device did not acknowledge disable\n");
	/* Whatever the device did above, it can no longer DMA after this. */
	pci_clear_master(pdev);
	kdc_free_irqs(kdev);
	mutex_lock(&kdev->files_mutex);
	kdc_collect_all(kdev, &list, -ENODEV, KDC_ABORT_REMOVE);
	mutex_unlock(&kdev->files_mutex);
	kdc_end_list(kdev, &list);
	kdc_wake_files(kdev);
	mutex_lock(&kdev->files_mutex);
	/*
	 * Every command has ended, so none holds a registered buffer; open files
	 * keep theirs pinned until close(), but nothing may stay mapped for a
	 * device that is going away. No ioctl runs any more (dead is set).
	 */
	kdc_fixed_unmap_all(kdev);
	kdc_teardown(kdev);
	mutex_unlock(&kdev->files_mutex);
	mutex_unlock(&kdev->reset_lock);
	put_device(&kdev->dev);
}

static const struct pci_device_id kdc_ids[] = {
	{ PCI_DEVICE(KDC_PCI_VENDOR_ID, KDC_PCI_DEVICE_ID) },
	{}
};
MODULE_DEVICE_TABLE(pci, kdc_ids);

static const struct pci_error_handlers kdc_err_handler = {
	.reset_prepare = kdc_reset_prepare,
	.reset_done = kdc_reset_done,
};

static struct pci_driver kdc_driver = {
	.name = "kdc",
	.id_table = kdc_ids,
	.probe = kdc_probe,
	.remove = kdc_remove,
	.err_handler = &kdc_err_handler,
};

static int __init kdc_init(void)
{
	int err;

	kdc_wq = alloc_workqueue("kdc", WQ_UNBOUND, 0);
	if (!kdc_wq)
		return -ENOMEM;
	err = alloc_chrdev_region(&kdc_devt, 0, KDC_MINORS, "kdc");
	if (err)
		goto unwq;
	err = class_register(&kdc_class);
	if (err)
		goto unregion;
	kdc_debugfs_root = debugfs_create_dir("kdc", NULL);
	debugfs_create_atomic_t("cmds", 0444, kdc_debugfs_root, &kdc_live_cmds);
	debugfs_create_atomic_t("pinned_pages", 0444, kdc_debugfs_root,
				&kdc_global_pinned);
	debugfs_create_atomic_t("mapped_sgts", 0444, kdc_debugfs_root,
				&kdc_global_mapped);
	debugfs_create_atomic_t("dst_ranges", 0444, kdc_debugfs_root,
				&kdc_dst_ranges);
	err = pci_register_driver(&kdc_driver);
	if (err)
		goto unclass;
	return 0;
unclass:
	debugfs_remove_recursive(kdc_debugfs_root);
	class_unregister(&kdc_class);
unregion:
	unregister_chrdev_region(kdc_devt, KDC_MINORS);
unwq:
	destroy_workqueue(kdc_wq);
	return err;
}

static void __exit kdc_exit(void)
{
	pci_unregister_driver(&kdc_driver);
	debugfs_remove_recursive(kdc_debugfs_root);
	class_unregister(&kdc_class);
	unregister_chrdev_region(kdc_devt, KDC_MINORS);
	destroy_workqueue(kdc_wq);
}
module_init(kdc_init);
module_exit(kdc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for the emulated KDC DMA engine (QEMU lab device)");
