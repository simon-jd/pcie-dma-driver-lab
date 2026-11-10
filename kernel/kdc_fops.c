// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/kdcN file operations: the per-file ownership contract.
 *
 * Each open file has its own request ids, epoch, quota and completion list.
 * submit pins and maps first, then admits under the file lock. cancel releases
 * a command at once only while the device cannot see it; a device-owned
 * command gets a device abort and keeps its buffers until the device posts a
 * completion or is reset. reset and close wait for every device-owned command;
 * the wait is bounded because an abort left unanswered for abort_timeout_ms
 * escalates to a device reset (kdc_recover.c).
 */
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "kdc_drv.h"

static bool kdc_ctx_quiet(struct kdc_ctx *ctx)
{
	bool quiet;

	spin_lock(&ctx->lock);
	quiet = !ctx->nr_inflight;
	spin_unlock(&ctx->lock);
	return quiet;
}

/* Called once per command, after its DMA resources have been released. */
void kdc_cmd_publish(struct kdc_cmd *cmd, int status)
{
	struct kdc_ctx *ctx = cmd->ctx;

	spin_lock(&ctx->lock);
	cmd->status = status;
	ctx->bytes -= cmd->quota_bytes;
	ctx->pinned_pages -= cmd->src_npages + cmd->dst_npages;
	ctx->nr_inflight--;
	WRITE_ONCE(cmd->phase, KDC_DONE);
	list_add_tail(&cmd->node, &ctx->done);
	ctx->nr_ready++;
	/*
	 * Wake under the lock: once a closer sees nr_inflight == 0 and takes
	 * the lock, nothing here touches ctx again.
	 */
	wake_up_all(&ctx->wq);
	spin_unlock(&ctx->lock);
}

static struct kdc_cmd *kdc_find(struct kdc_ctx *ctx, u64 id, const u64 *epoch)
{
	struct kdc_cmd *cmd;

	lockdep_assert_held(&ctx->lock);
	list_for_each_entry(cmd, &ctx->live, ctx_node)
		if (cmd->id == id && (!epoch || cmd->epoch == *epoch) &&
		    READ_ONCE(cmd->phase) != KDC_FREE)
			return cmd;
	return NULL;
}

/*
 * Take every driver-owned command off its list and ask the device to abort the
 * ones it owns. Caller holds ctx->lock. The commands moved to @release must be
 * released and published by the caller after dropping the lock.
 */
static void kdc_ctx_abort_all(struct kdc_ctx *ctx, enum kdc_abort_reason reason,
			      struct list_head *release)
{
	struct kdc_cmd *cmd;

	lockdep_assert_held(&ctx->lock);
	list_for_each_entry(cmd, &ctx->live, ctx_node) {
		struct kdc_queue *q = cmd->q;

		switch (READ_ONCE(cmd->phase)) {
		case KDC_STAGED:
			list_move_tail(&cmd->node, release);
			ctx->nr_staged--;
			cmd->abort_reason = reason;
			WRITE_ONCE(cmd->phase, KDC_COMPLETING);
			break;
		case KDC_PENDING:
		case KDC_DEVICE:
			spin_lock(&q->lock);
			if (READ_ONCE(cmd->phase) == KDC_PENDING) {
				list_move_tail(&cmd->node, release);
				cmd->abort_reason = reason;
				WRITE_ONCE(cmd->phase, KDC_COMPLETING);
			} else {
				kdc_send_abort_locked(q, cmd, reason);
			}
			spin_unlock(&q->lock);
			break;
		default:
			break;
		}
	}
}

static void kdc_release_unsent(struct kdc_dev *kdev, struct list_head *list)
{
	struct kdc_cmd *cmd, *tmp;

	list_for_each_entry_safe(cmd, tmp, list, node) {
		list_del_init(&cmd->node);
		kdc_cmd_release(kdev, cmd, false);
		kdc_cmd_publish(cmd, kdc_unsent_errno(cmd->abort_reason));
	}
}

static void kdc_wait_quiet(struct kdc_ctx *ctx)
{
	/*
	 * Device-owned buffers stay pinned until the device answers or is
	 * reset; recovery bounds this. The warning means that bound failed.
	 */
	while (!wait_event_timeout(ctx->wq, kdc_ctx_quiet(ctx), 10 * HZ))
		dev_warn(&ctx->kdev->pdev->dev,
			 "still waiting for %u device-owned commands\n",
			 READ_ONCE(ctx->nr_inflight));
}

