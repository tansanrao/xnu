/*
 * Copyright (c) 2026. All rights reserved.
 */

#ifndef _PEXPERT_ARM64_BCM2711_H
#define _PEXPERT_ARM64_BCM2711_H

/*
 * QEMU raspi4b models BCM2711 with Cortex-A72 cores. Keep this header
 * deliberately small: it describes generic ARMv8.0 behaviour plus the PL011
 * UART used for early serial, without opting into Apple ARM64 platform code.
 */

#define NO_MONITOR                         1
#define BCM2711                            1

#define __ARM_ARCH__                       8
#define __ARM_VMSA__                       8
#define __ARM_VFP__                        4
#define __ARM_COHERENT_CACHE__             1
#define __ARM_COHERENT_IO__                1
#define __ARM_DEBUG__                      7
#define __ARM_ENABLE_SWAP__                1

#define ARM_ARCH_TIMER                     1

#ifndef ASSEMBLER
#define PL011_UART                         1
#endif /* !ASSEMBLER */

#endif /* !_PEXPERT_ARM64_BCM2711_H */
