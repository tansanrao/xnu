#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_BCM2711) && (DEVELOPMENT || DEBUG)
#include "BCM2711InterruptController.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>

extern "C" void bcm2711_irq_controller_test(void);

#define CHECK(x) do { if (!(x)) panic("BCM2711 IRQ test: %s", #x); } while (0)

struct TestInterrupt {
	_Atomic uint32_t count;
	_Atomic bool finished;
	bool slow;
	uint32_t cpu;
};

static void
testInterrupt(OSObject *, void *refCon, IOService *, int)
{
	TestInterrupt *test = static_cast<TestInterrupt *>(refCon);
	CHECK(bcm2711_gic_current_cpu() == test->cpu);
	os_atomic_inc(&test->count, release);
	if (test->slow) {
		IODelay(10000); /* Let another CPU begin unregister during the callback. */
	}
	os_atomic_store(&test->finished, true, release);
}

static void
waitCount(TestInterrupt *test, uint32_t count)
{
	for (unsigned int i = 0; i < 1000 && os_atomic_load(&test->count, acquire) < count; i++) {
		IOSleep(1);
	}
	CHECK(os_atomic_load(&test->count, acquire) == count);
}

static IOService *
makeNub(const OSSymbol *name, const bcm2711_irq_spec &spec)
{
	IOService *nub = new IOService;
	CHECK(nub && nub->init());
	OSArray *controllers = OSArray::withCapacity(1);
	OSArray *specifiers = OSArray::withCapacity(1);
	OSData *data = OSData::withBytes(&spec, sizeof(spec));
	CHECK(controllers && specifiers && data);
	CHECK(controllers->setObject(name) && specifiers->setObject(data));
	CHECK(nub->setProperty(gIOInterruptControllersKey, controllers));
	CHECK(nub->setProperty(gIOInterruptSpecifiersKey, specifiers));
	data->release();
	controllers->release();
	specifiers->release();
	return nub;
}

static void
bindCPU(unsigned int cpu)
{
	bcm2711_irq_test_bind_cpu((int)cpu);
	CHECK(bcm2711_gic_current_cpu() == cpu);
}

