// SPDX-License-Identifier: GPL-2.0
/*
 * Per-command DMA resources.
 *
 * Zero-copy: pin both user ranges, build one sg_table per side, map them with
 * dma_map_sgtable() and describe the mapped segments to the device in an SG
 * list block from a coherent DMA pool. Only whole cache lines of the
 * destination are mapped for device writes; the partial first and last lines
 * are written by the device into the SG block and copied into place after
 * completion. Bounce: copy the source into a pooled coherent buffer at submit,
 * let the device write the second half of that buffer, and copy it into the
 * pinned destination pages when the device has completed.
 *
 * Before anything is mapped, the destination's physical byte ranges are
 * entered in a module-wide interval tree; a destination that shares a byte
 * with any in-flight command's destination (any file, any kdc device) is
 * refused with EADDRINUSE. Together with the head/tail split this keeps every
 * device-writable streaming mapping off cache lines that other live mappings
 * or CPU copy-backs use.
 *
 * Fixed (registered-buffer) commands: the buffers were pinned and mapped once
 * by KDC_IOC_REGISTER; a command takes a reference on each, builds its SG
 * lists from the per-page DMA addresses and keeps the head/tail split of the
 * zero-copy path. The tracker sees their destinations like any other.
 *
 * Everything is released by kdc_cmd_release(), which callers run only once the
 * device can no longer touch the command: after its completion, after a reset
 * that stopped the device, or after bus mastering was turned off. That is also
 * when a fixed command drops its buffer references.
 */
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "kdc_drv.h"

atomic_t kdc_live_cmds, kdc_global_pinned, kdc_global_mapped, kdc_dst_ranges;

static bool track_overlap = true;
module_param(track_overlap, bool, 0444);
MODULE_PARM_DESC(track_overlap,
		 "Refuse destinations that overlap an in-flight one (EADDRINUSE). Turning it off breaks the "
		 "destination rule; it exists only to measure the tracker's cost");


int kdc_fill_hw_sgl(struct kdc_hw_sge *out, unsigned int max,
		    struct sg_table *sgt, u32 length)
{
	struct scatterlist *sg;
	unsigned int i, n = 0;
	u64 total = 0;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (n == max)
			return -E2BIG;
		out[n].addr = cpu_to_le64(sg_dma_address(sg));
		out[n].len = cpu_to_le32(sg_dma_len(sg));
		out[n].rsvd = 0;
		total += sg_dma_len(sg);
		n++;
	}
	if (!n || total != length)
		return -EINVAL;
	return n;
}

static int kdc_pin(struct kdc_dev *kdev, u64 uaddr, u32 len,
		   unsigned int gup_flags, struct page ***pagesp,
		   unsigned int *npagesp)
{
	unsigned long n = kdc_pages_spanned(uaddr, len);
	struct page **pages;
	int got;

	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	got = pin_user_pages_fast(uaddr & PAGE_MASK, n, gup_flags, pages);
	if (got != n) {
		if (got > 0)
			unpin_user_pages(pages, got);
		kvfree(pages);
		return got < 0 ? got : -EFAULT;
	}
	*pagesp = pages;
	*npagesp = n;
	atomic64_add(n, &kdev->pinned_pages);
	atomic_add(n, &kdc_global_pinned);
	return 0;
}

static int kdc_map(struct kdc_dev *kdev, struct sg_table *sgt,
		   struct page **pages, unsigned int npages, u64 uaddr, u32 len,
		   enum dma_data_direction dir, bool *mapped)
{
	struct device *dev = &kdev->pdev->dev;
	int err;

	err = sg_alloc_table_from_pages_segment(sgt, pages, npages,
						offset_in_page(uaddr), len,
						dma_get_max_seg_size(dev),
						GFP_KERNEL);
	if (err)
		return err;
	err = dma_map_sgtable(dev, sgt, dir, 0);
	if (err)
		return err;
	*mapped = true;
	atomic64_inc(&kdev->mapped_sgts);
	atomic_inc(&kdc_global_mapped);
	return 0;
}

