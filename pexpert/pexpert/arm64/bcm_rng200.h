/* RNG200 FIFO protocol shared by the loader and BCM2712 runtime.
 * Register interface: Broadcom's upstream iproc-rng200 driver.
 * No entropy estimate is inferred from the additional stuck-output checks.
 */
#ifndef BCM_RNG200_H
#define BCM_RNG200_H
#include <stdint.h>
#include <stdbool.h>

struct bcm_rng200 {
    void *cookie;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint32_t partial[4], previous[4];
    unsigned used;
    bool previous_valid, failed;
};

static inline void bcm_rng200_enable(struct bcm_rng200 *r, bool enable)
{
    uint32_t control = r->read(r->cookie, 0);
    r->write(r->cookie, 0, (control & ~0x1fffU) | (enable ? 1U : 0U));
}

static inline void bcm_rng200_reset(struct bcm_rng200 *r)
{
    bcm_rng200_enable(r, false);
    r->write(r->cookie, 0x18, UINT32_MAX); /* W1C interrupt status. */
    r->write(r->cookie, 8, r->read(r->cookie, 8) | 1U);
    r->write(r->cookie, 4, r->read(r->cookie, 4) | 1U);
    r->write(r->cookie, 4, r->read(r->cookie, 4) & ~1U);
    r->write(r->cookie, 8, r->read(r->cookie, 8) & ~1U);
    r->used = 0;
    r->failed = r->previous_valid = false;
    for (unsigned i = 0; i < 4; ++i) r->partial[i] = r->previous[i] = 0;
    bcm_rng200_enable(r, true);
}

/* 1: complete healthy block; 0: FIFO not ready; -1: sticky source failure.
 * At most four FIFO reads; never waits. Caller serializes this state.
 */
static inline int bcm_rng200_block(struct bcm_rng200 *r, uint32_t out[4])
{
    if (r->failed) return -1;
    for (unsigned i = r->used; i < 4; ++i) {
        if (r->read(r->cookie, 0x18) & 0x80000020U) goto failure;
        if (!(r->read(r->cookie, 0x24) & 0xffU)) return 0;
        r->partial[r->used++] = r->read(r->cookie, 0x20);
    }
    if (r->read(r->cookie, 0x18) & 0x80000020U) goto failure;
    uint32_t any = 0, all = UINT32_MAX, different = 0;
    for (unsigned i = 0; i < 4; ++i) {
        any |= r->partial[i]; all &= r->partial[i];
        different |= r->partial[i] ^ r->previous[i];
    }
    if (!any || all == UINT32_MAX || (r->previous_valid && !different)) goto failure;
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = r->previous[i] = r->partial[i];
        r->partial[i] = 0;
    }
    r->used = 0;
    r->previous_valid = true;
    return 1;
failure:
    r->failed = true;
    r->used = 0;
    for (unsigned i = 0; i < 4; ++i) r->partial[i] = r->previous[i] = 0;
    bcm_rng200_enable(r, false);
    return -1;
}
#endif
