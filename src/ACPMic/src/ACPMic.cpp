//
// ACPMic - built-in digital microphone (PDM) behind the AMD Audio
// Co-Processor (ACP 6.x, PCI 1022:15E2 rev 0x60) of the MSI Bravo 15 B7ED,
// exposed to CoreAudio through IOAudioFamily as a 2 ch / 48 kHz input.
//
// SAFETY: OFF BY DEFAULT.
//   no boot-arg            probe() returns NULL: nothing is read or written.
//   -acpmic                full driver.
//   -acpmic -acpmicprobe   read-only: dumps registers into the IOPCIDevice
//                          property "ACPMic-probe" and does NOT attach.
//   acpmicgain=0..3        PDM hardware gain (default 3, same as Linux).
//   acpmicboost=0..4       extra software gain, x2 per step (default 0).
//
// Interrupts are never enabled: the driver polls the DMA byte counter from
// a 4 ms timer on the audio work loop and timestamps buffer wraps by
// interpolation. All register sequences and arithmetic live in acp_core.c
// and are covered by host tests (test/run_tests.sh).
//
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#include "acp_core.h"

#define LOG(fmt, ...) IOLog("ACPMic: " fmt "\n", ##__VA_ARGS__)

static const uint32_t kPollUS          = 4000;        // timer period
static const uint32_t kSafetyFrames    = 256;         // HAL stays this far behind DMA
static const uint64_t kStallNS         = 300000000;   // 300 ms without progress
static const uint32_t kMaxWrapsPerTick = 64;

// ---------------------------------------------------------------------------
// MMIO glue for acp_io_t
// ---------------------------------------------------------------------------
struct ACPMmio {
    volatile uint8_t *base;
    uint32_t          len;
};

static uint32_t mmioRead(void *ctx, uint32_t off)
{
    ACPMmio *m = (ACPMmio *)ctx;
    if (!m->base || off + 4 > m->len || (off & 3))
        return 0xFFFFFFFFu;
    return *(volatile uint32_t *)(m->base + off);
}

static void mmioWrite(void *ctx, uint32_t off, uint32_t val)
{
    ACPMmio *m = (ACPMmio *)ctx;
    if (!m->base || off + 4 > m->len || (off & 3))
        return;
    *(volatile uint32_t *)(m->base + off) = val;
}

static void mmioDelay(void *, uint32_t us)
{
    IODelay(us);
}

static uint8_t findPMCap(IOPCIDevice *pci)
{
    if (!(pci->configRead16(kIOPCIConfigStatus) & 0x10))
        return 0;
    uint8_t p = pci->configRead8(kIOPCIConfigCapabilitiesPtr) & 0xFC;
    for (int i = 0; p >= 0x40 && i < 48; i++) {
        if (pci->configRead8(p) == 0x01)
            return p;
        p = pci->configRead8(p + 1) & 0xFC;
    }
    return 0;
}