static void kdc_unmap(struct kdc_dev *kdev, struct sg_table *sgt,
		      enum dma_data_direction dir, bool *mapped)
{
	if (*mapped) {
		dma_unmap_sgtable(&kdev->pdev->dev, sgt, dir, 0);
		atomic64_dec(&kdev->mapped_sgts);
		atomic_dec(&kdc_global_mapped);
		*mapped = false;
	}
	if (sgt->sgl)
		sg_free_table(sgt);
	sgt->sgl = NULL;
}

static void kdc_unpin(struct kdc_dev *kdev, struct page ***pagesp,
		      unsigned int npages, bool dirty)
{
	if (!*pagesp)
		return;
	unpin_user_pages_dirty_lock(*pagesp, npages, dirty);
	atomic64_sub(npages, &kdev->pinned_pages);
	atomic_sub(npages, &kdc_global_pinned);
	kvfree(*pagesp);
	*pagesp = NULL;
}

/* ---- destination overlap tracker ---- */

static struct kdc_tracker kdc_dst = {
	.lock = __SPIN_LOCK_UNLOCKED(kdc_dst.lock),
	.tree = RB_ROOT_CACHED,
};

/*
 * Physically contiguous runs of [off, off + len) of @pages into @r (room for
 * @npages runs); returns the number of runs.
 */
static unsigned int kdc_ranges_build(struct kdc_dst_range *r, struct page **pages,
				     unsigned int npages, u64 off, u64 len)
{
	unsigned int i, n = 0;

	for (i = 0; i < npages && len; i++) {
		u64 chunk = min_t(u64, len, PAGE_SIZE - off);
		phys_addr_t start = page_to_phys(pages[i]) + off;

		if (n && r[n - 1].it.last + 1 == start) {
			r[n - 1].it.last += chunk;
		} else {
			r[n].it.start = start;
			r[n].it.last = start + chunk - 1;
			n++;
		}
		len -= chunk;
		off = 0;
	}
	return n;
}

/*
 * Insert all runs or none. Each run is checked against everything inserted
 * before it, including the same caller's earlier runs, so a destination that
 * maps one physical page twice is refused as well.
 */
static int kdc_track(struct kdc_tracker *t, struct kdc_dst_range *r, unsigned int nr)
{
	unsigned int i;

	spin_lock(&t->lock);
	for (i = 0; i < nr; i++) {
		struct interval_tree_node *it = &r[i].it;

		if (interval_tree_iter_first(&t->tree, it->start, it->last))
			break;
		interval_tree_insert(it, &t->tree);
	}
	if (i < nr) {
		while (i--)
			interval_tree_remove(&r[i].it, &t->tree);
		spin_unlock(&t->lock);
		return -EADDRINUSE;
	}
	spin_unlock(&t->lock);
	atomic_add(nr, &kdc_dst_ranges);
	return 0;
}

static void kdc_untrack(struct kdc_tracker *t, struct kdc_dst_range *r, unsigned int nr)
{
	unsigned int i;

	spin_lock(&t->lock);
	for (i = 0; i < nr; i++)
		interval_tree_remove(&r[i].it, &t->tree);
	spin_unlock(&t->lock);
	atomic_sub(nr, &kdc_dst_ranges);
}

