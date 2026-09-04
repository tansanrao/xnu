#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <machine/machine_routines.h>

#if defined(ARM64_BOARD_CONFIG_BCM2712)

#include <arm64/bcm2712_interrupt.h>

#define super IODTPlatformExpert

class BCM2712PlatformExpert : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(BCM2712PlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	int haltRestart(unsigned int type) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *rootEntry) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(BCM2712PlatformExpert, IODTPlatformExpert)

bool
BCM2712PlatformExpert::start(IOService *provider)
{
	IOLog("BCM2712PlatformExpert: starting\n");
	bool result = super::start(provider);
	if (result) {
		const unsigned int max_cpus = ml_get_max_cpu_number() + 1;
		ml_set_max_cpus(max_cpus);
		IOLog("BCM2712PlatformExpert: finalized %u CPUs\n",
		    max_cpus);
	}
	IOLog("BCM2712PlatformExpert: start returned %d\n", result);
	return result;
}

int
BCM2712PlatformExpert::haltRestart(unsigned int type)
{
	if (type != kPERestartCPU && type != kPEPanicRestartCPU &&
	    type != kPEPanicRestartCPUNoCallouts) {
		return super::haltRestart(type);
	}

	IOLog("BCM2712PlatformExpert: requesting PSCI system reset\n");
	int64_t result = bcm2712_psci_system_reset();
	IOLog("BCM2712PlatformExpert: PSCI system reset failed: %lld\n",
	    result);
	return -1;
}

void
BCM2712PlatformExpert::processTopLevel(IORegistryEntry *rootEntry)
{
	IORegistryEntry *cpus = rootEntry->childFromPath("cpus", gIODTPlane);
	if (cpus != nullptr) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, nullptr));
		cpus->release();
	}

	IORegistryEntry *soc = rootEntry->childFromPath("arm-io", gIODTPlane);
	if (soc != nullptr) {
		createNubs(this, IODTFindMatchingEntries(soc, kIODTExclusive, nullptr));
		soc->release();
	}

	createNubs(this, IODTFindMatchingEntries(rootEntry, kIODTExclusive,
	    "('chosen', 'cpus', 'defaults')"));
}

const char *
BCM2712PlatformExpert::deleteList(void)
{
	return "()";
}

const char *
BCM2712PlatformExpert::excludeList(void)
{
	return "('chosen', 'cpus', 'defaults')";
}

#endif /* ARM64_BOARD_CONFIG_BCM2712 */
