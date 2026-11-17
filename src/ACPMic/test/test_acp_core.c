/*
 * Host tests for ACPMic's core. Builds against src/acp_core.c unchanged and
 * drives it with a simulated ACP register file.
 *
 *   ./test/run_tests.sh          quick run
 *   ./test/run_tests.sh --full   also checks all 2^32 int->float conversions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/acp_core.h"

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    if (_a == _b) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* ---------------- simulated ACP ---------------- */

#define SIM_WORDS (ACP_BAR0_MIN_LEN / 4)
#define MAX_TRACE 4096

typedef struct { uint32_t off, val; } wr_t;

typedef struct sim {
    uint32_t reg[SIM_WORDS];
    int dead;                 /* all reads 0xFFFFFFFF */
    int pg_stuck, reset_stuck, dma_stuck;
    int pg_count, reset_count, dma_count;
    uint64_t counter;
    int counter_carry_once;   /* bump high word between the two HIGH reads */
    int hi_reads;
    wr_t trace[MAX_TRACE];
    int ntrace;
    uint64_t delayed_us;
    int bad_access;
} sim_t;

static uint32_t sim_rd(void *c, uint32_t off)
{
    sim_t *s = c;
    if (off >= ACP_BAR0_MIN_LEN || (off & 3)) { s->bad_access++; return 0xFFFFFFFFu; }
    if (s->dead) return 0xFFFFFFFFu;

    if (off == ACP_OFF(ACP_REG_PGFSM_STATUS) && s->pg_count > 0 && !s->pg_stuck) {
        if (--s->pg_count == 0) s->reg[off / 4] = 0;
    }
    if (off == ACP_OFF(ACP_REG_SOFT_RESET) && s->reset_stuck)
        return 1;                              /* reset never completes or clears */
    if (off == ACP_OFF(ACP_REG_SOFT_RESET) && s->reset_count > 0) {
        if (--s->reset_count == 0)
            s->reg[off / 4] = s->reg[off / 4] ? ACP_SOFT_RESET_AUDDONE_MASK : 0;
    }
    if (off == ACP_OFF(ACP_REG_WOV_PDM_DMA_ENABLE) && s->dma_count > 0 && !s->dma_stuck) {
        if (--s->dma_count == 0) {
            uint32_t v = s->reg[off / 4];
            s->reg[off / 4] = (v == ACP_PDM_DMA_EN) ? (ACP_PDM_DMA_EN | ACP_PDM_DMA_EN_STATUS) : 0;
        }
    }
    if (off == ACP_OFF(ACP_REG_WOV_RX_LINEARPOS_HIGH)) {
        s->hi_reads++;
        if (s->counter_carry_once && s->hi_reads == 2) {
            s->counter = (s->counter | 0xFFFFFFFFull) + 1 + 0x40; /* low wrapped */
            s->counter_carry_once = 0;
        }
        return (uint32_t)(s->counter >> 32);
    }
    if (off == ACP_OFF(ACP_REG_WOV_RX_LINEARPOS_LOW))
        return (uint32_t)s->counter;
    return s->reg[off / 4];
}

static void sim_wr(void *c, uint32_t off, uint32_t v)
{
    sim_t *s = c;
    if (off >= ACP_BAR0_MIN_LEN || (off & 3)) { s->bad_access++; return; }
    if (s->ntrace < MAX_TRACE) { s->trace[s->ntrace].off = off; s->trace[s->ntrace].val = v; s->ntrace++; }
    if (s->dead) return;
    if (off == ACP_OFF(ACP_REG_EXTERNAL_INTR_STAT)) { s->reg[off / 4] &= ~v; return; }
    if (off == ACP_OFF(ACP_REG_PGFSM_CONTROL) && v == ACP_PGFSM_CNTL_POWER_ON) {
        s->reg[ACP_OFF(ACP_REG_PGFSM_STATUS) / 4] = ACP_PGFSM_POWER_ON_IN_PROGRESS;
        s->pg_count = 3;
    }
    if (off == ACP_OFF(ACP_REG_SOFT_RESET)) { s->reg[off / 4] = v; s->reset_count = 2; return; }
    if (off == ACP_OFF(ACP_REG_WOV_PDM_DMA_ENABLE)) {
        if (v == ACP_PDM_DMA_STOP_REQ)
            s->reg[off / 4] = ACP_PDM_DMA_EN_STATUS;   /* still running until countdown */
        else
            s->reg[off / 4] = v;
        s->dma_count = 2;
        return;
    }
    s->reg[off / 4] = v;
}