static int kdc_dst_track(struct kdc_dev *kdev, struct kdc_cmd *cmd)
{
	int err;

	cmd->ranges = kmalloc_array(cmd->dst_npages, sizeof(*cmd->ranges), GFP_KERNEL);
	if (!cmd->ranges)
		return -ENOMEM;
	cmd->nr_ranges = kdc_ranges_build(cmd->ranges, cmd->dst_pages, cmd->dst_npages,
					  cmd->dst_offset, cmd->length);
	/*
	 * A registered buffer's pages are in the module-wide tree for as long as
	 * it exists, so fixed destinations can only meet other fixed
	 * destinations of the same buffer: they are checked there.
	 */
	cmd->tracker = cmd->fixed ? &cmd->dst_buf->fixed_dst : &kdc_dst;
	err = kdc_track(cmd->tracker, cmd->ranges, cmd->nr_ranges);
	if (err) {
		atomic64_inc(&kdev->overlaps);
		return err;
	}
	cmd->ranges_tracked = true;
	return 0;
}

static void kdc_dst_untrack(struct kdc_cmd *cmd)
{
	if (cmd->ranges_tracked) {
		kdc_untrack(cmd->tracker, cmd->ranges, cmd->nr_ranges);
		cmd->ranges_tracked = false;
	}
	kfree(cmd->ranges);
	cmd->ranges = NULL;
}

/* ---- release ---- */

static void kdc_copy_to_pages(struct page **pages, unsigned int offset,
			      const u8 *src, size_t len)
{
	pages += offset >> PAGE_SHIFT;
	offset &= ~PAGE_MASK;
	while (len) {
		size_t chunk = min_t(size_t, len, PAGE_SIZE - offset);

		memcpy_to_page(*pages++, offset, src, chunk);
		src += chunk;
		len -= chunk;
		offset = 0;
	}
}

/* ---- registered buffers ---- */

struct kdc_regbuf *kdc_regbuf_create(struct kdc_dev *kdev, u64 uaddr, u64 len, int *errp)
{
	struct device *dev = &kdev->pdev->dev;
	unsigned int i, n = len >> PAGE_SHIFT;
	struct kdc_regbuf *b;
	int got, err = -ENOMEM;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b) {
		*errp = -ENOMEM;
		return NULL;
	}
	b->uaddr = uaddr;
	b->len = len;
	spin_lock_init(&b->fixed_dst.lock);
	b->fixed_dst.tree = RB_ROOT_CACHED;
	b->pages = kvmalloc_array(n, sizeof(*b->pages), GFP_KERNEL);
	b->dma = kvmalloc_array(n, sizeof(*b->dma), GFP_KERNEL);
	if (!b->pages || !b->dma)
		goto out_free;
	got = pin_user_pages_fast(uaddr, n, FOLL_WRITE | FOLL_LONGTERM, b->pages);
	if (got != n) {
		if (got > 0)
			unpin_user_pages(b->pages, got);
		err = got < 0 ? got : -EFAULT;
		goto out_free;
	}
	b->npages = n;
	atomic64_add(n, &kdev->pinned_pages);
	atomic_add(n, &kdc_global_pinned);
	/*
	 * Its pages stay device-writable while it is registered, so no other
	 * destination (any file, any device, any path) may share them; checked
	 * before anything is mapped, as for a command.
	 */
	b->ranges = kvmalloc_array(n, sizeof(*b->ranges), GFP_KERNEL);
	if (b->ranges)
		b->nr_ranges = kdc_ranges_build(b->ranges, b->pages, n, 0, len);
	err = b->ranges ? kdc_track(&kdc_dst, b->ranges, b->nr_ranges) : -ENOMEM;
	if (err) {
		if (err == -EADDRINUSE)
			atomic64_inc(&kdev->overlaps);
		goto destroy;
	}
	b->tracked = true;
	b->mapped = true;
	atomic64_inc(&kdev->mapped_sgts);
	atomic_inc(&kdc_global_mapped);
	for (i = 0; i < n; i++) {
		b->dma[i] = dma_map_page(dev, b->pages[i], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, b->dma[i])) {
			err = -ENOMEM;
			goto destroy;
		}
		b->nmapped = i + 1;
	}
	return b;
destroy:
	kdc_regbuf_destroy(kdev, b);
	*errp = err;
	return NULL;
