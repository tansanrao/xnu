#ifndef _PEXPERT_ARM64_BCM2712_H
#define _PEXPERT_ARM64_BCM2712_H

/* Cortex-A76 baseline for the BCM2712 platform port. */
#define NO_MONITOR 1
#define NO_ECORE 1
#define BCM2712 1

#define __ARM_ARCH__ 8
#define __ARM_VMSA__ 8
#define __ARM_VFP__ 4
/* Cortex-A76 sets PAN on EL1 exception entry with XNU's SCTLR.SPAN=0.
 * Use XNU's existing copyio guards to access user memory deliberately. */
#define __ARM_PAN_AVAILABLE__ 1
#define __ARM_COHERENT_CACHE__ 1
/* Do not define __ARM_COHERENT_IO__: an MMU-off Cortex-A76 secondary
 * cannot observe dirty cache lines. XNU's cache-maintenance calls must
 * issue real PoC operations, including when publishing CPU reset data. */
#define __ARM_IC_NOALIAS_ICACHE__ 1
#define __ARM_DEBUG__ 7
#define __ARM_ENABLE_SWAP__ 1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1

#define ARM_ARCH_TIMER 1

/* Retain the port's 4 KiB granule; do not enable __ARM_16K_PG__. */
#ifndef ASSEMBLER
#define PL011_UART 1
#define PLATFORM_PANIC_LOG_DISABLED 1
#endif

#endif /* _PEXPERT_ARM64_BCM2712_H */
