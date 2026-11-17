/*
 * ACPMic core: every register decision and every number the driver computes.
 *
 * Nothing here knows about IOKit. Hardware access goes through acp_io_t, so
 * the same code runs against real MMIO in the kext and against a simulated
 * ACP in test/test_acp_core.c. Written from the behaviour of AMD's Linux
 * driver (sound/soc/amd/yc), not copied from it.
 */
#ifndef ACPMIC_ACP_CORE_H
#define ACPMIC_ACP_CORE_H

#include <stdint.h>
#include "acp_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. */
#define ACP_OK              0
#define ACP_ERR_TIMEOUT    -1
#define ACP_ERR_DEAD       -2   /* MMIO reads all-ones: device off / not decoding */
#define ACP_ERR_ARG        -3
#define ACP_ERR_NOT_PDM    -4   /* PIN_CONFIG says the pads are not wired for PDM */

typedef struct acp_io {
    void     *ctx;
    uint32_t (*rd)(void *ctx, uint32_t bar_off);
    void     (*wr)(void *ctx, uint32_t bar_off, uint32_t val);
    void     (*delay_us)(void *ctx, uint32_t us);
} acp_io_t;

/* ---- pure decisions (no I/O) ------------------------------------------ */

/* PCI revision IDs of ACP 6.x that use this PDM block (Rembrandt). */
int      acp_revision_supported(uint8_t rev);
/* PIN_CONFIG values for which AMD's driver registers the PDM microphone. */
int      acp_pin_config_is_pdm(uint32_t pin_config);
/* All-ones read = function powered off or BAR not decoding. */
int      acp_mmio_dead(uint32_t v);
/* Must PGFSM_CONTROL be written to power on (status not 0 and not already
 * powering on)? */
int      acp_pgfsm_needs_kick(uint32_t pgfsm_status);
/* MISC_CTRL with the WOV gain field replaced (gain clamped to 0..3). */
uint32_t acp_misc_ctrl_with_gain(uint32_t misc_ctrl, int gain);
/* ATU group-1 base register value. */
uint32_t acp_atu_grp1_base(void);
/* One page-table entry (low and high words) for a 4 KiB page at bus addr. */
void     acp_pte(uint64_t page_bus_addr, uint32_t *lo, uint32_t *hi);
/* Validate a list of page bus addresses for the ring: count matches, each
 * 4 KiB aligned and below 4 GiB (the AMD driver uses the default 32-bit
 * DMA mask). Returns ACP_OK or ACP_ERR_ARG. */
int      acp_validate_pages(const uint64_t *pages, uint32_t npages, uint32_t ring_bytes);
/* Combine the two halves of the linear position counter, tolerating a
 * carry between the two high reads. */
uint64_t acp_counter_combine(uint32_t hi1, uint32_t lo, uint32_t hi2, uint32_t lo2);

/* PDM S32LE -> Float32 conversion done with integer ops only (no FPU use in
 * the kernel). Result is bit-identical to (float)s / 2^31 with
 * round-to-nearest-even. 'boost' is a left shift 0..4 with saturation. */
uint32_t acp_s32_to_f32_bits(int32_t s);
int32_t  acp_boost(int32_t s, int boost);
void     acp_convert_block(const int32_t *src, uint32_t *dst_f32_bits,
                           uint32_t nsamples, int boost);

/* ---- position / timestamp bookkeeping (pure) -------------------------- */

typedef struct acp_pos {
    uint64_t base;          /* counter value when DMA was (re)started */
    uint64_t last;          /* last counter value seen */
    uint64_t wraps;         /* ring wraps already reported */
    uint32_t ring_bytes;
    uint32_t frame_bytes;
} acp_pos_t;

typedef struct acp_pos_update {
    int      valid;              /* 0: counter went backwards (reset/sleep) */
    uint32_t ring_frame;         /* write position inside the ring, frames */
    uint32_t new_wraps;          /* wraps completed since previous update */
    uint64_t frames_past_wrap;   /* frames written after the newest wrap */
} acp_pos_update_t;

void     acp_pos_start(acp_pos_t *p, uint64_t counter, uint32_t ring_bytes,
                       uint32_t frame_bytes);
void     acp_pos_update(acp_pos_t *p, uint64_t counter, acp_pos_update_t *out);
/* Nanoseconds covered by 'frames' at 'rate' Hz (rounded down). */
uint64_t acp_frames_to_ns(uint64_t frames, uint32_t rate);
/* For wrap i (0 = oldest) of n reported in one update, how many frames
 * before 'now' did it happen. */
uint64_t acp_wrap_age_frames(const acp_pos_update_t *u, uint32_t i,
                             uint32_t ring_frames);

/* DMA watchdog: the byte counter has not moved since start although more
 * than threshold_ns elapsed. */
int      acp_dma_stalled(uint64_t counter_now, uint64_t counter_at_start,
                         uint64_t elapsed_ns, uint64_t threshold_ns);
/* Counter value a running 48 kHz engine would have reached after elapsed_ns
 * (used to keep CoreAudio's clock alive, delivering silence, if the DMA
 * never starts moving). Overflow-safe for centuries. */
uint64_t acp_synth_counter(uint64_t base, uint64_t elapsed_ns, uint32_t rate,
                           uint32_t frame_bytes);

/* ---- hardware sequences (through acp_io_t) ---------------------------- */

int      acp_hw_power_on(const acp_io_t *io);
int      acp_hw_soft_reset(const acp_io_t *io);
/* Power on, enable, reset, select clock. Leaves all ACP interrupts disabled
 * (ACPMic polls; it never takes the INTx/MSI line). */
int      acp_hw_init(const acp_io_t *io);
int      acp_hw_init_ex(const acp_io_t *io, int reset_if_on);  /* 0: no reset if firmware left it on */
int      acp_hw_deinit(const acp_io_t *io);
/* Program ATU + PTEs + ring registers for 'npages' 4 KiB pages. */
int      acp_hw_program_ring(const acp_io_t *io, const uint64_t *pages,
                             uint32_t npages, uint32_t ring_bytes,
                             uint32_t watermark_bytes);
int      acp_hw_pdm_running(const acp_io_t *io);
int      acp_hw_pdm_start(const acp_io_t *io, int gain);
int      acp_hw_pdm_stop(const acp_io_t *io);
uint64_t acp_hw_read_counter(const acp_io_t *io);
/* Write-1-to-clear the PDM DMA status bit if the hardware latched it. */
void     acp_hw_ack_pdm_status(const acp_io_t *io);

/* Registers dumped by the read-only probe mode. */
typedef struct acp_named_reg { const char *name; uint32_t addr; } acp_named_reg_t;
extern const acp_named_reg_t acp_dump_regs[];
extern const uint32_t acp_dump_regs_count;

#ifdef __cplusplus
}
#endif
#endif