static int bootArgInt(const char *name, int def, int lo, int hi)
{
    int v = def;
    if (!PE_parse_boot_argn(name, &v, sizeof(v)))
        return def;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

class ACPMicDevice;

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class ACPMicEngine : public IOAudioEngine {
    OSDeclareDefaultStructors(ACPMicEngine)

    ACPMicDevice             *fDev = nullptr;
    IOBufferMemoryDescriptor *fBuf = nullptr;
    IODMACommand             *fDMA = nullptr;
    uint64_t                  fPages[ACP_RING_PAGES] = {};
    IOTimerEventSource       *fTimer = nullptr;
    acp_pos_t                 fPos = {};
    uint64_t                  fStartCounter = 0;
    uint64_t                  fStartTimeAbs = 0;
    volatile uint32_t         fRingFrame = 0;
    bool                      fHwRunning = false;
    bool                      fSynthetic = false;
    int                       fBoost = 0;

    void timerFired(IOTimerEventSource *t);
    bool allocRing();

public:
    bool initWithDevice(ACPMicDevice *dev, int boost);
    bool initHardware(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn performAudioEngineStart() override;
    IOReturn performAudioEngineStop() override;
    UInt32   getCurrentSampleFrame() override;
    IOReturn performFormatChange(IOAudioStream *s, const IOAudioStreamFormat *f,
                                 const IOAudioSampleRate *r) override;
    IOReturn convertInputSamples(const void *sampleBuf, void *destBuf,
                                 UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                 const IOAudioStreamFormat *fmt,
                                 IOAudioStream *stream) override;

    IOReturn hwStart();
    void     hwStop();
    bool     hwRunning() const { return fHwRunning; }
};

OSDefineMetaClassAndStructors(ACPMicEngine, IOAudioEngine)

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
class ACPMicDevice : public IOAudioDevice {
    OSDeclareDefaultStructors(ACPMicDevice)

    IOPCIDevice  *fPCI = nullptr;
    IOMemoryMap  *fMap = nullptr;
    ACPMmio       fMmio = {};
    ACPMicEngine *fEngine = nullptr;
    bool          fHwUp = false;
    int           fResetIfOn = 1;   /* 0 with -acpmicnoreset: no soft reset if firmware left the ACP on */
    bool          fResumeAfterWake = false;
    int           fGain = 3;

    bool mapBar();
    static void probeOnly(IOPCIDevice *pci);

public:
    acp_io_t      fIO = {};

    IOService *probe(IOService *provider, SInt32 *score) override;
    bool initHardware(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn performPowerStateChange(IOAudioDevicePowerState oldState,
                                     IOAudioDevicePowerState newState,
                                     UInt32 *usUntilComplete) override;

    bool hwUp() const { return fHwUp; }
    int  gain() const { return fGain; }
    IOPCIDevice *pci() const { return fPCI; }
    void status(const char *s) { setProperty("ACPMicStatus", s); LOG("%s", s); }
};

OSDefineMetaClassAndStructors(ACPMicDevice, IOAudioDevice)

// ---- probe ---------------------------------------------------------------

void ACPMicDevice::probeOnly(IOPCIDevice *pci)
{
    OSDictionary *d = OSDictionary::withCapacity(32);
    if (!d)
        return;
    uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    uint8_t  pm  = findPMCap(pci);
    uint16_t pmcsr = pm ? pci->configRead16(pm + 4) : 0xFFFF;
    OSNumber *n;
    if ((n = OSNumber::withNumber(cmd, 16)))   { d->setObject("PCICommand", n); n->release(); }
    if ((n = OSNumber::withNumber(pmcsr, 16))) { d->setObject("PMCSR", n); n->release(); }
    if ((n = OSNumber::withNumber(pci->configRead8(kIOPCIConfigRevisionID), 8))) {
        d->setObject("PCIRevision", n); n->release();
    }

    const char *note = "ok";
    if (!(cmd & kIOPCICommandMemorySpace)) {
        note = "memory decode off; registers not read";
    } else if ((pmcsr & 3) != 0) {
        note = "function not in D0; registers not read";
    } else {
        IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        if (!map) {
            note = "BAR0 map failed";
        } else {
            ACPMmio m = { (volatile uint8_t *)map->getVirtualAddress(), (uint32_t)map->getLength() };
            if ((n = OSNumber::withNumber(m.len, 32))) { d->setObject("BAR0Length", n); n->release(); }
            if (m.len < ACP_BAR0_MIN_LEN) {
                note = "BAR0 too small";
            } else {
                uint32_t pg = mmioRead(&m, ACP_OFF(ACP_REG_PGFSM_STATUS));
                // Only the always-on PGFSM block is read unless the ACP is
                // already powered (status 0), as the AMD driver does.
                for (uint32_t i = 0; i < acp_dump_regs_count; i++) {
                    uint32_t a = acp_dump_regs[i].addr;
                    bool alwaysOn = a == ACP_REG_PGFSM_STATUS || a == ACP_REG_PGFSM_CONTROL;
                    if (!alwaysOn && pg != 0)
                        continue;
                    uint32_t v = mmioRead(&m, ACP_OFF(a));
                    LOG("probe %-20s = 0x%08x", acp_dump_regs[i].name, v);
                    if ((n = OSNumber::withNumber(v, 32))) {
                        d->setObject(acp_dump_regs[i].name, n); n->release();
                    }
                }
                if (pg != 0)
                    note = "ACP power-gated (PGFSM_STATUS != 0); only PGFSM read";
                else if (acp_pin_config_is_pdm(mmioRead(&m, ACP_OFF(ACP_REG_PIN_CONFIG))))
                    note = "ok: PIN_CONFIG selects PDM microphone";
                else
                    note = "PIN_CONFIG is not a PDM config";
            }
            map->release();
        }
    }
    OSString *s = OSString::withCString(note);
    if (s) { d->setObject("Result", s); s->release(); }
    pci->setProperty("ACPMic-probe", d);
    d->release();
    LOG("probe-only: %s (not attaching)", note);
}

IOService *ACPMicDevice::probe(IOService *provider, SInt32 *score)
{
    char arg[16];
    if (!PE_parse_boot_argn("-acpmic", arg, sizeof(arg)))
        return nullptr;                      // default: do absolutely nothing

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return nullptr;
    uint32_t id  = pci->configRead32(kIOPCIConfigVendorID);
    uint8_t  rev = pci->configRead8(kIOPCIConfigRevisionID);
    if (id != 0x15E21022u || !acp_revision_supported(rev)) {
        LOG("unsupported device %08x rev %02x", id, rev);
        return nullptr;
    }
    if (PE_parse_boot_argn("-acpmicprobe", arg, sizeof(arg))) {
        probeOnly(pci);
        return nullptr;
    }
    return IOAudioDevice::probe(provider, score);
}

// ---- hardware bring-up -----------------------------------------------------

bool ACPMicDevice::mapBar()
{
    fMap = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fMap)
        return false;
    fMmio.base = (volatile uint8_t *)fMap->getVirtualAddress();
    fMmio.len  = (uint32_t)fMap->getLength();
    fIO.ctx = &fMmio;
    fIO.rd = mmioRead;
    fIO.wr = mmioWrite;
    fIO.delay_us = mmioDelay;
    setProperty("BAR0Length", fMmio.len, 32);
    return fMmio.len >= ACP_BAR0_MIN_LEN;
}

bool ACPMicDevice::initHardware(IOService *provider)
{
    if (!IOAudioDevice::initHardware(provider))
        return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();

    fGain = bootArgInt("acpmicgain", 3, 0, 3);
    int boost = bootArgInt("acpmicboost", 0, 0, 4);
    setProperty("Gain", (unsigned long long)fGain, 32);
    setProperty("Boost", (unsigned long long)boost, 32);

    uint8_t pm = findPMCap(fPCI);
    if (pm) {
        uint16_t pmcsr = fPCI->configRead16(pm + 4);
        if (pmcsr & 3) {
            LOG("function in D%u, moving to D0", pmcsr & 3);
            fPCI->configWrite16(pm + 4, pmcsr & ~3);
            IOSleep(10);
        }
    }
    fPCI->setMemoryEnable(true);

    if (!mapBar()) {
        status("BAR0 map failed or too small");
        return false;
    }
    {
        char na[8];
        fResetIfOn = PE_parse_boot_argn("-acpmicnoreset", na, sizeof(na)) ? 0 : 1;
        setProperty("ResetIfOn", (unsigned long long)fResetIfOn, 32);
        setProperty("PGFSMAtStart", fIO.rd(fIO.ctx, ACP_OFF(ACP_REG_PGFSM_STATUS)), 32);
    }
    int r = acp_hw_init_ex(&fIO, fResetIfOn);
    if (r != ACP_OK) {
        status(r == ACP_ERR_DEAD ? "ACP registers read all-ones (off?)" : "ACP power-on/reset timed out");
        return false;
    }
    fHwUp = true;

    uint32_t pin = fIO.rd(fIO.ctx, ACP_OFF(ACP_REG_PIN_CONFIG));
    setProperty("PinConfig", pin, 32);
    if (!acp_pin_config_is_pdm(pin)) {
        acp_hw_deinit(&fIO);
        fHwUp = false;
        status("PIN_CONFIG is not a PDM microphone config; giving up");
        return false;
    }

    setDeviceName("Digital Microphone (ACP)");
    setDeviceShortName("DMIC");
    setManufacturerName("AMD");
    setDeviceTransportType(kIOAudioDeviceTransportTypeBuiltIn);

    fEngine = new ACPMicEngine;
    if (!fEngine || !fEngine->initWithDevice(this, boost)) {
        status("engine init failed");
        OSSafeReleaseNULL(fEngine);
        acp_hw_deinit(&fIO);
        fHwUp = false;
        return false;
    }
    if (activateAudioEngine(fEngine) != kIOReturnSuccess) {
        status("activateAudioEngine failed");
        OSSafeReleaseNULL(fEngine);
        acp_hw_deinit(&fIO);
        fHwUp = false;
        return false;
    }
    status("ready");
    return true;
}

static IOReturn gatedHwStop(OSObject *, void *engine, void *, void *, void *)
{
    ((ACPMicEngine *)engine)->hwStop();
    return kIOReturnSuccess;
}

void ACPMicDevice::stop(IOService *provider)
{
    if (fEngine) {                           // DMA off before anything is freed
        IOCommandGate *g = getCommandGate();
        if (g)
            g->runAction(gatedHwStop, fEngine);
        else
            fEngine->hwStop();
    }
    IOAudioDevice::stop(provider);
    if (fHwUp) {
        acp_hw_deinit(&fIO);
        fHwUp = false;
    }
    if (fPCI)
        fPCI->setBusMasterEnable(false);
}

void ACPMicDevice::free()
{
    OSSafeReleaseNULL(fEngine);
    OSSafeReleaseNULL(fMap);
    OSSafeReleaseNULL(fPCI);
    IOAudioDevice::free();
}

IOReturn ACPMicDevice::performPowerStateChange(IOAudioDevicePowerState oldState,
                                               IOAudioDevicePowerState newState,
                                               UInt32 *usUntilComplete)
{
    if (newState == kIOAudioDeviceSleep && oldState != kIOAudioDeviceSleep) {
        if (fEngine && fEngine->hwRunning()) {
            fEngine->pauseAudioEngine();     // tells the HAL; may or may not stop
            fEngine->hwStop();               // so stop DMA ourselves (idempotent)
            fResumeAfterWake = true;
        }
        if (fHwUp)
            acp_hw_deinit(&fIO);
        fHwUp = false;
        if (fPCI)
            fPCI->setBusMasterEnable(false);
    } else if (oldState == kIOAudioDeviceSleep && newState != kIOAudioDeviceSleep) {
        if (fPCI)
            fPCI->setMemoryEnable(true);
        fHwUp = acp_hw_init_ex(&fIO, fResetIfOn) == ACP_OK;
        if (!fHwUp)
            status("ACP init after wake failed");
        if (fResumeAfterWake && fEngine && fHwUp) {
            fEngine->resumeAudioEngine();    // tells the HAL to resync
            if (!fEngine->hwRunning())
                fEngine->hwStart();          // restart DMA if resume did not
        }
        fResumeAfterWake = false;
    }
    return IOAudioDevice::performPowerStateChange(oldState, newState, usUntilComplete);
}

// ---------------------------------------------------------------------------
// Engine implementation
// ---------------------------------------------------------------------------
bool ACPMicEngine::initWithDevice(ACPMicDevice *dev, int boost)
{
    if (!IOAudioEngine::init(nullptr))
        return false;
    fDev = dev;
    fBoost = boost;
    return true;
}

bool ACPMicEngine::allocRing()
{
    // Wired, physically contiguous, 4 KiB aligned, below 4 GiB.
    fBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        ACP_RING_BYTES, 0x00000000FFFFF000ULL);
    if (!fBuf)
        return false;
    bzero(fBuf->getBytesNoCopy(), ACP_RING_BYTES);

    fDMA = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32,
                                           ACP_PAGE_SIZE, IODMACommand::kMapped,
                                           0, 1);
    if (!fDMA || fDMA->setMemoryDescriptor(fBuf, true) != kIOReturnSuccess)
        return false;

    UInt64 offset = 0;
    uint32_t page = 0;
    while (offset < ACP_RING_BYTES && page < ACP_RING_PAGES) {
        IODMACommand::Segment64 seg;
        UInt32 nseg = 1;
        if (fDMA->gen64IOVMSegments(&offset, &seg, &nseg) != kIOReturnSuccess || nseg != 1)
            return false;
        // Split each segment into 4 KiB pages for the ACP page table.
        for (UInt64 o = 0; o < seg.fLength && page < ACP_RING_PAGES; o += ACP_PAGE_SIZE)
            fPages[page++] = seg.fIOVMAddr + o;
    }
    if (page != ACP_RING_PAGES ||
        acp_validate_pages(fPages, ACP_RING_PAGES, ACP_RING_BYTES) != ACP_OK)
        return false;
    LOG("ring %u bytes at bus 0x%llx", ACP_RING_BYTES, fPages[0]);
    return true;
}

