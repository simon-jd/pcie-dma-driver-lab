/*
 * KDC DMA engine: an emulated PCIe DMA copy + CRC32C offload device.
 *
 * The register map and descriptor formats are in kdc_hw.h. In short: each queue
 * pair is a submission ring and a completion ring in guest memory. A doorbell
 * write schedules a fetch; every fetched command occupies a device slot until
 * its service time has elapsed, then the engine reads the source scatter list,
 * writes the destination scatter list and posts a completion with the CRC32C.
 * All guest memory access goes through pci_dma_read/pci_dma_write, so an
 * emulated IOMMU translates (and can fault) every access.
 *
 * Each queue has a serial engine: commands on one queue are serviced one after
 * another (a command starts when it has been fetched and the engine is free),
 * queues run in parallel. An abort of a queued command costs no engine time.
 * Fetch and completion run in bottom halves guarded against re-entrancy, never
 * in the MMIO handler.
 *
 * With shadow doorbells (DBBUF register) a queue's SQ tail lives in host memory
 * and the doorbell write only says "look": the device registers an ioeventfd
 * on the doorbell address, so under KVM the write is handled in the host
 * kernel and never exits to QEMU, and the device publishes an event index so
 * the driver can skip doorbells while the device still has entries to fetch.
 *
 * Fault knobs model a misbehaving device for the driver's recovery paths:
 * a stalled engine, a fatal state, dropped or corrupted completions, DMA to a
 * bad address, a controller that ignores its reset, one that never becomes
 * ready again, and a device that has stopped responding altogether (BAR and
 * config reads return all-ones, no DMA, no interrupts), as after a surprise
 * removal.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "qemu/crc32c.h"
#include "qemu/event_notifier.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "kdc_hw.h"

#define TYPE_KDC_DMA "kdc-dma"
OBJECT_DECLARE_SIMPLE_TYPE(KdcState, KDC_DMA)

#define KDC_MAX_DEPTH_LOG2 12
#define KDC_MAX_SEGS 128
#define KDC_MAX_XFER (1 * MiB)

typedef enum { SLOT_FREE, SLOT_BUSY, SLOT_DONE } KdcSlotState;

typedef struct KdcSlot {
    KdcSlotState state;
    bool aborted;
    uint16_t cid;
    uint16_t code;
    uint32_t result;
    uint32_t bytes;
    int64_t fetched_at;
    int64_t deadline;
    struct kdc_hw_sqe sqe;
} KdcSlot;

typedef struct KdcQueue {
    KdcState *s;
    uint16_t id;
    uint64_t sq_base, cq_base;
    uint32_t size_log2, status, vector, coalesce;
    uint32_t depth; /* latched when the queue is enabled */
    uint32_t sq_head, sq_tail, cq_head, cq_tail;
    bool phase;
    KdcSlot *slots; /* ring of depth slots in fetch order */
    uint32_t slot_head, slot_count;
    int32_t running; /* slot the engine is servicing, or -1 */
    int64_t engine_free_at;
    uint32_t irq_pending;
    bool pass_fired; /* this pass has raised its first interrupt */
    QEMUTimer service_timer, coalesce_timer;
    QEMUBH *fetch_bh, *complete_bh;
    bool shadow; /* latched at enable: SQ tail from the DBBUF page */
    uint64_t db_addr; /* this queue's struct kdc_hw_dbbuf */
    EventNotifier sq_notifier; /* ioeventfd on the SQ doorbell (shadow queues) */
    bool notifier_ok, ioeventfd_on;
} KdcQueue;

struct KdcState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t nqueues;
    bool hw_crc; /* property: SSE4.2 crc32 instruction when the host has it */
    bool use_sse42;
    bool shadow_db; /* property: offer KDC_FEAT_SHADOW_DB */
    bool ioeventfd; /* property: doorbells of shadow queues through an ioeventfd */
    uint64_t dbbuf; /* KDC_REG_DBBUF: page address | KDC_DBBUF_ENABLE */
    uint32_t service_ns;
    uint32_t ctrl, status, err_status, err_info, err_vector;
    uint32_t fault, fault_drop, fault_bad_cid, fault_bad_dma;
    bool stuck; /* KDC_FAULT_STUCK: survives every reset */
    bool gone; /* not responding at all; host-side knob only */
    uint32_t stat_done, stat_aborted, stat_errors, stat_irqs, stat_doorbells;
    uint32_t stat_cq_doorbells, scratch;
    KdcQueue queues[KDC_MAX_QUEUES];
    struct kdc_hw_sge sgl[2][KDC_MAX_SEGS];
    uint8_t *xfer;
};

static void kdc_complete(KdcQueue *q);

static bool kdc_queue_running(KdcQueue *q)
{
    KdcState *s = q->s;

    return !s->gone && (s->status & KDC_STATUS_READY) &&
           !(s->status & KDC_STATUS_FATAL) &&
           (q->status & KDC_QSTATUS_ENABLED) && !(q->status & KDC_QSTATUS_HALTED);
}

static void kdc_notify(KdcState *s, uint32_t vector)
{
    PCIDevice *pdev = PCI_DEVICE(s);

    if (!s->gone && msix_enabled(pdev) && vector < s->nqueues + 1) {
        msix_notify(pdev, vector);
        s->stat_irqs++;
    }
}

static void kdc_error(KdcState *s, uint32_t bits, uint32_t queue)
{
    s->err_status |= bits;
    s->err_info = queue;
    s->stat_errors++;
    kdc_notify(s, s->err_vector);
}

