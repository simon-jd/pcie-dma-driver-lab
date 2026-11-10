// SPDX-License-Identifier: GPL-2.0
/* KUnit tests for the pure pieces: ring/phase/cid math, SG building, states. */
#include <kunit/test.h>
#include <linux/scatterlist.h>
#include "kdc_drv.h"

static void cid_generation(struct kunit *test)
{
	u16 seen[16];
	u8 gen = 0;
	int i, j;

	for (i = 0; i < 16; i++) {
		gen = kdc_gen_next(gen);
		seen[i] = kdc_cid_make(0xabc, gen);
		KUNIT_EXPECT_EQ(test, kdc_cid_tag(seen[i]), 0xabc);
		for (j = 0; j < i; j++)
			KUNIT_EXPECT_NE(test, seen[i], seen[j]);
	}
	KUNIT_EXPECT_EQ(test, kdc_gen_next(15), 0);
	KUNIT_EXPECT_EQ(test, kdc_cid_tag(kdc_cid_make(0x1fff, 3)), 0xfff);
	KUNIT_EXPECT_EQ(test, kdc_cid_make(5, 0x13) >> KDC_TAG_BITS, 3);
}

static void ring_math(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, kdc_ring_used(0, 0, 64), 0);
	KUNIT_EXPECT_EQ(test, kdc_ring_used(63, 0, 64), 63);
	KUNIT_EXPECT_EQ(test, kdc_ring_used(2, 60, 64), 6);
	KUNIT_EXPECT_EQ(test, kdc_ring_next(63, 64), 0);
	KUNIT_EXPECT_EQ(test, kdc_ring_next(1, 2), 0);
}

static void phase_flips_on_wrap(struct kunit *test)
{
	u16 head = 0;
	u8 phase = 1;
	int i;

	for (i = 0; i < 7; i++) {
		kdc_cq_advance(&head, &phase, 8);
		KUNIT_EXPECT_EQ(test, phase, 1);
	}
	kdc_cq_advance(&head, &phase, 8);
	KUNIT_EXPECT_EQ(test, head, 0);
	KUNIT_EXPECT_EQ(test, phase, 0);
	/* A zeroed ring is not "new" for the first pass, new once written. */
	KUNIT_EXPECT_FALSE(test, kdc_cqe_is_new(0, 1));
	KUNIT_EXPECT_TRUE(test, kdc_cqe_is_new(KDC_SC_ABORTED << 1 | 1, 1));
	KUNIT_EXPECT_TRUE(test, kdc_cqe_is_new(0, 0));
}

static void page_span_and_overlap(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, kdc_pages_spanned(0x1000, 0), 0);
	KUNIT_EXPECT_EQ(test, kdc_pages_spanned(0x1000, 1), 1);
	KUNIT_EXPECT_EQ(test, kdc_pages_spanned(0x1fff, 2), 2);
	KUNIT_EXPECT_EQ(test, kdc_pages_spanned(0x1000, 0x1000), 1);
	KUNIT_EXPECT_EQ(test, kdc_pages_spanned(0x1001, 0x40000), 65);
	KUNIT_EXPECT_EQ(test, kdc_pages_spanned(U64_MAX - 10, 100), 0);
	KUNIT_EXPECT_TRUE(test, kdc_ranges_overlap(0x1000, 0x1fff, 0x1000));
	KUNIT_EXPECT_FALSE(test, kdc_ranges_overlap(0x1000, 0x2000, 0x1000));
	KUNIT_EXPECT_FALSE(test, kdc_ranges_overlap(0x2000, 0x1000, 0x1000));
}

static void cancel_by_ownership(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_STAGED, false, 0), KDC_CANCEL_NOW);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_PENDING, false, 0), KDC_CANCEL_NOW);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_DEVICE, false, 0),
			KDC_CANCEL_SEND_ABORT);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_DEVICE, true, 0), KDC_CANCEL_WAIT);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_COMPLETING, false, 0),
			KDC_CANCEL_WAIT);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_DONE, true, -ECANCELED),
			KDC_CANCEL_DONE_OK);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_DONE, true, 0),
			KDC_CANCEL_DONE_LATE);
	KUNIT_EXPECT_EQ(test, kdc_cancel_decide(KDC_FREE, false, 0),
			KDC_CANCEL_NOT_FOUND);
	KUNIT_EXPECT_EQ(test, kdc_cancel_errno(KDC_CANCEL_NOW), 0);
	KUNIT_EXPECT_EQ(test, kdc_cancel_errno(KDC_CANCEL_SEND_ABORT), -EINPROGRESS);
	KUNIT_EXPECT_EQ(test, kdc_cancel_errno(KDC_CANCEL_WAIT), -EINPROGRESS);
	KUNIT_EXPECT_EQ(test, kdc_cancel_errno(KDC_CANCEL_DONE_LATE), -EALREADY);
	KUNIT_EXPECT_EQ(test, kdc_cancel_errno(KDC_CANCEL_NOT_FOUND), -ENOENT);
}