static int kdc_open(struct inode *inode, struct file *file)
{
	struct kdc_dev *kdev = container_of(inode->i_cdev, struct kdc_dev, cdev);
	struct kdc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->kdev = kdev;
	spin_lock_init(&ctx->lock);
	init_waitqueue_head(&ctx->wq);
	INIT_LIST_HEAD(&ctx->live);
	INIT_LIST_HEAD(&ctx->staged);
	INIT_LIST_HEAD(&ctx->done);
	ctx->epoch = 1;
	down_read(&kdev->gone_lock);
	if (kdev->dead) {
		up_read(&kdev->gone_lock);
		kfree(ctx);
		return -ENODEV;
	}
	get_device(&kdev->dev);
	mutex_lock(&kdev->files_mutex);
	list_add_tail(&ctx->dev_node, &kdev->files);
	mutex_unlock(&kdev->files_mutex);
	atomic64_inc(&kdev->open_files);
	up_read(&kdev->gone_lock);
	file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int kdc_release(struct inode *inode, struct file *file)
{
	struct kdc_ctx *ctx = file->private_data;
	struct kdc_dev *kdev = ctx->kdev;
	struct kdc_cmd *cmd, *tmp;
	LIST_HEAD(release);

	/*
	 * files_mutex excludes remove's teardown until the commands taken off
	 * the lists here have gone back to the DMA pools.
	 */
	mutex_lock(&kdev->files_mutex);
	spin_lock(&ctx->lock);
	ctx->closing = true;
	if (!READ_ONCE(kdev->dead))
		kdc_ctx_abort_all(ctx, KDC_ABORT_CLOSE, &release);
	spin_unlock(&ctx->lock);
	kdc_release_unsent(kdev, &release);
	mutex_unlock(&kdev->files_mutex);
	kdc_wait_quiet(ctx);
	/* The last publisher has left ctx->lock once we can take it. */
	spin_lock(&ctx->lock);
	spin_unlock(&ctx->lock);
	list_for_each_entry_safe(cmd, tmp, &ctx->live, ctx_node) {
		list_del(&cmd->ctx_node);
		kdc_cmd_free(cmd);
	}
	mutex_lock(&kdev->files_mutex);
	/*
	 * No command is left, so no command holds a registered buffer. Under
	 * files_mutex: remove() unmaps them from a departing device itself.
	 */
	kdc_fixed_close(ctx);
	list_del(&ctx->dev_node);
	mutex_unlock(&kdev->files_mutex);
	atomic64_dec(&kdev->open_files);
	put_device(&kdev->dev);
	kfree(ctx);
	return 0;
}

static long kdc_ioctl_submit(struct kdc_ctx *ctx, void __user *arg)
{
	struct kdc_dev *kdev = ctx->kdev;
	struct kdc_submit s;
	struct kdc_queue *q;
	struct kdc_cmd *cmd;
	int err;

	if (copy_from_user(&s, arg, sizeof(s)))
		return -EFAULT;
	if (s.flags & KDC_SUBMIT_FIXED)
		return kdc_submit_fixed(ctx, &s);
	err = kdc_submit_check(&s, kdev->nr_queues);
	if (err)
		return err;
	/* Cheap early rejection; repeated authoritatively below. */
	spin_lock(&ctx->lock);
	err = kdc_admission(s.epoch, ctx->epoch, ctx->resetting, ctx->closing,
			    READ_ONCE(kdev->state)) ?:
	      kdc_quota(ctx->nr_live, ctx->bytes, 2ull * s.length);
	if (!err && kdc_find(ctx, s.id, NULL))
		err = -EEXIST;
	spin_unlock(&ctx->lock);
	if (err)
		return err;

	cmd = kdc_cmd_prepare(kdev, &s, &err);
	if (!cmd)
		return err;
	q = &kdev->queues[s.queue == KDC_QUEUE_AUTO ?
				  kdev->cpu_queue[raw_smp_processor_id()] :
				  s.queue];
	cmd->q = q;
	cmd->ctx = ctx;

	/* Authoritative: kdc_fail_locked() sets the state before collecting. */
	spin_lock(&ctx->lock);
	err = kdc_admission(s.epoch, ctx->epoch, ctx->resetting, ctx->closing,
			    READ_ONCE(kdev->state)) ?:
	      kdc_quota(ctx->nr_live, ctx->bytes, cmd->quota_bytes);
	if (!err && kdc_find(ctx, s.id, NULL))
		err = -EEXIST;
	if (err) {
		spin_unlock(&ctx->lock);
		kdc_cmd_release(kdev, cmd, false);
		kdc_cmd_free(cmd);
		return err;
	}
	cmd->epoch = ctx->epoch;
	list_add_tail(&cmd->ctx_node, &ctx->live);
	ctx->nr_live++;
	ctx->nr_inflight++;
	ctx->bytes += cmd->quota_bytes;
	ctx->pinned_pages += cmd->src_npages + cmd->dst_npages;
	atomic64_inc(&kdev->submitted);
	if (s.flags & KDC_SUBMIT_BATCH) {
		WRITE_ONCE(cmd->phase, KDC_STAGED);
		list_add_tail(&cmd->node, &ctx->staged);
		ctx->nr_staged++;
	} else {
		spin_lock(&q->lock);
		WRITE_ONCE(cmd->phase, KDC_PENDING);
		list_add_tail(&cmd->node, &q->pending);
		kdc_queue_dispatch_locked(q);
		spin_unlock(&q->lock);
	}
	spin_unlock(&ctx->lock);
	return 0;
}

static long kdc_ioctl_flush(struct kdc_ctx *ctx)
{
	struct kdc_dev *kdev = ctx->kdev;
	DECLARE_BITMAP(touched, KDC_MAX_QUEUES) = { 0 };
	struct kdc_cmd *cmd, *tmp;
	unsigned int i;

	spin_lock(&ctx->lock);
	list_for_each_entry_safe(cmd, tmp, &ctx->staged, node) {
		struct kdc_queue *q = cmd->q;

		spin_lock(&q->lock);
		WRITE_ONCE(cmd->phase, KDC_PENDING);
		list_move_tail(&cmd->node, &q->pending);
		spin_unlock(&q->lock);
		__set_bit(q->qid, touched);
	}
	ctx->nr_staged = 0;
	/* One doorbell per queue for the whole batch. */
	for_each_set_bit(i, touched, KDC_MAX_QUEUES) {
		spin_lock(&kdev->queues[i].lock);
		kdc_queue_dispatch_locked(&kdev->queues[i]);
		spin_unlock(&kdev->queues[i].lock);
	}
	spin_unlock(&ctx->lock);
	return 0;
}

static long kdc_ioctl_cancel(struct kdc_ctx *ctx, void __user *arg)
{
	struct kdc_cancel c;
	enum kdc_cancel_action action;
	struct kdc_queue *q;
	struct kdc_cmd *cmd;

	if (copy_from_user(&c, arg, sizeof(c)))
		return -EFAULT;
	if (c.version != KDC_ABI_VERSION)
		return -EPROTO;
	if (c.size != sizeof(c) || c.reserved)
		return -EINVAL;
	spin_lock(&ctx->lock);
	cmd = kdc_find(ctx, c.id, &c.epoch);
	if (!cmd) {
		spin_unlock(&ctx->lock);
		return -ENOENT;
	}
	q = cmd->q;
	spin_lock(&q->lock);
	action = kdc_cancel_decide(READ_ONCE(cmd->phase), cmd->abort_sent,
				   cmd->status);
	if (action == KDC_CANCEL_NOW) {
		if (READ_ONCE(cmd->phase) == KDC_STAGED)
			ctx->nr_staged--;
		list_del_init(&cmd->node);
		cmd->abort_reason = KDC_ABORT_CANCEL;
		WRITE_ONCE(cmd->phase, KDC_COMPLETING);
	} else if (action == KDC_CANCEL_SEND_ABORT) {
		kdc_send_abort_locked(q, cmd, KDC_ABORT_CANCEL);
	}
	spin_unlock(&q->lock);
	spin_unlock(&ctx->lock);
	if (action == KDC_CANCEL_NOW) {
		/* The device never saw it: release now. */
		kdc_cmd_release(ctx->kdev, cmd, false);
		kdc_cmd_publish(cmd, -ECANCELED);
	}
	return kdc_cancel_errno(action);
}

static long kdc_ioctl_reset(struct kdc_ctx *ctx)
{
	LIST_HEAD(release);

	spin_lock(&ctx->lock);
	if (ctx->resetting || ctx->epoch == U64_MAX) {
		spin_unlock(&ctx->lock);
		return ctx->resetting ? -EBUSY : -EOVERFLOW;
	}
	ctx->resetting = true;
	kdc_ctx_abort_all(ctx, KDC_ABORT_RESET, &release);
	spin_unlock(&ctx->lock);
	kdc_release_unsent(ctx->kdev, &release);
	/* Do not hold off remove while waiting on the device. */
	up_read(&ctx->kdev->gone_lock);
	kdc_wait_quiet(ctx);
	down_read(&ctx->kdev->gone_lock);
	spin_lock(&ctx->lock);
	ctx->epoch++;
	ctx->resetting = false;
	spin_unlock(&ctx->lock);
	return 0;
}

static long kdc_ioctl_stats(struct kdc_ctx *ctx, void __user *arg)
{
	struct kdc_dev *kdev = ctx->kdev;
	struct kdc_stats st = { .version = KDC_ABI_VERSION, .size = sizeof(st) };
	struct kdc_cmd *cmd;
	unsigned int i;

	spin_lock(&ctx->lock);
	st.epoch = ctx->epoch;
	st.live = ctx->nr_live;
	st.staged = ctx->nr_staged;
	st.ready = ctx->nr_ready;
	st.resetting = ctx->resetting;
	st.bytes = ctx->bytes;
	st.pinned_pages = ctx->pinned_pages;
	list_for_each_entry(cmd, &ctx->live, ctx_node) {
		enum kdc_phase phase = READ_ONCE(cmd->phase);

		st.pending += phase == KDC_PENDING;
		st.device_owned += phase == KDC_DEVICE || phase == KDC_COMPLETING;
	}
	spin_unlock(&ctx->lock);
	st.dev_pinned_pages = atomic64_read(&kdev->pinned_pages);
	st.dev_mapped_sgts = atomic64_read(&kdev->mapped_sgts);
	st.dev_open_files = atomic64_read(&kdev->open_files);
	st.dev_submitted = atomic64_read(&kdev->submitted);
	st.dev_completed = atomic64_read(&kdev->completed);
	st.dev_aborts_sent = atomic64_read(&kdev->aborts_sent);
	st.dev_bad_completions = atomic64_read(&kdev->bad_completions);
	st.dev_errors = atomic64_read(&kdev->errors);
	st.dev_state = READ_ONCE(kdev->state);
	if (st.dev_state == KDC_STATE_LIVE && READ_ONCE(kdev->recovering))
		st.dev_state = KDC_STATE_RECOVERING;
	st.dev_timeouts = atomic64_read(&kdev->timeouts);
	st.dev_resets = atomic64_read(&kdev->resets);
	st.dev_flrs = atomic64_read(&kdev->flrs);
	st.dev_failures = atomic64_read(&kdev->failures);
	st.dev_requeued = atomic64_read(&kdev->requeued);
	st.dev_overlaps = atomic64_read(&kdev->overlaps);
	st.nr_queues = kdev->nr_queues;
	st.queue_depth = kdev->depth;
	st.bounce_max = kdev->bounce_max;
	st.max_length = KDC_MAX_LENGTH;
	for (i = 0; i < kdev->nr_queues && i < KDC_STATS_QUEUES; i++) {
		spin_lock(&kdev->queues[i].lock);
		st.queue_submitted[i] = kdev->queues[i].submitted;
		spin_unlock(&kdev->queues[i].lock);
	}
	return copy_to_user(arg, &st, sizeof(st)) ? -EFAULT : 0;
}

static long kdc_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct kdc_ctx *ctx = file->private_data;
	struct kdc_dev *kdev = ctx->kdev;
	void __user *uarg = (void __user *)arg;
	long ret;

	down_read(&kdev->gone_lock);
	if (kdev->dead) {
		up_read(&kdev->gone_lock);
		return -ENODEV;
	}
	switch (command) {
	case KDC_IOC_SUBMIT:
		ret = kdc_ioctl_submit(ctx, uarg);
		break;
	case KDC_IOC_CANCEL:
		ret = kdc_ioctl_cancel(ctx, uarg);
		break;
	case KDC_IOC_RESET:
		ret = kdc_ioctl_reset(ctx);
		break;
	case KDC_IOC_FLUSH:
		ret = kdc_ioctl_flush(ctx);
		break;
	case KDC_IOC_STATS:
		ret = kdc_ioctl_stats(ctx, uarg);
		break;
	case KDC_IOC_REGISTER:
		ret = kdc_ioctl_register(ctx, uarg);
		break;
	case KDC_IOC_UNREGISTER:
		ret = kdc_ioctl_unregister(ctx, uarg);
		break;
	default:
		ret = -ENOTTY;
	}
	up_read(&kdev->gone_lock);
	return ret;
}