static void kdc_queue_drop(KdcQueue *q)
{
    if (q->ioeventfd_on) {
        memory_region_del_eventfd(&q->s->mmio, KDC_QREG(q->id, KDC_Q_SQ_TAIL), 0, false, 0,
                                  &q->sq_notifier);
        q->ioeventfd_on = false;
        event_notifier_test_and_clear(&q->sq_notifier); /* a stale kick */
    }
    q->shadow = false;
    timer_del(&q->service_timer);
    timer_del(&q->coalesce_timer);
    qemu_bh_cancel(q->fetch_bh);
    qemu_bh_cancel(q->complete_bh);
    g_free(q->slots);
    q->slots = NULL;
    q->status = 0;
    q->depth = 0;
    q->sq_head = q->sq_tail = q->cq_head = q->cq_tail = 0;
    q->slot_head = q->slot_count = 0;
    q->running = -1;
    q->irq_pending = 0;
    q->pass_fired = false;
}

static void kdc_queue_halt(KdcQueue *q)
{
    q->status |= KDC_QSTATUS_HALTED;
    timer_del(&q->service_timer);
    kdc_error(q->s, KDC_ERR_RING_DMA, q->id);
}

static void kdc_reset_state(KdcState *s)
{
    for (unsigned i = 0; i < s->nqueues; i++) {
        KdcQueue *q = &s->queues[i];

        kdc_queue_drop(q);
        q->sq_base = q->cq_base = 0;
        q->size_log2 = 0;
        q->vector = i;
        q->coalesce = 0;
    }
    s->ctrl = s->status = 0;
    s->err_status = s->err_info = 0;
    s->err_vector = s->nqueues;
    s->fault = s->fault_drop = s->fault_bad_cid = s->fault_bad_dma = 0;
    s->stat_done = s->stat_aborted = s->stat_errors = 0;
    s->stat_irqs = s->stat_doorbells = s->stat_cq_doorbells = 0;
    s->scratch = 0;
    s->dbbuf = 0;
}

static void kdc_kick_all(KdcState *s)
{
    for (unsigned i = 0; i < s->nqueues; i++) {
        if (s->queues[i].slots) {
            qemu_bh_schedule(s->queues[i].fetch_bh);
            qemu_bh_schedule(s->queues[i].complete_bh);
        }
    }
}

static void kdc_enter_fatal(KdcState *s)
{
    if (s->status & KDC_STATUS_FATAL) {
        return;
    }
    s->status |= KDC_STATUS_FATAL;
    for (unsigned i = 0; i < s->nqueues; i++) {
        timer_del(&s->queues[i].service_timer);
    }
    kdc_error(s, KDC_ERR_FATAL, 0);
}

static void kdc_set_fault(KdcState *s, uint32_t value)
{
    bool was_stalled = s->fault & KDC_FAULT_STALL;

    s->fault = value & (KDC_FAULT_STALL | KDC_FAULT_RESET_HANG);
    s->stuck = value & KDC_FAULT_STUCK;
    if (value & KDC_FAULT_FATAL) {
        kdc_enter_fatal(s);
    }
    if (was_stalled && !(s->fault & KDC_FAULT_STALL)) {
        kdc_kick_all(s);
    }
}

/* ---- interrupts ---- */

static void kdc_queue_fire(KdcQueue *q)
{
    timer_del(&q->coalesce_timer);
    q->irq_pending = 0;
    kdc_notify(q->s, q->vector);
}

static void kdc_coalesce_timer(void *opaque)
{
    KdcQueue *q = opaque;

    if (q->irq_pending) {
        kdc_queue_fire(q);
    }
}

static bool kdc_coalescing(KdcQueue *q)
{
    return (q->coalesce & 0xffff) > 1 && (q->coalesce >> 16);
}

/*
 * Called for every posted completion. With coalescing, the count or the delay
 * decides. Without it, the first completion of a pass (one run of
 * kdc_complete()) interrupts at once and the rest of that pass is announced by
 * a single interrupt after its last completion (kdc_pass_end()): the first
 * completion is not delayed, and a burst costs two MSI-X messages, not one per
 * completion.
 */
static void kdc_queue_irq(KdcQueue *q)
{
    uint32_t count = q->coalesce & 0xffff;
    uint32_t delay_us = q->coalesce >> 16;

    q->irq_pending++;
    if (!kdc_coalescing(q)) {
        if (!q->pass_fired) {
            q->pass_fired = true;
            kdc_queue_fire(q);
        }
    } else if (q->irq_pending >= count) {
        kdc_queue_fire(q);
    } else if (!timer_pending(&q->coalesce_timer)) {
        timer_mod(&q->coalesce_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_us * 1000LL);
    }
}

static void kdc_pass_end(KdcQueue *q)
{
    if (q->irq_pending && !kdc_coalescing(q)) {
        kdc_queue_fire(q);
    }
    q->pass_fired = false;
}

/* ---- CRC32C ---- */

/*
 * QEMU's crc32c() is a byte-at-a-time table lookup, about 1.6 ns per byte on
 * the build host: a 64 KiB command spent ~100 us in it, far more than the
 * driver and VM exits around it. With hw-crc (the default) the device uses the
 * SSE4.2 crc32 instruction instead. Both continue a raw CRC and return it
 * inverted; hw-crc=off keeps the table version for comparison.
 */
#if defined(__x86_64__)
__attribute__((target("sse4.2")))
static uint32_t kdc_crc32c_sse42(uint32_t crc, const uint8_t *p, size_t len)
{
    uint64_t c = crc;

    for (; len >= 8; p += 8, len -= 8) {
        uint64_t v;

        memcpy(&v, p, sizeof(v));
        c = __builtin_ia32_crc32di(c, v);
    }
    while (len--) {
        c = __builtin_ia32_crc32qi((uint32_t)c, *p++);
    }
    return (uint32_t)c ^ 0xffffffff;
}
#endif

static uint32_t kdc_crc32c(KdcState *s, uint32_t crc, const uint8_t *p, size_t len)
{
#if defined(__x86_64__)
    if (s->use_sse42) {
        return kdc_crc32c_sse42(crc, p, len);
    }
#endif
    return crc32c(crc, p, len);
}

/* ---- command validation and execution ---- */