static void sim_delay(void *c, uint32_t us) { ((sim_t *)c)->delayed_us += us; }

static sim_t *sim_new(void)
{
    sim_t *s = calloc(1, sizeof(*s));
    s->reg[ACP_OFF(ACP_REG_PGFSM_STATUS) / 4] = ACP_PGFSM_POWERED_OFF;
    s->reg[ACP_OFF(ACP_REG_PIN_CONFIG) / 4] = 6;
    s->reg[ACP_OFF(ACP_REG_WOV_MISC_CTRL) / 4] = 0xA5A50000u | 0x7;
    return s;
}

static acp_io_t io_for(sim_t *s)
{
    acp_io_t io = { s, sim_rd, sim_wr, sim_delay };
    return io;
}

/* Registers ACPMic is allowed to write. Anything else is a bug. */
static int write_allowed(uint32_t off, uint32_t val)
{
    static const uint32_t ok[] = {
        ACP_REG_ATU_PAGE_SIZE_GRP_1, ACP_REG_ATU_BASE_ADDR_GRP_1, ACP_REG_ATU_CTRL,
        ACP_REG_SOFT_RESET, ACP_REG_CONTROL, ACP_REG_PGFSM_CONTROL, ACP_REG_CLKMUX_SEL,
        ACP_REG_EXTERNAL_INTR_STAT, ACP_REG_EXTERNAL_INTR_CNTL, ACP_REG_EXTERNAL_INTR_ENB,
        ACP_REG_WOV_PDM_ENABLE, ACP_REG_WOV_PDM_DMA_ENABLE, ACP_REG_WOV_RX_RINGBUFADDR,
        ACP_REG_WOV_RX_RINGBUFSIZE, ACP_REG_WOV_RX_INTR_WATERMARK, ACP_REG_WOV_PDM_FIFO_FLUSH,
        ACP_REG_WOV_PDM_NO_OF_CHANNELS, ACP_REG_WOV_PDM_DECIMATION_FACTOR,
        ACP_REG_WOV_MISC_CTRL, ACP_REG_WOV_CLK_CTRL,
    };
    size_t i;
    /* Interrupts must never be turned on: ACPMic has no interrupt handler. */
    if (off == ACP_OFF(ACP_REG_EXTERNAL_INTR_ENB) && val != 0) return 0;
    if (off == ACP_OFF(ACP_REG_EXTERNAL_INTR_CNTL) && val != 0) return 0;
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (off == ACP_OFF(ok[i])) return 1;
    /* PTE area: 8 pages x 2 words. */
    if (off >= ACP_OFF(ACP_REG_SCRATCH_REG_0) && off < ACP_OFF(ACP_REG_SCRATCH_REG_0) + 64 * 8) return 1;
    return 0;
}

static void check_trace_safe(sim_t *s, const char *what)
{
    int i, bad = 0;
    for (i = 0; i < s->ntrace; i++)
        if (!write_allowed(s->trace[i].off, s->trace[i].val)) {
            fprintf(stderr, "  [%s] unexpected write off=0x%x val=0x%x\n", what, s->trace[i].off, s->trace[i].val);
            bad++;
        }
    CHECK(bad == 0);
    CHECK(s->bad_access == 0);
}

static void expect_trace(sim_t *s, const wr_t *exp, int n, const char *what)
{
    int i, ok = (s->ntrace == n);
    for (i = 0; ok && i < n; i++)
        ok = s->trace[i].off == exp[i].off && s->trace[i].val == exp[i].val;
    if (!ok) {
        fprintf(stderr, "  [%s] trace mismatch, got %d writes:\n", what, s->ntrace);
        for (i = 0; i < s->ntrace; i++) fprintf(stderr, "    0x%05x <- 0x%08x\n", s->trace[i].off, s->trace[i].val);
    }
    CHECK(ok);
}

#define W(reg, v) { ACP_OFF(reg), (uint32_t)(v) }

/* ---------------- tests ---------------- */

