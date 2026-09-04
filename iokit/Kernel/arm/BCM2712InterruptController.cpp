#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_BCM2712)
#include "BCM2712InterruptController.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <machine/machine_routines.h>

#define super IOInterruptController
OSDefineMetaClassAndStructors(BCM2712InterruptController, IOInterruptController)

bool
BCM2712InterruptController::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	sourceCount = bcm2712_gic_intid_count() + 16 * BCM2712_GIC_MAX_CPUS;
	sources = IONewZero(Source, sourceCount);
	controllerLock = IOSimpleLockAlloc();
	reportCall = thread_call_allocate(reportUnowned, this);
	const OSSymbol *name = IODTInterruptControllerName(provider);
	if (!sources || !controllerLock || !reportCall || !name) {
		OSSafeReleaseNULL(name);
		return false;
	}
	/* No reset here: boot timers and CPU startup already use the GIC. */
	bcm2712_gic_set_dispatch(dispatch, this);
	getPlatform()->registerInterruptController(const_cast<OSSymbol *>(name), this);
	name->release();
	retain(); /* Machine IRQ callback has kernel lifetime, as do the CPU drivers. */
	registerService();
	IOLog("BCM2712GIC: registered %u INTIDs, per-CPU PPI banks\n",
	    bcm2712_gic_intid_count());
	return true;
}

void
BCM2712InterruptController::free(void)
{
	if (reportCall) {
		thread_call_free(reportCall);
	}
	if (sources) {
		IODelete(sources, Source, sourceCount);
	}
	if (controllerLock) {
		IOSimpleLockFree(controllerLock);
	}
	super::free();
}

bool
BCM2712InterruptController::decode(IOService *nub, int source, bcm2712_irq_spec *spec)
{
	if (!nub || source < 0 || source >= nub->_numInterruptSources || !nub->_interruptSources) {
		return false;
	}
	const IOInterruptSource &entry = nub->_interruptSources[source];
	OSData *data = entry.vectorData;
	if (entry.interruptController != this || !data || data->getLength() != sizeof(*spec)) {
		return false;
	}
	bcopy(data->getBytesNoCopy(), spec, sizeof(*spec));
	return spec->intid >= 16 && spec->intid < bcm2712_gic_intid_count() &&
	       spec->intid != BCM2712_GIC_TIMER &&
	       (spec->type == kIOInterruptTypeEdge || spec->type == kIOInterruptTypeLevel) &&
	       spec->priority <= 0xe0 && (spec->priority & 0xf) == 0 &&
	       spec->cpu < ml_get_cpu_count() && spec->cpu < BCM2712_GIC_MAX_CPUS;
}

BCM2712InterruptController::Source *
BCM2712InterruptController::lookup(const bcm2712_irq_spec &spec)
{
	if (spec.intid < 32) {
		/* Called with the controller lock and IRQs masked: bank selection
		 * cannot change under us. Remote PPI operations must be cross-called.
		 */
		if (spec.cpu != bcm2712_gic_current_cpu()) {
			return nullptr;
		}
		return &sources[bcm2712_gic_intid_count() + spec.cpu * 16 + spec.intid - 16];
	}
	return &sources[spec.intid];
}

IOReturn
BCM2712InterruptController::registerInterrupt(IOService *nub, int source,
    void *target, IOInterruptHandler handler, void *refCon)
{
	bcm2712_irq_spec spec;
	if (!handler || !decode(nub, source, &spec)) {
		return kIOReturnBadArgument;
	}
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	Source *slot = lookup(spec);
	IOReturn result = kIOReturnSuccess;
	if (!slot) {
		result = kIOReturnNotPermitted;
	} else if (slot->vector.interruptRegistered || slot->removing) {
		result = kIOReturnNoResources; /* Exclusive INTID ownership. */
	} else if (!bcm2712_gic_configure(&spec)) {
		result = kIOReturnUnsupported;
	} else {
		slot->spec = spec;
		IOInterruptVector &v = slot->vector;
		v.nub = nub;
		v.source = source;
		v.target = target;
		v.handler = handler;
		v.refCon = refCon;
		v.interruptDisabledSoft = v.interruptDisabledHard = 1;
		v.interruptRegistered = 1;
	}
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	return result;
}

IOReturn
BCM2712InterruptController::unregisterInterrupt(IOService *nub, int source)
{
	bcm2712_irq_spec spec;
	if (!decode(nub, source, &spec)) {
		return kIOReturnBadArgument;
	}
	/* Waiting on our own interrupt stack would deadlock. */
	if (getPlatform()->atInterruptLevel() || !ml_get_interrupts_enabled()) {
		return kIOReturnNotPermitted;
	}
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	Source *slot = lookup(spec);
	if (!slot || slot->vector.nub != nub || slot->vector.source != source || slot->removing) {
		IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
		return kIOReturnNoInterrupt;
	}
	slot->removing = true;
	slot->vector.interruptDisabledSoft = slot->vector.interruptDisabledHard = 1;
	bcm2712_gic_mask(spec.intid);
	bcm2712_gic_set_pending(spec.intid, false);
	/* The dispatch epilogue only touches this slot under the same lock.
	 * Keep ownership until it has finished using the client's target/refCon.
	 */
	while (slot->vector.interruptActive) {
		IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
		IOSleep(1);
		state = IOSimpleLockLockDisableInterrupt(controllerLock);
	}
	bzero(slot, sizeof(*slot));
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	return kIOReturnSuccess;
}

