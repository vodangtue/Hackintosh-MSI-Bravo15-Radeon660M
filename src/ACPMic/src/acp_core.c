/*
 * ACPMic core - see acp_core.h. No libc, no FPU, no kernel headers.
 */
#include "acp_core.h"

#define RD(io, reg)       ((io)->rd((io)->ctx, ACP_OFF(reg)))
#define WR(io, reg, v)    ((io)->wr((io)->ctx, ACP_OFF(reg), (uint32_t)(v)))
#define DELAY(io, us)     ((io)->delay_us((io)->ctx, (us)))

/* ---- pure decisions ---------------------------------------------------- */

int acp_revision_supported(uint8_t rev)
{
    /* 0x60: Rembrandt (this laptop reports 0x60), 0x6F: Rembrandt variant.
     * 0x62 (Raptor/Mendocino family) uses other pin configs; not claimed. */
    return rev == 0x60 || rev == 0x6F;
}

int acp_pin_config_is_pdm(uint32_t v)
{
    /* AMD's driver leaves configs 0,1,2,3,9,15 to other drivers (I2S/SOF/HS
     * codecs) and treats every other value as "PDM DMIC on the WOV pads".
     * Values above 15 cannot be a pin config; treat them as unknown = no. */
    switch (v) {
    case 0: case 1: case 2: case 3: case 9: case 15:
        return 0;
    default:
        return v <= 15;
    }
}

int acp_mmio_dead(uint32_t v)
{
    return v == 0xFFFFFFFFu;
}

int acp_pgfsm_needs_kick(uint32_t st)
{
    if (st == ACP_PGFSM_POWERED_ON)
        return 0;
    return (st & ACP_PGFSM_STATUS_MASK) != ACP_PGFSM_POWER_ON_IN_PROGRESS;
}

uint32_t acp_misc_ctrl_with_gain(uint32_t misc, int gain)
{
    if (gain < 0) gain = 0;
    if (gain > 3) gain = 3;
    misc &= ~ACP_WOV_GAIN_MASK;
    misc |= ((uint32_t)gain << ACP_WOV_GAIN_SHIFT) & ACP_WOV_GAIN_MASK;
    return misc;
}

uint32_t acp_atu_grp1_base(void)
{
    return ACP_SRAM_PTE_OFFSET | ACP_ATU_GRP_ENABLE;
}

void acp_pte(uint64_t addr, uint32_t *lo, uint32_t *hi)
{
    *lo = (uint32_t)addr;
    *hi = (uint32_t)(addr >> 32) | ACP_PTE_VALID;
}

int acp_validate_pages(const uint64_t *pages, uint32_t npages, uint32_t ring_bytes)
{
    uint32_t i;
    if (!pages || npages == 0 || ring_bytes == 0)
        return ACP_ERR_ARG;
    if (ring_bytes % ACP_PAGE_SIZE != 0)
        return ACP_ERR_ARG;
    if (npages != ring_bytes / ACP_PAGE_SIZE)
        return ACP_ERR_ARG;
    /* PTE table must stay well inside the scratch SRAM (64 PTEs = 256 KiB of
     * audio is far more than the 32 KiB ring we use). */
    if (npages > 64)
        return ACP_ERR_ARG;
    for (i = 0; i < npages; i++) {
        if (pages[i] == 0 || (pages[i] & (ACP_PAGE_SIZE - 1)) != 0)
            return ACP_ERR_ARG;
        if (pages[i] + ACP_PAGE_SIZE > 0x100000000ULL)
            return ACP_ERR_ARG;
    }
    return ACP_OK;
}

uint64_t acp_counter_combine(uint32_t hi1, uint32_t lo, uint32_t hi2, uint32_t lo2)
{
    if (hi1 == hi2)
        return ((uint64_t)hi1 << 32) | lo;
    return ((uint64_t)hi2 << 32) | lo2;
}

/* ---- sample conversion -------------------------------------------------- */

