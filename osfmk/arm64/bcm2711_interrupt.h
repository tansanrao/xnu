#ifndef _ARM64_BCM2711_INTERRUPT_H_
#define _ARM64_BCM2711_INTERRUPT_H_

#include <mach/boolean.h>
#include <mach/vm_types.h>
#include <stdint.h>
#include <stdbool.h>

#define BCM2711_GIC_MAX_CPUS 8U
#define BCM2711_GIC_MAX_INTIDS 1020U
#define BCM2711_GIC_IPI 0U
#define BCM2711_GIC_TIMER 27U

/* Native-endian Apple DT cells, not a Linux GIC interrupt specifier.
 * PPIs are owned independently on each logical CPU. Operations on a PPI must
 * execute on that CPU with migration disabled. SGIs and the timer are reserved.
 */
struct bcm2711_irq_spec {
	uint32_t intid;
	uint32_t type;       /* kIOInterruptTypeEdge or kIOInterruptTypeLevel */
	uint32_t priority;   /* 0x00..0xe0, multiples of 16; lower wins */
	uint32_t cpu;        /* logical CPU; SPI target or PPI bank */
};

struct bcm2711_gic_stats {
	uint64_t timer[BCM2711_GIC_MAX_CPUS];
	uint64_t ipi[BCM2711_GIC_MAX_CPUS];
	uint64_t spurious;
	uint64_t unowned;
	uint32_t validated_cpus;
};

#ifdef __cplusplus
extern "C" {
#endif

void bcm2711_gic_init(vm_offset_t, vm_offset_t);
void bcm2711_gic_init_cpu(void);
void bcm2711_gic_send_ipi(uint32_t physical_cpu);
uint32_t bcm2711_gic_ipi_seen_mask(void);
uint32_t bcm2711_gic_intid_count(void);
uint32_t bcm2711_gic_current_cpu(void);
void bcm2711_gic_get_stats(struct bcm2711_gic_stats *);
void bcm2711_gic_validate_cpu(void);
#if DEVELOPMENT || DEBUG
void bcm2711_irq_test_bind_cpu(int cpu);
void bcm2711_irq_test_local_timer(void);
#endif

/* Installed once; the built-in IOKit controller has kernel lifetime. */
typedef bool (*bcm2711_irq_dispatch_t)(void *, uint32_t);
void bcm2711_gic_set_dispatch(bcm2711_irq_dispatch_t, void *);

/* Controller lock + local IRQ masking serialize all configuration changes.
 * These primitives do not touch SGIs or the reserved timer through IOKit.
 */
bool bcm2711_gic_configure(const struct bcm2711_irq_spec *);
void bcm2711_gic_mask(uint32_t);
void bcm2711_gic_unmask(uint32_t);
void bcm2711_gic_set_pending(uint32_t, bool);
bool bcm2711_gic_is_pending(uint32_t);
bool bcm2711_gic_is_enabled(uint32_t);

#ifdef __cplusplus
}
#endif
#endif