static void status_mapping(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_SUCCESS, KDC_ABORT_CANCEL), 0);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_ABORTED, KDC_ABORT_CANCEL),
			-ECANCELED);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_ABORTED, KDC_ABORT_RESET),
			-ECONNRESET);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_ABORTED, KDC_ABORT_REMOVE),
			-ENODEV);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_DATA_ERROR, KDC_ABORT_NONE),
			-EIO);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_INVALID_SGL, KDC_ABORT_NONE),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(0x7fff, KDC_ABORT_NONE), -EIO);
	KUNIT_EXPECT_EQ(test, kdc_unsent_errno(KDC_ABORT_CLOSE), -ECANCELED);
	KUNIT_EXPECT_EQ(test, kdc_status_errno(KDC_SC_ABORTED, KDC_ABORT_TIMEOUT),
			-ETIMEDOUT);
	/* ended by a reset or failure: a pending abort keeps its reason */
	KUNIT_EXPECT_EQ(test, kdc_final_status(true, KDC_ABORT_TIMEOUT, -EIO), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, kdc_final_status(true, KDC_ABORT_CANCEL, -ENODEV),
			-ECANCELED);
	KUNIT_EXPECT_EQ(test, kdc_final_status(true, KDC_ABORT_RESET, -EIO), -ECONNRESET);
	KUNIT_EXPECT_EQ(test, kdc_final_status(false, KDC_ABORT_NONE, -ENODEV), -ENODEV);
}

static void watchdog_and_reclaim(struct kunit *test)
{
	unsigned long t = 1000;

	KUNIT_EXPECT_EQ(test, kdc_watch_decide(t, t + 1, false, 0), KDC_WATCH_NONE);
	KUNIT_EXPECT_EQ(test, kdc_watch_decide(t, t, false, 0), KDC_WATCH_ABORT);
	/* an abort in flight is judged by its own deadline, not the command's */
	KUNIT_EXPECT_EQ(test, kdc_watch_decide(t, t - 5, true, t + 5), KDC_WATCH_NONE);
	KUNIT_EXPECT_EQ(test, kdc_watch_decide(t, t + 50, true, t), KDC_WATCH_RESET);
	/* jiffies wrap */
	KUNIT_EXPECT_EQ(test, kdc_watch_decide(5, ULONG_MAX - 5, false, 0),
			KDC_WATCH_ABORT);
	KUNIT_EXPECT_EQ(test, kdc_watch_decide(ULONG_MAX - 5, 5, false, 0),
			KDC_WATCH_NONE);
	KUNIT_EXPECT_TRUE(test, kdc_reclaim_requeue(true, false, 0));
	KUNIT_EXPECT_TRUE(test, kdc_reclaim_requeue(true, false, KDC_MAX_RETRIES - 1));
	KUNIT_EXPECT_FALSE(test, kdc_reclaim_requeue(true, false, KDC_MAX_RETRIES));
	KUNIT_EXPECT_FALSE(test, kdc_reclaim_requeue(true, true, 0));
	KUNIT_EXPECT_FALSE(test, kdc_reclaim_requeue(false, false, 0));
}

static void dst_split(struct kunit *test)
{
	u32 h, b, t;

	kdc_dst_split(0x1000, 4096, &h, &b, &t);
	KUNIT_EXPECT_EQ(test, h, 0); KUNIT_EXPECT_EQ(test, b, 4096); KUNIT_EXPECT_EQ(test, t, 0);
	kdc_dst_split(0x1001, 4096, &h, &b, &t);
	KUNIT_EXPECT_EQ(test, h, 63); KUNIT_EXPECT_EQ(test, b, 4032); KUNIT_EXPECT_EQ(test, t, 1);
	kdc_dst_split(0x100a, 20, &h, &b, &t); /* inside one line */
	KUNIT_EXPECT_EQ(test, h, 20); KUNIT_EXPECT_EQ(test, b, 0); KUNIT_EXPECT_EQ(test, t, 0);
	kdc_dst_split(0x1000, 20, &h, &b, &t); /* aligned start, short */
	KUNIT_EXPECT_EQ(test, h, 0); KUNIT_EXPECT_EQ(test, b, 0); KUNIT_EXPECT_EQ(test, t, 20);
	kdc_dst_split(0x100a, 54, &h, &b, &t); /* ends on a boundary */
	KUNIT_EXPECT_EQ(test, h, 54); KUNIT_EXPECT_EQ(test, b, 0); KUNIT_EXPECT_EQ(test, t, 0);
	kdc_dst_split(0x1030, 32, &h, &b, &t); /* crosses one boundary */
	KUNIT_EXPECT_EQ(test, h, 16); KUNIT_EXPECT_EQ(test, b, 0); KUNIT_EXPECT_EQ(test, t, 16);
	kdc_dst_split(0x1fff, 1, &h, &b, &t);
	KUNIT_EXPECT_EQ(test, h + b + t, 1);
	kdc_dst_split(0x1001, KDC_MAX_LENGTH, &h, &b, &t);
	KUNIT_EXPECT_EQ(test, h + b + t, KDC_MAX_LENGTH);
	KUNIT_EXPECT_EQ(test, (0x1001 + h) % KDC_CACHELINE, 0);
	KUNIT_EXPECT_EQ(test, b % KDC_CACHELINE, 0);
	KUNIT_EXPECT_LT(test, h, KDC_CACHELINE);
	KUNIT_EXPECT_LT(test, t, KDC_CACHELINE);
}