int32_t acp_boost(int32_t s, int boost)
{
    int64_t v;
    if (boost <= 0)
        return s;
    if (boost > 4)
        boost = 4;
    v = (int64_t)s * ((int64_t)1 << boost);
    if (v > 2147483647LL)  return 2147483647;
    if (v < -2147483647LL - 1) return (int32_t)(-2147483647LL - 1);
    return (int32_t)v;
}

static int msb32(uint32_t v)
{
    int n = 31;
    while (!(v & 0x80000000u)) { v <<= 1; n--; }
    return n;
}

uint32_t acp_s32_to_f32_bits(int32_t s)
{
    uint32_t sign, mag, mant, exp;
    int top;

    if (s == 0)
        return 0;
    sign = s < 0 ? 0x80000000u : 0;
    mag  = s < 0 ? (uint32_t)0 - (uint32_t)s : (uint32_t)s;   /* INT_MIN ok */
    top  = msb32(mag);                 /* value = mag * 2^-31 */
    exp  = (uint32_t)(top - 31 + 127); /* 96..127, never denormal */

    if (top <= 23) {
        mant = mag << (23 - top);
    } else {
        int sh = top - 23;
        uint32_t rem  = mag & ((1u << sh) - 1);
        uint32_t half = 1u << (sh - 1);
        mant = mag >> sh;
        if (rem > half || (rem == half && (mant & 1)))
            mant++;
        if (mant == (1u << 24)) {      /* rounding carried into a new bit */
            mant >>= 1;
            exp++;
        }
    }
    return sign | (exp << 23) | (mant & 0x7FFFFFu);
}

void acp_convert_block(const int32_t *src, uint32_t *dst, uint32_t n, int boost)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = acp_s32_to_f32_bits(acp_boost(src[i], boost));
}

/* ---- position bookkeeping ---------------------------------------------- */

void acp_pos_start(acp_pos_t *p, uint64_t counter, uint32_t ring_bytes, uint32_t frame_bytes)
{
    p->base = counter;
    p->last = counter;
    p->wraps = 0;
    p->ring_bytes = ring_bytes;
    p->frame_bytes = frame_bytes;
}

void acp_pos_update(acp_pos_t *p, uint64_t counter, acp_pos_update_t *u)
{
    uint64_t delta, total_wraps, in_ring;

    u->valid = 0;
    u->ring_frame = 0;
    u->new_wraps = 0;
    u->frames_past_wrap = 0;
    if (p->ring_bytes == 0 || p->frame_bytes == 0)
        return;
    if (counter < p->last)
        return;                         /* went backwards: caller restarts */
    p->last = counter;

    delta       = counter - p->base;
    total_wraps = delta / p->ring_bytes;
    in_ring     = delta % p->ring_bytes;

    u->valid = 1;
    u->ring_frame = (uint32_t)(in_ring / p->frame_bytes);
    u->frames_past_wrap = in_ring / p->frame_bytes;
    if (total_wraps > p->wraps) {
        uint64_t n = total_wraps - p->wraps;
        u->new_wraps = n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
        p->wraps = total_wraps;
    }
}

uint64_t acp_frames_to_ns(uint64_t frames, uint32_t rate)
{
    if (rate == 0)
        return 0;
    /* frames < 2^34 keeps frames * 1e9 inside 64 bits. */
    return (frames * 1000000000ull) / rate;
}

uint64_t acp_wrap_age_frames(const acp_pos_update_t *u, uint32_t i, uint32_t ring_frames)
{
    /* Wrap n-1 (newest) happened frames_past_wrap ago; each older one a full
     * ring earlier. */
    uint32_t newer = u->new_wraps - 1 - i;
    return u->frames_past_wrap + (uint64_t)newer * ring_frames;
}

int acp_dma_stalled(uint64_t now, uint64_t at_start, uint64_t elapsed_ns, uint64_t threshold_ns)
{
    return now == at_start && elapsed_ns > threshold_ns;
}

