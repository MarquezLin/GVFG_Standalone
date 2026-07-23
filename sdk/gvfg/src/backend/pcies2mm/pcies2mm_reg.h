#ifndef PCIES2MM_REG_H
#define PCIES2MM_REG_H

/*
 * GVFG PCIES2MM register map.
 * Synchronized with CaptureDemo/CaptureDemo/reg.h.
 * Definitions are internal capabilities; public SDK support is explicit elsewhere.
 */

#ifdef _KERNEL_MODE
#include <ntddk.h>
#endif

/* ========================================================================
 * Config BRAM Register Map (bramctrl_to_config)
 *
 * Address decoding (byte address):
 *   byte_addr = module_base + type_base + offset * 4
 *
 * Module base addresses:
 *   Module 0 (Interrupt)  = 0x000  (type bit ignored)
 *   Module 1 (CH0) Video  = 0x200  (type=0)
 *   Module 1 (CH0) Audio  = 0x280  (type=1)
 *   Module 2 (CH1) Video  = 0x400  (type=0)
 *   Module 2 (CH1) Audio  = 0x480  (type=1)
 *
 * Descriptor 100-bit format: [99:96]=ctrl, [95:64]=bytes,
 *                            [63:32]=addr_hi, [31:0]=addr_lo
 * ======================================================================== */

/* ---- Interrupt bit helper ---- */
#define BIT_N(i) (1 << (i))

/*
 * Interrupt pending status bit layout (irq_pending_status [7:0]):
 *   Each channel occupies 4 bits:
 *     bit[0] = video DMA controller interrupt (read dma_irq_status for details)
 *     bit[1] = video event interrupt
 *     bit[2] = audio DMA controller interrupt (read dma_irq_status for details)
 *     bit[3] = audio event interrupt
 *   CH0 = bits [3:0], CH1 = bits [7:4]
 *
 * dma_irq_status register [1:0] sub-bit definitions:
 *   bit[0] = DMA transfer done interrupt
 *   bit[1] = DMA soft reset complete interrupt
 */

/* Sub-interrupt type offsets within a channel's 4-bit field */
#define IRQ_SUB_VIDEO_DMA_CTRL 0
#define IRQ_SUB_VIDEO_EVENT 1
#define IRQ_SUB_AUDIO_DMA_CTRL 2
#define IRQ_SUB_AUDIO_EVENT 3
#define IRQ_SUBS_PER_CH 4

/* Bit position / mask for a specific sub-interrupt of a given channel */
#define IRQ_BIT(ch, sub) ((ch) * IRQ_SUBS_PER_CH + (sub))
#define IRQ_BIT_MASK(ch, sub) BIT_N(IRQ_BIT(ch, sub))
#define IRQ_CH_MASK(ch) (0xFUL << ((ch) * IRQ_SUBS_PER_CH))

/* Decompose a global bit position into channel number and sub-interrupt type */
#define IRQ_BIT_TO_CH(bit) ((bit) / IRQ_SUBS_PER_CH)
#define IRQ_BIT_TO_SUB(bit) ((bit) % IRQ_SUBS_PER_CH)

/* Concrete CH0 masks: bit[0]=0x01, bit[1]=0x02, bit[2]=0x04, bit[3]=0x08 */
#define IRQ_CH0_VIDEO_DMA_CTRL_MASK IRQ_BIT_MASK(0, IRQ_SUB_VIDEO_DMA_CTRL)
#define IRQ_CH0_VIDEO_EVENT_MASK IRQ_BIT_MASK(0, IRQ_SUB_VIDEO_EVENT)
#define IRQ_CH0_AUDIO_DMA_CTRL_MASK IRQ_BIT_MASK(0, IRQ_SUB_AUDIO_DMA_CTRL)
#define IRQ_CH0_AUDIO_EVENT_MASK IRQ_BIT_MASK(0, IRQ_SUB_AUDIO_EVENT)
#define IRQ_CH0_ALL_MASK IRQ_CH_MASK(0)

