/*
 * Raspberry Pi 4 static corecrypto provider bootstrap.
 *
 * Apple's normal kernel collection starts corecrypto as a root kext. The Pi 4
 * image has no kernel collection yet, so the same provider entry point is
 * linked into the kernel and invoked explicitly for the Phase 7 boot.
 */

#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kern_return_t corecrypto_kext_start(kmod_info_t *ki, void *data);
void rpi4_corecrypto_init(void);

void
rpi4_corecrypto_init(void)
{
	uint32_t enabled = 0;
	uint32_t fips_mode = 0;
	kmod_info_t corecrypto_kmod = {
		.info_version = KMOD_INFO_VERSION,
		.address = (vm_address_t)&_mh_execute_header,
	};

	printf("RPI4: CORECRYPTO PROBE\n");
	if (!PE_parse_boot_argn("rpi4_corecrypto", &enabled, sizeof(enabled)) ||
	    enabled == 0) {
		printf("RPI4: CORECRYPTO SKIP BOOTARG=0\n");
		return;
	}

	(void)PE_parse_boot_argn("fips_mode", &fips_mode, sizeof(fips_mode));
	printf("RPI4: CORECRYPTO START SOURCE=STATIC FIPS_MODE=%u\n", fips_mode);
	kern_return_t kr = corecrypto_kext_start(&corecrypto_kmod, NULL);
	if (kr != KERN_SUCCESS) {
		panic("RPI4: corecrypto provider failed: %d", kr);
	}
	printf("RPI4: CORECRYPTO READY FIPS_MODE=%u\n", fips_mode);
}
