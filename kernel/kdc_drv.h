/* SPDX-License-Identifier: GPL-2.0 */
#ifndef KDC_DRV_H
#define KDC_DRV_H
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/interval_tree.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include "kdc_state.h"

/*
 * One SG list block per zero-copy command, from a coherent DMA pool:
 * 128 source entries, 120 destination entries (head + body + tail), then the
 * head and tail bounce lines the device writes instead of partial user lines.
 */
#define KDC_DRV_SRC_SEGS 128
#define KDC_DRV_DST_SEGS 120
#define KDC_SGL_DST_OFF (KDC_DRV_SRC_SEGS * KDC_SGE_SIZE)
#define KDC_SGL_HEAD_OFF (KDC_SGL_DST_OFF + KDC_DRV_DST_SEGS * KDC_SGE_SIZE)
#define KDC_SGL_TAIL_OFF (KDC_SGL_HEAD_OFF + KDC_CACHELINE)
#define KDC_SGL_BLOCK (KDC_SGL_TAIL_OFF + KDC_CACHELINE)
#define KDC_MINORS 32

struct kdc_dev;
struct kdc_ctx;
struct kdc_cmd;

/*
 * Lock order: kdev->reset_lock -> kdev->files_mutex -> ctx->lock -> queue->lock.
 * queue->lock protects the rings, the slot table, the pending list and the
 * phase of commands that are PENDING or DEVICE. ctx->lock protects the file's
 * lists and counters and the STAGED/COMPLETING/DONE transitions. A reader that
 * must see a stable phase holds both. Nothing that can sleep (unpin, unmap of
 * a large table, copy to user) runs under either spinlock. reset_lock
 * serialises every reset path (recovery work, reset_prepare..reset_done and
 * remove's teardown).
 */
struct kdc_queue {
	struct kdc_dev *kdev;
	u16 qid, depth;
	struct kdc_hw_sqe *sq;
	struct kdc_hw_cqe *cq;
	dma_addr_t sq_dma, cq_dma;
	void __iomem *sq_db, *cq_db, *abort_reg;
	struct kdc_hw_dbbuf *db; /* shadow doorbell entry, or NULL */
	spinlock_t lock;
	u16 sq_tail, cq_head, outstanding;
	u16 cq_head_db; /* last value written to the CQ head doorbell */
	u8 phase;
	bool stopped; /* no doorbells: recovery, failure or removal */
	struct kdc_cmd **slots; /* by tag: commands the device owns */
	u8 *gen; /* by tag: generation of the last cid issued */
	unsigned long *tag_map;
	struct list_head pending; /* ready, waiting for ring space or a restart */
	u64 submitted, completed;
};

struct kdc_dbg_reg {
	struct kdc_dev *kdev;
	u32 offset;
};

struct kdc_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	struct device dev; /* the /dev/kdcN class device; owns this struct */
	struct cdev cdev;
	int minor;
	unsigned int nr_queues, depth, bounce_max;
	u32 coalesce; /* KDC_Q_COALESCE value for every queue: usecs << 16 | count */
	u32 cq_head_batch; /* reaped entries per CQ head doorbell, 1..depth - 1 */
	struct kdc_queue *queues;
	unsigned int *cpu_queue;
	struct dma_pool *sgl_pool, *bounce_pool;
	struct kdc_hw_dbbuf *dbbuf; /* shadow doorbell page (KDC_FEAT_SHADOW_DB), or NULL */
	dma_addr_t dbbuf_dma;
	struct rw_semaphore gone_lock; /* ioctls hold it shared; remove takes it to set dead */
	bool dead; /* removed */
	struct mutex files_mutex;
	struct list_head files;
	struct dentry *debugfs;
	struct kdc_dbg_reg dbg_regs[10];
	bool irqs_requested;
	/* recovery, kdc_recover.c */
	struct mutex reset_lock;
	struct delayed_work watchdog;
	struct work_struct recover_work;
	u32 state; /* KDC_STATE_LIVE, _FAILED or _GONE; READ_ONCE outside reset_lock */
	bool recovering; /* queues stopped by a reset in progress */
	bool removing;
	bool recovery_flr; /* the next reset_done is the FLR recovery asked for */
	unsigned long reset_seq; /* bumped by every reset_done */
	atomic64_t pinned_pages, mapped_sgts, open_files, submitted, completed;
	atomic64_t aborts_sent, bad_completions, errors;
	atomic64_t timeouts, resets, flrs, failures, requeued, overlaps;
};

/* One physically contiguous run of a destination, in the overlap tracker. */
struct kdc_dst_range {
	struct interval_tree_node it;
};

/* Destination byte ranges by physical address. */
struct kdc_tracker {
	spinlock_t lock;
	struct rb_root_cached tree;
};

/*
 * A registered buffer (KDC_IOC_REGISTER): pinned once and mapped page by page
 * for DMA_BIDIRECTIONAL. users counts commands from submit until their
 * kdc_cmd_release(), i.e. while the device may still reach the buffer; it is
 * unmapped only when that is zero, or by remove() once the device has stopped.
 * Its pages are in the module-wide tracker for as long as it is registered,
 * so no other destination can share them; fixed commands inside it are
 * checked against each other in fixed_dst.
 */