out_free:
	kvfree(b->pages);
	kvfree(b->dma);
	kfree(b);
	*errp = err;
	return NULL;
}

/* Callers guarantee no command holds @b, or that the device can no longer DMA. */
void kdc_regbuf_unmap(struct kdc_dev *kdev, struct kdc_regbuf *b)
{
	unsigned int i;

	if (!b->mapped)
		return;
	for (i = 0; i < b->nmapped; i++)
		dma_unmap_page(&kdev->pdev->dev, b->dma[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
	b->nmapped = 0;
	b->mapped = false;
	atomic64_dec(&kdev->mapped_sgts);
	atomic_dec(&kdc_global_mapped);
}

void kdc_regbuf_destroy(struct kdc_dev *kdev, struct kdc_regbuf *b)
{
	WARN_ON_ONCE(atomic_read(&b->users));
	kdc_regbuf_unmap(kdev, b);
	if (b->tracked)
		kdc_untrack(&kdc_dst, b->ranges, b->nr_ranges);
	kvfree(b->ranges);
	/* the device may have written any of it */
	unpin_user_pages_dirty_lock(b->pages, b->npages, true);
	atomic64_sub(b->npages, &kdev->pinned_pages);
	atomic_sub(b->npages, &kdc_global_pinned);
	kvfree(b->pages);
	kvfree(b->dma);
	kfree(b);
}

/* Cache maintenance for [off, off + len) of a registered buffer (no-op on coherent x86). */
static void kdc_regbuf_sync(struct kdc_dev *kdev, struct kdc_regbuf *b, u64 off, u32 len,
			    bool for_device)
{
	struct device *dev = &kdev->pdev->dev;

	while (len) {
		u32 in = offset_in_page(off), chunk = min_t(u64, len, PAGE_SIZE - in);
		dma_addr_t a = b->dma[off >> PAGE_SHIFT];

		if (for_device)
			dma_sync_single_range_for_device(dev, a, in, chunk, DMA_BIDIRECTIONAL);
		else
			dma_sync_single_range_for_cpu(dev, a, in, chunk, DMA_BIDIRECTIONAL);
		off += chunk;
		len -= chunk;
	}
}

/* Device SG entries for [off, off + len) of @b, merging DMA-contiguous pages. */
static int kdc_regbuf_sgl(struct kdc_hw_sge *out, unsigned int max, const struct kdc_regbuf *b,
			  u64 off, u32 len)
{
	unsigned int n = 0;

	while (len) {
		u32 in = offset_in_page(off), chunk = min_t(u64, len, PAGE_SIZE - in);
		dma_addr_t a = b->dma[off >> PAGE_SHIFT] + in;

		if (n && le64_to_cpu(out[n - 1].addr) + le32_to_cpu(out[n - 1].len) == a) {
			out[n - 1].len = cpu_to_le32(le32_to_cpu(out[n - 1].len) + chunk);
		} else {
			if (n == max)
				return -E2BIG;
			out[n].addr = cpu_to_le64(a);
			out[n].len = cpu_to_le32(chunk);
			out[n].rsvd = 0;
			n++;
		}
		off += chunk;
		len -= chunk;
	}
	return n;
}

void kdc_cmd_release(struct kdc_dev *kdev, struct kdc_cmd *cmd, bool copy_back)
{
	kdc_unmap(kdev, &cmd->src_sgt, DMA_TO_DEVICE, &cmd->src_mapped);
	kdc_unmap(kdev, &cmd->dst_sgt, DMA_FROM_DEVICE, &cmd->dst_mapped);
	if (cmd->dst_buf && cmd->length > cmd->head_len + cmd->tail_len)
		kdc_regbuf_sync(kdev, cmd->dst_buf, cmd->dst - cmd->dst_buf->uaddr + cmd->head_len,
				cmd->length - cmd->head_len - cmd->tail_len, false);
	if (copy_back && cmd->dst_pages) {
		if (cmd->bounce_buf)
			kdc_copy_to_pages(cmd->dst_pages, cmd->dst_offset,
					  (u8 *)cmd->bounce_buf + kdev->bounce_max,
					  cmd->length);
		if (cmd->sgl && cmd->head_len)
			kdc_copy_to_pages(cmd->dst_pages, cmd->dst_offset,
					  (u8 *)cmd->sgl + KDC_SGL_HEAD_OFF,
					  cmd->head_len);
		if (cmd->sgl && cmd->tail_len)
			kdc_copy_to_pages(cmd->dst_pages,
					  cmd->dst_offset + cmd->length -
						  cmd->tail_len,
					  (u8 *)cmd->sgl + KDC_SGL_TAIL_OFF,
					  cmd->tail_len);
	}
	if (cmd->fixed) {
		cmd->dst_pages = NULL; /* borrowed from the registered buffer */
	} else {
		kdc_unpin(kdev, &cmd->src_pages, cmd->src_npages, false);
		/* A zero-copy destination may have been written even on error. */
		kdc_unpin(kdev, &cmd->dst_pages, cmd->dst_npages,
			  !cmd->use_bounce || copy_back);
	}
	kdc_dst_untrack(cmd);
	if (cmd->bounce_buf) {
		dma_pool_free(kdev->bounce_pool, cmd->bounce_buf, cmd->bounce_dma);
		cmd->bounce_buf = NULL;
	}
	if (cmd->sgl) {
		dma_pool_free(kdev->sgl_pool, cmd->sgl, cmd->sgl_dma);
		cmd->sgl = NULL;
	}
	/* From here the device cannot reach the registered buffers through us. */
	if (cmd->src_buf)
		atomic_dec(&cmd->src_buf->users);
	if (cmd->dst_buf)
		atomic_dec(&cmd->dst_buf->users);
	cmd->src_buf = cmd->dst_buf = NULL;
}

void kdc_cmd_free(struct kdc_cmd *cmd)
{
	kfree(cmd);
	atomic_dec(&kdc_live_cmds);
}

/* ---- prepare ---- */

static int kdc_prepare_bounce(struct kdc_dev *kdev, struct kdc_cmd *cmd,
			      const struct kdc_submit *s)
{
	cmd->bounce_buf = dma_pool_alloc(kdev->bounce_pool, GFP_KERNEL,
					 &cmd->bounce_dma);
	if (!cmd->bounce_buf)
		return -ENOMEM;
	if (copy_from_user(cmd->bounce_buf, u64_to_user_ptr(s->src), s->length))
		return -EFAULT;
	cmd->sqe.src = cpu_to_le64(cmd->bounce_dma);
	cmd->sqe.dst = cpu_to_le64(cmd->bounce_dma + kdev->bounce_max);
	return 0;
}

static void kdc_sge(struct kdc_hw_sge *e, dma_addr_t addr, u32 len)
{
	e->addr = cpu_to_le64(addr);
	e->len = cpu_to_le32(len);
	e->rsvd = 0;
}

static int kdc_prepare_zerocopy(struct kdc_dev *kdev, struct kdc_cmd *cmd,
				const struct kdc_submit *s)
{
	struct kdc_hw_sge *src_sgl, *dst_sgl;
	u32 head, body, tail;
	int nsrc, nbody, n = 0;
	int err;

	err = kdc_pin(kdev, s->src, s->length, 0, &cmd->src_pages,
		      &cmd->src_npages);
	if (err)
		return err;
	err = kdc_map(kdev, &cmd->src_sgt, cmd->src_pages, cmd->src_npages,
		      s->src, s->length, DMA_TO_DEVICE, &cmd->src_mapped);
	if (err)
		return err;
	kdc_dst_split(s->dst, s->length, &head, &body, &tail);
	cmd->head_len = head;
	cmd->tail_len = tail;
	if (body) {
		u64 b = s->dst + head;
		unsigned int first = (b >> PAGE_SHIFT) - (s->dst >> PAGE_SHIFT);

		err = kdc_map(kdev, &cmd->dst_sgt, cmd->dst_pages + first,
			      kdc_pages_spanned(b, body), b, body,
			      DMA_FROM_DEVICE, &cmd->dst_mapped);
		if (err)
			return err;
	}
	cmd->sgl = dma_pool_alloc(kdev->sgl_pool, GFP_KERNEL, &cmd->sgl_dma);
	if (!cmd->sgl)
		return -ENOMEM;
	src_sgl = cmd->sgl;
	dst_sgl = (struct kdc_hw_sge *)((u8 *)cmd->sgl + KDC_SGL_DST_OFF);
	nsrc = kdc_fill_hw_sgl(src_sgl, KDC_DRV_SRC_SEGS, &cmd->src_sgt,
			       s->length);
	if (nsrc < 0)
		return nsrc;
	if (head)
		kdc_sge(&dst_sgl[n++], cmd->sgl_dma + KDC_SGL_HEAD_OFF, head);
	if (body) {
		nbody = kdc_fill_hw_sgl(dst_sgl + n, KDC_DRV_DST_SEGS - 2,
					&cmd->dst_sgt, body);
		if (nbody < 0)
			return nbody;
		n += nbody;
	}
	if (tail)
		kdc_sge(&dst_sgl[n++], cmd->sgl_dma + KDC_SGL_TAIL_OFF, tail);
	cmd->sqe.flags = KDC_SQE_F_SRC_SGL | KDC_SQE_F_DST_SGL;
	cmd->sqe.src = cpu_to_le64(cmd->sgl_dma);
	cmd->sqe.dst = cpu_to_le64(cmd->sgl_dma + KDC_SGL_DST_OFF);
	cmd->sqe.src_nents = cpu_to_le16(nsrc);
	cmd->sqe.dst_nents = cpu_to_le16(n);
	return 0;
}

static int kdc_prepare_fixed(struct kdc_dev *kdev, struct kdc_cmd *cmd,
			     const struct kdc_submit *s)
{
	struct kdc_regbuf *sb = cmd->src_buf, *db = cmd->dst_buf;
	u64 soff = s->src - sb->uaddr, doff = s->dst - db->uaddr;
	struct kdc_hw_sge *src_sgl, *dst_sgl;
	u32 head, body, tail;
	int nsrc, nbody, n = 0;

	kdc_dst_split(s->dst, s->length, &head, &body, &tail);
	cmd->head_len = head;
	cmd->tail_len = tail;
	cmd->sgl = dma_pool_alloc(kdev->sgl_pool, GFP_KERNEL, &cmd->sgl_dma);
	if (!cmd->sgl)
		return -ENOMEM;
	src_sgl = cmd->sgl;
	dst_sgl = (struct kdc_hw_sge *)((u8 *)cmd->sgl + KDC_SGL_DST_OFF);
	nsrc = kdc_regbuf_sgl(src_sgl, KDC_DRV_SRC_SEGS, sb, soff, s->length);
	if (nsrc < 0)
		return nsrc;
	if (head)
		kdc_sge(&dst_sgl[n++], cmd->sgl_dma + KDC_SGL_HEAD_OFF, head);
	if (body) {
		nbody = kdc_regbuf_sgl(dst_sgl + n, KDC_DRV_DST_SEGS - 2, db, doff + head, body);
		if (nbody < 0)
			return nbody;
		n += nbody;
	}
	if (tail)
		kdc_sge(&dst_sgl[n++], cmd->sgl_dma + KDC_SGL_TAIL_OFF, tail);
	kdc_regbuf_sync(kdev, sb, soff, s->length, true);
	if (body)
		kdc_regbuf_sync(kdev, db, doff + head, body, true);
	cmd->sqe.flags = KDC_SQE_F_SRC_SGL | KDC_SQE_F_DST_SGL;
	cmd->sqe.src = cpu_to_le64(cmd->sgl_dma);
	cmd->sqe.dst = cpu_to_le64(cmd->sgl_dma + KDC_SGL_DST_OFF);
	cmd->sqe.dst_nents = cpu_to_le16(n);
	cmd->sqe.src_nents = cpu_to_le16(nsrc);
	return 0;
}

/*
 * Runs in the submitter's context without locks: pins, maps, copies. For a
 * fixed command the caller passes the registered buffers with a reference
 * taken; from here on kdc_cmd_release() drops it, on every path.
 */
struct kdc_cmd *kdc_cmd_prepare(struct kdc_dev *kdev, const struct kdc_submit *s,
				struct kdc_regbuf *src_buf, struct kdc_regbuf *dst_buf,
				int *errp)
{
	struct kdc_cmd *cmd;
	bool bounce;
	int err;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd) {
		if (src_buf)
			atomic_dec(&src_buf->users);
		if (dst_buf)
			atomic_dec(&dst_buf->users);
		*errp = -ENOMEM;
		return NULL;
	}
	atomic_inc(&kdc_live_cmds);
	INIT_LIST_HEAD(&cmd->node);
	INIT_LIST_HEAD(&cmd->ctx_node);
	cmd->src_buf = src_buf;
	cmd->dst_buf = dst_buf;
	cmd->id = s->id;
	cmd->length = s->length;
	cmd->timeout = msecs_to_jiffies(s->timeout_ms ?: READ_ONCE(cmd_timeout_ms));
	if (s->flags & KDC_SUBMIT_NOP) {
		cmd->sqe.opcode = KDC_OP_NOP;
		return cmd;
	}
	cmd->sqe.opcode = KDC_OP_COPY_CRC;
	cmd->sqe.length = cpu_to_le32(s->length);
	cmd->sqe.crc_seed = cpu_to_le32(s->crc_seed);
	cmd->quota_bytes = 2ull * s->length;
	cmd->dst = s->dst;
	if (s->flags & KDC_SUBMIT_FIXED) {
		u64 doff = s->dst - dst_buf->uaddr;

		cmd->fixed = true;
		cmd->dst_pages = dst_buf->pages + (doff >> PAGE_SHIFT);
		cmd->dst_npages = kdc_pages_spanned(s->dst, s->length);
		cmd->dst_offset = offset_in_page(s->dst);
		err = (track_overlap ? kdc_dst_track(kdev, cmd) : 0) ?:
		      kdc_prepare_fixed(kdev, cmd, s);
		if (err)
			goto fail;
		return cmd;
	}
	bounce = s->flags & KDC_SUBMIT_BOUNCE ||
		 (!(s->flags & KDC_SUBMIT_ZEROCOPY) && s->length <= kdev->bounce_max);
	if (bounce && s->length > kdev->bounce_max) {
		err = -EINVAL; /* forced bounce larger than the bounce buffer */
		goto fail;
	}
	err = kdc_pin(kdev, s->dst, s->length, FOLL_WRITE, &cmd->dst_pages,
		      &cmd->dst_npages);
	if (err)
		goto fail;
	cmd->dst_offset = offset_in_page(s->dst);
	cmd->use_bounce = bounce;
	/* Before any mapping exists: DMA-API debug checks overlaps at map time. */
	err = track_overlap ? kdc_dst_track(kdev, cmd) : 0;
	if (err)
		goto fail;
	err = bounce ? kdc_prepare_bounce(kdev, cmd, s) :
		       kdc_prepare_zerocopy(kdev, cmd, s);
	if (err)
		goto fail;
	return cmd;
fail:
	kdc_cmd_release(kdev, cmd, false);
	kdc_cmd_free(cmd);
	*errp = err;
	return NULL;
}
