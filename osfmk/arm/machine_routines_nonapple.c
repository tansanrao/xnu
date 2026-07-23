/*
 * Minimal machine routines for non-Apple ARM64 boards that use the classic
 * pmap without a secure monitor or a KDK-provided machine support archive.
 */

#include <machine/machine_routines.h>

void
ml_enable_monitor(void)
{
}

boolean_t
ml_device_is_prod_fused(void)
{
	return TRUE;
}
