//
// XHCIPoll - periodically runs AppleUSBXHCI::manuallyProcessInterrupts() on the
// controller's own work loop.
//
// Why: on AMD Rembrandt each xHCI controller has a single MSI vector. Apple's
// driver posts isochronous transfer events to interrupter 2, and on this
// machine those events often raise no interrupt, so the completions are only
// collected by the driver's ~2 s watchdog (webcam: one 1320-packet batch every
// ~2 s). manuallyProcessInterrupts() is Apple's own "run the interrupt path
// now" routine: it calls filterOccurred() (which drains every interrupter,
// including the isochronous one) and interruptOccurred() for the primary
// source.
//
// v2: v1 matched on AppleUSBXHCIPCI and was never probed. This version matches
// the xHCI IOPCIDevice (like most third-party PCI kexts), then looks for the
// AppleUSBXHCIPCI child and attaches a timer to that child's work loop.
//
// Boot-args:  -xhcipolloff      do nothing
//             xhcipoll=<ms>     interval, 1..100, default 4
//
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSNumber.h>

// Exported by com.apple.driver.usb.AppleUSBXHCI (non-virtual member; `this`
// is the AppleUSBXHCI object).
extern "C" void xhcipoll_manual(void *self) __asm("__ZN12AppleUSBXHCI25manuallyProcessInterruptsEv");

class XHCIPoll : public IOService {
    OSDeclareDefaultStructors(XHCIPoll)

    IOPCIDevice        *fPCI      = nullptr;
    IOService          *fCtl      = nullptr;   // AppleUSBXHCIPCI, retained
    IOWorkLoop         *fOwnWL    = nullptr;   // ours, for the discovery timer
    IOTimerEventSource *fFind     = nullptr;
    IOWorkLoop         *fCtlWL    = nullptr;   // controller's
    IOTimerEventSource *fTimer    = nullptr;
    uint32_t            fMs       = 4;
    uint32_t            fTries    = 0;
    uint64_t            fPolls    = 0;
    uint64_t            fSkips    = 0;
    volatile bool       fStopping = false;

    static void findTick(OSObject *owner, IOTimerEventSource *sender);
    static void pollTick(OSObject *owner, IOTimerEventSource *sender);
    IOService *findController();
    bool attachPoller(IOService *ctl);
    void publish();

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(XHCIPoll, IOService)

void XHCIPoll::publish()
{
    OSNumber *n = OSNumber::withNumber(fPolls, 64);
    if (n) { setProperty("polls", n); n->release(); }
    n = OSNumber::withNumber(fSkips, 64);
    if (n) { setProperty("skips", n); n->release(); }
}

IOService *XHCIPoll::findController()
{
    OSIterator *it = fPCI->getChildIterator(gIOServicePlane);
    if (!it)
        return nullptr;
    IOService *found = nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOService *s = OSDynamicCast(IOService, o);
        if (!s || s == this)
            continue;
        const char *cls = s->getMetaClass()->getClassName();
        if (cls && strcmp(cls, "AppleUSBXHCIPCI") == 0 && !s->isInactive()) {
            found = s;
            found->retain();
            break;
        }
    }
    it->release();
    return found;
}

bool XHCIPoll::attachPoller(IOService *ctl)
{
    IOWorkLoop *wl = ctl->getWorkLoop();
    if (!wl)
        return false;
    IOTimerEventSource *t = IOTimerEventSource::timerEventSource(this, &XHCIPoll::pollTick);
    if (!t)
        return false;
    if (wl->addEventSource(t) != kIOReturnSuccess) {
        t->release();
        return false;
    }
    wl->retain();
    fCtl   = ctl;           // already retained by findController
    fCtlWL = wl;
    fTimer = t;
    setProperty("controller", ctl->getName());
    fTimer->setTimeoutMS(fMs);
    IOLog("XHCIPoll: %s: polling %s every %u ms\n", fPCI->getName(), ctl->getName(), fMs);
    return true;
}

void XHCIPoll::findTick(OSObject *owner, IOTimerEventSource *sender)
{
    XHCIPoll *me = OSDynamicCast(XHCIPoll, owner);
    if (!me || me->fStopping || me->fCtl)
        return;
    IOService *ctl = me->findController();
    if (ctl) {
        if (!me->attachPoller(ctl)) {
            ctl->release();
            IOLog("XHCIPoll: %s: could not attach to %s\n", me->fPCI->getName(), ctl->getName());
        }
        return;
    }
    if (++me->fTries < 240)                 // keep looking for up to 2 minutes
        sender->setTimeoutMS(500);
    else
        IOLog("XHCIPoll: %s: no AppleUSBXHCIPCI child, giving up\n", me->fPCI->getName());
}

void XHCIPoll::pollTick(OSObject *owner, IOTimerEventSource *sender)
{
    XHCIPoll *me = OSDynamicCast(XHCIPoll, owner);
    if (!me || me->fStopping || !me->fCtl)
        return;

    bool alive = !me->fCtl->isInactive();
    if (alive) {
        // A powered-off / surprise-removed function reads back all ones.
        if (me->fPCI->configRead16(kIOPCIConfigVendorID) == 0xFFFF)
            alive = false;
        else if ((me->fPCI->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace) == 0)
            alive = false;
    }

    if (alive) {
        xhcipoll_manual(me->fCtl);
        me->fPolls++;
    } else {
        me->fSkips++;
    }

    if (((me->fPolls + me->fSkips) & 0x3FF) == 0)
        me->publish();

    if (!me->fStopping && !me->fCtl->isInactive())
        sender->setTimeoutMS(me->fMs);
}

bool XHCIPoll::start(IOService *provider)
{
    char arg[16];
    if (PE_parse_boot_argn("-xhcipolloff", arg, sizeof(arg))) {
        IOLog("XHCIPoll: disabled by -xhcipolloff\n");
        return false;
    }
    if (!IOService::start(provider))
        return false;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;

    uint32_t ms = 0;
    if (PE_parse_boot_argn("xhcipoll", &ms, sizeof(ms)) && ms >= 1 && ms <= 100)
        fMs = ms;

    fOwnWL = IOWorkLoop::workLoop();
    fFind  = fOwnWL ? IOTimerEventSource::timerEventSource(this, &XHCIPoll::findTick) : nullptr;
    if (!fFind || fOwnWL->addEventSource(fFind) != kIOReturnSuccess) {
        OSSafeReleaseNULL(fFind);
        OSSafeReleaseNULL(fOwnWL);
        return false;
    }

    setProperty("interval-ms", fMs, 32);
    publish();
    fFind->setTimeoutMS(1000);   // let AppleUSBXHCIPCI attach and start first
    IOLog("XHCIPoll: %s: started, waiting for controller\n", provider->getName());
    registerService();
    return true;
}

void XHCIPoll::stop(IOService *provider)
{
    fStopping = true;
    if (fFind) {
        fFind->cancelTimeout();
        if (fOwnWL)
            fOwnWL->removeEventSource(fFind);
        OSSafeReleaseNULL(fFind);
    }
    OSSafeReleaseNULL(fOwnWL);
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fCtlWL)
            fCtlWL->removeEventSource(fTimer);
        OSSafeReleaseNULL(fTimer);
    }
    OSSafeReleaseNULL(fCtlWL);
    OSSafeReleaseNULL(fCtl);
    fPCI = nullptr;
    IOService::stop(provider);
}
