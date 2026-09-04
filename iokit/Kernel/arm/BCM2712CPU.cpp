#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>

#if defined(ARM64_BOARD_CONFIG_BCM2712)

#include <arm64/bcm2712_interrupt.h>

extern "C" void bcm2712_gic_set_ipi_handler(ipi_handler_t handler);
extern "C" vm_offset_t reset_vector_vaddr;
extern "C" void bcm_wait_for_all_cpus(void);

static volatile SInt32 registeredCPUs;
static const char *bootCPURegistered = "BCM2712BootCPURegistered";

extern "C" void
bcm_wait_for_all_cpus(void)
{
	if (IOService::waitForService(
	    IOService::resourceMatching(gIOAllCPUInitializedKey)) == nullptr) {
		panic("BCM2712CPU: CPU initialization resource wait failed");
	}
}

#define super IOCPU

class BCM2712CPU : public IOCPU
{
	OSDeclareDefaultStructors(BCM2712CPU);

private:
	UInt32 physicalID;
	UInt64 psciEntryAddress;
	const OSSymbol *cpuName;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	kern_return_t startCPU(vm_offset_t start_paddr,
	    vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	void haltCPU(void) APPLE_KEXT_OVERRIDE;
	void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE;
	const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(BCM2712CPU, IOCPU)

static bool
readDataProperty(IOService *provider, const char *name, void *value,
    unsigned int size)
{
	OSData *data = OSDynamicCast(OSData, provider->getProperty(name));
	if (data == nullptr || data->getLength() != size) {
		return false;
	}

	bcopy(data->getBytesNoCopy(), value, size);
	return true;
}

bool
BCM2712CPU::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}

	if (!readDataProperty(provider, "reg", &physicalID,
	    sizeof(physicalID)) ||
	    !readDataProperty(provider, "psci-entry-addr", &psciEntryAddress,
	    sizeof(psciEntryAddress))) {
		panic("BCM2712CPU: CPU node has invalid register properties");
	}

	int logicalID = ml_get_cpu_number(physicalID);
	if (logicalID < 0) {
		panic("BCM2712CPU: unknown physical CPU %u", physicalID);
	}

	setCPUNumber((UInt32)logicalID);
	cpuName = OSSymbol::withCStringNoCopy("BCM2712CPU");

	const ml_topology_cpu_t *topology =
	    &ml_get_topology_info()->cpus[logicalID];
	ml_processor_info_t processorInfo = {};
	processorInfo.cpu_id = static_cast<cpu_id_t>(this);
	processorInfo.phys_id = topology->phys_id;
	processorInfo.log_id = topology->cpu_id;
	processorInfo.cluster_id = topology->cluster_id;
	processorInfo.cluster_type = topology->cluster_type;
	processorInfo.l2_cache_id = topology->l2_cache_id;
	processorInfo.l2_cache_size = topology->l2_cache_size;
	processorInfo.l3_cache_id = topology->l3_cache_id;
	processorInfo.l3_cache_size = topology->l3_cache_size;

	perfmon_interrupt_handler_func pmiHandler;
	/* AP startup can immediately cause an IPI from the boot CPU. */
	if (logicalID != ml_get_boot_cpu_number() &&
	    IOService::waitForService(IOService::resourceMatching(bootCPURegistered)) == nullptr) {
		panic("BCM2712CPU: boot CPU registration wait failed");
	}
	kern_return_t result = ml_processor_register(&processorInfo,
	    &machProcessor, &ipi_handler, &pmiHandler);
	if (result != KERN_SUCCESS) {
		panic("BCM2712CPU: ml_processor_register(%d) failed: %d",
		    logicalID, result);
	}

	IOLog("BCM2712CPU: registered CPU %d (MPIDR 0x%x)\n",
	    logicalID, physicalID);
	if (logicalID == ml_get_boot_cpu_number()) {
		IOService::publishResource(bootCPURegistered, kOSBooleanTrue);
	}
	if ((UInt32)OSIncrementAtomic(&registeredCPUs) + 1 ==
	    ml_get_cpu_count()) {
		ml_cpu_init_completed();
		IOService::cpusRunning();
		IOService::publishResource(gIOAllCPUInitializedKey,
		    kOSBooleanTrue);
	}
	registerService();
	return true;
}

void
BCM2712CPU::initCPU(__unused bool boot)
{
	bcm2712_gic_set_ipi_handler(ipi_handler);
	bcm2712_gic_init_cpu();
	setCPUState(kIOCPUStateRunning);

	/* The first IPI enables XNU's software signal path on this CPU. */
	signalCPU(this);
}

kern_return_t
BCM2712CPU::startCPU(__unused vm_offset_t start_paddr,
    __unused vm_offset_t arg_paddr)
{
	UInt64 resetAddress = ml_vtophys(reset_vector_vaddr);
	int64_t result = bcm2712_psci_cpu_on(physicalID, psciEntryAddress,
	    resetAddress);
	if (result != 0) {
		IOLog("BCM2712CPU: PSCI CPU_ON MPIDR 0x%x entry 0x%llx "
		    "context 0x%llx failed: %lld\n", physicalID, psciEntryAddress,
		    resetAddress, result);
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

void
BCM2712CPU::signalCPU(IOCPU *target)
{
	BCM2712CPU *bcmTarget = OSDynamicCast(BCM2712CPU, target);
	if (bcmTarget == nullptr) {
		panic("BCM2712CPU: cannot signal a non-BCM2712 CPU");
	}
	bcm2712_gic_send_ipi(bcmTarget->physicalID);
}

void
BCM2712CPU::quiesceCPU(void)
{
	panic("BCM2712CPU: CPU quiesce is not implemented");
}

void
BCM2712CPU::haltCPU(void)
{
	panic("BCM2712CPU: CPU halt is not implemented");
}

const OSSymbol *
BCM2712CPU::getCPUName(void)
{
	return cpuName;
}

#endif /* ARM64_BOARD_CONFIG_BCM2712 */
