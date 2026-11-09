// SPDX-License-Identifier: GPL-2.0
/*
 * Bounded waits and recovery.
 *
 * A watchdog runs every 100 ms while the device owns commands. A command past
 * its deadline gets a device abort. An abort that stays unanswered for
 * abort_timeout_ms (whoever sent it: the timeout, cancel, a file reset or
 * close) escalates, as does a fatal or unresponsive device:
 *
 *   1. controller reset: disable the controller and wait for READY to clear,
 *      which the device defines as "no more DMA"; then take every command
 *      back, re-initialise the rings and MSI-X vector registers, re-enable;
 *   2. function-level reset through pci_try_reset_function(), which calls
 *      reset_prepare/reset_done below, so a reset started from sysfs goes
 *      through exactly the same code;
 *   3. give up: clear bus mastering, end every command, refuse new ones until
 *      a later reset revives the device or it is unbound.
 *
 * Buffers are unmapped and unpinned only when the device can no longer DMA
 * into them: after its completion, after step 1 or 2 has stopped it, or after
 * bus mastering is off. A device that reads all-ones and has vanished from
 * config space is treated as removed and is not reset at all.
 *
 * Commands the device held when it was reset are reissued if nobody asked to
 * abort them (copies are idempotent); commands with an abort pending end with
 * the abort's reason (-ETIMEDOUT, -ECANCELED, -ECONNRESET). Resets the driver
 * starts itself count against a command's KDC_MAX_RETRIES reissues, so a
 * command that keeps bringing the device down ends with -EIO; resets requested
 * from outside (sysfs) do not count.
 */
#include <linux/delay.h>
#include "kdc_drv.h"

#define KDC_WATCHDOG_PERIOD (HZ / 10)

void kdc_arm_watchdog(struct kdc_dev *kdev)
{
	if (!delayed_work_pending(&kdev->watchdog))
		queue_delayed_work(kdc_wq, &kdev->watchdog, KDC_WATCHDOG_PERIOD);
}

void kdc_watchdog_fn(struct work_struct *work)
{
	struct kdc_dev *kdev = container_of(to_delayed_work(work),
					    struct kdc_dev, watchdog);
	unsigned long now = jiffies;
	bool busy = false, reset = false;
	unsigned int i, tag;
	u32 status;

	/* A reset in progress re-arms the watchdog when it dispatches again. */
	if (READ_ONCE(kdev->removing) || READ_ONCE(kdev->recovering) ||
	    READ_ONCE(kdev->state) != KDC_STATE_LIVE)
		return;
	status = readl(kdev->regs + KDC_REG_STATUS);
	/* Not responding, or fatal with its interrupt lost. */
	if (status == ~0u || (status & KDC_STATUS_FATAL))
		reset = true;
	for (i = 0; i < kdev->nr_queues; i++) {
		struct kdc_queue *q = &kdev->queues[i];

		spin_lock(&q->lock);
		if (q->stopped) {
			spin_unlock(&q->lock);
			continue;
		}
		for_each_set_bit(tag, q->tag_map, q->depth) {
			struct kdc_cmd *cmd = q->slots[tag];

			busy = true;
			switch (kdc_watch_decide(now, cmd->deadline, cmd->abort_sent,
						 cmd->abort_deadline)) {
			case KDC_WATCH_ABORT:
				atomic64_inc(&kdev->timeouts);
				dev_warn_ratelimited(&kdev->pdev->dev,
						     "queue %u cid %#x timed out after %u ms; aborting\n",
						     q->qid, cmd->cid,
						     jiffies_to_msecs(cmd->timeout));
				kdc_send_abort_locked(q, cmd, KDC_ABORT_TIMEOUT);
				break;
			case KDC_WATCH_RESET:
				reset = true;
				break;
			default:
				break;
			}
		}
		spin_unlock(&q->lock);
	}
	if (reset)
		queue_work(kdc_wq, &kdev->recover_work);
	else if (busy)
		queue_delayed_work(kdc_wq, &kdev->watchdog, KDC_WATCHDOG_PERIOD);
}

/*
 * Recovery can be requested by the watchdog, the error interrupt or both, and
 * a sysfs reset may have fixed things in the meantime: check before acting.
 */