static void test_layout(void)
{
    CHECK_EQ(ACP_OFF(ACP_REG_PIN_CONFIG), 0x1440);
    CHECK_EQ(ACP_OFF(ACP_REG_PGFSM_STATUS), 0x1028);
    CHECK_EQ(ACP_OFF(ACP_REG_WOV_RX_LINEARPOS_LOW), 0x2C1C);
    CHECK_EQ(ACP_OFF(ACP_REG_SCRATCH_REG_0), 0x10000);
    CHECK_EQ(ACP_OFF(ACP_REG_ATU_CTRL), 0x0C40);
    CHECK(ACP_OFF(ACP_REG_SCRATCH_REG_0) + 64 * 8 <= ACP_BAR0_MIN_LEN);
    CHECK(ACP_BAR0_MIN_LEN <= 0x40000);   /* BAR0 on this machine is 256 KiB */
    CHECK_EQ(ACP_RING_BYTES, 32768);
    CHECK_EQ(ACP_RING_FRAMES, 4096);
    CHECK_EQ(ACP_RING_PAGES, 8);
    CHECK_EQ(ACP_FRAME_BYTES, 8);
    for (uint32_t i = 0; i < acp_dump_regs_count; i++) {
        CHECK(ACP_OFF(acp_dump_regs[i].addr) < ACP_BAR0_MIN_LEN);
        CHECK((ACP_OFF(acp_dump_regs[i].addr) & 3) == 0);
    }
}

static void test_decisions(void)
{
    CHECK(acp_revision_supported(0x60));
    CHECK(acp_revision_supported(0x6F));
    CHECK(!acp_revision_supported(0x62));
    CHECK(!acp_revision_supported(0x00));
    CHECK(!acp_revision_supported(0x70));

    static const int pdm[16] = {0,0,0,0,1,1,1,1,1,0,1,1,1,1,1,0};
    for (uint32_t v = 0; v < 16; v++) CHECK_EQ(acp_pin_config_is_pdm(v), pdm[v]);
    CHECK(!acp_pin_config_is_pdm(16));
    CHECK(!acp_pin_config_is_pdm(0xFFFFFFFFu));

    CHECK(acp_mmio_dead(0xFFFFFFFFu));
    CHECK(!acp_mmio_dead(0));

    CHECK(!acp_pgfsm_needs_kick(0));
    CHECK(acp_pgfsm_needs_kick(2));
    CHECK(acp_pgfsm_needs_kick(3));
    CHECK(!acp_pgfsm_needs_kick(1));

    CHECK_EQ(acp_misc_ctrl_with_gain(0, 3), 0x18);
    CHECK_EQ(acp_misc_ctrl_with_gain(0xFFFFFFFFu, 0), 0xFFFFFFE7u);
    CHECK_EQ(acp_misc_ctrl_with_gain(0x1, 9), 0x19);
    CHECK_EQ(acp_misc_ctrl_with_gain(0x18, -4), 0x0);
    CHECK_EQ(acp_misc_ctrl_with_gain(0x0, 1), 0x8);

    CHECK_EQ(acp_atu_grp1_base(), 0x83800000u);

    uint32_t lo, hi;
    acp_pte(0x12345000ull, &lo, &hi);
    CHECK_EQ(lo, 0x12345000u); CHECK_EQ(hi, 0x80000000u);
    acp_pte(0x1ABCDE000ull, &lo, &hi);
    CHECK_EQ(lo, 0xABCDE000u); CHECK_EQ(hi, 0x80000001u);

    uint64_t pages[8];
    for (int i = 0; i < 8; i++) pages[i] = 0x7F000000ull + i * 0x1000;
    CHECK_EQ(acp_validate_pages(pages, 8, ACP_RING_BYTES), ACP_OK);
    CHECK_EQ(acp_validate_pages(pages, 7, ACP_RING_BYTES), ACP_ERR_ARG);
    CHECK_EQ(acp_validate_pages(NULL, 8, ACP_RING_BYTES), ACP_ERR_ARG);
    CHECK_EQ(acp_validate_pages(pages, 8, ACP_RING_BYTES + 1), ACP_ERR_ARG);
    pages[3] += 0x10;
    CHECK_EQ(acp_validate_pages(pages, 8, ACP_RING_BYTES), ACP_ERR_ARG);
    pages[3] = 0x100000000ull;
    CHECK_EQ(acp_validate_pages(pages, 8, ACP_RING_BYTES), ACP_ERR_ARG);
    pages[3] = 0xFFFFF000ull;
    CHECK_EQ(acp_validate_pages(pages, 8, ACP_RING_BYTES), ACP_OK);
    pages[3] = 0;
    CHECK_EQ(acp_validate_pages(pages, 8, ACP_RING_BYTES), ACP_ERR_ARG);

    CHECK_EQ(acp_counter_combine(1, 0x10, 1, 0x99), 0x100000010ull);
    CHECK_EQ(acp_counter_combine(1, 0xFFFFFFF0u, 2, 0x8), 0x200000008ull);
}

