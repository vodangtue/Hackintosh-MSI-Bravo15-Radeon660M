/*
 * ACPMic - AMD ACP 6.x (Rembrandt / "Yellow Carp", PCI 1022:15E2 rev 0x60/0x6F)
 * register map, restricted to what the PDM (digital microphone) path needs.
 *
 * Addresses are the ACP's own bus addresses as documented by AMD in the Linux
 * header sound/soc/amd/yc/acp6x_chip_offset_byte.h. The PCI BAR0 window starts
 * at ACP_BAR_ORIGIN, so a register lives at BAR0 + (addr - ACP_BAR_ORIGIN).
 * Use ACP_OFF(name) to get the BAR0 offset.
 *
 * Plain C, no kernel or libc headers, so the host tests include it as is.
 */
#ifndef ACPMIC_ACP_REGS_H
#define ACPMIC_ACP_REGS_H

#define ACP_BAR_ORIGIN                     0x1240000u
#define ACP_OFF(abs_addr)                  ((unsigned)((abs_addr) - ACP_BAR_ORIGIN))

/* AXI-to-AXI address translation unit (the ACP's IOMMU-like page table). */
#define ACP_REG_ATU_PAGE_SIZE_GRP_1        0x1240C00u
#define ACP_REG_ATU_BASE_ADDR_GRP_1        0x1240C04u
#define ACP_REG_ATU_CTRL                   0x1240C40u

/* Top-level control. */
#define ACP_REG_SOFT_RESET                 0x1241000u
#define ACP_REG_CONTROL                    0x1241004u
#define ACP_REG_PGFSM_CONTROL              0x1241024u
#define ACP_REG_PGFSM_STATUS               0x1241028u
#define ACP_REG_CLKMUX_SEL                 0x124102Cu
#define ACP_REG_PIN_CONFIG                 0x1241440u

/* Interrupt routing to the host. ACPMic never enables these. */
#define ACP_REG_EXTERNAL_INTR_ENB          0x1241A00u
#define ACP_REG_EXTERNAL_INTR_CNTL         0x1241A04u
#define ACP_REG_EXTERNAL_INTR_STAT         0x1241A0Cu

/* WOV = "wake on voice" block, which is the PDM microphone receiver. */
#define ACP_REG_WOV_PDM_ENABLE             0x1242C04u
#define ACP_REG_WOV_PDM_DMA_ENABLE         0x1242C08u
#define ACP_REG_WOV_RX_RINGBUFADDR         0x1242C0Cu
#define ACP_REG_WOV_RX_RINGBUFSIZE         0x1242C10u
#define ACP_REG_WOV_RX_LINKPOSITIONCNTR    0x1242C14u
#define ACP_REG_WOV_RX_LINEARPOS_HIGH      0x1242C18u
#define ACP_REG_WOV_RX_LINEARPOS_LOW       0x1242C1Cu
#define ACP_REG_WOV_RX_INTR_WATERMARK      0x1242C20u
#define ACP_REG_WOV_PDM_FIFO_FLUSH         0x1242C24u
#define ACP_REG_WOV_PDM_NO_OF_CHANNELS     0x1242C28u
#define ACP_REG_WOV_PDM_DECIMATION_FACTOR  0x1242C2Cu
#define ACP_REG_WOV_MISC_CTRL              0x1242C5Cu
#define ACP_REG_WOV_CLK_CTRL               0x1242C60u
#define ACP_REG_WOV_ERROR_STATUS           0x1242C68u

/* SRAM-backed scratch registers; the ATU page table for group 1 lives here. */
#define ACP_REG_SCRATCH_REG_0              0x1250000u

/* Highest offset ACPMic ever touches (+ 8-byte PTE * max pages), must stay
 * inside BAR0 (256 KiB on this machine, checked at runtime too). */
#define ACP_BAR0_MIN_LEN                   0x20000u

/* ---- Field values ---------------------------------------------------- */

#define ACP_PGFSM_CNTL_POWER_ON            1u
#define ACP_PGFSM_STATUS_MASK              3u
#define ACP_PGFSM_POWERED_ON               0u
#define ACP_PGFSM_POWER_ON_IN_PROGRESS     1u
#define ACP_PGFSM_POWERED_OFF              2u
#define ACP_PGFSM_POWER_OFF_IN_PROGRESS    3u

#define ACP_SOFT_RESET_AUDDONE_MASK        0x00010001u
#define ACP_CLKMUX_SEL_ACLK                3u      /* value the AMD driver uses */

#define ACP_PDM_DMA_INTR_BIT               (1u << 16) /* in INTR_CNTL / INTR_STAT */

#define ACP_PDM_ENABLE_ON                  1u
#define ACP_PDM_DMA_EN                     1u      /* write: request enable */
#define ACP_PDM_DMA_STOP_REQ               2u      /* write: request stop */
#define ACP_PDM_DMA_EN_STATUS              2u      /* read: engine is running */
#define ACP_PDM_CLK_FREQ                   7u
#define ACP_PDM_CH_MASK_2CH                0u
#define ACP_PDM_DECIMATION                 2u
#define ACP_WOV_GAIN_SHIFT                 3
#define ACP_WOV_GAIN_MASK                  (3u << ACP_WOV_GAIN_SHIFT)

/* ATU: group 1 maps the ACP-internal window PDM_MEM_WINDOW_START onwards
 * through PTEs stored at SCRATCH_REG_0 + PDM_PTE_OFFSET. */
#define ACP_SRAM_PTE_OFFSET                0x03800000u
#define ACP_ATU_GRP_ENABLE                 (1u << 31)
#define ACP_ATU_PAGE_SIZE_4K               2u
#define ACP_PDM_PTE_OFFSET                 0u
#define ACP_PTE_VALID                      (1u << 31) /* set in the high word */
#define ACP_PDM_MEM_WINDOW_START           0x04000000u
#define ACP_PAGE_SIZE                      4096u

/* Polling budgets, same as the AMD driver. */
#define ACP_PGFSM_POLL_US                  500u    /* x 1 us */
#define ACP_RESET_POLL_COUNT               500u    /* x 1 us */
#define ACP_DMA_POLL_COUNT                 20000u  /* x 5 us = 100 ms */
#define ACP_DMA_POLL_DELAY_US              5u

/* Stream geometry: 2 ch, S32LE, 48 kHz, 4 x 8 KiB periods (AMD maxima). */
#define ACP_CHANNELS                       2u
#define ACP_SAMPLE_BYTES                   4u
#define ACP_FRAME_BYTES                    (ACP_CHANNELS * ACP_SAMPLE_BYTES)
#define ACP_SAMPLE_RATE                    48000u
#define ACP_PERIOD_BYTES                   8192u
#define ACP_NUM_PERIODS                    4u
#define ACP_RING_BYTES                     (ACP_PERIOD_BYTES * ACP_NUM_PERIODS)
#define ACP_RING_FRAMES                    (ACP_RING_BYTES / ACP_FRAME_BYTES)
#define ACP_RING_PAGES                     (ACP_RING_BYTES / ACP_PAGE_SIZE)

#endif
