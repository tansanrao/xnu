#ifndef BCM2712_ENTROPY_H
#define BCM2712_ENTROPY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t bcm2712_entropy_batches(void);
void bcm2712_entropy_start(void);
int bcm2712_entropy_read(uint8_t seed[64]);
#ifdef __cplusplus
}
#endif
#endif
