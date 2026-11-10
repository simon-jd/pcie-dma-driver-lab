// SPDX-License-Identifier: GPL-2.0
/*
 * Registered buffers (KDC_IOC_REGISTER) and the KDC_SUBMIT_FIXED commands that
 * use them.
 *
 * A registered buffer is pinned (FOLL_LONGTERM) and mapped once, page by page
 * for DMA_BIDIRECTIONAL. A fixed command whose source and destination lie in
 * the file's buffers takes a reference on each, builds its SG lists from the
 * per-page DMA addresses, syncs its ranges (a no-op on x86) and keeps the
 * head/tail split of the zero-copy path. The ownership contract is the one of
 * every other command: the references are dropped in kdc_fixed_release(),
 * which kdc_cmd_release() calls under the same release rule;
 * KDC_IOC_UNREGISTER fails with EBUSY while one is held; close() frees the
 * buffers after its last command; remove() unmaps every open file's buffers
 * once all commands have ended (they stay pinned until the file closes).
 *
 * A buffer's pages enter the module-wide overlap tracker for as long as it is
 * registered, so no other destination can share them; fixed destinations are
 * checked against each other in the buffer's own tree.
 *
 * Everything here lives in its own functions, called from the shared paths by
 * one branch each (submit, release, close, remove): the zero-copy and bounce
 * paths run the same code as without registered buffers.
 */
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "kdc_drv.h"