bool ACPMicEngine::initHardware(IOService *provider)
{
    if (!IOAudioEngine::initHardware(provider))
        return false;
    if (!allocRing()) {
        fDev->status("DMA buffer allocation failed");
        return false;
    }

    IOAudioSampleRate rate = { ACP_SAMPLE_RATE, 0 };
    setSampleRate(&rate);
    setNumSampleFramesPerBuffer(ACP_RING_FRAMES);
    setDescription("Built-in Digital Microphone");
    setSampleOffset(kSafetyFrames);
    setInputSampleOffset(kSafetyFrames);     // HAL reads this far behind the DMA
    setInputSampleLatency(kSafetyFrames);

    IOAudioStream *stream = new IOAudioStream;
    if (!stream || !stream->initWithAudioEngine(this, kIOAudioStreamDirectionInput, 1, "DMIC")) {
        OSSafeReleaseNULL(stream);
        return false;
    }
    IOAudioStreamFormat fmt = {
        ACP_CHANNELS,
        kIOAudioStreamSampleFormatLinearPCM,
        kIOAudioStreamNumericRepresentationSignedInt,
        32, 32,
        kIOAudioStreamAlignmentHighByte,
        kIOAudioStreamByteOrderLittleEndian,
        true,
        0
    };
    stream->setSampleBuffer(fBuf->getBytesNoCopy(), ACP_RING_BYTES);
    stream->addAvailableFormat(&fmt, &rate, &rate);
    stream->setFormat(&fmt);
    stream->setTerminalType(INPUT_MICROPHONE);
    addAudioStream(stream);
    stream->release();

    fTimer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &ACPMicEngine::timerFired));
    if (!fTimer || !getWorkLoop() || getWorkLoop()->addEventSource(fTimer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(fTimer);
        return false;
    }
    return true;
}