static uint32_t f2bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void test_convert(int full)
{
    static const int32_t edge[] = { 0, 1, -1, 2, 3, 0x7FFFFFFF, (int32_t)0x80000000, 0x00FFFFFF,
        0x01000000, 0x01000001, 0x01000002, 0x01000003, 0x7FFFFFC0, 0x7FFFFFBF, 0x7FFFFF80,
        -0x7FFFFFC0, 0x40000000, -0x40000000, 12345678, -87654321, 0x00800000, 0x00800001 };
    int mism = 0;
    for (size_t i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        float ref = (float)edge[i] / 2147483648.0f;
        if (acp_s32_to_f32_bits(edge[i]) != f2bits(ref)) {
            mism++;
            fprintf(stderr, "  convert %d: got 0x%08x want 0x%08x\n", edge[i], acp_s32_to_f32_bits(edge[i]), f2bits(ref));
        }
    }
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int32_t s = (int32_t)(uint32_t)x;
        if (i & 1) s >>= (int)(x >> 59);           /* also small magnitudes */
        float ref = (float)s / 2147483648.0f;
        if (acp_s32_to_f32_bits(s) != f2bits(ref)) { if (mism++ < 5) fprintf(stderr, "  convert %d mismatch\n", s); }
    }
    if (full) {
        uint32_t u = 0;
        do {
            int32_t s = (int32_t)u;
            float ref = (float)s / 2147483648.0f;
            if (acp_s32_to_f32_bits(s) != f2bits(ref)) { if (mism++ < 5) fprintf(stderr, "  convert %d mismatch\n", s); }
        } while (++u != 0);
        printf("  exhaustive 2^32 conversion check done\n");
    }
    CHECK_EQ(mism, 0);

    CHECK_EQ(acp_boost(1000, 0), 1000);
    CHECK_EQ(acp_boost(1000, 2), 4000);
    CHECK_EQ(acp_boost(1000, 9), 16000);           /* clamped to x16 */
    CHECK_EQ(acp_boost(0x10000000, 4), 0x7FFFFFFF);
    CHECK_EQ((uint32_t)acp_boost(-0x10000000, 4), 0x80000000u);
    CHECK_EQ(acp_boost(-5, -1), -5);

    int32_t src[4] = { 0, 0x40000000, -0x40000000, 0x08000000 };
    uint32_t dst[4];
    acp_convert_block(src, dst, 4, 1);
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[1], f2bits(1.0f - 1.0f / 2147483648.0f));  /* saturated 0x7FFFFFFF */
    CHECK_EQ(dst[2], f2bits(-1.0f));
    CHECK_EQ(dst[3], f2bits(0.125f));
}

