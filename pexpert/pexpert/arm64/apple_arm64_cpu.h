/*
 * Copyright (c) 2023 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#pragma once

#if !defined(__ASSEMBLER__)

#include <stdbool.h>
#include <sys/cdefs.h>

#if defined(APPLE_ARM64_ARCH_FAMILY)
#include <pexpert/arm64/apple_arm64_regs.h>
#endif

/**
 * Defines the core type of the executing CPU.
 */
__enum_closed_decl(arm64_core_type_t, unsigned int, {
	E_CORE = 0,
	P_CORE = 1,
});

/*
 * Get the core type of the executing CPU.
 *
 * @return Whether the executing CPU is an E-core, P-core, or non-PE core.
 */
static inline arm64_core_type_t
arm64_core_type(void)
{
#if defined(__arm64__) && defined(APPLE_ARM64_ARCH_FAMILY)
	return (arm64_core_type_t)((__builtin_arm_rsr64("MPIDR_EL1") >> MPIDR_CORETYPE_SHIFT) & MPIDR_CORETYPE_MASK);
#else
	/**
	 * Non-Apple ARM platforms do not encode Apple E/P core type in MPIDR_EL1.
	 * The SPTM userspace test build can also include this header on non-arm64
	 * hosts where `__builtin_arm_rsr64()` does not exist.
	 */
	return P_CORE;
#endif /* defined(__arm64__) && defined(APPLE_ARM64_ARCH_FAMILY) */
}

/*
 * Convenience wrapper around arm64_core_type() which determines whether the
 * executing CPU is an E-core.
 *
 * @return Whether the executing CPU is an E-core.
 */
static inline bool
arm64_is_e_core(void)
{
	return arm64_core_type() == E_CORE;
}


/*
 * Convenience wrapper around arm64_core_type() which determines whether the
 * executing CPU is a P-core.
 *
 * @return Whether the executing CPU is a P-core.
 */
static inline bool
arm64_is_p_core(void)
{
	return arm64_core_type() == P_CORE;
}

/*
 * Convert a core type to a printable string.
 *
 * @param type The core type to convert.
 *
 * @return String describing whether the given core type corresponds to an
 *         E-core, P-core, or non-PE core.
 */
static inline const char *
arm64_core_type_to_string(arm64_core_type_t core_type)
{
	switch (core_type) {
	case E_CORE:
		return "E-core";
	case P_CORE:
		return "P-core";
	default:
		return "<< UNKNOWN OR INVALID CORE TYPE >>";
	}
}

/*
 * Convenience wrapper around arm64_core_type_to_string() which gets a printable
 * string describing the core type of the executing CPU.
 *
 * @return String describing whether the executing CPU is an E-core, P-core,
 *         or non-PE core.
 */
static inline const char *
arm64_core_type_as_string(void)
{
	return arm64_core_type_to_string(arm64_core_type());
}

#endif /* !defined(__ASSEMBLER__) */