static bool kdc_needs_recovery(struct kdc_dev *kdev)
{
	unsigned long now = jiffies;
	unsigned int i, tag;
	bool need = false;
	u32 status;

	status = readl(kdev->regs + KDC_REG_STATUS);
	if (status == ~0u || (status & KDC_STATUS_FATAL) ||
	    !(status & KDC_STATUS_READY))
		return true;
	for (i = 0; i < kdev->nr_queues && !need; i++) {
		struct kdc_queue *q = &kdev->queues[i];

		if (readl(kdev->regs + KDC_QREG(i, KDC_Q_STATUS)) & KDC_QSTATUS_HALTED)
			return true;
		spin_lock(&q->lock);
		for_each_set_bit(tag, q->tag_map, q->depth) {
			struct kdc_cmd *cmd = q->slots[tag];

			if (cmd->abort_sent &&
			    time_after_eq(now, cmd->abort_deadline)) {
				need = true;
				break;
			}
		}
		spin_unlock(&q->lock);
	}
	return need;
}

static void kdc_quiesce(struct kdc_dev *kdev)
{
	unsigned int i;

	for (i = 0; i < kdev->nr_queues; i++) {
		spin_lock(&kdev->queues[i].lock);
		kdev->queues[i].stopped = true;
		spin_unlock(&kdev->queues[i].lock);
	}
	WRITE_ONCE(kdev->recovering, true);
}

static void kdc_unquiesce(struct kdc_dev *kdev)
{
	unsigned int i;

	WRITE_ONCE(kdev->recovering, false);
	for (i = 0; i < kdev->nr_queues; i++) {
		struct kdc_queue *q = &kdev->queues[i];

		spin_lock(&q->lock);
		q->stopped = false;
		kdc_queue_dispatch_locked(q);
		spin_unlock(&q->lock);
	}
}

/*
 * The device has stopped (or cannot reach memory): take back everything it
 * held. Completions it posted before stopping are honoured. With @reset_ok the
 * rest is reissued where allowed (@count: this reset counts against the retry
 * limit); everything else ends with @base or the reason of its pending abort.
 */
static void kdc_reclaim(struct kdc_dev *kdev, bool reset_ok, bool count, int base)
{
	unsigned int i, tag;

	for (i = 0; i < kdev->nr_queues; i++) {
		struct kdc_queue *q = &kdev->queues[i];
		LIST_HEAD(done);
		LIST_HEAD(ended);
		LIST_HEAD(again);

		spin_lock(&q->lock);
		kdc_queue_reap_locked(q, &done);
		for_each_set_bit(tag, q->tag_map, q->depth) {
			struct kdc_cmd *cmd = q->slots[tag];

			q->slots[tag] = NULL;
			if (kdc_reclaim_requeue(reset_ok, cmd->abort_sent,
						count ? cmd->retries : 0)) {
				cmd->retries += count;
				WRITE_ONCE(cmd->phase, KDC_PENDING);
				list_add_tail(&cmd->node, &again);
				atomic64_inc(&kdev->requeued);
			} else {
				kdc_cmd_end(cmd, base, &ended);
			}
		}
		bitmap_zero(q->tag_map, q->depth);
		q->outstanding = 0;
		list_splice(&again, &q->pending); /* ahead of newer work */
		spin_unlock(&q->lock);
		kdc_finish_list(kdev, &done);
		kdc_end_list(kdev, &ended);
	}
}

/* Give up on the device. Called with reset_lock held. */
void kdc_fail_locked(struct kdc_dev *kdev, u32 state)
{
	LIST_HEAD(list);

	lockdep_assert_held(&kdev->reset_lock);
	/* From here the device cannot DMA, whatever state it is in. */
	pci_clear_master(kdev->pdev);
	WRITE_ONCE(kdev->state, state);
	WRITE_ONCE(kdev->recovering, false);
	atomic64_inc(&kdev->failures);
	if (state == KDC_STATE_GONE)
		dev_err(&kdev->pdev->dev, "device not responding; ending all commands\n");
	else
		dev_err(&kdev->pdev->dev, "recovery failed; device disabled until reset or rebind\n");
	kdc_reclaim(kdev, false, false, state == KDC_STATE_GONE ? -ENODEV : -EIO);
	mutex_lock(&kdev->files_mutex);
	kdc_collect_all(kdev, &list, state == KDC_STATE_GONE ? -ENODEV : -EIO,
			KDC_ABORT_NONE);
	mutex_unlock(&kdev->files_mutex);
	kdc_end_list(kdev, &list);
	kdc_wake_files(kdev);
}

