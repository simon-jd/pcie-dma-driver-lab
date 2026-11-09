/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Hardware interface of the emulated KDC DMA engine ("kdc-dma" QEMU device).
 *
 * Shared by the QEMU device model, the Linux driver and the host-side qtest
 * suite (which reads the #define lines of this file). All registers are 32-bit,
 * all in-memory structures are little-endian. There is no physical part behind
 * this interface; the PCI IDs below are a lab-only choice under the QEMU
 * community vendor ID and must not be used outside an emulator.
 */
#ifndef KDC_HW_H
#define KDC_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#define KDC_U8 __u8
#define KDC_LE16 __le16
#define KDC_LE32 __le32
#define KDC_LE64 __le64
#else
#include <stdint.h>
#define KDC_U8 uint8_t
#define KDC_LE16 uint16_t
#define KDC_LE32 uint32_t
#define KDC_LE64 uint64_t
#endif

#define KDC_PCI_VENDOR_ID 0x1234
#define KDC_PCI_DEVICE_ID 0x4b44
#define KDC_PCI_REVISION 0x01

#define KDC_BAR_REGS 0 /* 64-bit memory BAR, registers */
#define KDC_BAR_MSIX 2 /* MSI-X table and PBA */
#define KDC_BAR0_SIZE 0x4000

/* Global registers (BAR0) */
#define KDC_REG_ID 0x000 /* RO */
#define KDC_ID_VALUE 0x6b646331 /* "kdc1" */
#define KDC_REG_VERSION 0x004 /* RO: major << 16 | minor */
#define KDC_VERSION_MAJOR 1
#define KDC_VERSION_MINOR 0
#define KDC_REG_CAP 0x008 /* RO */
#define KDC_CAP_QUEUES_MASK 0xff /* [7:0] number of queue pairs */
#define KDC_CAP_DEPTH_SHIFT 8 /* [11:8] log2 of the largest ring */
#define KDC_CAP_DEPTH_MASK 0xf
#define KDC_CAP_SEGS_SHIFT 16 /* [31:16] SG entries per list */
#define KDC_REG_MAX_XFER 0x00c /* RO: largest transfer in bytes */
#define KDC_REG_FEATURES 0x010 /* RO */
#define KDC_FEAT_CRC32C 0x01
#define KDC_FEAT_ABORT 0x02
#define KDC_FEAT_COALESCE 0x04
#define KDC_FEAT_FAULTS 0x08
#define KDC_FEAT_FLR 0x10

#define KDC_REG_CTRL 0x020 /* RW */
#define KDC_CTRL_ENABLE 0x01 /* 1->0 drops all in-flight work without completions */
#define KDC_REG_STATUS 0x024 /* RO */
#define KDC_STATUS_READY 0x01
#define KDC_STATUS_FATAL 0x02 /* stopped; only a reset clears it */
#define KDC_REG_RESET 0x028 /* WO: write KDC_RESET_MAGIC for a controller reset */
#define KDC_RESET_MAGIC 0x52534554
#define KDC_REG_ERR_STATUS 0x030 /* RW1C */
#define KDC_ERR_DOORBELL 0x01 /* doorbell value out of range (ignored) */
#define KDC_ERR_RING_DMA 0x02 /* SQ fetch or CQ write failed; queue halted */
#define KDC_ERR_QUEUE_CONFIG 0x04 /* queue enable rejected */
#define KDC_ERR_FATAL 0x08 /* device entered the fatal state */
#define KDC_REG_ERR_INFO 0x034 /* RO: queue of the last error */
#define KDC_REG_ERR_VECTOR 0x038 /* RW: MSI-X vector for device errors */

/* Timing and fault-injection knobs; also QOM properties of the device. */
#define KDC_REG_SERVICE_NS 0x040 /* RW: service time per command */
#define KDC_REG_FAULT 0x044 /* RW */
#define KDC_FAULT_STALL 0x01 /* engine hangs: no completions, aborts not honoured */
#define KDC_FAULT_FATAL 0x02 /* write 1: enter the fatal state */
#define KDC_FAULT_RESET_HANG 0x04 /* controller reset and disable are ignored; FLR clears it */
#define KDC_FAULT_STUCK 0x08 /* never becomes READY again; survives every reset */
#define KDC_REG_FAULT_DROP 0x048 /* RW: drop the next N completions */
#define KDC_REG_FAULT_BAD_CID 0x04c /* RW: corrupt the cid of the next N completions */
#define KDC_REG_FAULT_BAD_DMA 0x050 /* RW: next N copies target KDC_BAD_DMA_ADDR */
#define KDC_BAD_DMA_ADDR 0xfffffffffff00000ULL

/* Device counters (RO, wrap at 2^32, cleared by reset) */
#define KDC_REG_STAT_DONE 0x060
#define KDC_REG_STAT_ABORTED 0x064
#define KDC_REG_STAT_ERRORS 0x068
#define KDC_REG_STAT_IRQS 0x06c
#define KDC_REG_STAT_DOORBELLS 0x070 /* submission queue tail writes */
#define KDC_REG_STAT_CQ_DOORBELLS 0x074 /* completion queue head writes */

/* RW, no effect on the device; cleared by reset. Lets test code on the host and
 * in the guest hand each other a token (the host writes it as a QOM property). */
#define KDC_REG_SCRATCH 0x078

/* Per-queue registers */
#define KDC_QUEUE_BASE 0x1000
#define KDC_QUEUE_STRIDE 0x100
#define KDC_MAX_QUEUES 16
#define KDC_QREG(q, r) (KDC_QUEUE_BASE + (q) * KDC_QUEUE_STRIDE + (r))
#define KDC_Q_SQ_BASE_LO 0x00
#define KDC_Q_SQ_BASE_HI 0x04
#define KDC_Q_CQ_BASE_LO 0x08
#define KDC_Q_CQ_BASE_HI 0x0c
#define KDC_Q_SIZE_LOG2 0x10 /* entries in each ring = 1 << value */
#define KDC_Q_CTRL 0x14
#define KDC_QCTRL_ENABLE 0x01
#define KDC_Q_STATUS 0x18
#define KDC_QSTATUS_ENABLED 0x01
#define KDC_QSTATUS_HALTED 0x02
#define KDC_Q_IRQ_VECTOR 0x1c
#define KDC_Q_COALESCE 0x20 /* [15:0] completions per interrupt, [31:16] max delay us */
#define KDC_Q_SQ_TAIL 0x24 /* doorbell */
#define KDC_Q_CQ_HEAD 0x28 /* doorbell */
#define KDC_Q_SQ_HEAD 0x2c /* RO: next entry the device will fetch */
#define KDC_Q_ABORT 0x30 /* WO: cid of an in-flight command to abort */
#define KDC_Q_INFLIGHT 0x34 /* RO: commands the device currently holds */

#define KDC_MIN_DEPTH_LOG2 1
#define KDC_RING_ALIGN 4096

/* Submission queue entry */
#define KDC_OP_NOP 0x00
#define KDC_OP_COPY_CRC 0x01 /* copy src -> dst, return CRC32C of the data */
#define KDC_SQE_F_SRC_SGL 0x01 /* src points to src_nents SG entries */
#define KDC_SQE_F_DST_SGL 0x02 /* dst points to dst_nents SG entries */
#define KDC_SQE_FLAGS_MASK 0x03

struct kdc_hw_sqe {
	KDC_U8 opcode;
	KDC_U8 flags;
	KDC_LE16 cid;
	KDC_LE32 length;
	KDC_LE64 src; /* buffer or SG list address */
	KDC_LE64 dst;
	KDC_LE16 src_nents;
	KDC_LE16 dst_nents;
	KDC_LE32 crc_seed; /* CRC32C value to continue from; 0 for a fresh CRC */
	KDC_LE64 rsvd[4]; /* must be zero */
};
#define KDC_SQE_SIZE 64

struct kdc_hw_sge {
	KDC_LE64 addr;
	KDC_LE32 len; /* nonzero */
	KDC_LE32 rsvd; /* must be zero */
};
#define KDC_SGE_SIZE 16

/* Completion queue entry; the device writes status (with the phase bit) last. */
struct kdc_hw_cqe {
	KDC_LE32 result; /* CRC32C for COPY_CRC */
	KDC_LE32 bytes; /* bytes written to dst */
	KDC_LE16 sq_head;
	KDC_LE16 sqid;
	KDC_LE16 cid;
	KDC_LE16 status; /* code << 1 | phase */
};
#define KDC_CQE_SIZE 16
#define KDC_CQE_PHASE 0x1
#define KDC_CQE_CODE_SHIFT 1

#define KDC_SC_SUCCESS 0
#define KDC_SC_INVALID_OPCODE 1
#define KDC_SC_INVALID_FIELD 2
#define KDC_SC_INVALID_SGL 3
#define KDC_SC_DATA_ERROR 4 /* a DMA read or write failed (IOMMU fault, bad address) */
#define KDC_SC_ABORTED 5
#define KDC_SC_INTERNAL 6

#endif
