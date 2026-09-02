#ifndef _PEXPERT_ARM64_BCM2711_H
#define _PEXPERT_ARM64_BCM2711_H

#define NO_MONITOR 1
#define NO_ECORE 1
#define BCM2711 1

#define __ARM_ARCH__ 8
#define __ARM_VMSA__ 8
#define __ARM_VFP__ 4
#define __ARM_COHERENT_CACHE__ 1
#define __ARM_COHERENT_IO__ 1
#define __ARM_IC_NOALIAS_ICACHE__ 1
#define __ARM_DEBUG__ 7
#define __ARM_ENABLE_SWAP__ 1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1

#define ARM_ARCH_TIMER 1

#ifndef ASSEMBLER
#define PL011_UART 1
#define PLATFORM_PANIC_LOG_DISABLED 1
#endif

#endif /* _PEXPERT_ARM64_BCM2711_H */