void kdc_recover_fn(struct work_struct *work)
{
	struct kdc_dev *kdev = container_of(work, struct kdc_dev, recover_work);
	struct pci_dev *pdev = kdev->pdev;
	unsigned long seq;
	int err = -EAGAIN, i;

	mutex_lock(&kdev->reset_lock);
	if (READ_ONCE(kdev->removing) || kdev->state != KDC_STATE_LIVE ||
	    !kdc_needs_recovery(kdev)) {
		mutex_unlock(&kdev->reset_lock);
		return;
	}
	kdc_quiesce(kdev);
	if (kdc_device_gone(kdev)) {
		kdc_fail_locked(kdev, KDC_STATE_GONE);
		mutex_unlock(&kdev->reset_lock);
		return;
	}
	dev_warn(&pdev->dev, "resetting controller\n");
	if (!kdc_hw_stop(kdev)) {
		/* READY is clear: the device has stopped all DMA. */
		kdc_reclaim(kdev, true, true, -EIO);
		if (!kdc_restart_hw(kdev)) {
			atomic64_inc(&kdev->resets);
			kdc_unquiesce(kdev);
			mutex_unlock(&kdev->reset_lock);
			return;
		}
	}
	seq = kdev->reset_seq;
	kdev->recovery_flr = true;
	mutex_unlock(&kdev->reset_lock);

	/*
	 * Not pci_reset_function(): remove() holds the device lock while it
	 * waits for this work, so we must not block on that lock.
	 */
	dev_warn(&pdev->dev, "controller reset failed; trying a function reset\n");
	for (i = 0; i < 50 && !READ_ONCE(kdev->removing); i++) {
		err = pci_try_reset_function(pdev);
		if (err != -EAGAIN)
			break;
		msleep(20);
	}
	mutex_lock(&kdev->reset_lock);
	kdev->recovery_flr = false;
	/* Unless reset_done() ran meanwhile (ours or one from sysfs) and decided. */
	if (kdev->reset_seq == seq && !READ_ONCE(kdev->removing) &&
	    kdev->state == KDC_STATE_LIVE) {
		dev_err(&pdev->dev, "function reset not possible (%d)\n", err);
		kdc_fail_locked(kdev, KDC_STATE_FAILED);
	}
	mutex_unlock(&kdev->reset_lock);
}

/*
 * reset_prepare/reset_done bracket every reset the PCI core performs on the
 * device (recovery above, sysfs "reset", other users of pci_reset_function),
 * always in the same task and under the device lock; reset_lock is held from
 * one to the other.
 */
void kdc_reset_prepare(struct pci_dev *pdev)
{
	struct kdc_dev *kdev = pci_get_drvdata(pdev);

	mutex_lock(&kdev->reset_lock);
	kdc_quiesce(kdev);
}

void kdc_reset_done(struct pci_dev *pdev)
{
	struct kdc_dev *kdev = pci_get_drvdata(pdev);
	bool ours = kdev->recovery_flr;

	lockdep_assert_held(&kdev->reset_lock);
	kdev->reset_seq++;
	kdev->recovery_flr = false;
	if (kdev->state == KDC_STATE_GONE)
		goto out;
	/* After a function reset READY is clear; if not, the reset did not take. */
	if (kdc_hw_stop(kdev)) {
		kdc_fail_locked(kdev, kdc_device_gone(kdev) ? KDC_STATE_GONE :
							      KDC_STATE_FAILED);
		goto out;
	}
	kdc_reclaim(kdev, true, ours, -EIO);
	if (kdc_restart_hw(kdev)) {
		kdc_fail_locked(kdev, KDC_STATE_FAILED);
		goto out;
	}
	atomic64_inc(&kdev->flrs);
	if (kdev->state == KDC_STATE_FAILED)
		dev_info(&pdev->dev, "device working again after reset\n");
	WRITE_ONCE(kdev->state, KDC_STATE_LIVE);
	kdc_unquiesce(kdev);
out:
	mutex_unlock(&kdev->reset_lock);
}
