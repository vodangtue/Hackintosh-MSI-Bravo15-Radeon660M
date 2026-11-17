// PCIPMManaged - lets the Mac sleep on this MSI Bravo 15 B7ED.
//
// WHY. Every sleep attempt stopped in DarkWake with the kernel line
//   "PMRD: System sleep prevented by kPMPCIUnsupported".
// IOPMrootDomain::checkSystemSleepAllowed calls the platform function
// "IOPMSetSleepSupported"; AppleACPIPlatformExpert::queryPlatformCanSleep
// (disassembled from this machine's BootKernelExtensions.kc) walks the PCI
// slot devices and passes one only if it has the property
// "IOPMIsPowerManaged" == kOSBooleanTrue or a child in the IOPower plane;
// otherwise "PCISlot: %s does not have a driver" and sleep is refused.
// Measured on 2026-11-17: no entry carries IOPMIsPowerManaged, and these
// PCI functions have no power child (no macOS driver uses them):
//   144d:a80b  NVMe 6:0:0  (the Windows SSD, hidden from macOS on purpose)
//   1002:ab28  HDMI audio of the RX 6550M (9:0:1)
//   1002:1640  HDMI audio of the iGPU (2:0:1)
//   1022:1649  PSP (2:0:2)
//   1022:15e2  ACP audio coprocessor (2:0:5)
//
// WHAT IT DOES: in probe() it sets IOPMIsPowerManaged = true on the
// IOPCIDevice and returns NULL, so it never attaches and never starts.
// It does NOT read or write config space, BARs or any register: the Windows
// SSD is not touched in any way. S3 removes their power regardless.
//
// Boot-arg -pcipmmanagedoff makes probe() do nothing.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>

class PCIPMManaged : public IOService {
    OSDeclareDefaultStructors(PCIPMManaged)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
};

OSDefineMetaClassAndStructors(PCIPMManaged, IOService)

IOService *PCIPMManaged::probe(IOService *provider, SInt32 *score)
{
    char arg[8];
    if (PE_parse_boot_argn("-pcipmmanagedoff", arg, sizeof(arg))) {
        return nullptr;
    }
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        return nullptr;
    }
    if (pci->getProperty("IOPMIsPowerManaged") != kOSBooleanTrue) {
        pci->setProperty("IOPMIsPowerManaged", kOSBooleanTrue);
        pci->setProperty("PCIPMManagedMarked", kOSBooleanTrue);
        IOLog("PCIPMManaged: marked %s IOPMIsPowerManaged (no hardware access)\n",
              pci->getName());
    }
    return nullptr;   /* never attach, never start */
}