/* Concrete CH1 masks: bit[4]=0x10, bit[5]=0x20, bit[6]=0x40, bit[7]=0x80 */
#define IRQ_CH1_VIDEO_DMA_CTRL_MASK IRQ_BIT_MASK(1, IRQ_SUB_VIDEO_DMA_CTRL)
#define IRQ_CH1_VIDEO_EVENT_MASK IRQ_BIT_MASK(1, IRQ_SUB_VIDEO_EVENT)
#define IRQ_CH1_AUDIO_DMA_CTRL_MASK IRQ_BIT_MASK(1, IRQ_SUB_AUDIO_DMA_CTRL)
#define IRQ_CH1_AUDIO_EVENT_MASK IRQ_BIT_MASK(1, IRQ_SUB_AUDIO_EVENT)
#define IRQ_CH1_ALL_MASK IRQ_CH_MASK(1)

#define IRQ_ALL_MASK (IRQ_CH0_ALL_MASK | IRQ_CH1_ALL_MASK)

/* ---- Config BRAM Base Addresses (byte offsets) ---- */
#define INTERRUPT_BASE 0x00000000
#define CH0_VIDEO_BASE 0x00000200
#define CH0_AUDIO_BASE 0x00000280
#define CH1_VIDEO_BASE 0x00000400
#define CH1_AUDIO_BASE 0x00000480

/* Channel-indexed base: CH(n) video = 0x200 + n*0x200, audio = 0x280 + n*0x200 */
#define CH_VIDEO_BASE(ch) (CH0_VIDEO_BASE + (ch) * 0x200)
#define CH_AUDIO_BASE(ch) (CH0_AUDIO_BASE + (ch) * 0x200)

/* ========================================================================
 * Interrupt Registers (module 0, base 0x000)
 *
 * Note: 0x000/0x004/0x008 have different R/W semantics:
 *   Read  = RO status
 *   Write = WO control
 * ======================================================================== */
#define IRQ_PENDING_STATUS_OFFSET 0x000 /* RO: Interrupt pending status (read) */
#define IRQ_CLEAR_OFFSET 0x000          /* WO: Write-1-to-clear pending (write) */
#define IRQ_MASKED_STATUS_OFFSET 0x004  /* RO: Interrupt masked status (read) */
#define IRQ_MASK_W1S_OFFSET 0x004       /* WO: Write-1-to-set mask (write) */
#define IRQ_MASK_STATUS_OFFSET 0x008    /* RO: Interrupt mask register status (read) */
#define IRQ_MASK_W1C_OFFSET 0x008       /* WO: Write-1-to-clear mask (write) */
#define MAX_CHANNEL_COUNT_OFFSET 0x00C  /* RO: Max channel count [2:0], fixed=2 */

/* ========================================================================
 * Video Register Offsets (relative to CHx_VIDEO_BASE)
 * ======================================================================== */
