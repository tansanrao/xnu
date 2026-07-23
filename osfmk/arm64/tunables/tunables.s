/*
 * The open-source ARM64 start path expects a per-SoC register-tunables macro.
 * Raspberry Pi 4 uses architectural Cortex-A72 registers only, so there are
 * no Apple HID/EHID or implementation-defined tunables to apply here.
 */

#if !defined(ARM64_BOARD_CONFIG_RPI4)
#error This no-op tunables implementation is only valid for RPI4
#endif

.macro APPLY_TUNABLES midr, tmp1, tmp2
.endmacro