extern "C" void
bcm2711_irq_controller_test(void)
{
	auto controller = OSDynamicCast(BCM2711InterruptController,
	    IOService::waitForService(IOService::serviceMatching("BCM2711InterruptController")));
	CHECK(controller);
	const OSSymbol *name = IODTInterruptControllerName(controller->getProvider());
	CHECK(name);
	/* All four copies of PPI 20 coexist; their pending and enable bits must
	 * remain local. SPI 200 is otherwise unused in this bring-up image.
	 */
	IOService *ppis[BCM2711_GIC_MAX_CPUS] = {};
	TestInterrupt tests[BCM2711_GIC_MAX_CPUS] = {};
	for (unsigned int cpu = 0; cpu < ml_get_cpu_count(); cpu++) {
		bindCPU(cpu);
		bcm2711_gic_validate_cpu();
		bcm2711_irq_test_local_timer();
		bcm2711_irq_spec spec = {20, kIOInterruptTypeLevel, 0xa0, cpu};
		ppis[cpu] = makeNub(name, spec);
		tests[cpu].cpu = cpu;
		CHECK(ppis[cpu]->registerInterrupt(0, controller, testInterrupt, &tests[cpu]) == kIOReturnSuccess);
		CHECK(!bcm2711_gic_is_enabled(20) && !bcm2711_gic_is_pending(20));
	}
	bindCPU(1);
	CHECK(ppis[0]->enableInterrupt(0) != kIOReturnSuccess); /* Wrong bank, still owned. */
	CHECK(!bcm2711_gic_is_enabled(20));
	CHECK(ppis[1]->causeInterrupt(0) == kIOReturnSuccess);
	bindCPU(0);
	CHECK(controller->setPending(ppis[0], 0, false) == kIOReturnSuccess);
	bindCPU(1);
	if (!bcm2711_gic_is_pending(20)) {
		unsigned int strict = 0;
		PE_parse_boot_argn("bcm2711-gic-strict", &strict, sizeof(strict));
		CHECK(!strict);
		IOLog("BCM2711 IRQ test: WARNING pending-clear bank isolation FAILED (QEMU GIC clears all CPU banks); use bcm2711-gic-strict=1 to require it\n");
	}
	CHECK(controller->setPending(ppis[1], 0, false) == kIOReturnSuccess);
	/* Set all banks after configuration, then acknowledge them without a
	 * clear-pending write in another bank. This still tests independent
	 * pending-set, enable, IAR acknowledgement and callback ownership.
	 */
	for (unsigned int cpu = 0; cpu < ml_get_cpu_count(); cpu++) {
		bindCPU(cpu);
		CHECK(ppis[cpu]->causeInterrupt(0) == kIOReturnSuccess);
		bool pending = false;
		CHECK(controller->getPending(ppis[cpu], 0, &pending) == kIOReturnSuccess && pending);
		CHECK(os_atomic_load(&tests[cpu].count, acquire) == 0);
	}
	for (unsigned int cpu = 0; cpu < ml_get_cpu_count(); cpu++) {
		bindCPU(cpu);
		CHECK(bcm2711_gic_is_pending(20));
		CHECK(ppis[cpu]->enableInterrupt(0) == kIOReturnSuccess);
		waitCount(&tests[cpu], 1);
		CHECK(ppis[cpu]->disableInterrupt(0) == kIOReturnSuccess);
		CHECK(!bcm2711_gic_is_enabled(20));
	}
	for (unsigned int cpu = 0; cpu < ml_get_cpu_count(); cpu++) {
		bindCPU(cpu);
		CHECK(ppis[cpu]->causeInterrupt(0) == kIOReturnSuccess);
		CHECK(controller->setPending(ppis[cpu], 0, false) == kIOReturnSuccess);
		CHECK(!bcm2711_gic_is_pending(20));
		CHECK(ppis[cpu]->unregisterInterrupt(0) == kIOReturnSuccess);
	}
	bindCPU(1);
	CHECK(ppis[0]->enableInterrupt(0) != kIOReturnSuccess); /* Wrong bank. */
	for (unsigned int cpu = 0; cpu < ml_get_cpu_count(); cpu++) {
		ppis[cpu]->release();
	}

	bcm2711_irq_spec spec = {200, kIOInterruptTypeEdge, 0x90, 0};
	IOService *nub = makeNub(name, spec);
	IOService *duplicate = makeNub(name, spec);
	TestInterrupt test = {};
	CHECK(nub->registerInterrupt(0, controller, testInterrupt, &test) == kIOReturnSuccess);
	CHECK(duplicate->registerInterrupt(0, controller, testInterrupt, &test) == kIOReturnNoResources);
	int type = -1;
	CHECK(nub->getInterruptType(0, &type) == kIOReturnSuccess && type == kIOInterruptTypeEdge);
	CHECK(nub->causeInterrupt(0) == kIOReturnSuccess);
	CHECK(nub->enableInterrupt(0) == kIOReturnSuccess);
	waitCount(&test, 1);
	CHECK(nub->disableInterrupt(0) == kIOReturnSuccess);
	CHECK(nub->causeInterrupt(0) == kIOReturnSuccess);
	IOSleep(20);
	CHECK(os_atomic_load(&test.count, acquire) == 1);
	CHECK(controller->setPending(nub, 0, false) == kIOReturnSuccess);
	CHECK(nub->enableInterrupt(0) == kIOReturnSuccess);
	IOSleep(20);
	CHECK(os_atomic_load(&test.count, acquire) == 1);
	CHECK(nub->unregisterInterrupt(0) == kIOReturnSuccess);

	/* Replacement owner; prove unregister does not return before an active
	 * handler has stopped accessing its context on CPU 0.
	 */
	test.slow = true;
	os_atomic_store(&test.finished, false, relaxed);
	CHECK(duplicate->registerInterrupt(0, controller, testInterrupt, &test) == kIOReturnSuccess);
	CHECK(duplicate->enableInterrupt(0) == kIOReturnSuccess);
	CHECK(duplicate->causeInterrupt(0) == kIOReturnSuccess);
	waitCount(&test, 2);
	CHECK(duplicate->unregisterInterrupt(0) == kIOReturnSuccess);
	CHECK(os_atomic_load(&test.finished, acquire));
	CHECK(!bcm2711_gic_is_enabled(200));

	/* The low-level unowned path must quarantine, even with pending set. */
	bcm2711_gic_stats before, after;
	bcm2711_gic_get_stats(&before);
	bcm2711_gic_unmask(200);
	bcm2711_gic_set_pending(200, true);
	IOSleep(20);
	bcm2711_gic_get_stats(&after);
	CHECK(after.unowned == before.unowned + 1);
	CHECK(!bcm2711_gic_is_enabled(200));
	CHECK(!bcm2711_gic_is_pending(200));
	IOSleep(20);
	bcm2711_gic_get_stats(&before);
	CHECK(before.unowned == after.unowned);

	/* Validate rejection before any hardware access. */
	const bcm2711_irq_spec invalid[] = {
		{1023, 1, 0xa0, 0}, {27, 1, 0xa0, 0}, {0, 0, 0x40, 0},
		{200, 2, 0xa0, 0}, {200, 1, 0xff, 0}, {200, 1, 0xa0, 8}
	};
	for (const auto &bad : invalid) {
		IOService *badNub = makeNub(name, bad);
		CHECK(badNub->registerInterrupt(0, controller, testInterrupt, &test) == kIOReturnBadArgument);
		badNub->release();
	}
	nub->release();
	duplicate->release();
	name->release();
	bcm2711_irq_test_bind_cpu(-1);
	IOLog("BCM2711 IRQ test: controller PASS (PPI delivery, SPI, pending, removal, unowned, invalid)\n");
}
#endif