#define VIDEO_EN_OFFSET 0x000             /* R/W: Video enable, bit[0] */
#define VIDEO_DMA_ADDR_LO_OFFSET 0x004    /* R/W: DMA address [31:0] */
#define VIDEO_DMA_ADDR_HI_OFFSET 0x008    /* R/W: DMA address [63:32] */
#define VIDEO_DMA_BYTES_OFFSET 0x00C      /* R/W: DMA transfer byte count */
#define VIDEO_DMA_CTRL_OFFSET 0x010       /* R/W: DMA control [3:0] */
#define VIDEO_DMA_WR_PTR_OFFSET 0x014     /* R/W: Descriptor write pointer [5:0] */
#define VIDEO_DMA_DESC_WR_OFFSET 0x018    /* WO:  Descriptor write pulse (self-clearing) */
#define VIDEO_DESC_MAX_CNT_OFFSET 0x01C   /* R/W: Max descriptor count [5:0] */
#define VIDEO_DMA_EN_OFFSET 0x020         /* R/W: DMA engine enable, bit[0] */
#define VIDEO_DMA_DONE_PTR_OFFSET 0x024   /* RO:  DMA done pointer [5:0] */
#define VIDEO_LOST_FRAME_CNT_OFFSET 0x028 /* R/W: Lost frame count (write=clear, read=current) [31:0] */
#define VIDEO_HSIZE_OFFSET 0x02C          /* RO:  Horizontal resolution [31:0] */
#define VIDEO_VSIZE_OFFSET 0x030          /* RO:  Vertical resolution [31:0] */
#define VIDEO_FORMAT_OFFSET 0x034         /* RO:  Video format (FOURCC) [31:0] */
#define VIDEO_IRQ_STATUS_OFFSET 0x038     /* WO/RO: Video IRQ status [7:0] (write=clear, read=external) */
#define VIDEO_DMA_IRQ_STATUS_OFFSET 0x03C /* WO/RO: Video DMA IRQ status [1:0] (read=external, write=clear); bit[0]=DMA done, bit[1]=reset done */
#define VIDEO_DMA_SOFT_RESET_OFFSET 0x040 /* WO:   DMA soft reset [31:0] (write value determines whether to send IRQ after reset) */

/* ========================================================================
 * Audio Register Offsets (relative to CHx_AUDIO_BASE)
 * ======================================================================== */
#define AUDIO_EN_OFFSET 0x000             /* R/W: Audio enable, bit[0] */
#define AUDIO_DMA_ADDR_LO_OFFSET 0x004    /* R/W: DMA address [31:0] */
#define AUDIO_DMA_ADDR_HI_OFFSET 0x008    /* R/W: DMA address [63:32] */
#define AUDIO_DMA_BYTES_OFFSET 0x00C      /* R/W: DMA transfer byte count */
#define AUDIO_DMA_CTRL_OFFSET 0x010       /* R/W: DMA control [3:0] */
#define AUDIO_DMA_WR_PTR_OFFSET 0x014     /* R/W: Descriptor write pointer [5:0] */
#define AUDIO_DMA_DESC_WR_OFFSET 0x018    /* WO:  Descriptor write pulse (self-clearing) */
#define AUDIO_DESC_MAX_CNT_OFFSET 0x01C   /* R/W: Max descriptor count [5:0] */
#define AUDIO_DMA_EN_OFFSET 0x020         /* R/W: DMA engine enable, bit[0] */
#define AUDIO_DMA_DONE_PTR_OFFSET 0x024   /* RO:  DMA done pointer [5:0] */
#define AUDIO_LOST_FRAME_CNT_OFFSET 0x028 /* R/W: Lost frame count (write=clear, read=current) [31:0] */
#define AUDIO_HAS_AUDIO_OFFSET 0x02C      /* RO:  Has audio flag, bit[0] */
#define AUDIO_THRES_OFFSET 0x030          /* R/W: Audio interrupt threshold [31:0] */
#define AUDIO_SAMPLE_OFFSET 0x034         /* RO:  Audio sample rate [31:0] */
#define AUDIO_DEPTH_OFFSET 0x038          /* RO:  Audio bit depth [7:0] */
#define AUDIO_NUM_OFFSET 0x03C            /* RO:  Audio channel count [3:0] */
#define AUDIO_IRQ_STATUS_OFFSET 0x040     /* WO/RO: Audio IRQ status [7:0] (write=clear, read=external) */
#define AUDIO_DMA_IRQ_STATUS_OFFSET 0x044 /* WO/RO: Audio DMA IRQ status [1:0] (read=external, write=clear); bit[0]=DMA done, bit[1]=reset done */
#define AUDIO_DMA_SOFT_RESET_OFFSET 0x048 /* WO:   DMA soft reset [31:0] (write value determines whether to send IRQ after reset) */

