#ifndef _ARM64_BCM2712_INTERRUPT_H_
#define _ARM64_BCM2712_INTERRUPT_H_

#include <mach/boolean.h>
#include <mach/vm_types.h>
#include <stdint.h>
#include <stdbool.h>

#define BCM2712_GIC_MAX_CPUS 8U
#define BCM2712_GIC_MAX_INTIDS 1020U
#define BCM2712_GIC_IPI 0U
#define BCM2712_GIC_TIMER 27U

/* Native-endian Apple DT cells, not a Linux GIC interrupt specifier.
 * PPIs are owned independently on each logical CPU. Operations on a PPI must
 * execute on that CPU with migration disabled. SGIs and the timer are reserved.
 */
struct bcm2712_irq_spec {
	uint32_t intid;
	uint32_t type;       /* kIOInterruptTypeEdge or kIOInterruptTypeLevel */
	uint32_t priority;   /* 0x00..0xe0, multiples of 16; lower wins */
	uint32_t cpu;        /* logical CPU; SPI target or PPI bank */
};

struct bcm2712_gic_stats {
	uint64_t timer[BCM2712_GIC_MAX_CPUS];
	uint64_t ipi[BCM2712_GIC_MAX_CPUS];
	uint64_t spurious;
	uint64_t unowned;
	uint32_t validated_cpus;
};

#ifdef __cplusplus
extern "C" {
#endif

void bcm2712_gic_init(vm_offset_t, vm_offset_t);
void bcm2712_gic_init_cpu(void);
void bcm2712_gic_send_ipi(uint32_t physical_cpu);
int64_t bcm2712_psci_cpu_on(uint64_t physical_cpu, uint64_t entry,
    uint64_t context);
int64_t bcm2712_psci_system_reset(void);
uint32_t bcm2712_gic_ipi_seen_mask(void);
uint32_t bcm2712_gic_intid_count(void);
uint32_t bcm2712_gic_current_cpu(void);
void bcm2712_gic_get_stats(struct bcm2712_gic_stats *);
void bcm2712_gic_validate_cpu(void);
#if DEVELOPMENT || DEBUG
void bcm2712_irq_test_bind_cpu(int cpu);
void bcm2712_irq_test_local_timer(void);
#endif

/* Installed once; the built-in IOKit controller has kernel lifetime. */
typedef bool (*bcm2712_irq_dispatch_t)(void *, uint32_t);
void bcm2712_gic_set_dispatch(bcm2712_irq_dispatch_t, void *);

/* Controller lock + local IRQ masking serialize all configuration changes.
 * These primitives do not touch SGIs or the reserved timer through IOKit.
 */
bool bcm2712_gic_configure(const struct bcm2712_irq_spec *);
void bcm2712_gic_mask(uint32_t);
void bcm2712_gic_unmask(uint32_t);
void bcm2712_gic_set_pending(uint32_t, bool);
bool bcm2712_gic_is_pending(uint32_t);
bool bcm2712_gic_is_enabled(uint32_t);

#ifdef __cplusplus
}
#endif
#endif