/* Physically contiguous runs of [off, off + len) of @pages; returns the count. */
static unsigned int kdc_fixed_ranges(struct kdc_dst_range *r, struct page **pages,
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

/* Insert all runs or none, each checked against everything inserted before it. */
static int kdc_fixed_track(spinlock_t *lock, struct rb_root_cached *tree,
			   struct kdc_dst_range *r, unsigned int nr)
{
	unsigned int i;

	spin_lock(lock);
	for (i = 0; i < nr; i++) {
		struct interval_tree_node *it = &r[i].it;

		if (interval_tree_iter_first(tree, it->start, it->last))
			break;
		interval_tree_insert(it, tree);
	}
	if (i < nr) {
		while (i--)
			interval_tree_remove(&r[i].it, tree);
		spin_unlock(lock);
		return -EADDRINUSE;
	}
	spin_unlock(lock);
	atomic_add(nr, &kdc_dst_ranges);
	return 0;
}

static void kdc_fixed_untrack(spinlock_t *lock, struct rb_root_cached *tree,
			      struct kdc_dst_range *r, unsigned int nr)
{
	unsigned int i;

	spin_lock(lock);
	for (i = 0; i < nr; i++)
		interval_tree_remove(&r[i].it, tree);
	spin_unlock(lock);
	atomic_sub(nr, &kdc_dst_ranges);
}

/* ---- the buffers ---- */

static void kdc_regbuf_unmap(struct kdc_dev *kdev, struct kdc_regbuf *b)
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

/* Callers guarantee no command holds @b (users == 0) and hold files_mutex. */
static void kdc_regbuf_destroy(struct kdc_dev *kdev, struct kdc_regbuf *b)
{
	WARN_ON_ONCE(atomic_read(&b->users));
	kdc_regbuf_unmap(kdev, b);
	if (b->tracked)
		kdc_fixed_untrack(&kdc_dst_lock, &kdc_dst_tree, b->ranges, b->nr_ranges);
	kvfree(b->ranges);
	/* the device may have written any of it */
	unpin_user_pages_dirty_lock(b->pages, b->npages, true);
	atomic64_sub(b->npages, &kdev->pinned_pages);
	atomic_sub(b->npages, &kdc_global_pinned);
	kvfree(b->pages);
	kvfree(b->dma);
	kfree(b);
}

static struct kdc_regbuf *kdc_regbuf_create(struct kdc_dev *kdev, u64 uaddr, u64 len, int *errp)
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
	spin_lock_init(&b->fixed_lock);
	b->fixed_tree = RB_ROOT_CACHED;
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
		b->nr_ranges = kdc_fixed_ranges(b->ranges, b->pages, n, 0, len);
	err = b->ranges ? kdc_fixed_track(&kdc_dst_lock, &kdc_dst_tree, b->ranges, b->nr_ranges) :
			  -ENOMEM;
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

/* ---- fixed commands ---- */

static void kdc_fixed_sge(struct kdc_hw_sge *e, dma_addr_t addr, u32 len)
{
	e->addr = cpu_to_le64(addr);
	e->len = cpu_to_le32(len);
	e->rsvd = 0;
}

static void kdc_fixed_copy_to_pages(struct page **pages, unsigned int offset,
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

/*
 * kdc_cmd_release() for a fixed command: called under the same rule, once the
 * device can no longer touch it. The destination's pages are the buffer's;
 * the device wrote the partial first and last lines into the SG block.
 */
void kdc_fixed_release(struct kdc_dev *kdev, struct kdc_cmd *cmd, bool copy_back)
{
	struct kdc_regbuf *db = cmd->dst_buf;

	if (db) {
		u64 doff = cmd->dst - db->uaddr;
		struct page **pages = db->pages + (doff >> PAGE_SHIFT);
		unsigned int off = offset_in_page(cmd->dst);

		if (cmd->length > cmd->head_len + cmd->tail_len)
			kdc_regbuf_sync(kdev, db, doff + cmd->head_len,
					cmd->length - cmd->head_len - cmd->tail_len, false);
		if (copy_back && cmd->sgl && cmd->head_len)
			kdc_fixed_copy_to_pages(pages, off, (u8 *)cmd->sgl + KDC_SGL_HEAD_OFF,
						cmd->head_len);
		if (copy_back && cmd->sgl && cmd->tail_len)
			kdc_fixed_copy_to_pages(pages, off + cmd->length - cmd->tail_len,
						(u8 *)cmd->sgl + KDC_SGL_TAIL_OFF, cmd->tail_len);
	}
	if (cmd->ranges_tracked) {
		kdc_fixed_untrack(&db->fixed_lock, &db->fixed_tree, cmd->ranges, cmd->nr_ranges);
		cmd->ranges_tracked = false;
	}
	kfree(cmd->ranges);
	cmd->ranges = NULL;
	if (cmd->sgl) {
		dma_pool_free(kdev->sgl_pool, cmd->sgl, cmd->sgl_dma);
		cmd->sgl = NULL;
	}
	/* From here the device cannot reach the registered buffers through us. */
	if (cmd->src_buf)
		atomic_dec(&cmd->src_buf->users);
	if (db)
		atomic_dec(&db->users);
	cmd->src_buf = cmd->dst_buf = NULL;
}

static int kdc_fixed_sgls(struct kdc_dev *kdev, struct kdc_cmd *cmd, const struct kdc_submit *s)
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
		kdc_fixed_sge(&dst_sgl[n++], cmd->sgl_dma + KDC_SGL_HEAD_OFF, head);
	if (body) {
		nbody = kdc_regbuf_sgl(dst_sgl + n, KDC_DRV_DST_SEGS - 2, db, doff + head, body);
		if (nbody < 0)
			return nbody;
		n += nbody;
	}
	if (tail)
		kdc_fixed_sge(&dst_sgl[n++], cmd->sgl_dma + KDC_SGL_TAIL_OFF, tail);
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
 * The fixed counterpart of kdc_cmd_prepare(): no pinning, no mapping. The
 * caller passes the buffers with a reference taken; from here on
 * kdc_fixed_release() drops it, on every path.
 */
static struct kdc_cmd *kdc_fixed_prepare(struct kdc_dev *kdev, const struct kdc_submit *s,
					 struct kdc_regbuf *src_buf, struct kdc_regbuf *dst_buf,
					 int *errp)
{
	u64 doff = s->dst - dst_buf->uaddr;
	struct kdc_cmd *cmd;
	int err = 0;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd) {
		atomic_dec(&src_buf->users);
		atomic_dec(&dst_buf->users);
		*errp = -ENOMEM;
		return NULL;
	}
	atomic_inc(&kdc_live_cmds);
	INIT_LIST_HEAD(&cmd->node);
	INIT_LIST_HEAD(&cmd->ctx_node);
	cmd->fixed = true;
	cmd->src_buf = src_buf;
	cmd->dst_buf = dst_buf;
	cmd->id = s->id;
	cmd->length = s->length;
	cmd->dst = s->dst;
	cmd->timeout = msecs_to_jiffies(s->timeout_ms ?: READ_ONCE(cmd_timeout_ms));
	cmd->sqe.opcode = KDC_OP_COPY_CRC;
	cmd->sqe.length = cpu_to_le32(s->length);
	cmd->sqe.crc_seed = cpu_to_le32(s->crc_seed);
	cmd->quota_bytes = 2ull * s->length;
	/*
	 * The buffer's pages are in the module-wide tree for as long as it
	 * exists, so a fixed destination can only meet another fixed destination
	 * in the same buffer: checked in the buffer's own tree.
	 */
	if (kdc_track_overlap) {
		unsigned int npages = kdc_pages_spanned(s->dst, s->length);

		cmd->ranges = kmalloc_array(npages, sizeof(*cmd->ranges), GFP_KERNEL);
		if (!cmd->ranges) {
			err = -ENOMEM;
			goto fail;
		}
		cmd->nr_ranges = kdc_fixed_ranges(cmd->ranges, dst_buf->pages + (doff >> PAGE_SHIFT),
						  npages, offset_in_page(s->dst), s->length);
		err = kdc_fixed_track(&dst_buf->fixed_lock, &dst_buf->fixed_tree, cmd->ranges,
				      cmd->nr_ranges);
		if (err) {
			atomic64_inc(&kdev->overlaps);
			goto fail;
		}
		cmd->ranges_tracked = true;
	}
	err = kdc_fixed_sgls(kdev, cmd, s);
	if (err)
		goto fail;
	return cmd;
fail:
	kdc_fixed_release(kdev, cmd, false);
	kdc_cmd_free(cmd);
	*errp = err;
	return NULL;
}

/* ---- file operations ---- */

static bool kdc_fixed_dup(struct kdc_ctx *ctx, u64 id)
{
	struct kdc_cmd *cmd;

	lockdep_assert_held(&ctx->lock);
	hash_for_each_possible(ctx->ids, cmd, id_node, id)
		if (cmd->id == id && READ_ONCE(cmd->phase) != KDC_FREE)
			return true;
	return false;
}

/* The registered buffer of this file that holds [addr, addr + len), or NULL. */
static struct kdc_regbuf *kdc_find_buf(struct kdc_ctx *ctx, u64 addr, u32 len)
{
	unsigned int i;

	lockdep_assert_held(&ctx->lock);
	for (i = 0; i < KDC_MAX_FIXED_BUFS; i++) {
		struct kdc_regbuf *b = ctx->bufs[i];

		if (b && addr >= b->uaddr && addr - b->uaddr + len <= b->len)
			return b;
	}
	return NULL;
}

/* KDC_IOC_SUBMIT with KDC_SUBMIT_FIXED; the same admission as kdc_ioctl_submit(). */
long kdc_submit_fixed(struct kdc_ctx *ctx, const struct kdc_submit *s)
{
	struct kdc_dev *kdev = ctx->kdev;
	struct kdc_regbuf *src_buf, *dst_buf;
	struct kdc_queue *q;
	struct kdc_cmd *cmd;
	int err;

	err = kdc_fixed_check(s, kdev->nr_queues);
	if (err)
		return err;
	spin_lock(&ctx->lock);
	err = kdc_admission(s->epoch, ctx->epoch, ctx->resetting, ctx->closing,
			    READ_ONCE(kdev->state)) ?:
	      kdc_quota(ctx->nr_live, ctx->bytes, 2ull * s->length);
	if (!err && kdc_fixed_dup(ctx, s->id))
		err = -EEXIST;
	src_buf = err ? NULL : kdc_find_buf(ctx, s->src, s->length);
	dst_buf = err ? NULL : kdc_find_buf(ctx, s->dst, s->length);
	if (!err && src_buf && dst_buf) {
		/* held until kdc_fixed_release(); unregister refuses meanwhile */
		atomic_inc(&src_buf->users);
		atomic_inc(&dst_buf->users);
	} else if (!err) {
		err = -EINVAL;
	}
	spin_unlock(&ctx->lock);
	if (err)
		return err;

	cmd = kdc_fixed_prepare(kdev, s, src_buf, dst_buf, &err);
	if (!cmd)
		return err;
	q = &kdev->queues[s->queue == KDC_QUEUE_AUTO ?
				  kdev->cpu_queue[raw_smp_processor_id()] :
				  s->queue];
	cmd->q = q;
	cmd->ctx = ctx;

	/* Authoritative: kdc_fail_locked() sets the state before collecting. */
	spin_lock(&ctx->lock);
	err = kdc_admission(s->epoch, ctx->epoch, ctx->resetting, ctx->closing,
			    READ_ONCE(kdev->state)) ?:
	      kdc_quota(ctx->nr_live, ctx->bytes, cmd->quota_bytes);
	if (!err && kdc_fixed_dup(ctx, s->id))
		err = -EEXIST;
	if (err) {
		spin_unlock(&ctx->lock);
		kdc_cmd_release(kdev, cmd, false);
		kdc_cmd_free(cmd);
		return err;
	}
	cmd->epoch = ctx->epoch;
	list_add_tail(&cmd->ctx_node, &ctx->live);
	hash_add(ctx->ids, &cmd->id_node, cmd->id);
	ctx->nr_live++;
	ctx->nr_inflight++;
	ctx->bytes += cmd->quota_bytes;
	atomic64_inc(&kdev->submitted);
	if (s->flags & KDC_SUBMIT_BATCH) {
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

/* A free index for [addr, addr + len) in this file: -EEXIST if it overlaps a buffer, -ENOSPC. */
static int kdc_buf_slot(struct kdc_ctx *ctx, u64 addr, u64 len, unsigned int *slot)
{
	unsigned int i;
	int err = -ENOSPC;

	lockdep_assert_held(&ctx->lock);
	for (i = 0; i < KDC_MAX_FIXED_BUFS; i++) {
		struct kdc_regbuf *o = ctx->bufs[i];

		if (o && addr < o->uaddr + o->len && o->uaddr < addr + len)
			return -EEXIST;
		if (!o && err) {
			*slot = i;
			err = 0;
		}
	}
	return err;
}

long kdc_ioctl_register(struct kdc_ctx *ctx, void __user *arg)
{
	struct kdc_dev *kdev = ctx->kdev;
	struct kdc_regbuf *b;
	struct kdc_buffer kb;
	unsigned int slot;
	int err;

	if (copy_from_user(&kb, arg, sizeof(kb)))
		return -EFAULT;
	err = kdc_buffer_check(&kb);
	if (err)
		return err;
	/* Cheap early rejection; repeated authoritatively below. */
	spin_lock(&ctx->lock);
	err = kdc_buf_slot(ctx, kb.addr, kb.length, &slot);
	spin_unlock(&ctx->lock);
	if (err)
		return err;
	b = kdc_regbuf_create(kdev, kb.addr, kb.length, &err);
	if (!b)
		return err;
	spin_lock(&ctx->lock);
	err = kdc_buf_slot(ctx, kb.addr, kb.length, &slot);
	if (!err) {
		ctx->bufs[slot] = b;
		ctx->pinned_pages += b->npages;
	}
	spin_unlock(&ctx->lock);
	if (err) {
		mutex_lock(&kdev->files_mutex);
		kdc_regbuf_destroy(kdev, b);
		mutex_unlock(&kdev->files_mutex);
		return err;
	}
	kb.index = slot;
	if (copy_to_user(arg, &kb, sizeof(kb))) {
		/* the buffer stays registered; close() or KDC_IOC_UNREGISTER frees it */
		return -EFAULT;
	}
	return 0;
}

long kdc_ioctl_unregister(struct kdc_ctx *ctx, void __user *arg)
{
	struct kdc_dev *kdev = ctx->kdev;
	struct kdc_regbuf *b;
	struct kdc_buffer kb;

	if (copy_from_user(&kb, arg, sizeof(kb)))
		return -EFAULT;
	if (kb.version != KDC_ABI_VERSION)
		return -EPROTO;
	if (kb.size != sizeof(kb) || kb.reserved || kb.reserved2[0] || kb.reserved2[1])
		return -EINVAL;
	spin_lock(&ctx->lock);
	b = kb.index < KDC_MAX_FIXED_BUFS ? ctx->bufs[kb.index] : NULL;
	/* Submits take references under ctx->lock, so none can appear after this. */
	if (b && atomic_read(&b->users)) {
		spin_unlock(&ctx->lock);
		return -EBUSY;
	}
	if (b) {
		ctx->bufs[kb.index] = NULL;
		ctx->pinned_pages -= b->npages;
	}
	spin_unlock(&ctx->lock);
	if (!b)
		return -ENOENT;
	mutex_lock(&kdev->files_mutex);
	kdc_regbuf_destroy(kdev, b);
	mutex_unlock(&kdev->files_mutex);
	return 0;
}

/* close(): no command is left. Caller holds files_mutex. */
void kdc_fixed_close(struct kdc_ctx *ctx)
{
	unsigned int i;

	lockdep_assert_held(&ctx->kdev->files_mutex);
	for (i = 0; i < KDC_MAX_FIXED_BUFS; i++)
		if (ctx->bufs[i])
			kdc_regbuf_destroy(ctx->kdev, ctx->bufs[i]);
}

/* remove(): every command has ended and the device cannot DMA. Caller holds files_mutex. */
void kdc_fixed_unmap_all(struct kdc_dev *kdev)
{
	struct kdc_ctx *ctx;
	unsigned int i;

	lockdep_assert_held(&kdev->files_mutex);
	list_for_each_entry(ctx, &kdev->files, dev_node)
		for (i = 0; i < KDC_MAX_FIXED_BUFS; i++)
			if (ctx->bufs[i])
				kdc_regbuf_unmap(kdev, ctx->bufs[i]);
}
