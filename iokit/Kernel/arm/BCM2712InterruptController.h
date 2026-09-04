#ifndef _BCM2712_INTERRUPT_CONTROLLER_H_
#define _BCM2712_INTERRUPT_CONTROLLER_H_

#include <IOKit/IOInterruptController.h>
#include <kern/thread_call.h>
#include <arm64/bcm2712_interrupt.h>

class BCM2712InterruptController : public IOInterruptController
{
	OSDeclareDefaultStructors(BCM2712InterruptController);

	struct Source {
		IOInterruptVector vector;
		bcm2712_irq_spec spec;
		bool removing;
	};
	Source *sources;
	unsigned int sourceCount;
	thread_call_t reportCall;
	uint32_t reported[BCM2712_GIC_MAX_CPUS][32];
	uint32_t reportsPending[BCM2712_GIC_MAX_CPUS][32];
	static void reportUnowned(thread_call_param_t, thread_call_param_t);

	bool decode(IOService *, int, bcm2712_irq_spec *);
	Source *lookup(const bcm2712_irq_spec &);
	IOReturn change(IOService *, int, bool);
	static bool dispatch(void *, uint32_t);
	bool dispatchInterrupt(uint32_t);

public:
	bool start(IOService *) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerInterrupt(IOService *, int, void *, IOInterruptHandler, void *) APPLE_KEXT_OVERRIDE;
	IOReturn unregisterInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
	IOReturn getInterruptType(IOService *, int, int *) APPLE_KEXT_OVERRIDE;
	IOReturn enableInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
	IOReturn disableInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
	IOReturn causeInterrupt(IOService *, int) APPLE_KEXT_OVERRIDE;
	IOReturn setPending(IOService *, int, bool);
	IOReturn getPending(IOService *, int, bool *);
};

#endif