IOReturn
BCM2712InterruptController::change(IOService *nub, int source, bool enabled)
{
	bcm2712_irq_spec spec;
	if (!decode(nub, source, &spec)) {
		return kIOReturnBadArgument;
	}
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	Source *slot = lookup(spec);
	if (!slot || slot->vector.nub != nub || slot->vector.source != source || slot->removing) {
		IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
		return kIOReturnNoInterrupt;
	}
	slot->vector.interruptDisabledSoft = !enabled;
	if (!enabled) {
		bcm2712_gic_mask(spec.intid);
		slot->vector.interruptDisabledHard = 1;
	} else if (!slot->vector.interruptActive) {
		bcm2712_gic_unmask(spec.intid);
		slot->vector.interruptDisabledHard = 0;
	}
	/* Like the base controller, disabling from thread context waits out an
	 * in-flight callback. An IRQ handler may disable its own source.
	 */
	if (!enabled && state && !getPlatform()->atInterruptLevel()) {
		while (slot->vector.interruptActive && slot->vector.nub == nub &&
		    slot->vector.source == source) {
			IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
			IOSleep(1);
			state = IOSimpleLockLockDisableInterrupt(controllerLock);
		}
	}
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	return kIOReturnSuccess;
}

IOReturn BCM2712InterruptController::enableInterrupt(IOService *nub, int source)
{
	return change(nub, source, true);
}
IOReturn BCM2712InterruptController::disableInterrupt(IOService *nub, int source)
{
	return change(nub, source, false);
}

IOReturn
BCM2712InterruptController::getInterruptType(IOService *nub, int source, int *type)
{
	bcm2712_irq_spec spec;
	if (!type || !decode(nub, source, &spec)) {
		return kIOReturnBadArgument;
	}
	*type = spec.type;
	return kIOReturnSuccess;
}

IOReturn
BCM2712InterruptController::setPending(IOService *nub, int source, bool pending)
{
	bcm2712_irq_spec spec;
	if (!decode(nub, source, &spec)) {
		return kIOReturnBadArgument;
	}
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	Source *slot = lookup(spec);
	IOReturn result = kIOReturnNoInterrupt;
	if (slot && slot->vector.nub == nub && slot->vector.source == source && !slot->removing) {
		bcm2712_gic_set_pending(spec.intid, pending);
		result = kIOReturnSuccess;
	}
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	return result;
}

IOReturn
BCM2712InterruptController::getPending(IOService *nub, int source, bool *pending)
{
	bcm2712_irq_spec spec;
	if (!pending || !decode(nub, source, &spec)) {
		return kIOReturnBadArgument;
	}
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	Source *slot = lookup(spec);
	IOReturn result = kIOReturnNoInterrupt;
	if (slot && slot->vector.nub == nub && slot->vector.source == source && !slot->removing) {
		*pending = bcm2712_gic_is_pending(spec.intid);
		result = kIOReturnSuccess;
	}
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	return result;
}

IOReturn BCM2712InterruptController::causeInterrupt(IOService *nub, int source)
{
	return setPending(nub, source, true);
}

bool BCM2712InterruptController::dispatch(void *target, uint32_t intid)
{
	return static_cast<BCM2712InterruptController *>(target)->dispatchInterrupt(intid);
}

bool
BCM2712InterruptController::dispatchInterrupt(uint32_t intid)
{
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
	bcm2712_irq_spec spec = { intid, 0, 0, bcm2712_gic_current_cpu() };
	Source *slot = lookup(spec);
	IOInterruptVector &v = slot->vector;
	bcm2712_gic_mask(intid);
	v.interruptDisabledHard = 1;
	if (!v.interruptRegistered) {
		/* Quarantine under the ownership lock, so a concurrent registration
		 * cannot be masked after it has installed and enabled its handler.
		 */
		bcm2712_gic_set_pending(intid, false);
		uint32_t cpu = bcm2712_gic_current_cpu();
		uint32_t word = intid / 32, bit = 1U << (intid % 32);
		if (!(reported[cpu][word] & bit)) {
			reported[cpu][word] |= bit;
			reportsPending[cpu][word] |= bit;
			thread_call_enter(reportCall);
		}
		IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
		return false;
	}
	if (v.interruptDisabledSoft || slot->removing) {
		/* An IAR already in flight can arrive after disable. Leave masked;
		 * preserve any new pending edge for the next enable.
		 */
		IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
		return true;
	}
	v.interruptActive = 1;
	IOInterruptHandler handler = v.handler;
	void *target = v.target;
	void *refCon = v.refCon;
	IOService *nub = v.nub;
	int source = v.source;
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);

	handler(target, refCon, nub, source);

	state = IOSimpleLockLockDisableInterrupt(controllerLock);
	v.interruptActive = 0;
	if (!v.interruptDisabledSoft && !slot->removing) {
		v.interruptDisabledHard = 0;
		bcm2712_gic_unmask(intid);
	}
	IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
	return true;
}
void
BCM2712InterruptController::reportUnowned(thread_call_param_t target,
    __unused thread_call_param_t unused)
{
	auto controller = static_cast<BCM2712InterruptController *>(target);
	for (uint32_t cpu = 0; cpu < BCM2712_GIC_MAX_CPUS; cpu++) {
		for (uint32_t word = 0; word < 32; word++) {
			IOInterruptState state = IOSimpleLockLockDisableInterrupt(controller->controllerLock);
			uint32_t bits = controller->reportsPending[cpu][word];
			controller->reportsPending[cpu][word] = 0;
			IOSimpleLockUnlockEnableInterrupt(controller->controllerLock, state);
			while (bits) {
				uint32_t bit = (uint32_t)__builtin_ctz(bits);
				bits &= bits - 1;
				IOLog("BCM2712GIC: masked unowned INTID %u on CPU %u\n", word * 32 + bit, cpu);
			}
		}
	}
}
#endif /* ARM64_BOARD_CONFIG_BCM2712 */