static void test_position(void)
{
    acp_pos_t p;
    acp_pos_update_t u;
    const uint64_t base = 0xFFFFFF00ull;   /* start near a 32-bit carry */

    acp_pos_start(&p, base, ACP_RING_BYTES, ACP_FRAME_BYTES);
    acp_pos_update(&p, base, &u);
    CHECK(u.valid); CHECK_EQ(u.ring_frame, 0); CHECK_EQ(u.new_wraps, 0);

    acp_pos_update(&p, base + 800, &u);
    CHECK(u.valid); CHECK_EQ(u.ring_frame, 100); CHECK_EQ(u.new_wraps, 0);

    acp_pos_update(&p, base + ACP_RING_BYTES + 80, &u);
    CHECK_EQ(u.new_wraps, 1); CHECK_EQ(u.ring_frame, 10); CHECK_EQ(u.frames_past_wrap, 10);
    CHECK_EQ(acp_wrap_age_frames(&u, 0, ACP_RING_FRAMES), 10);

    acp_pos_update(&p, base + ACP_RING_BYTES + 160, &u);
    CHECK_EQ(u.new_wraps, 0); CHECK_EQ(u.ring_frame, 20);

    /* timer stalled for three rings */
    acp_pos_update(&p, base + 4ull * ACP_RING_BYTES + 8, &u);
    CHECK_EQ(u.new_wraps, 3); CHECK_EQ(u.ring_frame, 1);
    CHECK_EQ(acp_wrap_age_frames(&u, 0, ACP_RING_FRAMES), 1 + 2 * 4096);
    CHECK_EQ(acp_wrap_age_frames(&u, 1, ACP_RING_FRAMES), 1 + 4096);
    CHECK_EQ(acp_wrap_age_frames(&u, 2, ACP_RING_FRAMES), 1);
    CHECK_EQ(p.wraps, 4);

    /* exactly on a boundary */
    acp_pos_update(&p, base + 5ull * ACP_RING_BYTES, &u);
    CHECK_EQ(u.new_wraps, 1); CHECK_EQ(u.ring_frame, 0); CHECK_EQ(u.frames_past_wrap, 0);

    /* backwards = invalid, state untouched */
    acp_pos_update(&p, base + 10, &u);
    CHECK(!u.valid); CHECK_EQ(u.new_wraps, 0); CHECK_EQ(p.wraps, 5);

    acp_pos_t z = {0};
    acp_pos_update(&z, 100, &u);
    CHECK(!u.valid);

    CHECK_EQ(acp_frames_to_ns(48000, 48000), 1000000000ull);
    CHECK_EQ(acp_frames_to_ns(1, 48000), 20833);
    CHECK_EQ(acp_frames_to_ns(4096, 48000), 85333333);
    CHECK_EQ(acp_frames_to_ns(5, 0), 0);

    CHECK(acp_dma_stalled(100, 100, 200000001ull, 200000000ull));
    CHECK(!acp_dma_stalled(100, 100, 200000000ull, 200000000ull));
    CHECK(!acp_dma_stalled(108, 100, 900000000ull, 200000000ull));

    CHECK_EQ(acp_synth_counter(1000, 1000000000ull, 48000, 8), 1000 + 48000 * 8);
    CHECK_EQ(acp_synth_counter(0, 20833, 48000, 8), 0);          /* < 1 frame */
    CHECK_EQ(acp_synth_counter(0, 20834, 48000, 8), 8);
    /* 10 years of uptime still exact, no overflow */
    uint64_t ten_years = 10ull * 365 * 24 * 3600 * 1000000000ull;
    CHECK_EQ(acp_synth_counter(0, ten_years, 48000, 8), 10ull * 365 * 24 * 3600 * 48000 * 8);
    /* synthetic counter fed into the position tracker produces wraps */
    acp_pos_t q;
    acp_pos_start(&q, 0, ACP_RING_BYTES, ACP_FRAME_BYTES);
    acp_pos_update(&q, acp_synth_counter(0, 1000000000ull, 48000, 8), &u);
    CHECK(u.valid); CHECK_EQ(u.new_wraps, 11); CHECK_EQ(u.ring_frame, 48000 - 11 * 4096);
}

static void test_init_from_off(void)
{
    sim_t *s = sim_new();
    acp_io_t io = io_for(s);
    CHECK_EQ(acp_hw_init(&io), ACP_OK);
    const wr_t exp[] = {
        W(ACP_REG_PGFSM_CONTROL, 1),
        W(ACP_REG_CONTROL, 1),
        W(ACP_REG_SOFT_RESET, 1),
        W(ACP_REG_SOFT_RESET, 0),
        W(ACP_REG_CLKMUX_SEL, 3),
        W(ACP_REG_EXTERNAL_INTR_STAT, 0xFFFFFFFFu),
        W(ACP_REG_EXTERNAL_INTR_CNTL, 0),
        W(ACP_REG_EXTERNAL_INTR_ENB, 0),
    };
    expect_trace(s, exp, (int)(sizeof(exp) / sizeof(exp[0])), "init from off");
    CHECK(s->delayed_us < 2000);
    check_trace_safe(s, "init");
    free(s);
}

