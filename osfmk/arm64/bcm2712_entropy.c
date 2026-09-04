/* BCM2712 hardware entropy; only health-checked complete batches leave here. */
#include <arm64/bcm2712_entropy.h>
#include <pexpert/arm64/bcm_rng200.h>
#ifdef BCM_ENTROPY_HOST_TEST
#include "bcm2712-entropy-host.h"
#else
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/debug.h>
#include <corecrypto/cc.h>
#include <os/atomic_private.h>
#include <kern/misc_protos.h>
#include <libsa/string.h>

#endif

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
LCK_GRP_DECLARE(rng_group, "bcm2712-entropy");
LCK_MTX_DECLARE(rng_lock, &rng_group);
static struct bcm_rng200 rng;
static _Atomic bool ready;
static uint8_t pending[64];
static unsigned used;
static uint64_t next_read, interval, empty_since;
static uint64_t batches;

uint64_t bcm2712_entropy_batches(void)
{
    lck_mtx_lock(&rng_lock);
    uint64_t value = batches;
    lck_mtx_unlock(&rng_lock);
    return value;
}

#ifdef BCM_ENTROPY_HOST_TEST
#define rng_read entropy_test_read
#define rng_write entropy_test_write
#else
static uint32_t rng_read(void *cookie, unsigned offset)
{
    __asm__ volatile("dmb sy" ::: "memory");
    uint32_t value = *(volatile uint32_t *)((uintptr_t)cookie + offset);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
static void rng_write(void *cookie, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)((uintptr_t)cookie + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
void bcm2712_entropy_start(void)
{
    vm_offset_t base = ml_io_map(0x107d208000ULL, 0x28);
    if (!base) panic("BCM2712 RNG200 mapping failed");
    rng.cookie = (void *)base;
    rng.read = rng_read;
    rng.write = rng_write;
    /* Preserve loader initialization and latched health faults; never reset here. */
    if ((rng_read(rng.cookie, 0) & 0x1fffU) != 1 ||
        (rng_read(rng.cookie, 0x18) & 0x80000020U)) {
        panic("BCM2712 RNG200 lost healthy loader state");
    }
    clock_interval_to_absolutetime_interval(1, NSEC_PER_SEC, &interval);
    os_atomic_store(&ready, true, release);
    printf("BCM2712 entropy: RNG200 runtime source ready\n");
}

int bcm2712_entropy_read(uint8_t seed[64])
{
    if (!os_atomic_load(&ready, acquire) || !lck_mtx_try_lock(&rng_lock)) return 0;
    uint64_t now = mach_absolute_time();
    if (now < next_read) { lck_mtx_unlock(&rng_lock); return 0; }
    uint32_t block[4] = {0};
    while (used < sizeof(pending)) {
        int result = bcm_rng200_block(&rng, block);
        if (result < 0) {
            cc_clear(sizeof(block), block);
            cc_clear(sizeof(pending), pending);
            lck_mtx_unlock(&rng_lock);
            panic("BCM2712 RNG200 health failure; refusing entropy");
        }
        if (!result) break;
        memcpy(pending + used, block, sizeof(block));
        used += sizeof(block);
    }
    int complete = used == sizeof(pending);
    if (complete) {
        memcpy(seed, pending, sizeof(pending));
        cc_clear(sizeof(pending), pending);
        used = 0;
        next_read = now + interval;
        batches++;
        empty_since = 0;
    }
    if (!complete) {
        if (!empty_since) empty_since = now;
        else if (now - empty_since > 5 * interval) {
            cc_clear(sizeof(block), block);
            cc_clear(sizeof(pending), pending);
            bcm_rng200_enable(&rng, false);
            lck_mtx_unlock(&rng_lock);
            panic("BCM2712 RNG200 FIFO stalled; refusing entropy");
        }
    }
    cc_clear(sizeof(block), block);
    lck_mtx_unlock(&rng_lock);
    return complete;
}
