/* SPDX-License-Identifier: GPL-2.0 */
/* Ownership state machine and ABI checks; pure logic shared with KUnit. */
#ifndef KDC_STATE_H
#define KDC_STATE_H
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/types.h>
#include "kdc_abi.h"
#include "kdc_hw.h"
#include "kdc_ring.h"

/*
 * STAGED and PENDING commands are owned by the driver: their buffers are pinned
 * and mapped but the device cannot see them, so cancel releases them at once.
 * From the doorbell on (DEVICE) the device owns the buffers; cancel can only
 * ask for a device abort, and buffers stay pinned and mapped until the device
 * posts a completion. COMPLETING means that completion (or a driver-side
 * cancel) has been claimed and resources are being released. A DONE command
 * holds no DMA resources; it only waits for read().
 */
enum kdc_phase {
	KDC_FREE,
	KDC_STAGED,
	KDC_PENDING,
	KDC_DEVICE,
	KDC_COMPLETING,
	KDC_DONE,
};

enum kdc_abort_reason {
	KDC_ABORT_NONE,
	KDC_ABORT_CANCEL,
	KDC_ABORT_RESET,
	KDC_ABORT_CLOSE,
	KDC_ABORT_REMOVE,
	KDC_ABORT_TIMEOUT,
};

enum kdc_cancel_action {
	KDC_CANCEL_NOW, /* driver-owned: release now, complete with -ECANCELED */
	KDC_CANCEL_SEND_ABORT, /* device-owned: write the abort register */
	KDC_CANCEL_WAIT, /* abort already sent or completion being processed */
	KDC_CANCEL_DONE_OK, /* already cancelled */
	KDC_CANCEL_DONE_LATE, /* finished with another status first */
	KDC_CANCEL_NOT_FOUND,
};

static inline enum kdc_cancel_action kdc_cancel_decide(enum kdc_phase phase,
							bool abort_sent,
							int status)
{
	switch (phase) {
	case KDC_STAGED:
	case KDC_PENDING:
		return KDC_CANCEL_NOW;
	case KDC_DEVICE:
		return abort_sent ? KDC_CANCEL_WAIT : KDC_CANCEL_SEND_ABORT;
	case KDC_COMPLETING:
		return KDC_CANCEL_WAIT;
	case KDC_DONE:
		return status == -ECANCELED ? KDC_CANCEL_DONE_OK :
					      KDC_CANCEL_DONE_LATE;
	default:
		return KDC_CANCEL_NOT_FOUND;
	}
}

static inline int kdc_cancel_errno(enum kdc_cancel_action action)
{
	switch (action) {
	case KDC_CANCEL_NOW:
	case KDC_CANCEL_DONE_OK:
		return 0;
	case KDC_CANCEL_SEND_ABORT:
	case KDC_CANCEL_WAIT:
		return -EINPROGRESS;
	case KDC_CANCEL_DONE_LATE:
		return -EALREADY;
	default:
		return -ENOENT;
	}
}

/* Status for a command released before the device ever saw it. */
static inline int kdc_unsent_errno(enum kdc_abort_reason reason)
{
	switch (reason) {
	case KDC_ABORT_RESET:
		return -ECONNRESET;
	case KDC_ABORT_REMOVE:
		return -ENODEV;
	case KDC_ABORT_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return -ECANCELED;
	}
}

/* Status for a device completion; an abort maps to the reason it was sent. */
static inline int kdc_status_errno(u16 code, enum kdc_abort_reason reason)
{
	switch (code) {
	case KDC_SC_SUCCESS:
		return 0;
	case KDC_SC_ABORTED:
		return kdc_unsent_errno(reason);
	case KDC_SC_INVALID_OPCODE:
	case KDC_SC_INVALID_FIELD:
	case KDC_SC_INVALID_SGL:
		return -EPROTO;
	default:
		return -EIO;
	}
}