static void test_init_noreset(void)
{
    /* already on + reset_if_on 0: no SOFT_RESET write at all */
    sim_t *s = sim_new();
    s->reg[ACP_OFF(ACP_REG_PGFSM_STATUS) / 4] = 0;
    acp_io_t io = io_for(s);
    CHECK_EQ(acp_hw_init_ex(&io, 0), ACP_OK);
    for (int i = 0; i < s->ntrace; i++) CHECK(s->trace[i].off != ACP_OFF(ACP_REG_SOFT_RESET));
    CHECK(s->ntrace > 0 && s->trace[0].off == ACP_OFF(ACP_REG_CONTROL));
    free(s);
    /* from off, reset_if_on 0 still resets (a block we powered up is ours) */
    s = sim_new();
    io = io_for(s);
    CHECK_EQ(acp_hw_init_ex(&io, 0), ACP_OK);
    {
        int saw = 0;
        for (int i = 0; i < s->ntrace; i++) saw |= s->trace[i].off == ACP_OFF(ACP_REG_SOFT_RESET);
        CHECK(saw);
    }
    free(s);
    /* already on + reset_if_on 1: the old sequence, reset included */
    s = sim_new();
    s->reg[ACP_OFF(ACP_REG_PGFSM_STATUS) / 4] = 0;
    io = io_for(s);
    CHECK_EQ(acp_hw_init_ex(&io, 1), ACP_OK);
    {
        int saw = 0;
        for (int i = 0; i < s->ntrace; i++) saw |= s->trace[i].off == ACP_OFF(ACP_REG_SOFT_RESET);
        CHECK(saw);
    }
    free(s);
}

static void test_init_already_on(void)
{
    sim_t *s = sim_new();
    s->reg[ACP_OFF(ACP_REG_PGFSM_STATUS) / 4] = 0;
    acp_io_t io = io_for(s);
    CHECK_EQ(acp_hw_init(&io), ACP_OK);
    CHECK(s->ntrace > 0 && s->trace[0].off == ACP_OFF(ACP_REG_CONTROL));
    for (int i = 0; i < s->ntrace; i++) CHECK(s->trace[i].off != ACP_OFF(ACP_REG_PGFSM_CONTROL));
    free(s);

    /* power-on already in progress: wait, but do not write PGFSM_CONTROL */
    s = sim_new();
    s->reg[ACP_OFF(ACP_REG_PGFSM_STATUS) / 4] = ACP_PGFSM_POWER_ON_IN_PROGRESS;
    s->pg_count = 4;
    io = io_for(s);
    CHECK_EQ(acp_hw_power_on(&io), ACP_OK);
    CHECK_EQ(s->ntrace, 0);
    free(s);
}

static void test_init_failures(void)
{
    sim_t *s = sim_new();
    s->dead = 1;
    acp_io_t io = io_for(s);
    CHECK_EQ(acp_hw_init(&io), ACP_ERR_DEAD);
    CHECK_EQ(s->ntrace, 0);                          /* nothing written */
    CHECK_EQ(acp_hw_deinit(&io), ACP_ERR_DEAD);
    CHECK_EQ(s->ntrace, 0);
    CHECK_EQ(acp_hw_pdm_stop(&io), ACP_ERR_DEAD);
    CHECK_EQ(s->ntrace, 0);
    CHECK(!acp_hw_pdm_running(&io));
    free(s);

    s = sim_new();
    s->pg_stuck = 1;
    io = io_for(s);
    CHECK_EQ(acp_hw_init(&io), ACP_ERR_TIMEOUT);
    CHECK_EQ(s->ntrace, 1);                          /* only the power-on request */
    CHECK(s->delayed_us <= ACP_PGFSM_POLL_US);
    free(s);

    s = sim_new();
    s->reset_stuck = 1;
    io = io_for(s);
    CHECK_EQ(acp_hw_init(&io), ACP_ERR_TIMEOUT);
    CHECK(s->delayed_us <= 2 * ACP_RESET_POLL_COUNT + ACP_PGFSM_POLL_US);
    check_trace_safe(s, "reset stuck");
    free(s);
}