uint64_t acp_synth_counter(uint64_t base, uint64_t elapsed_ns, uint32_t rate, uint32_t frame_bytes)
{
    uint64_t secs = elapsed_ns / 1000000000ull;
    uint64_t rem  = elapsed_ns % 1000000000ull;
    uint64_t frames = secs * rate + (rem * rate) / 1000000000ull;
    return base + frames * frame_bytes;
}

/* ---- hardware sequences ------------------------------------------------- */

int acp_hw_power_on(const acp_io_t *io)
{
    uint32_t st = RD(io, ACP_REG_PGFSM_STATUS), i;
    if (acp_mmio_dead(st))
        return ACP_ERR_DEAD;
    if (st == ACP_PGFSM_POWERED_ON)
        return ACP_OK;
    if (acp_pgfsm_needs_kick(st))
        WR(io, ACP_REG_PGFSM_CONTROL, ACP_PGFSM_CNTL_POWER_ON);
    for (i = 0; i < ACP_PGFSM_POLL_US; i++) {
        st = RD(io, ACP_REG_PGFSM_STATUS);
        if (st == ACP_PGFSM_POWERED_ON)
            return ACP_OK;
        DELAY(io, 1);
    }
    return ACP_ERR_TIMEOUT;
}

int acp_hw_soft_reset(const acp_io_t *io)
{
    uint32_t i, v;
    WR(io, ACP_REG_SOFT_RESET, 1);
    for (i = 0; i < ACP_RESET_POLL_COUNT; i++) {
        v = RD(io, ACP_REG_SOFT_RESET);
        if (v & ACP_SOFT_RESET_AUDDONE_MASK)
            break;
        DELAY(io, 1);
    }
    WR(io, ACP_REG_SOFT_RESET, 0);
    for (i = 0; i < ACP_RESET_POLL_COUNT; i++) {
        v = RD(io, ACP_REG_SOFT_RESET);
        if (v == 0)
            return ACP_OK;
        DELAY(io, 1);
    }
    return ACP_ERR_TIMEOUT;
}

static void acp_hw_irqs_off(const acp_io_t *io)
{
    WR(io, ACP_REG_EXTERNAL_INTR_STAT, 0xFFFFFFFFu);   /* W1C everything */
    WR(io, ACP_REG_EXTERNAL_INTR_CNTL, 0);
    WR(io, ACP_REG_EXTERNAL_INTR_ENB, 0);
}

/* 2026-11-17: with ACPMic running, the speakers (SimpleHDA on the HDA
 * function, PCI .6) went silent while CoreAudio still played into them.
 * Suspect: this whole-block soft reset, issued after SimpleHDA had set the
 * codec up. `reset_if_on` = 0 skips the reset when the firmware left the ACP
 * powered on (PGFSM already POWERED_ON); from off it always resets. */
int acp_hw_init_ex(const acp_io_t *io, int reset_if_on)
{
    const int was_on = RD(io, ACP_REG_PGFSM_STATUS) == ACP_PGFSM_POWERED_ON;
    int r = acp_hw_power_on(io);
    if (r)
        return r;
    WR(io, ACP_REG_CONTROL, 1);
    if (!was_on || reset_if_on) {
        r = acp_hw_soft_reset(io);
        if (r)
            return r;
    }
    WR(io, ACP_REG_CLKMUX_SEL, ACP_CLKMUX_SEL_ACLK);
    acp_hw_irqs_off(io);
    return ACP_OK;
}

int acp_hw_init(const acp_io_t *io)
{
    return acp_hw_init_ex(io, 1);
}

int acp_hw_deinit(const acp_io_t *io)
{
    int r;
    if (acp_mmio_dead(RD(io, ACP_REG_PGFSM_STATUS)))
        return ACP_ERR_DEAD;
    acp_hw_irqs_off(io);
    r = acp_hw_soft_reset(io);
    WR(io, ACP_REG_CLKMUX_SEL, 0);
    WR(io, ACP_REG_CONTROL, 0);
    return r;
}

