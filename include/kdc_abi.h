/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * User ABI of /dev/kdcN, version 3.
 *
 * Every struct starts with {version, size}; the driver rejects other versions
 * (EPROTO), other sizes (EINVAL) and nonzero reserved fields (EINVAL), so that
 * reserved fields can gain meaning later without breaking old callers.
 *
 * Version 3 gave kdc_submit a timeout (a former reserved field) and added the
 * recovery counters to kdc_stats. Registered buffers (KDC_IOC_REGISTER,
 * KDC_SUBMIT_FIXED) were added later without a version change: callers that
 * do not use them see no difference.
 */
#ifndef KDC_ABI_H
#define KDC_ABI_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define KDC_ABI_VERSION 3
#define KDC_MAX_LENGTH (256 * 1024) /* bytes per command */
#define KDC_MAX_CMDS_PER_FILE 64 /* submitted and not yet read */
#define KDC_FILE_BYTE_QUOTA (4 * 1024 * 1024) /* src + dst bytes in flight */
#define KDC_QUEUE_AUTO 0xffff /* pick the queue mapped to the submitting CPU */
#define KDC_STATS_QUEUES 16
#define KDC_MAX_TIMEOUT_MS 600000

/* kdc_submit.flags */
#define KDC_SUBMIT_BATCH 0x1 /* stage only; KDC_IOC_FLUSH rings the doorbell */
#define KDC_SUBMIT_BOUNCE 0x2 /* force the bounce-buffer path */
#define KDC_SUBMIT_ZEROCOPY 0x4 /* force the pinned zero-copy path */
#define KDC_SUBMIT_NOP 0x8 /* no data; src, dst and length must be 0 */
#define KDC_SUBMIT_FIXED 0x10 /* src and dst lie in buffers registered on this file */
#define KDC_SUBMIT_FLAGS 0x1f

/*
 * Registered buffers: KDC_IOC_REGISTER pins and maps a page-aligned user range
 * once; a KDC_SUBMIT_FIXED command whose src and dst each lie inside one such
 * buffer then needs no pinning or DMA mapping of its own. The buffer stays
 * pinned and mapped until KDC_IOC_UNREGISTER or close(); unregistering fails
 * with EBUSY while a command that uses it is not finished (the device may
 * still reach it). Up to KDC_MAX_FIXED_BUFS per file, KDC_MAX_FIXED_BYTES each.
 * Register errors: EINVAL (alignment, length), EEXIST (overlaps a buffer of
 * this file), ENOSPC (no free index), EFAULT, ENOMEM. A fixed submit outside
 * the registered buffers fails with EINVAL; unregister of an unknown index
 * with ENOENT.
 */
#define KDC_MAX_FIXED_BUFS 8
#define KDC_MAX_FIXED_BYTES (16 * 1024 * 1024)
struct kdc_buffer {
	__u32 version, size;
	__aligned_u64 addr; /* page aligned */
	__u64 length; /* page multiple, 1 page .. KDC_MAX_FIXED_BYTES */
	__u32 index; /* KDC_IOC_REGISTER returns it; KDC_IOC_UNREGISTER takes it */
	__u32 reserved;
	__u64 reserved2[2];
};

/*
 * Submit errors besides the usual EINVAL/EFAULT/ENOMEM:
 *   EADDRINUSE  dst shares bytes with the destination of a command that is
 *               still in flight (any file, any kdc device; compared by
 *               physical address)
 *   ENOSPC      file quota; EBUSY file reset in progress; ESTALE old epoch;
 *   EEXIST      id in use; EIO device failed recovery; ENODEV device gone.
 */