/* dev_state is KDC_STATE_*; a recovering device still accepts commands. */
static inline int kdc_admission(u64 supplied_epoch, u64 epoch, bool resetting,
				bool closing, u32 dev_state)
{
	if (dev_state == KDC_STATE_GONE)
		return -ENODEV;
	if (dev_state == KDC_STATE_FAILED)
		return -EIO;
	if (closing)
		return -EBADF;
	if (resetting)
		return -EBUSY;
	if (supplied_epoch != epoch)
		return -ESTALE;
	return 0;
}

static inline int kdc_quota(u32 live, u64 bytes, u64 more)
{
	if (live >= KDC_MAX_CMDS_PER_FILE || more > KDC_FILE_BYTE_QUOTA - bytes)
		return -ENOSPC;
	return 0;
}

/* Field validation that needs no locks; returns 0 or -errno. */
static inline int kdc_submit_check(const struct kdc_submit *s,
				   unsigned int nr_queues)
{
	if (s->version != KDC_ABI_VERSION)
		return -EPROTO;
	if (s->size != sizeof(*s) || s->reserved0 || s->reserved1 ||
	    s->reserved[0] || s->reserved[1] || s->flags & ~KDC_SUBMIT_FLAGS ||
	    s->timeout_ms > KDC_MAX_TIMEOUT_MS)
		return -EINVAL;
	if ((s->flags & KDC_SUBMIT_BOUNCE) && (s->flags & KDC_SUBMIT_ZEROCOPY))
		return -EINVAL;
	if (s->queue != KDC_QUEUE_AUTO && s->queue >= nr_queues)
		return -EINVAL;
	if (s->flags & KDC_SUBMIT_NOP)
		return s->src || s->dst || s->length || s->crc_seed ||
				       (s->flags & (KDC_SUBMIT_BOUNCE |
						    KDC_SUBMIT_ZEROCOPY)) ?
			       -EINVAL :
			       0;
	if (!s->length || s->length > KDC_MAX_LENGTH || !s->src || !s->dst ||
	    !kdc_pages_spanned(s->src, s->length) ||
	    !kdc_pages_spanned(s->dst, s->length) ||
	    kdc_ranges_overlap(s->src, s->dst, s->length))
		return -EINVAL;
	return 0;
}

/*
 * Final status of a command that the device will never complete because the
 * device was reset, failed or went away. A command somebody already asked to
 * abort (cancel, file reset, close, its own timeout) ends with that reason;
 * every other command gets @base.
 */
static inline int kdc_final_status(bool abort_requested,
				   enum kdc_abort_reason reason, int base)
{
	return abort_requested ? kdc_unsent_errno(reason) : base;
}

/* Watchdog decision for one device-owned command. */
enum kdc_watch_action {
	KDC_WATCH_NONE,
	KDC_WATCH_ABORT, /* its timeout expired: send a device abort */
	KDC_WATCH_RESET, /* an abort went unanswered: reset the device */
};

static inline enum kdc_watch_action kdc_watch_decide(unsigned long now,
						      unsigned long deadline,
						      bool abort_sent,
						      unsigned long abort_deadline)
{
	if (abort_sent)
		return time_after_eq(now, abort_deadline) ? KDC_WATCH_RESET :
							    KDC_WATCH_NONE;
	return time_after_eq(now, deadline) ? KDC_WATCH_ABORT : KDC_WATCH_NONE;
}

/*
 * After a successful reset, a device-owned command that nobody asked to abort
 * is reissued (a copy is idempotent). Resets the driver starts itself count
 * against KDC_MAX_RETRIES, so that a command which itself brings the device
 * down cannot loop forever; the caller passes 0 retries for other resets.
 */
#define KDC_MAX_RETRIES 2

static inline bool kdc_reclaim_requeue(bool reset_ok, bool abort_sent,
				       unsigned int retries)
{
	return reset_ok && !abort_sent && retries < KDC_MAX_RETRIES;
}
#endif