static void kdc_fill_completion(struct kdc_completion *c, struct kdc_cmd *cmd)
{
	*c = (struct kdc_completion){
		.id = cmd->id,
		.epoch = cmd->epoch,
		.status = cmd->status,
		.crc = cmd->status ? 0 : cmd->crc,
		.length = cmd->done_bytes,
		.queue = cmd->q->qid,
		.hw_status = cmd->hw_status,
		.flags = (cmd->use_bounce ? KDC_COMPL_BOUNCE : 0) |
			 (cmd->hw_status == KDC_SC_ABORTED ? KDC_COMPL_ABORTED : 0) |
			 (cmd->ended_by_reset ? KDC_COMPL_RESET : 0),
	};
}

/*
 * The device is gone and this file has nothing left in flight, so no further
 * completion can arrive. Checking only the device would race with removal or
 * failure, which marks the device first and ends its commands afterwards.
 */
static bool kdc_ctx_finished(struct kdc_ctx *ctx)
{
	return (READ_ONCE(ctx->kdev->dead) ||
		READ_ONCE(ctx->kdev->state) == KDC_STATE_GONE) &&
	       !READ_ONCE(ctx->nr_inflight);
}

/*
 * Returns as many completions as fit; blocks only for the first one. Once the
 * device is gone and every completion has been read, returns -ENODEV.
 */
