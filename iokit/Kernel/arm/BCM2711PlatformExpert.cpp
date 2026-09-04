#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <machine/machine_routines.h>

#if defined(ARM64_BOARD_CONFIG_BCM2711)

#define super IODTPlatformExpert

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" vm_offset_t pe_arm_get_soc_base_phys(void);

#define BCM2711_PM_RSTC               0x1cU
#define BCM2711_PM_WDOG               0x24U
#define BCM2711_PM_PASSWORD           0x5a000000U
#define BCM2711_PM_RSTC_WRCFG_CLEAR   0xffffffcfU
#define BCM2711_PM_RSTC_FULL_RESET    0x20U

class BCM2711PlatformExpert : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(BCM2711PlatformExpert);

private:
	UInt64 watchdogPhysicalAddress;
	UInt64 watchdogLength;
	vm_offset_t watchdogRegisters;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	int haltRestart(unsigned int type) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *rootEntry) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(BCM2711PlatformExpert, IODTPlatformExpert)

bool
BCM2711PlatformExpert::start(IOService *provider)
{
	bool result = super::start(provider);
	if (result) {
		IORegistryEntry *watchdog = IORegistryEntry::fromPath(
		    "/arm-io/watchdog", gIODTPlane);
		OSData *reg = watchdog == nullptr ? nullptr :
		    OSDynamicCast(OSData, watchdog->getProperty("reg"));
		if (reg == nullptr || reg->getLength() != 2 * sizeof(UInt64)) {
			panic("BCM2711PlatformExpert: invalid watchdog reg property");
		}

		UInt64 range[2];
		bcopy(reg->getBytesNoCopy(), range, sizeof(range));
		watchdogPhysicalAddress = pe_arm_get_soc_base_phys() + range[0];
		watchdogLength = range[1];
		watchdog->release();
		/* Panic restart cannot allocate mappings or acquire VM locks. */
		watchdogRegisters = ml_io_map(watchdogPhysicalAddress, watchdogLength);
		if (watchdogRegisters == 0) {
			panic("BCM2711PlatformExpert: unable to map watchdog");
		}

		const unsigned int max_cpus = ml_get_max_cpu_number() + 1;
		ml_set_max_cpus(max_cpus);
	}
	return result;
}

int
BCM2711PlatformExpert::haltRestart(unsigned int type)
{
	if (type != kPERestartCPU && type != kPEPanicRestartCPU &&
	    type != kPEPanicRestartCPUNoCallouts) {
		return super::haltRestart(type);
	}

	vm_offset_t pm = watchdogRegisters;
	if (pm == 0) {
		return -1;
	}

	volatile UInt32 *rstc = reinterpret_cast<volatile UInt32 *>(
	    pm + BCM2711_PM_RSTC);
	volatile UInt32 *wdog = reinterpret_cast<volatile UInt32 *>(
	    pm + BCM2711_PM_WDOG);

	/* Ten 1/65536-second watchdog ticks are about 153 microseconds. */
	*wdog = BCM2711_PM_PASSWORD | 10U;
	*rstc = BCM2711_PM_PASSWORD |
	    (*rstc & BCM2711_PM_RSTC_WRCFG_CLEAR) |
	    BCM2711_PM_RSTC_FULL_RESET;
	__asm__ volatile ("dsb sy" ::: "memory");

	for (;;) {
		__asm__ volatile ("wfe");
	}
}

void
BCM2711PlatformExpert::processTopLevel(IORegistryEntry *rootEntry)
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
BCM2711PlatformExpert::deleteList(void)
{
	return "()";
}

const char *
BCM2711PlatformExpert::excludeList(void)
{
	return "('chosen', 'cpus', 'defaults')";
}

#endif /* ARM64_BOARD_CONFIG_BCM2711 */
