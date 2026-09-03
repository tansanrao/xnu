/*
 * Start the real libpthread kernel component in a monolithic kernel image.
 * Its registration must precede machine_lockdown(), which seals the shims'
 * function table.  pthread_init() still runs normally from bsd_init().
 */

#include <mach/kmod.h>
#include <kern/debug.h>
#include <kern/startup.h>

extern kern_return_t pthread_start(kmod_info_t *info, void *data);

__startup_func
static void
pthread_static_start(void)
{
	kern_return_t result = pthread_start(NULL, NULL);
	if (result != KERN_SUCCESS) {
		panic("static pthread component failed to start: %d", result);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, pthread_static_start);