static ssize_t kdc_read(struct file *file, char __user *buf, size_t size,
			loff_t *off)
{
	struct kdc_ctx *ctx = file->private_data;
	size_t n = 0, max = size / sizeof(struct kdc_completion);
	int err;

	if (!max)
		return -EINVAL;
	while (n < max) {
		struct kdc_completion c;
		struct kdc_cmd *cmd;

		spin_lock(&ctx->lock);
		cmd = list_first_entry_or_null(&ctx->done, struct kdc_cmd, node);
		if (cmd) {
			list_del_init(&cmd->node);
			ctx->nr_ready--;
			WRITE_ONCE(cmd->phase, KDC_FREE);
		}
		spin_unlock(&ctx->lock);
		if (!cmd) {
			if (n)
				break;
			if (kdc_ctx_finished(ctx))
				return -ENODEV;
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			err = wait_event_interruptible(ctx->wq,
						       READ_ONCE(ctx->nr_ready) ||
						       kdc_ctx_finished(ctx));
			if (err)
				return err;
			continue;
		}
		kdc_fill_completion(&c, cmd);
		if (copy_to_user(buf + n * sizeof(c), &c, sizeof(c))) {
			spin_lock(&ctx->lock);
			WRITE_ONCE(cmd->phase, KDC_DONE);
			list_add(&cmd->node, &ctx->done);
			ctx->nr_ready++;
			spin_unlock(&ctx->lock);
			return n ? n * sizeof(c) : -EFAULT;
		}
		spin_lock(&ctx->lock);
		list_del(&cmd->ctx_node);
		ctx->nr_live--;
		spin_unlock(&ctx->lock);
		kdc_cmd_free(cmd);
		n++;
	}
	return n * sizeof(struct kdc_completion);
}

static __poll_t kdc_poll(struct file *file, struct poll_table_struct *wait)
{
	struct kdc_ctx *ctx = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctx->wq, wait);
	if (READ_ONCE(ctx->nr_ready))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (kdc_ctx_finished(ctx))
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

const struct file_operations kdc_fops = {
	.owner = THIS_MODULE,
	.open = kdc_open,
	.release = kdc_release,
	.unlocked_ioctl = kdc_ioctl,
	.read = kdc_read,
	.poll = kdc_poll,
};