static uint16_t kdc_validate_sqe(const struct kdc_hw_sqe *e)
{
    uint32_t length = le32_to_cpu(e->length);
    uint16_t src_nents = le16_to_cpu(e->src_nents);
    uint16_t dst_nents = le16_to_cpu(e->dst_nents);

    for (unsigned i = 0; i < ARRAY_SIZE(e->rsvd); i++) {
        if (e->rsvd[i]) {
            return KDC_SC_INVALID_FIELD;
        }
    }
    switch (e->opcode) {
    case KDC_OP_NOP:
        if (e->flags || length || e->src || e->dst || src_nents || dst_nents ||
            e->crc_seed) {
            return KDC_SC_INVALID_FIELD;
        }
        return KDC_SC_SUCCESS;
    case KDC_OP_COPY_CRC:
        if (e->flags & ~KDC_SQE_FLAGS_MASK || !length || length > KDC_MAX_XFER) {
            return KDC_SC_INVALID_FIELD;
        }
        if (!(e->flags & KDC_SQE_F_SRC_SGL) ? src_nents != 0 :
            (!src_nents || src_nents > KDC_MAX_SEGS)) {
            return (e->flags & KDC_SQE_F_SRC_SGL) ? KDC_SC_INVALID_SGL :
                                                    KDC_SC_INVALID_FIELD;
        }
        if (!(e->flags & KDC_SQE_F_DST_SGL) ? dst_nents != 0 :
            (!dst_nents || dst_nents > KDC_MAX_SEGS)) {
            return (e->flags & KDC_SQE_F_DST_SGL) ? KDC_SC_INVALID_SGL :
                                                    KDC_SC_INVALID_FIELD;
        }
        return KDC_SC_SUCCESS;
    default:
        return KDC_SC_INVALID_OPCODE;
    }
}

/* Load one side's scatter list into s->sgl[side]; returns entries or -code. */
static int kdc_load_sgl(KdcState *s, int side, bool is_list, uint64_t addr,
                        uint16_t nents, uint32_t length)
{
    struct kdc_hw_sge *sgl = s->sgl[side];
    uint64_t total = 0;

    if (!is_list) {
        sgl[0].addr = addr;
        sgl[0].len = length;
        sgl[0].rsvd = 0;
        return 1;
    }
    if (pci_dma_read(PCI_DEVICE(s), addr, sgl, nents * sizeof(*sgl)) !=
        MEMTX_OK) {
        return -KDC_SC_DATA_ERROR;
    }
    for (unsigned i = 0; i < nents; i++) {
        sgl[i].addr = le64_to_cpu(sgl[i].addr);
        sgl[i].len = le32_to_cpu(sgl[i].len);
        if (!sgl[i].len || sgl[i].rsvd) {
            return -KDC_SC_INVALID_SGL;
        }
        total += sgl[i].len;
    }
    return total == length ? nents : -KDC_SC_INVALID_SGL;
}

static void kdc_execute(KdcQueue *q, KdcSlot *slot)
{
    KdcState *s = q->s;
    PCIDevice *pdev = PCI_DEVICE(s);
    const struct kdc_hw_sqe *e = &slot->sqe;
    uint32_t length = le32_to_cpu(e->length);
    uint64_t offset = 0;
    int nsrc, ndst;

    slot->state = SLOT_DONE;
    slot->result = slot->bytes = 0;
    if (e->opcode == KDC_OP_NOP) {
        slot->code = KDC_SC_SUCCESS;
        return;
    }
    nsrc = kdc_load_sgl(s, 0, e->flags & KDC_SQE_F_SRC_SGL, le64_to_cpu(e->src),
                        le16_to_cpu(e->src_nents), length);
    ndst = nsrc < 0 ? nsrc :
           kdc_load_sgl(s, 1, e->flags & KDC_SQE_F_DST_SGL,
                        le64_to_cpu(e->dst), le16_to_cpu(e->dst_nents), length);
    if (nsrc < 0 || ndst < 0) {
        slot->code = -(nsrc < 0 ? nsrc : ndst);
        return;
    }
    if (s->fault_bad_dma) {
        s->fault_bad_dma--;
        for (int i = 0; i < ndst; i++) {
            s->sgl[1][i].addr = KDC_BAD_DMA_ADDR;
        }
    }
    for (int i = 0; i < nsrc; i++) {
        if (pci_dma_read(pdev, s->sgl[0][i].addr, s->xfer + offset,
                         s->sgl[0][i].len) != MEMTX_OK) {
            slot->code = KDC_SC_DATA_ERROR;
            return;
        }
        offset += s->sgl[0][i].len;
    }
    slot->result = kdc_crc32c(s, ~le32_to_cpu(e->crc_seed), s->xfer, length);
    offset = 0;
    for (int i = 0; i < ndst; i++) {
        if (pci_dma_write(pdev, s->sgl[1][i].addr, s->xfer + offset,
                          s->sgl[1][i].len) != MEMTX_OK) {
            slot->code = KDC_SC_DATA_ERROR;
            slot->bytes = offset;
            return;
        }
        offset += s->sgl[1][i].len;
    }
    slot->bytes = length;
    slot->code = KDC_SC_SUCCESS;
}

/* ---- rings ---- */

static KdcSlot *kdc_slot(KdcQueue *q, uint32_t i)
{
    return &q->slots[(q->slot_head + i) & (q->depth - 1)];
}