/* ========================================================================
 * Bit-field Definitions
 * ======================================================================== */
#define EN_BIT 0
#define DMA_CTRL_WIDTH 4
#define DMA_CTRL_MASK 0x0FUL
#define DESC_PTR_WIDTH 6
#define DESC_PTR_MASK 0x3FUL
#define MAX_CHANNEL_COUNT_MASK 0x07UL
#define AUDIO_NUM_MASK 0x0FUL
#define AUDIO_DEPTH_MASK 0xFFUL
#define IRQ_STATUS_MASK 0xFFUL
#define DMA_IRQ_STATUS_MASK 0x03UL

/* DMA IRQ status sub-bit definitions (within dma_irq_status [1:0]) */
#define DMA_IRQ_SUB_DONE 0                                             /* bit[0]: DMA transfer done */
#define DMA_IRQ_SUB_RESET_DONE 1                                       /* bit[1]: DMA soft reset complete */
#define DMA_IRQ_DONE_MASK BIT_N(DMA_IRQ_SUB_DONE)                      /* 0x01 */
#define DMA_IRQ_RESET_DONE_MASK BIT_N(DMA_IRQ_SUB_RESET_DONE)          /* 0x02 */
#define DMA_IRQ_ALL_MASK (DMA_IRQ_DONE_MASK | DMA_IRQ_RESET_DONE_MASK) /* 0x03 */

/* ========================================================================
 * Helper Macros - Config BRAM Register Access
 * ======================================================================== */
#ifdef _KERNEL_MODE
#define REG_READ32(Base, Offset) \
    READ_REGISTER_ULONG((PULONG)((PUCHAR)(Base) + (Offset)))

#define REG_WRITE32(Base, Offset, Value) \
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)(Base) + (Offset)), (Value))

/* ---- Interrupt Helpers ---- */
#define READ_INT_STATUS(Base) \
    REG_READ32(Base, IRQ_PENDING_STATUS_OFFSET)
#define WRITE_INT_CLEAR(Base, Val) \
    REG_WRITE32(Base, IRQ_CLEAR_OFFSET, (Val))
#define WRITE_INT_MASK_W1S(Base, Val) \
    REG_WRITE32(Base, IRQ_MASK_W1S_OFFSET, (Val))
#define WRITE_INT_MASK_W1C(Base, Val) \
    REG_WRITE32(Base, IRQ_MASK_W1C_OFFSET, (Val))
#define READ_INT_MASK(Base) \
    REG_READ32(Base, IRQ_MASK_STATUS_OFFSET)
#define READ_MAX_CHANNEL_COUNT(Base) \
    (REG_READ32(Base, MAX_CHANNEL_COUNT_OFFSET) & MAX_CHANNEL_COUNT_MASK)

/* ---- CH0 Video Helpers ---- */
#define CH0_VIDEO_READ_EN(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_EN_OFFSET)
#define CH0_VIDEO_WRITE_EN(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_EN_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_ADDR_LO(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_ADDR_LO_OFFSET)
#define CH0_VIDEO_WRITE_DMA_ADDR_LO(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_ADDR_LO_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_ADDR_HI(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_ADDR_HI_OFFSET)
#define CH0_VIDEO_WRITE_DMA_ADDR_HI(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_ADDR_HI_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_BYTES(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_BYTES_OFFSET)
#define CH0_VIDEO_WRITE_DMA_BYTES(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_BYTES_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_CTRL(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_CTRL_OFFSET)
#define CH0_VIDEO_WRITE_DMA_CTRL(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_CTRL_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_WR_PTR(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_WR_PTR_OFFSET)
#define CH0_VIDEO_WRITE_DMA_WR_PTR(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_WR_PTR_OFFSET, (Val))
#define CH0_VIDEO_WRITE_DESC_WR(Base) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_DESC_WR_OFFSET, 1)
#define CH0_VIDEO_READ_DESC_MAX_CNT(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DESC_MAX_CNT_OFFSET)
#define CH0_VIDEO_WRITE_DESC_MAX_CNT(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DESC_MAX_CNT_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_EN(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_EN_OFFSET)
#define CH0_VIDEO_WRITE_DMA_EN(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_EN_OFFSET, (Val))
#define CH0_VIDEO_READ_DMA_DONE_PTR(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_DONE_PTR_OFFSET)
#define CH0_VIDEO_READ_LOST_FRAME_CNT(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_LOST_FRAME_CNT_OFFSET)
#define CH0_VIDEO_WRITE_LOST_FRAME_CLR(Base) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_LOST_FRAME_CNT_OFFSET, 0)
#define CH0_VIDEO_READ_HSIZE(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_HSIZE_OFFSET)
#define CH0_VIDEO_READ_VSIZE(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_VSIZE_OFFSET)
#define CH0_VIDEO_READ_FORMAT(Base) \
    REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_FORMAT_OFFSET)