void ACPMicEngine::stop(IOService *provider)
{
    hwStop();
    if (fTimer) {
        fTimer->cancelTimeout();
        if (getWorkLoop())
            getWorkLoop()->removeEventSource(fTimer);
        OSSafeReleaseNULL(fTimer);
    }
    IOAudioEngine::stop(provider);
}

void ACPMicEngine::free()
{
    if (fDMA) {
        fDMA->clearMemoryDescriptor(true);
        OSSafeReleaseNULL(fDMA);
    }
    OSSafeReleaseNULL(fBuf);
    OSSafeReleaseNULL(fTimer);
    IOAudioEngine::free();
}

IOReturn ACPMicEngine::hwStart()
{
    if (!fDev || !fDev->hwUp() || !fBuf)
        return kIOReturnNotReady;
    const acp_io_t *io = &fDev->fIO;

    bzero(fBuf->getBytesNoCopy(), ACP_RING_BYTES);
    fDev->pci()->setBusMasterEnable(true);
    if (acp_hw_program_ring(io, fPages, ACP_RING_PAGES, ACP_RING_BYTES, ACP_PERIOD_BYTES) != ACP_OK)
        return kIOReturnError;

    fStartCounter = acp_hw_read_counter(io);
    acp_pos_start(&fPos, fStartCounter, ACP_RING_BYTES, ACP_FRAME_BYTES);
    fRingFrame = 0;
    fSynthetic = false;

    int r = acp_hw_pdm_start(io, fDev->gain());
    if (r != ACP_OK) {
        acp_hw_pdm_stop(io);
        fDev->pci()->setBusMasterEnable(false);
        fDev->status(r == ACP_ERR_TIMEOUT ? "PDM DMA did not start (timeout)" : "PDM start failed");
        return kIOReturnError;
    }
    clock_get_uptime(&fStartTimeAbs);
    fHwRunning = true;
    takeTimeStamp(false);
    fTimer->setTimeoutUS(kPollUS);
    fDev->status("running");
    return kIOReturnSuccess;
}