/* Returns false when the completion ring is full or the queue halted. */
static bool kdc_post(KdcQueue *q, KdcSlot *slot)
{
    KdcState *s = q->s;
    struct kdc_hw_cqe cqe;
    uint64_t addr;
    uint16_t cid = slot->cid;

    if (((q->cq_tail + 1) & (q->depth - 1)) == q->cq_head) {
        return false;
    }
    slot->state = SLOT_FREE;
    if (slot->code == KDC_SC_ABORTED) {
        s->stat_aborted++;
    }
    if (s->fault_drop) {
        s->fault_drop--;
        return true;
    }
    if (s->fault_bad_cid) {
        s->fault_bad_cid--;
        cid ^= 0x0fff;
    }
    cqe.result = cpu_to_le32(slot->result);
    cqe.bytes = cpu_to_le32(slot->bytes);
    cqe.sq_head = cpu_to_le16(q->sq_head);
    cqe.sqid = cpu_to_le16(q->id);
    cqe.cid = cpu_to_le16(cid);
    cqe.status = cpu_to_le16(slot->code << KDC_CQE_CODE_SHIFT | q->phase);
    addr = q->cq_base + (uint64_t)q->cq_tail * KDC_CQE_SIZE;
    /* The phase bit is in the last field; write it after the rest. */
    if (pci_dma_write(PCI_DEVICE(s), addr, &cqe,
                      offsetof(struct kdc_hw_cqe, status)) != MEMTX_OK ||
        pci_dma_write(PCI_DEVICE(s), addr + offsetof(struct kdc_hw_cqe, status),
                      &cqe.status, sizeof(cqe.status)) != MEMTX_OK) {
        kdc_queue_halt(q);
        return false;
    }
    q->cq_tail = (q->cq_tail + 1) & (q->depth - 1);
    if (!q->cq_tail) {
        q->phase = !q->phase;
    }
    s->stat_done++;
    kdc_queue_irq(q);
    return true;
}

/* Shadow doorbell: the driver's SQ tail from the DBBUF page; false if the queue halted. */
static bool kdc_shadow_tail(KdcQueue *q)
{
    uint32_t tail;

    if (pci_dma_read(PCI_DEVICE(q->s), q->db_addr + offsetof(struct kdc_hw_dbbuf, sq_tail),
                     &tail, sizeof(tail)) != MEMTX_OK) {
        kdc_queue_halt(q);
        return false;
    }
    tail = le32_to_cpu(tail);
    if (tail >= q->depth) {
        kdc_error(q->s, KDC_ERR_DOORBELL, q->id); /* ignored, like a bad doorbell value */
    } else {
        q->sq_tail = tail;
    }
    return true;
}

static void kdc_fetch(void *opaque)
{
    KdcQueue *q = opaque;
    KdcState *s = q->s;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool fetched = false;

    if (!kdc_queue_running(q) || (q->shadow && !kdc_shadow_tail(q))) {
        return;
    }
again:
    while (q->sq_head != q->sq_tail && q->slot_count < q->depth) {
        KdcSlot *slot = kdc_slot(q, q->slot_count);
        uint64_t addr = q->sq_base + (uint64_t)q->sq_head * KDC_SQE_SIZE;

        if (pci_dma_read(PCI_DEVICE(s), addr, &slot->sqe, sizeof(slot->sqe)) !=
            MEMTX_OK) {
            kdc_queue_halt(q);
            return;
        }
        q->sq_head = (q->sq_head + 1) & (q->depth - 1);
        q->slot_count++;
        slot->cid = le16_to_cpu(slot->sqe.cid);
        slot->aborted = false;
        slot->code = kdc_validate_sqe(&slot->sqe);
        slot->result = slot->bytes = 0;
        slot->fetched_at = now;
        slot->state = slot->code == KDC_SC_SUCCESS ? SLOT_BUSY : SLOT_DONE;
        fetched = true;
    }
    if (q->shadow && q->sq_head == q->sq_tail) {
        /*
         * Everything seen is fetched: publish that tail as the event index,
         * then read the shadow tail again. A driver that read the old index
         * skipped its doorbell, but it wrote its tail before that read (full
         * barrier on both sides), so this second read sees the new tail.
         */
        uint32_t seen = q->sq_tail, ev = cpu_to_le32(seen);

        if (pci_dma_write(PCI_DEVICE(s), q->db_addr + offsetof(struct kdc_hw_dbbuf, sq_event),
                          &ev, sizeof(ev)) != MEMTX_OK) {
            kdc_queue_halt(q);
            return;
        }
        smp_mb();
        if (!kdc_shadow_tail(q)) {
            return;
        }
        if (q->sq_tail != seen) {
            goto again;
        }
    }
    if (fetched) {
        kdc_complete(q);
    }
}

/* Run the serial engine up to @now; arms the service timer if work remains. */
static void kdc_engine(KdcQueue *q, int64_t now)
{
    for (;;) {
        KdcSlot *next = NULL;
        uint32_t i;

        if (q->running >= 0) {
            KdcSlot *slot = &q->slots[q->running];

            if (slot->deadline > now) {
                timer_mod(&q->service_timer, slot->deadline);
                return;
            }
            q->engine_free_at = slot->deadline;
            q->running = -1;
            kdc_execute(q, slot);
        }
        for (i = 0; i < q->slot_count && !next; i++) {
            if (kdc_slot(q, i)->state == SLOT_BUSY) {
                next = kdc_slot(q, i);
            }
        }
        if (!next) {
            return;
        }
        next->deadline = MAX(q->engine_free_at, next->fetched_at) + q->s->service_ns;
        q->running = next - q->slots;
    }
}

static void kdc_complete(KdcQueue *q)
{
    KdcState *s = q->s;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool freed = false;
    uint32_t i;

    if (!kdc_queue_running(q) || (s->fault & KDC_FAULT_STALL)) {
        return;
    }
    /* Aborts first, then the engine, then post finished commands in order. */
    for (i = 0; i < q->slot_count; i++) {
        KdcSlot *slot = kdc_slot(q, i);

        if (slot->state == SLOT_BUSY && slot->aborted) {
            if (q->running == slot - q->slots) {
                q->running = -1;
                q->engine_free_at = now;
            }
            slot->state = SLOT_DONE;
            slot->code = KDC_SC_ABORTED;
        }
    }
    kdc_engine(q, now);
    for (i = 0; i < q->slot_count; i++) {
        KdcSlot *slot = kdc_slot(q, i);

        if (slot->state == SLOT_DONE && !kdc_post(q, slot)) {
            break;
        }
    }
    kdc_pass_end(q);
    if (!kdc_queue_running(q)) {
        return;
    }
    while (q->slot_count && kdc_slot(q, 0)->state == SLOT_FREE) {
        q->slot_head = (q->slot_head + 1) & (q->depth - 1);
        q->slot_count--;
        freed = true;
    }
    if (freed && q->sq_head != q->sq_tail) {
        qemu_bh_schedule(q->fetch_bh);
    }
}