#define CH0_VIDEO_READ_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_IRQ_STATUS_OFFSET) & IRQ_STATUS_MASK)
#define CH0_VIDEO_WRITE_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_IRQ_STATUS_OFFSET, (Val) & IRQ_STATUS_MASK)
#define CH0_VIDEO_READ_DMA_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH0_VIDEO_BASE + VIDEO_DMA_IRQ_STATUS_OFFSET) & DMA_IRQ_STATUS_MASK)
#define CH0_VIDEO_WRITE_DMA_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_IRQ_STATUS_OFFSET, (Val) & DMA_IRQ_STATUS_MASK)
#define CH0_VIDEO_WRITE_DMA_SOFT_RESET(Base, Val) \
    REG_WRITE32(Base, CH0_VIDEO_BASE + VIDEO_DMA_SOFT_RESET_OFFSET, (Val))

/* ---- CH0 Audio Helpers ---- */
#define CH0_AUDIO_READ_EN(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_EN_OFFSET)
#define CH0_AUDIO_WRITE_EN(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_EN_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_ADDR_LO(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_ADDR_LO_OFFSET)
#define CH0_AUDIO_WRITE_DMA_ADDR_LO(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_ADDR_LO_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_ADDR_HI(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_ADDR_HI_OFFSET)
#define CH0_AUDIO_WRITE_DMA_ADDR_HI(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_ADDR_HI_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_BYTES(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_BYTES_OFFSET)
#define CH0_AUDIO_WRITE_DMA_BYTES(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_BYTES_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_CTRL(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_CTRL_OFFSET)
#define CH0_AUDIO_WRITE_DMA_CTRL(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_CTRL_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_WR_PTR(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_WR_PTR_OFFSET)
#define CH0_AUDIO_WRITE_DMA_WR_PTR(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_WR_PTR_OFFSET, (Val))
#define CH0_AUDIO_WRITE_DESC_WR(Base) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_DESC_WR_OFFSET, 1)
#define CH0_AUDIO_READ_DESC_MAX_CNT(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DESC_MAX_CNT_OFFSET)
#define CH0_AUDIO_WRITE_DESC_MAX_CNT(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DESC_MAX_CNT_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_EN(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_EN_OFFSET)
#define CH0_AUDIO_WRITE_DMA_EN(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_EN_OFFSET, (Val))
#define CH0_AUDIO_READ_DMA_DONE_PTR(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_DONE_PTR_OFFSET)
#define CH0_AUDIO_READ_LOST_FRAME_CNT(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_LOST_FRAME_CNT_OFFSET)
#define CH0_AUDIO_WRITE_LOST_FRAME_CLR(Base) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_LOST_FRAME_CNT_OFFSET, 0)
#define CH0_AUDIO_READ_HAS_AUDIO(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_HAS_AUDIO_OFFSET)
#define CH0_AUDIO_READ_THRES(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_THRES_OFFSET)
#define CH0_AUDIO_WRITE_THRES(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_THRES_OFFSET, (Val))
#define CH0_AUDIO_READ_SAMPLE(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_SAMPLE_OFFSET)
#define CH0_AUDIO_READ_DEPTH(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DEPTH_OFFSET)
#define CH0_AUDIO_READ_NUM(Base) \
    REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_NUM_OFFSET)