void ACPMicEngine::hwStop()
{
    if (fTimer)
        fTimer->cancelTimeout();
    if (!fHwRunning)
        return;
    fHwRunning = false;
    if (fDev && fDev->hwUp()) {
        if (acp_hw_pdm_stop(&fDev->fIO) != ACP_OK)
            fDev->status("PDM DMA did not stop cleanly");
        else
            fDev->status("stopped");
    }
}

IOReturn ACPMicEngine::performAudioEngineStart()
{
    return hwStart();
}

IOReturn ACPMicEngine::performAudioEngineStop()
{
    hwStop();
    return kIOReturnSuccess;
}

void ACPMicEngine::timerFired(IOTimerEventSource *t)
{
    if (!fHwRunning || !fDev || !fDev->hwUp())
        return;
    const acp_io_t *io = &fDev->fIO;

    acp_hw_ack_pdm_status(io);
    uint64_t counter = acp_hw_read_counter(io);
    uint64_t now;                            // sampled right after the counter
    clock_get_uptime(&now);
    uint64_t elapsedNs;
    absolutetime_to_nanoseconds(now - fStartTimeAbs, &elapsedNs);

    if (!fSynthetic && acp_dma_stalled(counter, fStartCounter, elapsedNs, kStallNS)) {
        fSynthetic = true;
        fDev->status("PDM DMA counter not moving: delivering silence on a software clock");
    }
    if (fSynthetic)
        counter = acp_synth_counter(fStartCounter, elapsedNs, ACP_SAMPLE_RATE, ACP_FRAME_BYTES);

    acp_pos_update_t u;
    acp_pos_update(&fPos, counter, &u);
    if (!u.valid) {
        // Counter went backwards (hardware reset underneath us). Re-base.
        acp_pos_start(&fPos, counter, ACP_RING_BYTES, ACP_FRAME_BYTES);
        fStartCounter = counter;
        fStartTimeAbs = now;
        fDev->status("DMA counter reset; re-based");
    } else {
        fRingFrame = u.ring_frame;
        uint32_t n = u.new_wraps;
        uint32_t first = n > kMaxWrapsPerTick ? n - kMaxWrapsPerTick : 0;
        for (uint32_t i = first; i < n; i++) {
            uint64_t ageNs = acp_frames_to_ns(acp_wrap_age_frames(&u, i, ACP_RING_FRAMES),
                                              ACP_SAMPLE_RATE);
            uint64_t ageAbs;
            nanoseconds_to_absolutetime(ageNs, &ageAbs);
            AbsoluteTime ts = now > ageAbs ? now - ageAbs : now;
            takeTimeStamp(true, &ts);
        }
    }
    t->setTimeoutUS(kPollUS);
}