static void kdc_complete_bh(void *opaque)
{
    kdc_complete(opaque);
}

static void kdc_service_timer(void *opaque)
{
    KdcQueue *q = opaque;

    qemu_bh_schedule(q->complete_bh);
}

static void kdc_abort(KdcQueue *q, uint16_t cid)
{
    for (uint32_t i = 0; i < q->slot_count; i++) {
        KdcSlot *slot = kdc_slot(q, i);

        if (slot->state == SLOT_BUSY && slot->cid == cid) {
            slot->aborted = true;
            qemu_bh_schedule(q->complete_bh);
            return;
        }
    }
}

/* A kick for a shadow queue, from its ioeventfd (KVM, in the host kernel). */
static void kdc_sq_notify(EventNotifier *e)
{
    KdcQueue *q = container_of(e, KdcQueue, sq_notifier);

    if (!event_notifier_test_and_clear(e) || q->s->gone || !q->shadow) {
        return;
    }
    q->s->stat_doorbells++;
    qemu_bh_schedule(q->fetch_bh);
}

static void kdc_queue_enable(KdcQueue *q, bool enable)
{
    KdcState *s = q->s;
    bool shadow = s->dbbuf & KDC_DBBUF_ENABLE;

    if (!enable) {
        kdc_queue_drop(q);
        return;
    }
    if (q->status & KDC_QSTATUS_ENABLED) {
        return;
    }
    if (!(s->status & KDC_STATUS_READY) || (s->status & KDC_STATUS_FATAL) ||
        q->size_log2 < KDC_MIN_DEPTH_LOG2 || q->size_log2 > KDC_MAX_DEPTH_LOG2 ||
        !q->sq_base || !q->cq_base || (q->sq_base | q->cq_base) % KDC_RING_ALIGN ||
        (shadow && (s->dbbuf & ~(uint64_t)KDC_DBBUF_ENABLE) % KDC_RING_ALIGN)) {
        kdc_error(s, KDC_ERR_QUEUE_CONFIG, q->id);
        return;
    }
    q->depth = 1u << q->size_log2;
    q->slots = g_new0(KdcSlot, q->depth);
    q->sq_head = q->sq_tail = q->cq_head = q->cq_tail = 0;
    q->slot_head = q->slot_count = 0;
    q->running = -1;
    q->phase = true;
    q->engine_free_at = 0;
    q->irq_pending = 0;
    q->status = KDC_QSTATUS_ENABLED;
    q->shadow = shadow;
    q->db_addr = (s->dbbuf & ~(uint64_t)KDC_DBBUF_ENABLE) + q->id * KDC_DBBUF_SIZE;
    if (shadow && s->ioeventfd && q->notifier_ok) {
        /* size 0: any write; KVM handles it in the kernel (fast MMIO). Without
         * KVM the write reaches kdc_queue_write() as before. */
        memory_region_add_eventfd(&s->mmio, KDC_QREG(q->id, KDC_Q_SQ_TAIL), 0, false, 0,
                                  &q->sq_notifier);
        q->ioeventfd_on = true;
    }
}

static uint32_t kdc_inflight(KdcQueue *q)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < q->slot_count; i++) {
        n += kdc_slot(q, i)->state != SLOT_FREE;
    }
    return n;
}

/* ---- MMIO ---- */

static uint64_t kdc_queue_read(KdcQueue *q, hwaddr reg)
{
    switch (reg) {
    case KDC_Q_SQ_BASE_LO: return (uint32_t)q->sq_base;
    case KDC_Q_SQ_BASE_HI: return q->sq_base >> 32;
    case KDC_Q_CQ_BASE_LO: return (uint32_t)q->cq_base;
    case KDC_Q_CQ_BASE_HI: return q->cq_base >> 32;
    case KDC_Q_SIZE_LOG2: return q->size_log2;
    case KDC_Q_CTRL: return !!(q->status & KDC_QSTATUS_ENABLED);
    case KDC_Q_STATUS: return q->status;
    case KDC_Q_IRQ_VECTOR: return q->vector;
    case KDC_Q_COALESCE: return q->coalesce;
    case KDC_Q_SQ_TAIL: return q->sq_tail;
    case KDC_Q_CQ_HEAD: return q->cq_head;
    case KDC_Q_SQ_HEAD: return q->sq_head;
    case KDC_Q_INFLIGHT: return kdc_inflight(q);
    default: return 0;
    }
}