#define CH0_AUDIO_READ_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_IRQ_STATUS_OFFSET) & IRQ_STATUS_MASK)
#define CH0_AUDIO_WRITE_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_IRQ_STATUS_OFFSET, (Val) & IRQ_STATUS_MASK)
#define CH0_AUDIO_READ_DMA_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH0_AUDIO_BASE + AUDIO_DMA_IRQ_STATUS_OFFSET) & DMA_IRQ_STATUS_MASK)
#define CH0_AUDIO_WRITE_DMA_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_IRQ_STATUS_OFFSET, (Val) & DMA_IRQ_STATUS_MASK)
#define CH0_AUDIO_WRITE_DMA_SOFT_RESET(Base, Val) \
    REG_WRITE32(Base, CH0_AUDIO_BASE + AUDIO_DMA_SOFT_RESET_OFFSET, (Val))

/* ---- CH1 Video Helpers ---- */
#define CH1_VIDEO_READ_EN(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_EN_OFFSET)
#define CH1_VIDEO_WRITE_EN(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_EN_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_ADDR_LO(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_ADDR_LO_OFFSET)
#define CH1_VIDEO_WRITE_DMA_ADDR_LO(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_ADDR_LO_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_ADDR_HI(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_ADDR_HI_OFFSET)
#define CH1_VIDEO_WRITE_DMA_ADDR_HI(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_ADDR_HI_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_BYTES(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_BYTES_OFFSET)
#define CH1_VIDEO_WRITE_DMA_BYTES(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_BYTES_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_CTRL(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_CTRL_OFFSET)
#define CH1_VIDEO_WRITE_DMA_CTRL(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_CTRL_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_WR_PTR(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_WR_PTR_OFFSET)
#define CH1_VIDEO_WRITE_DMA_WR_PTR(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_WR_PTR_OFFSET, (Val))
#define CH1_VIDEO_WRITE_DESC_WR(Base) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_DESC_WR_OFFSET, 1)
#define CH1_VIDEO_READ_DESC_MAX_CNT(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DESC_MAX_CNT_OFFSET)
#define CH1_VIDEO_WRITE_DESC_MAX_CNT(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DESC_MAX_CNT_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_EN(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_EN_OFFSET)
#define CH1_VIDEO_WRITE_DMA_EN(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_EN_OFFSET, (Val))
#define CH1_VIDEO_READ_DMA_DONE_PTR(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_DONE_PTR_OFFSET)
#define CH1_VIDEO_READ_LOST_FRAME_CNT(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_LOST_FRAME_CNT_OFFSET)
#define CH1_VIDEO_WRITE_LOST_FRAME_CLR(Base) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_LOST_FRAME_CNT_OFFSET, 0)
#define CH1_VIDEO_READ_HSIZE(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_HSIZE_OFFSET)
#define CH1_VIDEO_READ_VSIZE(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_VSIZE_OFFSET)
#define CH1_VIDEO_READ_FORMAT(Base) \
    REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_FORMAT_OFFSET)
#define CH1_VIDEO_READ_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_IRQ_STATUS_OFFSET) & IRQ_STATUS_MASK)
#define CH1_VIDEO_WRITE_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_IRQ_STATUS_OFFSET, (Val) & IRQ_STATUS_MASK)
#define CH1_VIDEO_READ_DMA_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH1_VIDEO_BASE + VIDEO_DMA_IRQ_STATUS_OFFSET) & DMA_IRQ_STATUS_MASK)
#define CH1_VIDEO_WRITE_DMA_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_IRQ_STATUS_OFFSET, (Val) & DMA_IRQ_STATUS_MASK)
#define CH1_VIDEO_WRITE_DMA_SOFT_RESET(Base, Val) \
    REG_WRITE32(Base, CH1_VIDEO_BASE + VIDEO_DMA_SOFT_RESET_OFFSET, (Val))

