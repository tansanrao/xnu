/*
 * Raspberry Pi 4 / Cortex-A72 architectural configuration.
 *
 * Keep this header limited to architectural facts. In particular, it must
 * not inherit Apple SoC, AIC, monitor, pointer-authentication, or virtual
 * platform definitions.
 */

#ifndef _PEXPERT_ARM64_RPI4_H
#define _PEXPERT_ARM64_RPI4_H

#define RPI4                                   1
#define NO_MONITOR                             1

#define __ARM_ARCH__                           8
#define __ARM_VMSA__                           8
#define __ARM_VFP__                            4
#define __ARM_COHERENT_CACHE__                 1
#define __ARM_IC_NOALIAS_ICACHE__              1
#define __ARM_DEBUG__                          7
#define __ARM_V8_CRYPTO_EXTENSIONS__           1

#define ARM_ARCH_TIMER
#define HAS_FAST_CNTVCT                        1
#define ARM_VM_PAGE_SIZE_FIXED_TO_HW           1

#ifndef ASSEMBLER
#define PL011_UART
#define PLATFORM_PANIC_LOG_DISABLED
#endif /* !ASSEMBLER */

#endif /* _PEXPERT_ARM64_RPI4_H */