int acp_hw_program_ring(const acp_io_t *io, const uint64_t *pages, uint32_t npages,
                        uint32_t ring_bytes, uint32_t watermark)
{
    uint32_t i, lo, hi;
    if (acp_validate_pages(pages, npages, ring_bytes) != ACP_OK)
        return ACP_ERR_ARG;
    if (watermark == 0 || watermark > ring_bytes)
        return ACP_ERR_ARG;

    WR(io, ACP_REG_ATU_BASE_ADDR_GRP_1, acp_atu_grp1_base());
    WR(io, ACP_REG_ATU_PAGE_SIZE_GRP_1, ACP_ATU_PAGE_SIZE_4K);
    for (i = 0; i < npages; i++) {
        uint32_t off = ACP_PDM_PTE_OFFSET + i * 8;
        acp_pte(pages[i], &lo, &hi);
        WR(io, ACP_REG_SCRATCH_REG_0 + off, lo);
        WR(io, ACP_REG_SCRATCH_REG_0 + off + 4, hi);
    }
    WR(io, ACP_REG_WOV_RX_RINGBUFADDR, ACP_PDM_MEM_WINDOW_START);
    WR(io, ACP_REG_WOV_RX_RINGBUFSIZE, ring_bytes);
    WR(io, ACP_REG_WOV_RX_INTR_WATERMARK, watermark);
    WR(io, ACP_REG_ATU_CTRL, 1);
    return ACP_OK;
}

int acp_hw_pdm_running(const acp_io_t *io)
{
    uint32_t en  = RD(io, ACP_REG_WOV_PDM_ENABLE);
    uint32_t dma = RD(io, ACP_REG_WOV_PDM_DMA_ENABLE);
    if (acp_mmio_dead(en) || acp_mmio_dead(dma))
        return 0;
    return (en & ACP_PDM_ENABLE_ON) && (dma & ACP_PDM_DMA_EN_STATUS);
}

int acp_hw_pdm_start(const acp_io_t *io, int gain)
{
    uint32_t i, v;

    WR(io, ACP_REG_WOV_PDM_NO_OF_CHANNELS, ACP_PDM_CH_MASK_2CH);
    WR(io, ACP_REG_WOV_PDM_DECIMATION_FACTOR, ACP_PDM_DECIMATION);
    if (acp_hw_pdm_running(io))
        return ACP_OK;

    WR(io, ACP_REG_WOV_CLK_CTRL, ACP_PDM_CLK_FREQ);
    v = RD(io, ACP_REG_WOV_MISC_CTRL);
    if (acp_mmio_dead(v))
        return ACP_ERR_DEAD;
    WR(io, ACP_REG_WOV_MISC_CTRL, acp_misc_ctrl_with_gain(v, gain));
    WR(io, ACP_REG_WOV_PDM_ENABLE, ACP_PDM_ENABLE_ON);
    WR(io, ACP_REG_WOV_PDM_DMA_ENABLE, ACP_PDM_DMA_EN);
    for (i = 0; i < ACP_DMA_POLL_COUNT; i++) {
        v = RD(io, ACP_REG_WOV_PDM_DMA_ENABLE);
        if ((v & ACP_PDM_DMA_EN_STATUS) == ACP_PDM_DMA_EN_STATUS && !acp_mmio_dead(v))
            return ACP_OK;
        DELAY(io, ACP_DMA_POLL_DELAY_US);
    }
    return ACP_ERR_TIMEOUT;
}