static void test_ring(void)
{
    sim_t *s = sim_new();
    acp_io_t io = io_for(s);
    uint64_t pages[ACP_RING_PAGES];
    for (unsigned i = 0; i < ACP_RING_PAGES; i++) pages[i] = 0x3A000000ull + i * 0x1000;

    CHECK_EQ(acp_hw_program_ring(&io, pages, ACP_RING_PAGES, ACP_RING_BYTES, ACP_PERIOD_BYTES), ACP_OK);
    CHECK_EQ(s->ntrace, 2 + 2 * ACP_RING_PAGES + 4);
    CHECK_EQ(s->trace[0].off, ACP_OFF(ACP_REG_ATU_BASE_ADDR_GRP_1));
    CHECK_EQ(s->trace[0].val, 0x83800000u);
    CHECK_EQ(s->trace[1].off, ACP_OFF(ACP_REG_ATU_PAGE_SIZE_GRP_1));
    CHECK_EQ(s->trace[1].val, 2);
    for (unsigned i = 0; i < ACP_RING_PAGES; i++) {
        CHECK_EQ(s->trace[2 + 2 * i].off, 0x10000 + 8 * i);
        CHECK_EQ(s->trace[2 + 2 * i].val, 0x3A000000u + i * 0x1000);
        CHECK_EQ(s->trace[3 + 2 * i].off, 0x10004 + 8 * i);
        CHECK_EQ(s->trace[3 + 2 * i].val, 0x80000000u);
    }
    int k = 2 + 2 * ACP_RING_PAGES;
    CHECK_EQ(s->trace[k].off, ACP_OFF(ACP_REG_WOV_RX_RINGBUFADDR));     CHECK_EQ(s->trace[k].val, 0x04000000u);
    CHECK_EQ(s->trace[k + 1].off, ACP_OFF(ACP_REG_WOV_RX_RINGBUFSIZE)); CHECK_EQ(s->trace[k + 1].val, 32768);
    CHECK_EQ(s->trace[k + 2].off, ACP_OFF(ACP_REG_WOV_RX_INTR_WATERMARK)); CHECK_EQ(s->trace[k + 2].val, 8192);
    CHECK_EQ(s->trace[k + 3].off, ACP_OFF(ACP_REG_ATU_CTRL));           CHECK_EQ(s->trace[k + 3].val, 1);
    check_trace_safe(s, "ring");

    s->ntrace = 0;
    pages[5] = 0x123456789000ull;   /* above 4 GiB */
    CHECK_EQ(acp_hw_program_ring(&io, pages, ACP_RING_PAGES, ACP_RING_BYTES, ACP_PERIOD_BYTES), ACP_ERR_ARG);
    CHECK_EQ(s->ntrace, 0);
    pages[5] = 0x3A005000ull;
    CHECK_EQ(acp_hw_program_ring(&io, pages, ACP_RING_PAGES, ACP_RING_BYTES, 0), ACP_ERR_ARG);
    CHECK_EQ(acp_hw_program_ring(&io, pages, ACP_RING_PAGES, ACP_RING_BYTES, ACP_RING_BYTES + 4), ACP_ERR_ARG);
    CHECK_EQ(s->ntrace, 0);
    free(s);
}

static void test_pdm_start_stop(void)
{
    sim_t *s = sim_new();
    acp_io_t io = io_for(s);
    CHECK(!acp_hw_pdm_running(&io));
    CHECK_EQ(acp_hw_pdm_start(&io, 3), ACP_OK);
    const wr_t exp[] = {
        W(ACP_REG_WOV_PDM_NO_OF_CHANNELS, 0),
        W(ACP_REG_WOV_PDM_DECIMATION_FACTOR, 2),
        W(ACP_REG_WOV_CLK_CTRL, 7),
        W(ACP_REG_WOV_MISC_CTRL, 0xA5A50000u | 0x7 | 0x18),
        W(ACP_REG_WOV_PDM_ENABLE, 1),
        W(ACP_REG_WOV_PDM_DMA_ENABLE, 1),
    };
    expect_trace(s, exp, 6, "pdm start");
    CHECK(acp_hw_pdm_running(&io));

    /* start while running: only channel/decimation rewritten */
    s->ntrace = 0;
    CHECK_EQ(acp_hw_pdm_start(&io, 3), ACP_OK);
    CHECK_EQ(s->ntrace, 2);

    s->ntrace = 0;
    CHECK_EQ(acp_hw_pdm_stop(&io), ACP_OK);
    const wr_t exp2[] = {
        W(ACP_REG_WOV_PDM_DMA_ENABLE, 2),
        W(ACP_REG_WOV_PDM_ENABLE, 0),
        W(ACP_REG_WOV_PDM_FIFO_FLUSH, 1),
    };
    expect_trace(s, exp2, 3, "pdm stop");
    CHECK(!acp_hw_pdm_running(&io));

    /* stop when idle: no DMA write, flush only */
    s->ntrace = 0;
    CHECK_EQ(acp_hw_pdm_stop(&io), ACP_OK);
    CHECK_EQ(s->ntrace, 1);
    CHECK_EQ(s->trace[0].off, ACP_OFF(ACP_REG_WOV_PDM_FIFO_FLUSH));
    check_trace_safe(s, "pdm");
    free(s);

    s = sim_new();
    s->dma_stuck = 1;
    io = io_for(s);
    CHECK_EQ(acp_hw_pdm_start(&io, 3), ACP_ERR_TIMEOUT);
    CHECK(s->delayed_us <= (uint64_t)ACP_DMA_POLL_COUNT * ACP_DMA_POLL_DELAY_US);   /* <= 100 ms */
    s->reg[ACP_OFF(ACP_REG_WOV_PDM_DMA_ENABLE) / 4] = 3;
    s->delayed_us = 0;
    CHECK_EQ(acp_hw_pdm_stop(&io), ACP_ERR_TIMEOUT);
    CHECK(s->delayed_us <= (uint64_t)ACP_DMA_POLL_COUNT * ACP_DMA_POLL_DELAY_US);
    free(s);
}