static void kdc_queue_write(KdcQueue *q, hwaddr reg, uint32_t val)
{
    KdcState *s = q->s;
    bool enabled = q->status & KDC_QSTATUS_ENABLED;

    switch (reg) {
    case KDC_Q_SQ_BASE_LO:
        if (!enabled) {
            q->sq_base = deposit64(q->sq_base, 0, 32, val);
        }
        break;
    case KDC_Q_SQ_BASE_HI:
        if (!enabled) {
            q->sq_base = deposit64(q->sq_base, 32, 32, val);
        }
        break;
    case KDC_Q_CQ_BASE_LO:
        if (!enabled) {
            q->cq_base = deposit64(q->cq_base, 0, 32, val);
        }
        break;
    case KDC_Q_CQ_BASE_HI:
        if (!enabled) {
            q->cq_base = deposit64(q->cq_base, 32, 32, val);
        }
        break;
    case KDC_Q_SIZE_LOG2:
        if (!enabled) {
            q->size_log2 = val;
        }
        break;
    case KDC_Q_CTRL:
        kdc_queue_enable(q, val & KDC_QCTRL_ENABLE);
        break;
    case KDC_Q_IRQ_VECTOR:
        q->vector = val;
        break;
    case KDC_Q_COALESCE:
        q->coalesce = val;
        break;
    case KDC_Q_SQ_TAIL:
        if (enabled && q->shadow) { /* only a kick: the tail is in the DBBUF page */
            s->stat_doorbells++;
            qemu_bh_schedule(q->fetch_bh);
            break;
        }
        if (!enabled || val >= q->depth) {
            kdc_error(s, KDC_ERR_DOORBELL, q->id);
            break;
        }
        s->stat_doorbells++;
        q->sq_tail = val;
        qemu_bh_schedule(q->fetch_bh);
        break;
    case KDC_Q_CQ_HEAD:
        if (!enabled || val >= q->depth) {
            kdc_error(s, KDC_ERR_DOORBELL, q->id);
            break;
        }
        s->stat_cq_doorbells++;
        q->cq_head = val;
        qemu_bh_schedule(q->complete_bh);
        break;
    case KDC_Q_ABORT:
        if (enabled) {
            kdc_abort(q, val);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "kdc-dma: write to RO/unknown queue reg 0x%"
                      HWADDR_PRIx "\n", reg);
    }
}