int acp_hw_pdm_stop(const acp_io_t *io)
{
    uint32_t en  = RD(io, ACP_REG_WOV_PDM_ENABLE);
    uint32_t dma = RD(io, ACP_REG_WOV_PDM_DMA_ENABLE);
    uint32_t i;

    if (acp_mmio_dead(en) || acp_mmio_dead(dma))
        return ACP_ERR_DEAD;
    if (dma & ACP_PDM_DMA_EN) {
        WR(io, ACP_REG_WOV_PDM_DMA_ENABLE, ACP_PDM_DMA_STOP_REQ);
        for (i = 0; i < ACP_DMA_POLL_COUNT; i++) {
            dma = RD(io, ACP_REG_WOV_PDM_DMA_ENABLE);
            if ((dma & ACP_PDM_DMA_EN_STATUS) == 0)
                break;
            DELAY(io, ACP_DMA_POLL_DELAY_US);
        }
        if (i == ACP_DMA_POLL_COUNT)
            return ACP_ERR_TIMEOUT;
    }
    if (en == ACP_PDM_ENABLE_ON)
        WR(io, ACP_REG_WOV_PDM_ENABLE, 0);
    WR(io, ACP_REG_WOV_PDM_FIFO_FLUSH, 1);
    return ACP_OK;
}

uint64_t acp_hw_read_counter(const acp_io_t *io)
{
    uint32_t hi1 = RD(io, ACP_REG_WOV_RX_LINEARPOS_HIGH);
    uint32_t lo  = RD(io, ACP_REG_WOV_RX_LINEARPOS_LOW);
    uint32_t hi2 = RD(io, ACP_REG_WOV_RX_LINEARPOS_HIGH);
    uint32_t lo2 = (hi1 == hi2) ? lo : RD(io, ACP_REG_WOV_RX_LINEARPOS_LOW);
    return acp_counter_combine(hi1, lo, hi2, lo2);
}

void acp_hw_ack_pdm_status(const acp_io_t *io)
{
    uint32_t st = RD(io, ACP_REG_EXTERNAL_INTR_STAT);
    if (!acp_mmio_dead(st) && (st & ACP_PDM_DMA_INTR_BIT))
        WR(io, ACP_REG_EXTERNAL_INTR_STAT, ACP_PDM_DMA_INTR_BIT);
}

const acp_named_reg_t acp_dump_regs[] = {
    { "PGFSM_STATUS",       ACP_REG_PGFSM_STATUS },
    { "PGFSM_CONTROL",      ACP_REG_PGFSM_CONTROL },
    { "SOFT_RESET",         ACP_REG_SOFT_RESET },
    { "CONTROL",            ACP_REG_CONTROL },
    { "CLKMUX_SEL",         ACP_REG_CLKMUX_SEL },
    { "PIN_CONFIG",         ACP_REG_PIN_CONFIG },
    { "EXT_INTR_ENB",       ACP_REG_EXTERNAL_INTR_ENB },
    { "EXT_INTR_CNTL",      ACP_REG_EXTERNAL_INTR_CNTL },
    { "EXT_INTR_STAT",      ACP_REG_EXTERNAL_INTR_STAT },
    { "WOV_PDM_ENABLE",     ACP_REG_WOV_PDM_ENABLE },
    { "WOV_PDM_DMA_ENABLE", ACP_REG_WOV_PDM_DMA_ENABLE },
    { "WOV_RINGBUFADDR",    ACP_REG_WOV_RX_RINGBUFADDR },
    { "WOV_RINGBUFSIZE",    ACP_REG_WOV_RX_RINGBUFSIZE },
    { "WOV_LINEARPOS_HI",   ACP_REG_WOV_RX_LINEARPOS_HIGH },
    { "WOV_LINEARPOS_LO",   ACP_REG_WOV_RX_LINEARPOS_LOW },
    { "WOV_MISC_CTRL",      ACP_REG_WOV_MISC_CTRL },
    { "WOV_CLK_CTRL",       ACP_REG_WOV_CLK_CTRL },
    { "WOV_ERROR_STATUS",   ACP_REG_WOV_ERROR_STATUS },
    { "ATU_CTRL",           ACP_REG_ATU_CTRL },
};
const uint32_t acp_dump_regs_count = sizeof(acp_dump_regs) / sizeof(acp_dump_regs[0]);