static void admission_and_quota(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, kdc_admission(1, 1, false, false, KDC_STATE_GONE), -ENODEV);
	KUNIT_EXPECT_EQ(test, kdc_admission(1, 1, false, false, KDC_STATE_FAILED), -EIO);
	KUNIT_EXPECT_EQ(test, kdc_admission(1, 1, false, true, KDC_STATE_LIVE), -EBADF);
	KUNIT_EXPECT_EQ(test, kdc_admission(1, 1, true, false, KDC_STATE_LIVE), -EBUSY);
	KUNIT_EXPECT_EQ(test, kdc_admission(1, 2, false, false, KDC_STATE_LIVE), -ESTALE);
	KUNIT_EXPECT_EQ(test, kdc_admission(2, 2, false, false, KDC_STATE_LIVE), 0);
	/* commands submitted during a reset wait for it instead of failing */
	KUNIT_EXPECT_EQ(test, kdc_admission(2, 2, false, false, KDC_STATE_RECOVERING), 0);
	KUNIT_EXPECT_EQ(test, kdc_quota(KDC_MAX_CMDS_PER_FILE - 1, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, kdc_quota(KDC_MAX_CMDS_PER_FILE, 0, 0), -ENOSPC);
	KUNIT_EXPECT_EQ(test, kdc_quota(0, KDC_FILE_BYTE_QUOTA - 8, 8), 0);
	KUNIT_EXPECT_EQ(test, kdc_quota(0, KDC_FILE_BYTE_QUOTA - 8, 9), -ENOSPC);
}

static void submit_field_checks(struct kunit *test)
{
	struct kdc_submit ok = { .version = KDC_ABI_VERSION, .size = sizeof(ok),
				 .id = 1, .epoch = 1, .src = 0x10000,
				 .dst = 0x20000, .length = 4096,
				 .queue = KDC_QUEUE_AUTO };
	struct kdc_submit s;

	KUNIT_EXPECT_EQ(test, kdc_submit_check(&ok, 4), 0);
	s = ok; s.version = 1;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EPROTO);
	s = ok; s.size--;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.reserved[1] = 1;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.reserved1 = 1;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.timeout_ms = KDC_MAX_TIMEOUT_MS;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), 0);
	s = ok; s.timeout_ms = KDC_MAX_TIMEOUT_MS + 1;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.flags = 0x20;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.flags = KDC_SUBMIT_FIXED;
	KUNIT_EXPECT_EQ(test, kdc_fixed_check(&s, 4), 0);
	s = ok; s.flags = KDC_SUBMIT_FIXED | KDC_SUBMIT_ZEROCOPY;
	KUNIT_EXPECT_EQ(test, kdc_fixed_check(&s, 4), -EINVAL);
	s = ok; s.flags = KDC_SUBMIT_FIXED | KDC_SUBMIT_BOUNCE;
	KUNIT_EXPECT_EQ(test, kdc_fixed_check(&s, 4), -EINVAL);
	s = ok; s.flags = KDC_SUBMIT_BOUNCE | KDC_SUBMIT_ZEROCOPY;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.queue = 4;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.queue = 3;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), 0);
	s = ok; s.length = 0;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.length = KDC_MAX_LENGTH + 1;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.dst = s.src + 4095;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.dst = s.src + 4096;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), 0);
	s = ok; s.src = U64_MAX - 100;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s = ok; s.flags = KDC_SUBMIT_NOP;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), -EINVAL);
	s.src = s.dst = 0; s.length = 0;
	KUNIT_EXPECT_EQ(test, kdc_submit_check(&s, 4), 0);
}

