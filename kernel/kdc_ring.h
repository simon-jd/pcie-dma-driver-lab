/* SPDX-License-Identifier: GPL-2.0 */
/* Pure ring, phase and command-id arithmetic; shared by the driver and KUnit. */
#ifndef KDC_RING_H
#define KDC_RING_H
#include <linux/types.h>
#include <linux/mm.h>

/*
 * A command id is a 12-bit tag (index into the queue's slot table) plus a 4-bit
 * generation that changes every time the tag is reused, so a stale or corrupted
 * completion for an old command cannot complete the tag's new owner.
 */
#define KDC_TAG_BITS 12
#define KDC_TAG_MASK ((1u << KDC_TAG_BITS) - 1)
#define KDC_GEN_MASK 0xfu

static inline u16 kdc_cid_make(u16 tag, u8 gen)
{
	return (u16)((gen & KDC_GEN_MASK) << KDC_TAG_BITS | (tag & KDC_TAG_MASK));
}

static inline u16 kdc_cid_tag(u16 cid)
{
	return cid & KDC_TAG_MASK;
}

static inline u8 kdc_gen_next(u8 gen)
{
	return (gen + 1) & KDC_GEN_MASK;
}

/* depth is a power of two; one slot stays empty so full != empty. */
static inline u16 kdc_ring_used(u16 tail, u16 head, u16 depth)
{
	return (tail - head) & (depth - 1);
}

static inline u16 kdc_ring_next(u16 index, u16 depth)
{
	return (index + 1) & (depth - 1);
}

/* A completion entry is new when its phase bit matches the expected phase. */
static inline bool kdc_cqe_is_new(u16 status, u8 phase)
{
	return (status & 1) == phase;
}

/* Advance the completion head; the expected phase flips on every wrap. */
static inline void kdc_cq_advance(u16 *head, u8 *phase, u16 depth)
{
	*head = kdc_ring_next(*head, depth);
	if (!*head)
		*phase ^= 1;
}

/* Pages a user range touches; 0 for an empty or wrapping range. */
static inline unsigned long kdc_pages_spanned(u64 addr, u32 len)
{
	if (!len || addr + len < addr)
		return 0;
	return ((addr + len - 1) >> PAGE_SHIFT) - (addr >> PAGE_SHIFT) + 1;
}

static inline bool kdc_ranges_overlap(u64 a, u64 b, u32 len)
{
	return a < b + len && b < a + len;
}

/*
 * Destination split for the zero-copy path. The device may write through a
 * streaming mapping only cache lines that lie entirely inside the destination;
 * the partial lines at either end (head, tail; < KDC_CACHELINE bytes each) go to
 * a coherent bounce area and are copied into place after completion. Then no
 * streaming device-writable mapping ever shares a cache line with memory the
 * command does not own.
 */
#define KDC_CACHELINE 64

static inline void kdc_dst_split(u64 dst, u32 len, u32 *head, u32 *body,
				 u32 *tail)
{
	u64 end = dst + len;
	u64 b = (dst + KDC_CACHELINE - 1) & ~(u64)(KDC_CACHELINE - 1);
	u64 e = end & ~(u64)(KDC_CACHELINE - 1);

	if (b > e) { /* inside one line, touching neither boundary */
		*head = len;
		*body = 0;
		*tail = 0;
		return;
	}
	*head = b - dst;
	*body = e - b;
	*tail = end - e;
}
#endif