struct kdc_regbuf {
	u64 uaddr, len;
	unsigned int npages, nmapped, nr_ranges;
	struct page **pages;
	dma_addr_t *dma;
	struct kdc_dst_range *ranges;
	struct kdc_tracker fixed_dst;
	bool mapped, tracked;
	atomic_t users;
};

struct kdc_cmd {
	struct list_head node; /* ctx->staged, queue->pending, ctx->done or a local list */
	struct list_head ctx_node; /* ctx->live until read() consumes it */
	struct kdc_ctx *ctx;
	struct kdc_queue *q;
	enum kdc_phase phase;
	enum kdc_abort_reason abort_reason;
	bool abort_sent, use_bounce, ended_by_reset;
	bool fixed; /* KDC_SUBMIT_FIXED: pages belong to registered buffers */
	struct kdc_regbuf *src_buf, *dst_buf; /* held until kdc_cmd_release() */
	u8 retries;
	u64 id, epoch, quota_bytes, dst;
	u32 length, head_len, tail_len;
	u16 cid, tag;
	unsigned long timeout, deadline, abort_deadline; /* jiffies */
	struct kdc_hw_sqe sqe; /* built at submit; cid filled at dispatch */
	struct page **src_pages, **dst_pages;
	unsigned int src_npages, dst_npages, dst_offset;
	struct sg_table src_sgt, dst_sgt;
	bool src_mapped, dst_mapped;
	struct kdc_dst_range *ranges;
	struct kdc_tracker *tracker; /* module-wide, or the registered buffer's */
	unsigned int nr_ranges;
	bool ranges_tracked;
	void *sgl, *bounce_buf;
	dma_addr_t sgl_dma, bounce_dma;
	int status;
	u32 crc, done_bytes;
	u16 hw_status;
};

struct kdc_ctx {
	struct kdc_dev *kdev;
	spinlock_t lock;
	wait_queue_head_t wq;
	struct list_head live, staged, done;
	struct list_head dev_node; /* kdev->files, under files_mutex */
	struct kdc_regbuf *bufs[KDC_MAX_FIXED_BUFS]; /* under ctx->lock */
	u64 epoch, bytes, pinned_pages;
	u32 nr_live, nr_staged, nr_ready, nr_inflight;
	bool closing, resetting;
};

/* module-wide counters (debugfs kdc/), for leak checks across removal */
extern atomic_t kdc_live_cmds, kdc_global_pinned, kdc_global_mapped,
	kdc_dst_ranges;

/* kdc_cmd.c */
struct kdc_cmd *kdc_cmd_prepare(struct kdc_dev *kdev, const struct kdc_submit *s,
				struct kdc_regbuf *src_buf, struct kdc_regbuf *dst_buf,
				int *errp);
struct kdc_regbuf *kdc_regbuf_create(struct kdc_dev *kdev, u64 uaddr, u64 len, int *errp);
void kdc_regbuf_unmap(struct kdc_dev *kdev, struct kdc_regbuf *b);
void kdc_regbuf_destroy(struct kdc_dev *kdev, struct kdc_regbuf *b);
static inline unsigned int kdc_cmd_pinned(const struct kdc_cmd *cmd)
{
	return cmd->fixed ? 0 : cmd->src_npages + cmd->dst_npages;
}
void kdc_cmd_release(struct kdc_dev *kdev, struct kdc_cmd *cmd, bool copy_back);
void kdc_cmd_free(struct kdc_cmd *cmd);
int kdc_fill_hw_sgl(struct kdc_hw_sge *out, unsigned int max,
		    struct sg_table *sgt, u32 length);

/* kdc_pci.c */
extern struct workqueue_struct *kdc_wq;
extern unsigned int cmd_timeout_ms, abort_timeout_ms, reset_timeout_ms;
void kdc_queue_dispatch_locked(struct kdc_queue *q);
void kdc_send_abort_locked(struct kdc_queue *q, struct kdc_cmd *cmd,
			   enum kdc_abort_reason reason);
void kdc_queue_reap_locked(struct kdc_queue *q, struct list_head *done);
void kdc_finish_list(struct kdc_dev *kdev, struct list_head *list);
void kdc_end_list(struct kdc_dev *kdev, struct list_head *list);
void kdc_collect_all(struct kdc_dev *kdev, struct list_head *list, int base,
		     enum kdc_abort_reason reason);
int kdc_hw_stop(struct kdc_dev *kdev);
int kdc_hw_start(struct kdc_dev *kdev);
int kdc_restart_hw(struct kdc_dev *kdev);
void kdc_cmd_end(struct kdc_cmd *cmd, int base, struct list_head *list);
bool kdc_device_gone(struct kdc_dev *kdev);
void kdc_wake_files(struct kdc_dev *kdev);

/* kdc_recover.c */
void kdc_watchdog_fn(struct work_struct *work);
void kdc_recover_fn(struct work_struct *work);
void kdc_arm_watchdog(struct kdc_dev *kdev);
void kdc_reset_prepare(struct pci_dev *pdev);
void kdc_reset_done(struct pci_dev *pdev);
void kdc_fail_locked(struct kdc_dev *kdev, u32 state);

/* kdc_fops.c */
void kdc_cmd_publish(struct kdc_cmd *cmd, int status);
extern const struct file_operations kdc_fops;
#endif