/* ---- CH1 Audio Helpers ---- */
#define CH1_AUDIO_READ_EN(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_EN_OFFSET)
#define CH1_AUDIO_WRITE_EN(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_EN_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_ADDR_LO(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_ADDR_LO_OFFSET)
#define CH1_AUDIO_WRITE_DMA_ADDR_LO(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_ADDR_LO_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_ADDR_HI(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_ADDR_HI_OFFSET)
#define CH1_AUDIO_WRITE_DMA_ADDR_HI(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_ADDR_HI_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_BYTES(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_BYTES_OFFSET)
#define CH1_AUDIO_WRITE_DMA_BYTES(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_BYTES_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_CTRL(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_CTRL_OFFSET)
#define CH1_AUDIO_WRITE_DMA_CTRL(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_CTRL_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_WR_PTR(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_WR_PTR_OFFSET)
#define CH1_AUDIO_WRITE_DMA_WR_PTR(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_WR_PTR_OFFSET, (Val))
#define CH1_AUDIO_WRITE_DESC_WR(Base) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_DESC_WR_OFFSET, 1)
#define CH1_AUDIO_READ_DESC_MAX_CNT(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DESC_MAX_CNT_OFFSET)
#define CH1_AUDIO_WRITE_DESC_MAX_CNT(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DESC_MAX_CNT_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_EN(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_EN_OFFSET)
#define CH1_AUDIO_WRITE_DMA_EN(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_EN_OFFSET, (Val))
#define CH1_AUDIO_READ_DMA_DONE_PTR(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_DONE_PTR_OFFSET)
#define CH1_AUDIO_READ_LOST_FRAME_CNT(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_LOST_FRAME_CNT_OFFSET)
#define CH1_AUDIO_WRITE_LOST_FRAME_CLR(Base) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_LOST_FRAME_CNT_OFFSET, 0)
#define CH1_AUDIO_READ_HAS_AUDIO(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_HAS_AUDIO_OFFSET)
#define CH1_AUDIO_READ_THRES(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_THRES_OFFSET)
#define CH1_AUDIO_WRITE_THRES(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_THRES_OFFSET, (Val))
#define CH1_AUDIO_READ_SAMPLE(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_SAMPLE_OFFSET)
#define CH1_AUDIO_READ_DEPTH(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DEPTH_OFFSET)
#define CH1_AUDIO_READ_NUM(Base) \
    REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_NUM_OFFSET)
#define CH1_AUDIO_READ_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_IRQ_STATUS_OFFSET) & IRQ_STATUS_MASK)
#define CH1_AUDIO_WRITE_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_IRQ_STATUS_OFFSET, (Val) & IRQ_STATUS_MASK)
#define CH1_AUDIO_READ_DMA_IRQ_STATUS(Base) \
    (REG_READ32(Base, CH1_AUDIO_BASE + AUDIO_DMA_IRQ_STATUS_OFFSET) & DMA_IRQ_STATUS_MASK)
#define CH1_AUDIO_WRITE_DMA_IRQ_STATUS_CLEAR(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_IRQ_STATUS_OFFSET, (Val) & DMA_IRQ_STATUS_MASK)
#define CH1_AUDIO_WRITE_DMA_SOFT_RESET(Base, Val) \
    REG_WRITE32(Base, CH1_AUDIO_BASE + AUDIO_DMA_SOFT_RESET_OFFSET, (Val))

#endif /* _KERNEL_MODE */

#endif /* PCIES2MM_REG_H */
