#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_BCM2712)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/arm/protos.h>

class BCM2712Serial : public IOService
{
	OSDeclareDefaultStructors(BCM2712Serial);
	bool installed;
	static void interrupt(OSObject *, void *, IOService *, int);
public:
	bool start(IOService *) APPLE_KEXT_OVERRIDE;
	void stop(IOService *) APPLE_KEXT_OVERRIDE;
};

#define super IOService
OSDefineMetaClassAndStructors(BCM2712Serial, IOService)

bool
BCM2712Serial::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	if (provider->registerInterrupt(0, this, interrupt) != kIOReturnSuccess) {
		return false;
	}
	installed = true;
	if (provider->enableInterrupt(0) != kIOReturnSuccess ||
	    serial_irq_enable(SERIAL_PL011_UART) != KERN_SUCCESS) {
		provider->unregisterInterrupt(0);
		installed = false;
		IOLog("BCM2712Serial: RX IRQ unavailable, using console polling\n");
	} else {
		IOLog("BCM2712Serial: PL011 RX/timeout IRQ ready\n");
	}
	registerService();
	return true;
}

void
BCM2712Serial::interrupt(__unused OSObject *target, __unused void *refCon,
    __unused IOService *provider, __unused int source)
{
	/* Mask and acknowledge in primary context. The serial keyboard thread
	 * drains RX and takes the tty lock; no tty work on the IRQ stack.
	 */
	if (serial_irq_filter(SERIAL_PL011_UART)) {
		serial_irq_action(SERIAL_PL011_UART);
	}
}

void
BCM2712Serial::stop(IOService *provider)
{
	if (installed) {
		serial_rx_irq_disable();
		provider->unregisterInterrupt(0);
		installed = false;
	}
	super::stop(provider);
}
#endif