struct kdc_submit {
	__u32 version, size;
	__u64 id; /* caller's tag, unique among this file's unread commands */
	__u64 epoch; /* must equal the file's current epoch */
	__aligned_u64 src; /* user address, read by the device */
	__aligned_u64 dst; /* user address, written by the device */
	__u32 length; /* 1..KDC_MAX_LENGTH; src and dst must not overlap */
	__u32 flags;
	__u32 crc_seed; /* CRC32C to continue from; 0 for a fresh CRC */
	__u16 queue; /* KDC_QUEUE_AUTO or an explicit queue index */
	__u16 reserved0;
	__u32 timeout_ms; /* from the doorbell; 0 = module default (cmd_timeout_ms) */
	__u32 reserved1;
	__u64 reserved[2];
};

struct kdc_cancel {
	__u32 version, size;
	__u64 id, epoch, reserved;
};

/*
 * read() returns one of these per finished command. status is 0 or one of
 *   ECANCELED   cancelled (or its file closed)   ECONNRESET  KDC_IOC_RESET
 *   ETIMEDOUT   exceeded its timeout             EIO         device error or
 *   EPROTO      device rejected the descriptor               failed recovery
 *   ENODEV      device removed or not responding
 * After any nonzero status the destination contents are undefined.
 */
#define KDC_COMPL_BOUNCE 0x1 /* data went through the bounce path */
#define KDC_COMPL_ABORTED 0x2 /* the device acknowledged an abort */
#define KDC_COMPL_RESET 0x4 /* ended by a device reset, failure or removal */
struct kdc_completion {
	__u64 id, epoch;
	__s32 status; /* 0 or -errno */
	__u32 crc; /* CRC32C of the copied data when status == 0 */
	__u32 length; /* bytes the device wrote to dst */
	__u16 queue;
	__u16 hw_status; /* raw device status code */
	__u32 flags;
	__u32 reserved;
};

/* kdc_stats.dev_state */
#define KDC_STATE_LIVE 0
#define KDC_STATE_RECOVERING 1 /* queues stopped while the device is reset */
#define KDC_STATE_FAILED 2 /* recovery gave up; bus mastering is off */
#define KDC_STATE_GONE 3 /* the device stopped responding (surprise removal) */

struct kdc_stats {
	__u32 version, size;
	/* this file */
	__u64 epoch;
	__u32 live; /* submitted and not yet read */
	__u32 staged; /* waiting for KDC_IOC_FLUSH */
	__u32 pending; /* waiting for ring space */
	__u32 device_owned; /* doorbell rung, no completion yet */
	__u32 ready; /* completions waiting for read() */
	__u32 resetting;
	__u64 bytes; /* quota in use */
	__u64 pinned_pages;
	/* this device */
	__u64 dev_pinned_pages;
	__u64 dev_mapped_sgts;
	__u64 dev_open_files;
	__u64 dev_submitted;
	__u64 dev_completed;
	__u64 dev_aborts_sent;
	__u64 dev_bad_completions;
	__u64 dev_errors;
	__u32 nr_queues, queue_depth;
	__u32 bounce_max, max_length;
	__u64 queue_submitted[KDC_STATS_QUEUES];
	/* recovery (version 3) */
	__u32 dev_state; /* KDC_STATE_* */
	__u32 reserved;
	__u64 dev_timeouts; /* commands that reached their timeout */
	__u64 dev_resets; /* successful controller resets */
	__u64 dev_flrs; /* function resets through reset_prepare/reset_done */
	__u64 dev_failures; /* times recovery gave up */
	__u64 dev_requeued; /* commands reissued after a reset */
	__u64 dev_overlaps; /* submits refused with EADDRINUSE */
};

#define KDC_IOC_SUBMIT _IOW('K', 0x20, struct kdc_submit)
#define KDC_IOC_CANCEL _IOW('K', 0x21, struct kdc_cancel)
#define KDC_IOC_RESET _IO('K', 0x22) /* abort this file's work, bump epoch */
#define KDC_IOC_FLUSH _IO('K', 0x23) /* dispatch staged commands */
#define KDC_IOC_STATS _IOR('K', 0x24, struct kdc_stats)
#define KDC_IOC_REGISTER _IOWR('K', 0x25, struct kdc_buffer)
#define KDC_IOC_UNREGISTER _IOW('K', 0x26, struct kdc_buffer)
#endif
