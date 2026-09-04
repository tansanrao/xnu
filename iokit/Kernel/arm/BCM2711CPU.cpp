#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>

#if defined(ARM64_BOARD_CONFIG_BCM2711)

#include <arm64/bcm2711_interrupt.h>

extern "C" void bcm2711_gic_set_ipi_handler(ipi_handler_t handler);
extern "C" vm_offset_t reset_vector_vaddr;
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void bcm2711_wait_for_all_cpus(void);

static volatile SInt32 registeredCPUs;
static const char *bootCPURegistered = "BCM2711BootCPURegistered";

extern "C" void
bcm2711_wait_for_all_cpus(void)
{
	if (IOService::waitForService(
	    IOService::resourceMatching(gIOAllCPUInitializedKey)) == nullptr) {
		panic("BCM2711CPU: CPU initialization resource wait failed");
	}
}

#define super IOCPU

class BCM2711CPU : public IOCPU
{
	OSDeclareDefaultStructors(BCM2711CPU);

private:
	UInt32 physicalID;
	UInt64 releaseAddress;
	volatile UInt64 *releaseSlot;
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

OSDefineMetaClassAndStructors(BCM2711CPU, IOCPU)

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
BCM2711CPU::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}

	if (!readDataProperty(provider, "reg", &physicalID,
	    sizeof(physicalID)) ||
	    !readDataProperty(provider, "cpu-release-addr", &releaseAddress,
	    sizeof(releaseAddress))) {
		panic("BCM2711CPU: CPU node has invalid register properties");
	}

	int logicalID = ml_get_cpu_number(physicalID);
	if (logicalID < 0) {
		panic("BCM2711CPU: unknown physical CPU %u", physicalID);
	}

	setCPUNumber((UInt32)logicalID);
	releaseSlot = reinterpret_cast<volatile UInt64 *>(
	    ml_io_map(releaseAddress, sizeof(*releaseSlot)));
	if (releaseSlot == nullptr) {
		panic("BCM2711CPU: cannot map release slot 0x%llx",
		    releaseAddress);
	}
	cpuName = OSSymbol::withCStringNoCopy("BCM2711CPU");

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
		panic("BCM2711CPU: boot CPU registration wait failed");
	}
	kern_return_t result = ml_processor_register(&processorInfo,
	    &machProcessor, &ipi_handler, &pmiHandler);
	if (result != KERN_SUCCESS) {
		panic("BCM2711CPU: ml_processor_register(%d) failed: %d",
		    logicalID, result);
	}

	IOLog("BCM2711CPU: registered CPU %d (MPIDR 0x%x)\n",
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
BCM2711CPU::initCPU(__unused bool boot)
{
	bcm2711_gic_set_ipi_handler(ipi_handler);
	bcm2711_gic_init_cpu();
	setCPUState(kIOCPUStateRunning);

	/* The first IPI enables XNU's software signal path on this CPU. */
	signalCPU(this);
}

kern_return_t
BCM2711CPU::startCPU(__unused vm_offset_t start_paddr,
    __unused vm_offset_t arg_paddr)
{
	UInt64 resetAddress = ml_vtophys(reset_vector_vaddr);
	*releaseSlot = resetAddress;
	__asm__ volatile ("dsb ishst\n\tsev" ::: "memory");
	return KERN_SUCCESS;
}

void
BCM2711CPU::signalCPU(IOCPU *target)
{
	BCM2711CPU *bcmTarget = OSDynamicCast(BCM2711CPU, target);
	if (bcmTarget == nullptr) {
		panic("BCM2711CPU: cannot signal a non-BCM2711 CPU");
	}
	bcm2711_gic_send_ipi(bcmTarget->physicalID);
}

void
BCM2711CPU::quiesceCPU(void)
{
	panic("BCM2711CPU: CPU quiesce is not implemented");
}

void
BCM2711CPU::haltCPU(void)
{
	panic("BCM2711CPU: CPU halt is not implemented");
}

const OSSymbol *
BCM2711CPU::getCPUName(void)
{
	return cpuName;
}

#endif /* ARM64_BOARD_CONFIG_BCM2711 */