static uint64_t kdc_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    KdcState *s = opaque;

    if (s->gone) {
        return 0xffffffff;
    }
    if (addr >= KDC_QUEUE_BASE) {
        unsigned q = (addr - KDC_QUEUE_BASE) / KDC_QUEUE_STRIDE;

        return q < s->nqueues ?
               kdc_queue_read(&s->queues[q], (addr - KDC_QUEUE_BASE) % KDC_QUEUE_STRIDE) : 0;
    }
    switch (addr) {
    case KDC_REG_ID: return KDC_ID_VALUE;
    case KDC_REG_VERSION: return KDC_VERSION_MAJOR << 16 | KDC_VERSION_MINOR;
    case KDC_REG_CAP:
        return s->nqueues | KDC_MAX_DEPTH_LOG2 << KDC_CAP_DEPTH_SHIFT |
               KDC_MAX_SEGS << KDC_CAP_SEGS_SHIFT;
    case KDC_REG_MAX_XFER: return KDC_MAX_XFER;
    case KDC_REG_FEATURES:
        return KDC_FEAT_CRC32C | KDC_FEAT_ABORT | KDC_FEAT_COALESCE |
               KDC_FEAT_FAULTS | KDC_FEAT_FLR | (s->shadow_db ? KDC_FEAT_SHADOW_DB : 0);
    case KDC_REG_CTRL: return s->ctrl;
    case KDC_REG_STATUS: return s->status;
    case KDC_REG_ERR_STATUS: return s->err_status;
    case KDC_REG_ERR_INFO: return s->err_info;
    case KDC_REG_ERR_VECTOR: return s->err_vector;
    case KDC_REG_SERVICE_NS: return s->service_ns;
    case KDC_REG_FAULT: return s->fault | (s->stuck ? KDC_FAULT_STUCK : 0);
    case KDC_REG_FAULT_DROP: return s->fault_drop;
    case KDC_REG_FAULT_BAD_CID: return s->fault_bad_cid;
    case KDC_REG_FAULT_BAD_DMA: return s->fault_bad_dma;
    case KDC_REG_STAT_DONE: return s->stat_done;
    case KDC_REG_STAT_ABORTED: return s->stat_aborted;
    case KDC_REG_STAT_ERRORS: return s->stat_errors;
    case KDC_REG_STAT_IRQS: return s->stat_irqs;
    case KDC_REG_STAT_DOORBELLS: return s->stat_doorbells;
    case KDC_REG_STAT_CQ_DOORBELLS: return s->stat_cq_doorbells;
    case KDC_REG_SCRATCH: return s->scratch;
    case KDC_REG_DBBUF_LO: return (uint32_t)s->dbbuf;
    case KDC_REG_DBBUF_HI: return s->dbbuf >> 32;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "kdc-dma: read of unknown reg 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void kdc_mmio_write(void *opaque, hwaddr addr, uint64_t val64,
                           unsigned size)
{
    KdcState *s = opaque;
    uint32_t val = val64;

    if (s->gone) {
        return;
    }
    if (addr >= KDC_QUEUE_BASE) {
        unsigned q = (addr - KDC_QUEUE_BASE) / KDC_QUEUE_STRIDE;

        if (q < s->nqueues) {
            kdc_queue_write(&s->queues[q], (addr - KDC_QUEUE_BASE) % KDC_QUEUE_STRIDE, val);
        }
        return;
    }
    switch (addr) {
    case KDC_REG_CTRL:
        if ((val & KDC_CTRL_ENABLE) && !(s->ctrl & KDC_CTRL_ENABLE)) {
            if (!(s->status & KDC_STATUS_FATAL) && !s->stuck) {
                s->status |= KDC_STATUS_READY;
            }
        } else if (!(val & KDC_CTRL_ENABLE) && (s->ctrl & KDC_CTRL_ENABLE)) {
            if (s->fault & KDC_FAULT_RESET_HANG) {
                break; /* the disable is never acknowledged */
            }
            for (unsigned i = 0; i < s->nqueues; i++) {
                kdc_queue_drop(&s->queues[i]);
            }
            s->status &= ~KDC_STATUS_READY;
        }
        s->ctrl = val & KDC_CTRL_ENABLE;
        break;
    case KDC_REG_RESET:
        if (val == KDC_RESET_MAGIC && !(s->fault & KDC_FAULT_RESET_HANG)) {
            kdc_reset_state(s);
        }
        break;
    case KDC_REG_ERR_STATUS:
        s->err_status &= ~val;
        break;
    case KDC_REG_ERR_VECTOR:
        s->err_vector = val;
        break;
    case KDC_REG_SERVICE_NS:
        s->service_ns = val;
        break;
    case KDC_REG_FAULT:
        kdc_set_fault(s, val);
        break;
    case KDC_REG_FAULT_DROP:
        s->fault_drop = val;
        break;
    case KDC_REG_FAULT_BAD_CID:
        s->fault_bad_cid = val;
        break;
    case KDC_REG_FAULT_BAD_DMA:
        s->fault_bad_dma = val;
        break;
    case KDC_REG_SCRATCH:
        s->scratch = val;
        break;
    case KDC_REG_DBBUF_LO:
        if (s->shadow_db) {
            s->dbbuf = deposit64(s->dbbuf, 0, 32, val);
        }
        break;
    case KDC_REG_DBBUF_HI:
        if (s->shadow_db) {
            s->dbbuf = deposit64(s->dbbuf, 32, 32, val);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "kdc-dma: write to RO/unknown reg 0x%"
                      HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps kdc_mmio_ops = {
    .read = kdc_mmio_read,
    .write = kdc_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- QOM knobs, settable at runtime with qom-set ---- */

enum {
    KNOB_SERVICE_NS, KNOB_STALL, KNOB_FATAL, KNOB_DROP, KNOB_BAD_CID, KNOB_BAD_DMA,
    KNOB_RESET_HANG, KNOB_STUCK, KNOB_GONE, KNOB_SCRATCH,
    /* read-only device counters, for host-side measurements */
    KNOB_STAT_DOORBELLS, KNOB_STAT_CQ_DOORBELLS, KNOB_STAT_IRQS, KNOB_STAT_DONE,
};

static uint32_t *kdc_knob_ptr(KdcState *s, uintptr_t knob)
{
    switch (knob) {
    case KNOB_SERVICE_NS: return &s->service_ns;
    case KNOB_DROP: return &s->fault_drop;
    case KNOB_BAD_CID: return &s->fault_bad_cid;
    case KNOB_BAD_DMA: return &s->fault_bad_dma;
    case KNOB_SCRATCH: return &s->scratch;
    case KNOB_STAT_DOORBELLS: return &s->stat_doorbells;
    case KNOB_STAT_CQ_DOORBELLS: return &s->stat_cq_doorbells;
    case KNOB_STAT_IRQS: return &s->stat_irqs;
    case KNOB_STAT_DONE: return &s->stat_done;
    default: return NULL;
    }
}

static void kdc_get_u32(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    uint32_t value = *kdc_knob_ptr(KDC_DMA(obj), (uintptr_t)opaque);

    visit_type_uint32(v, name, &value, errp);
}

static void kdc_set_u32(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    uint32_t value;

    if (visit_type_uint32(v, name, &value, errp)) {
        *kdc_knob_ptr(KDC_DMA(obj), (uintptr_t)opaque) = value;
    }
}

static void kdc_get_bool(Object *obj, Visitor *v, const char *name, void *opaque,
                         Error **errp)
{
    KdcState *s = KDC_DMA(obj);
    bool value;

    switch ((uintptr_t)opaque) {
    case KNOB_STALL: value = s->fault & KDC_FAULT_STALL; break;
    case KNOB_RESET_HANG: value = s->fault & KDC_FAULT_RESET_HANG; break;
    case KNOB_STUCK: value = s->stuck; break;
    case KNOB_GONE: value = s->gone; break;
    default: value = s->status & KDC_STATUS_FATAL; break;
    }
    visit_type_bool(v, name, &value, errp);
}

static void kdc_set_bool(Object *obj, Visitor *v, const char *name, void *opaque,
                         Error **errp)
{
    KdcState *s = KDC_DMA(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    switch ((uintptr_t)opaque) {
    case KNOB_STALL:
        kdc_set_fault(s, (value ? s->fault | KDC_FAULT_STALL :
                                  s->fault & ~KDC_FAULT_STALL) |
                         (s->stuck ? KDC_FAULT_STUCK : 0));
        break;
    case KNOB_RESET_HANG:
        if (value) {
            s->fault |= KDC_FAULT_RESET_HANG;
        } else {
            s->fault &= ~KDC_FAULT_RESET_HANG;
        }
        break;
    case KNOB_STUCK:
        s->stuck = value;
        break;
    case KNOB_GONE:
        s->gone = value;
        if (!value) {
            kdc_kick_all(s);
        }
        break;
    default:
        if (value) {
            kdc_enter_fatal(s);
        }
        break;
    }
}

/* ---- lifecycle ---- */

static void kdc_realize(PCIDevice *pdev, Error **errp)
{
    KdcState *s = KDC_DMA(pdev);

    if (!s->nqueues || s->nqueues > KDC_MAX_QUEUES) {
        error_setg(errp, "kdc-dma: queues must be 1..%d", KDC_MAX_QUEUES);
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &kdc_mmio_ops, s, "kdc-regs",
                          KDC_BAR0_SIZE);
    pci_register_bar(pdev, KDC_BAR_REGS,
                     PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->mmio);
    if (msix_init_exclusive_bar(pdev, s->nqueues + 1, KDC_BAR_MSIX, errp)) {
        return;
    }
    for (unsigned i = 0; i < s->nqueues + 1; i++) {
        msix_vector_use(pdev, i);
    }
    if (pcie_endpoint_cap_init(pdev, 0x80) < 0) {
        error_setg(errp, "kdc-dma: failed to add the PCIe capability");
        msix_uninit_exclusive_bar(pdev);
        return;
    }
    pcie_cap_flr_init(pdev);
#if defined(__x86_64__)
    s->use_sse42 = s->hw_crc && __builtin_cpu_supports("sse4.2");
#endif
    s->xfer = g_malloc(KDC_MAX_XFER);
    for (unsigned i = 0; i < s->nqueues; i++) {
        KdcQueue *q = &s->queues[i];

        q->s = s;
        q->id = i;
        timer_init_ns(&q->service_timer, QEMU_CLOCK_VIRTUAL, kdc_service_timer, q);
        timer_init_ns(&q->coalesce_timer, QEMU_CLOCK_VIRTUAL, kdc_coalesce_timer, q);
        q->fetch_bh = qemu_bh_new_guarded(kdc_fetch, q,
                                          &DEVICE(s)->mem_reentrancy_guard);
        q->complete_bh = qemu_bh_new_guarded(kdc_complete_bh, q,
                                             &DEVICE(s)->mem_reentrancy_guard);
        q->notifier_ok = s->ioeventfd && event_notifier_init(&q->sq_notifier, 0) == 0;
        if (q->notifier_ok) {
            event_notifier_set_handler(&q->sq_notifier, kdc_sq_notify);
        }
    }
    kdc_reset_state(s);
}

static void kdc_exit(PCIDevice *pdev)
{
    KdcState *s = KDC_DMA(pdev);

    for (unsigned i = 0; i < s->nqueues; i++) {
        KdcQueue *q = &s->queues[i];

        kdc_queue_drop(q);
        qemu_bh_delete(q->fetch_bh);
        qemu_bh_delete(q->complete_bh);
        if (q->notifier_ok) {
            event_notifier_set_handler(&q->sq_notifier, NULL);
            event_notifier_cleanup(&q->sq_notifier);
        }
    }
    g_free(s->xfer);
    msix_uninit_exclusive_bar(pdev);
}

/* A device that stopped responding answers config reads with all-ones. */
static uint32_t kdc_config_read(PCIDevice *pdev, uint32_t addr, int len)
{
    if (KDC_DMA(pdev)->gone) {
        return len == 4 ? 0xffffffff : (1u << (len * 8)) - 1;
    }
    return pci_default_read_config(pdev, addr, len);
}

static void kdc_config_write(PCIDevice *pdev, uint32_t addr, uint32_t val,
                             int len)
{
    if (KDC_DMA(pdev)->gone) {
        return;
    }
    pci_default_write_config(pdev, addr, val, len);
    pcie_cap_flr_write_config(pdev, addr, val, len);
}

static void kdc_device_reset(DeviceState *dev)
{
    kdc_reset_state(KDC_DMA(dev));
}

static const Property kdc_props[] = {
    DEFINE_PROP_UINT32("queues", KdcState, nqueues, 4),
    DEFINE_PROP_BOOL("hw-crc", KdcState, hw_crc, true),
    DEFINE_PROP_BOOL("shadow-db", KdcState, shadow_db, true),
    DEFINE_PROP_BOOL("ioeventfd", KdcState, ioeventfd, true),
};

static const VMStateDescription kdc_vmstate = {
    .name = "kdc-dma",
    .unmigratable = 1,
};

static void kdc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(oc);

    k->realize = kdc_realize;
    k->exit = kdc_exit;
    k->config_read = kdc_config_read;
    k->config_write = kdc_config_write;
    k->vendor_id = KDC_PCI_VENDOR_ID;
    k->device_id = KDC_PCI_DEVICE_ID;
    k->revision = KDC_PCI_REVISION;
    k->class_id = PCI_CLASS_SYSTEM_OTHER;
    dc->desc = "KDC DMA copy + CRC32C engine (lab device)";
    dc->vmsd = &kdc_vmstate;
    device_class_set_props(dc, kdc_props);
    device_class_set_legacy_reset(dc, kdc_device_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    object_class_property_add(oc, "service-ns", "uint32", kdc_get_u32, kdc_set_u32,
                              NULL, (void *)(uintptr_t)KNOB_SERVICE_NS);
    object_class_property_add(oc, "fault-drop", "uint32", kdc_get_u32, kdc_set_u32,
                              NULL, (void *)(uintptr_t)KNOB_DROP);
    object_class_property_add(oc, "fault-bad-cid", "uint32", kdc_get_u32,
                              kdc_set_u32, NULL, (void *)(uintptr_t)KNOB_BAD_CID);
    object_class_property_add(oc, "fault-bad-dma", "uint32", kdc_get_u32,
                              kdc_set_u32, NULL, (void *)(uintptr_t)KNOB_BAD_DMA);
    object_class_property_add(oc, "fault-stall", "bool", kdc_get_bool, kdc_set_bool,
                              NULL, (void *)(uintptr_t)KNOB_STALL);
    object_class_property_add(oc, "fault-fatal", "bool", kdc_get_bool, kdc_set_bool,
                              NULL, (void *)(uintptr_t)KNOB_FATAL);
    object_class_property_add(oc, "fault-reset-hang", "bool", kdc_get_bool,
                              kdc_set_bool, NULL, (void *)(uintptr_t)KNOB_RESET_HANG);
    object_class_property_add(oc, "fault-stuck", "bool", kdc_get_bool, kdc_set_bool,
                              NULL, (void *)(uintptr_t)KNOB_STUCK);
    object_class_property_add(oc, "fault-gone", "bool", kdc_get_bool, kdc_set_bool,
                              NULL, (void *)(uintptr_t)KNOB_GONE);
    object_class_property_add(oc, "scratch", "uint32", kdc_get_u32, kdc_set_u32,
                              NULL, (void *)(uintptr_t)KNOB_SCRATCH);
    object_class_property_add(oc, "stat-doorbells", "uint32", kdc_get_u32, NULL,
                              NULL, (void *)(uintptr_t)KNOB_STAT_DOORBELLS);
    object_class_property_add(oc, "stat-cq-doorbells", "uint32", kdc_get_u32, NULL,
                              NULL, (void *)(uintptr_t)KNOB_STAT_CQ_DOORBELLS);
    object_class_property_add(oc, "stat-irqs", "uint32", kdc_get_u32, NULL,
                              NULL, (void *)(uintptr_t)KNOB_STAT_IRQS);
    object_class_property_add(oc, "stat-done", "uint32", kdc_get_u32, NULL,
                              NULL, (void *)(uintptr_t)KNOB_STAT_DONE);
}

static const TypeInfo kdc_types[] = {
    {
        .name = TYPE_KDC_DMA,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(KdcState),
        .class_init = kdc_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(kdc_types)
