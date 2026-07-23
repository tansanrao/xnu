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

/*
 * The 39-bit TTBR1 layout leaves substantially less allocatable kernel VA
 * than a large-memory macOS target.  Use the existing constrained-platform
 * zone sizing while retaining the macOS userspace ABI.
 */
#define CONFIG_ZONE_MAP_MAX                    (8ULL << 30)
#define CONFIG_ZONE_MAP_VA_SIZE                (24ULL << 30)

/*
 * SMP is deliberately deferred until interrupt delivery is stable.  Keep the
 * hardware maximum at four CPUs, but fail explicitly if a secondary start is
 * attempted during the uniprocessor bring-up.
 */
#define RPI4_UP_ONLY                           1
#endif /* !ASSEMBLER */

#endif /* _PEXPERT_ARM64_RPI4_H */