UInt32 ACPMicEngine::getCurrentSampleFrame()
{
    return fRingFrame;
}

IOReturn ACPMicEngine::performFormatChange(IOAudioStream *, const IOAudioStreamFormat *f,
                                           const IOAudioSampleRate *r)
{
    if (f && (f->fNumChannels != ACP_CHANNELS || f->fBitWidth != 32))
        return kIOReturnUnsupported;
    if (r && r->whole != ACP_SAMPLE_RATE)
        return kIOReturnUnsupported;
    return kIOReturnSuccess;
}

IOReturn ACPMicEngine::convertInputSamples(const void *sampleBuf, void *destBuf,
                                           UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                           const IOAudioStreamFormat *, IOAudioStream *)
{
    if (!sampleBuf || !destBuf || firstSampleFrame >= ACP_RING_FRAMES)
        return kIOReturnBadArgument;
    if (numSampleFrames > ACP_RING_FRAMES - firstSampleFrame)
        numSampleFrames = ACP_RING_FRAMES - firstSampleFrame;
    const int32_t *src = (const int32_t *)sampleBuf + firstSampleFrame * ACP_CHANNELS;
    acp_convert_block(src, (uint32_t *)destBuf, numSampleFrames * ACP_CHANNELS, fBoost);
    return kIOReturnSuccess;
}