static void test_counter_and_ack(void)
{
    sim_t *s = sim_new();
    acp_io_t io = io_for(s);
    s->counter = 0x5000001234ull;
    CHECK_EQ(acp_hw_read_counter(&io), 0x5000001234ull);
    s->counter = 0x00000002FFFFFFF8ull;
    s->counter_carry_once = 1;
    s->hi_reads = 0;
    CHECK_EQ(acp_hw_read_counter(&io), 0x0000000300000040ull);

    s->reg[ACP_OFF(ACP_REG_EXTERNAL_INTR_STAT) / 4] = ACP_PDM_DMA_INTR_BIT | 0x1;
    s->ntrace = 0;
    acp_hw_ack_pdm_status(&io);
    CHECK_EQ(s->ntrace, 1);
    CHECK_EQ(s->trace[0].val, ACP_PDM_DMA_INTR_BIT);
    CHECK_EQ(s->reg[ACP_OFF(ACP_REG_EXTERNAL_INTR_STAT) / 4], 0x1);
    s->ntrace = 0;
    acp_hw_ack_pdm_status(&io);
    CHECK_EQ(s->ntrace, 0);
    free(s);
}

static void test_full_session(void)
{
    sim_t *s = sim_new();
    acp_io_t io = io_for(s);
    uint64_t pages[ACP_RING_PAGES];
    for (unsigned i = 0; i < ACP_RING_PAGES; i++) pages[i] = 0x10000000ull + i * 0x1000;

    CHECK_EQ(acp_hw_init(&io), ACP_OK);
    CHECK(acp_pin_config_is_pdm(io.rd(io.ctx, ACP_OFF(ACP_REG_PIN_CONFIG))));
    CHECK_EQ(acp_hw_program_ring(&io, pages, ACP_RING_PAGES, ACP_RING_BYTES, ACP_PERIOD_BYTES), ACP_OK);
    s->counter = 777;
    acp_pos_t p;
    acp_pos_start(&p, acp_hw_read_counter(&io), ACP_RING_BYTES, ACP_FRAME_BYTES);
    CHECK_EQ(acp_hw_pdm_start(&io, 2), ACP_OK);

    uint64_t wraps = 0;
    for (int tick = 0; tick < 1000; tick++) {          /* 4 ms ticks = 4 s */
        s->counter += 48 * 4 * ACP_FRAME_BYTES;
        acp_pos_update_t u;
        acp_pos_update(&p, acp_hw_read_counter(&io), &u);
        CHECK(u.valid);
        wraps += u.new_wraps;
    }
    /* 4 s * 48000 / 4096 frames = 46.875 rings */
    CHECK_EQ(wraps, 46);
    CHECK_EQ(acp_hw_pdm_stop(&io), ACP_OK);
    CHECK_EQ(acp_hw_deinit(&io), ACP_OK);
    check_trace_safe(s, "full session");
    free(s);
}

int main(int argc, char **argv)
{
    int full = argc > 1 && strcmp(argv[1], "--full") == 0;
    test_layout();
    test_decisions();
    test_convert(full);
    test_position();
    test_init_from_off();
    test_init_noreset();
    test_init_already_on();
    test_init_failures();
    test_ring();
    test_pdm_start_stop();
    test_counter_and_ack();
    test_full_session();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