static void sgl_from_dma_segments(struct kunit *test)
{
	static const u32 lens[] = { 100, 4096, 3900 };
	struct kdc_hw_sge out[4];
	struct scatterlist *sg;
	struct sg_table sgt;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, sg_alloc_table(&sgt, ARRAY_SIZE(lens), GFP_KERNEL), 0);
	for_each_sgtable_sg(&sgt, sg, i) {
		sg_dma_address(sg) = 0x100000000ull + i * 0x10000;
		sg_dma_len(sg) = lens[i];
	}
	sgt.nents = ARRAY_SIZE(lens);
	KUNIT_EXPECT_EQ(test, kdc_fill_hw_sgl(out, 4, &sgt, 8096), 3);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(out[2].addr), 0x100020000ull);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(out[1].len), 4096);
	KUNIT_EXPECT_EQ(test, out[0].rsvd, 0);
	KUNIT_EXPECT_EQ(test, kdc_fill_hw_sgl(out, 4, &sgt, 8095), -EINVAL);
	KUNIT_EXPECT_EQ(test, kdc_fill_hw_sgl(out, 2, &sgt, 8096), -E2BIG);
	/* Only mapped (merged) segments count: an IOMMU may merge them. */
	sgt.nents = 1;
	KUNIT_EXPECT_EQ(test, kdc_fill_hw_sgl(out, 1, &sgt, 100), 1);
	sg_free_table(&sgt);
}

static void hw_layout(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sizeof(struct kdc_hw_sqe), KDC_SQE_SIZE);
	KUNIT_EXPECT_EQ(test, sizeof(struct kdc_hw_sge), KDC_SGE_SIZE);
	KUNIT_EXPECT_EQ(test, sizeof(struct kdc_hw_cqe), KDC_CQE_SIZE);
	KUNIT_EXPECT_EQ(test, offsetof(struct kdc_hw_cqe, status), 14);
	KUNIT_EXPECT_GE(test, (unsigned long)KDC_DRV_SRC_SEGS,
			kdc_pages_spanned(1, KDC_MAX_LENGTH));
	/* head + body pages + tail */
	KUNIT_EXPECT_GE(test, (unsigned long)KDC_DRV_DST_SEGS,
			kdc_pages_spanned(1, KDC_MAX_LENGTH) + 2);
	KUNIT_EXPECT_EQ(test, KDC_SGL_BLOCK, 4096);
	KUNIT_EXPECT_EQ(test, KDC_SGL_HEAD_OFF % KDC_CACHELINE, 0);
	KUNIT_EXPECT_EQ(test, sizeof(struct kdc_submit), 80);
}

static void shadow_event_index(struct kunit *test)
{
	/* the device has fetched up to the old tail: ring */
	KUNIT_EXPECT_TRUE(test, kdc_need_event(5, 6, 5, 64));
	KUNIT_EXPECT_TRUE(test, kdc_need_event(5, 9, 5, 64));
	KUNIT_EXPECT_TRUE(test, kdc_need_event(63, 1, 63, 64));
	KUNIT_EXPECT_TRUE(test, kdc_need_event(0, 1, 63, 64));
	/* still behind the old tail: it reads the shadow tail again itself */
	KUNIT_EXPECT_FALSE(test, kdc_need_event(4, 6, 5, 64));
	KUNIT_EXPECT_FALSE(test, kdc_need_event(60, 2, 63, 64));
	/* already saw the new tail */
	KUNIT_EXPECT_FALSE(test, kdc_need_event(1, 1, 63, 64));
	KUNIT_EXPECT_EQ(test, sizeof(struct kdc_hw_dbbuf), KDC_DBBUF_SIZE);
	KUNIT_EXPECT_EQ(test, offsetof(struct kdc_hw_dbbuf, sq_event), 4);
}

static struct kunit_case kdc_cases[] = {
	KUNIT_CASE(cid_generation),
	KUNIT_CASE(ring_math),
	KUNIT_CASE(phase_flips_on_wrap),
	KUNIT_CASE(page_span_and_overlap),
	KUNIT_CASE(cancel_by_ownership),
	KUNIT_CASE(status_mapping),
	KUNIT_CASE(watchdog_and_reclaim),
	KUNIT_CASE(dst_split),
	KUNIT_CASE(admission_and_quota),
	KUNIT_CASE(submit_field_checks),
	KUNIT_CASE(sgl_from_dma_segments),
	KUNIT_CASE(hw_layout),
	KUNIT_CASE(shadow_event_index),
	{}
};

static struct kunit_suite kdc_suite = {
	.name = "kdc",
	.test_cases = kdc_cases,
};
kunit_test_suite(kdc_suite);
MODULE_LICENSE("GPL");
