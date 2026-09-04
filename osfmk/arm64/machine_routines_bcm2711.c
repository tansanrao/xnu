#include <arm/cpu_data_internal.h>
#include <arm/rtclock.h>
#include <arm64/bcm2711_interrupt.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <IOKit/IOInterrupts.h>
#if DEVELOPMENT || DEBUG
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <kern/timer_call.h>
#endif

#define GICD_CTLR       0x000
#define GICD_TYPER      0x004
#define GICD_IGROUPR    0x080
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_ISPENDR    0x200
#define GICD_ICPENDR    0x280
#define GICD_ICACTIVER  0x380
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICD_ICFGR      0xc00
#define GICD_SGIR       0xf00
#define GICD_CPENDSGIR  0xf10
#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_BPR        0x008
#define GICC_IAR        0x00c
#define GICC_EOIR       0x010

void bcm2711_gic_set_ipi_handler(ipi_handler_t handler);

static vm_offset_t gic_distributor_base;
static vm_offset_t gic_cpu_interface_base;
static _Atomic(ipi_handler_t) gic_ipi_handler;
static _Atomic(bcm2711_irq_dispatch_t) gic_dispatch;
static void *gic_dispatch_target;
static uint32_t gic_intids;
static _Atomic uint32_t gic_ipi_seen;
static _Atomic uint32_t gic_validated;
static _Atomic uint32_t gic_targets[BCM2711_GIC_MAX_CPUS];
static _Atomic uint64_t gic_timer_count[BCM2711_GIC_MAX_CPUS];
static _Atomic uint64_t gic_ipi_count[BCM2711_GIC_MAX_CPUS];
static _Atomic uint64_t gic_spurious;
static _Atomic uint64_t gic_unowned;

static inline uint32_t
gic_read32(vm_offset_t base, vm_offset_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
gic_write32(vm_offset_t base, vm_offset_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

static inline void
gic_sync(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
gic_bit_write(uint32_t reg, uint32_t intid)
{
	gic_write32(gic_distributor_base, reg + (intid / 32) * 4,
	    1U << (intid % 32));
	gic_sync();
}

uint32_t bcm2711_gic_intid_count(void) { return gic_intids; }
uint32_t bcm2711_gic_current_cpu(void) { return getCpuDatap()->cpu_number; }
void bcm2711_gic_mask(uint32_t intid) { gic_bit_write(GICD_ICENABLER, intid); }
void bcm2711_gic_unmask(uint32_t intid) { gic_bit_write(GICD_ISENABLER, intid); }

void
bcm2711_gic_set_pending(uint32_t intid, bool pending)
{
	gic_bit_write(pending ? GICD_ISPENDR : GICD_ICPENDR, intid);
}

bool
bcm2711_gic_is_pending(uint32_t intid)
{
	return (gic_read32(gic_distributor_base, GICD_ISPENDR + (intid / 32) * 4) &
	       (1U << (intid % 32))) != 0;
}

bool
bcm2711_gic_is_enabled(uint32_t intid)
{
	return (gic_read32(gic_distributor_base, GICD_ISENABLER + (intid / 32) * 4) &
	       (1U << (intid % 32))) != 0;
}

bool
bcm2711_gic_configure(const struct bcm2711_irq_spec *spec)
{
	uint32_t target = os_atomic_load(&gic_targets[spec->cpu], acquire);
	if (target == 0) {
		return false;
	}
	uint32_t intid = spec->intid;
	bcm2711_gic_mask(intid);
	bcm2711_gic_set_pending(intid, false);
	uint32_t group_reg = GICD_IGROUPR + (intid / 32) * 4;
	gic_write32(gic_distributor_base, group_reg,
	    gic_read32(gic_distributor_base, group_reg) | (1U << (intid % 32)));
	*(volatile uint8_t *)(gic_distributor_base + GICD_IPRIORITYR + intid) = (uint8_t)spec->priority;
	if (intid >= 32) {
		*(volatile uint8_t *)(gic_distributor_base + GICD_ITARGETSR + intid) = (uint8_t)target;
	}
	uint32_t reg = GICD_ICFGR + (intid / 16) * 4;
	uint32_t bit = 2U << (2 * (intid % 16));
	uint32_t config = gic_read32(gic_distributor_base, reg) & ~bit;
	if (spec->type == kIOInterruptTypeEdge) {
		config |= bit;
	}
	gic_write32(gic_distributor_base, reg, config);
	gic_sync();
	/* Some PPIs have a fixed trigger mode. Never claim a rejected change. */
	return (gic_read32(gic_distributor_base, reg) & bit) == (config & bit) &&
	       *(volatile uint8_t *)(gic_distributor_base + GICD_IPRIORITYR + intid) == (uint8_t)spec->priority;
}

void
bcm2711_gic_set_dispatch(bcm2711_irq_dispatch_t dispatch, void *target)
{
	if (os_atomic_load(&gic_dispatch, acquire) != NULL) {
		panic("BCM2711: duplicate GIC controller");
	}
	gic_dispatch_target = target;
	os_atomic_store(&gic_dispatch, dispatch, release);
}

static void
bcm2711_gic_handle_irq(__unused void *target, __unused void *refCon,
    __unused void *nub, __unused int source)
{
	uint32_t iar = gic_read32(gic_cpu_interface_base, GICC_IAR);
	uint32_t intid = iar & 0x3ffU;
	uint32_t cpu = bcm2711_gic_current_cpu();

	if (intid == 1023) {
		os_atomic_inc(&gic_spurious, relaxed);
		return; /* No acknowledgement, so no EOI. */
	}
	if (intid >= BCM2711_GIC_MAX_INTIDS || intid >= gic_intids) {
		/* 1022 is a wrong-group response, not an innocuous spurious IRQ. */
		panic("BCM2711: invalid/special GIC IAR 0x%x on CPU %u", iar, cpu);
	}
	if (intid == BCM2711_GIC_TIMER) {
		os_atomic_inc(&gic_timer_count[cpu], relaxed);
		getCpuDatap()->cpu_decrementer = (uint32_t)-1;
		rtclock_intr(TRUE);
	} else if (intid == BCM2711_GIC_IPI) {
		ipi_handler_t handler = os_atomic_load(&gic_ipi_handler, acquire);
		if (handler == NULL) {
			panic("BCM2711: IPI arrived before handler registration");
		}
		os_atomic_inc(&gic_ipi_count[cpu], relaxed);
		os_atomic_or(&gic_ipi_seen, 1U << cpu, relaxed);
		handler();
	} else {
		bcm2711_irq_dispatch_t dispatch = os_atomic_load(&gic_dispatch, acquire);
		if (intid < 16) {
			panic("BCM2711: unowned SGI %u on CPU %u", intid, cpu);
		}
		if (dispatch == NULL || !dispatch(gic_dispatch_target, intid)) {
			/* A level source can remain asserted: clearing pending alone is
			 * insufficient. Quarantine it before deactivation, report once.
			 */
			if (dispatch == NULL) {
				bcm2711_gic_mask(intid);
				bcm2711_gic_set_pending(intid, false);
				panic("BCM2711: unowned INTID %u before IOKit dispatch", intid);
			} /* IOKit quarantines and defers its diagnostic under the ownership lock. */
			os_atomic_inc(&gic_unowned, relaxed);
		}
	}

	/* Complete peripheral stores before EOI; preserve SGI source-CPU bits. */
	gic_sync();
	gic_write32(gic_cpu_interface_base, GICC_EOIR, iar);
}

uint32_t
bcm2711_gic_ipi_seen_mask(void)
{
	return os_atomic_load(&gic_ipi_seen, relaxed);
}

void
bcm2711_gic_get_stats(struct bcm2711_gic_stats *stats)
{
	for (unsigned int cpu = 0; cpu < BCM2711_GIC_MAX_CPUS; cpu++) {
		stats->timer[cpu] = os_atomic_load(&gic_timer_count[cpu], relaxed);
		stats->ipi[cpu] = os_atomic_load(&gic_ipi_count[cpu], relaxed);
	}
	stats->spurious = os_atomic_load(&gic_spurious, relaxed);
	stats->unowned = os_atomic_load(&gic_unowned, relaxed);
	stats->validated_cpus = os_atomic_load(&gic_validated, acquire);
}

void
bcm2711_gic_set_ipi_handler(ipi_handler_t handler)
{
	if (!os_atomic_cmpxchg(&gic_ipi_handler, NULL, handler, release) &&
	    os_atomic_load(&gic_ipi_handler, acquire) != handler) {
		panic("BCM2711: inconsistent IPI handlers");
	}
}

void
bcm2711_gic_validate_cpu(void)
{
	uint32_t cpu = bcm2711_gic_current_cpu();
	uint32_t target = *(volatile uint8_t *)(gic_distributor_base + GICD_ITARGETSR);
	uint32_t required = (1U << BCM2711_GIC_IPI) | (1U << BCM2711_GIC_TIMER);
	if (cpu >= BCM2711_GIC_MAX_CPUS || target == 0 || (target & (target - 1)) ||
	    target != os_atomic_load(&gic_targets[cpu], acquire) ||
	    (gic_read32(gic_distributor_base, GICD_IGROUPR) & required) != required ||
	    (gic_read32(gic_distributor_base, GICD_ISENABLER) & required) != required ||
	    *(volatile uint8_t *)(gic_distributor_base + GICD_IPRIORITYR) != 0x40 ||
	    *(volatile uint8_t *)(gic_distributor_base + GICD_IPRIORITYR + BCM2711_GIC_TIMER) != 0x60 ||
	    (gic_read32(gic_distributor_base, GICD_ICFGR + 4) & (2U << 22)) != 0 ||
	    (gic_read32(gic_cpu_interface_base, GICC_PMR) & 0xf0) != 0xf0 ||
	    (gic_read32(gic_cpu_interface_base, GICC_CTLR) & 0x607) != 7 ||
	    gic_read32(gic_cpu_interface_base, GICC_BPR) > 3) {
		panic("BCM2711: invalid banked GIC state on CPU %u target 0x%x", cpu, target);
	}
	for (unsigned int other = 0; other < BCM2711_GIC_MAX_CPUS; other++) {
		if (other != cpu && os_atomic_load(&gic_targets[other], acquire) == target) {
			panic("BCM2711: duplicate GIC CPU target 0x%x", target);
		}
	}
	os_atomic_or(&gic_validated, 1U << cpu, release);
}

void
bcm2711_gic_init_cpu(void)
{
	uint32_t cpu = bcm2711_gic_current_cpu();
	if (cpu >= BCM2711_GIC_MAX_CPUS) {
		panic("BCM2711: GIC CPU %u out of range", cpu);
	}
	/* Boot CPU's IOCPU callback may run after early initialization. Do not
	 * reset live PPIs or erase a pending timer on that second call.
	 */
	if (os_atomic_load(&gic_validated, acquire) & (1U << cpu)) {
		bcm2711_gic_validate_cpu();
		return;
	}
	gic_write32(gic_cpu_interface_base, GICC_CTLR, 0);
	gic_write32(gic_distributor_base, GICD_ICENABLER, 0xffff0000U);
	gic_write32(gic_distributor_base, GICD_ICPENDR, 0xffff0000U);
	gic_write32(gic_distributor_base, GICD_ICACTIVER, 0xffffffffU);
	for (unsigned int reg = 0; reg < 4; reg++) {
		gic_write32(gic_distributor_base, GICD_CPENDSGIR + reg * 4, 0xffffffffU);
	}
	gic_write32(gic_distributor_base, GICD_IGROUPR, 0xffffffffU);
	for (unsigned int intid = 0; intid < 32; intid += 4) {
		gic_write32(gic_distributor_base, GICD_IPRIORITYR + intid, 0xa0a0a0a0U);
	}
	*(volatile uint8_t *)(gic_distributor_base + GICD_IPRIORITYR) = 0x40;
	*(volatile uint8_t *)(gic_distributor_base + GICD_IPRIORITYR + BCM2711_GIC_TIMER) = 0x60;
	gic_write32(gic_distributor_base, GICD_ICFGR + 4, 0); /* PPIs level */
	gic_write32(gic_distributor_base, GICD_ISENABLER,
	    (1U << BCM2711_GIC_IPI) | (1U << BCM2711_GIC_TIMER));
	os_atomic_store(&gic_targets[cpu],
	    *(volatile uint8_t *)(gic_distributor_base + GICD_ITARGETSR), release);
	gic_write32(gic_cpu_interface_base, GICC_PMR, 0xffU);
	gic_write32(gic_cpu_interface_base, GICC_BPR, 0);
	/* AckCtl lets common IAR acknowledge Group 1 in QEMU's secure view.
	 * EOImode remains zero: EOIR drops priority AND deactivates the source.
	 */
	gic_write32(gic_cpu_interface_base, GICC_CTLR, 7U);
	__asm__ volatile ("dsb sy\n\tisb" ::: "memory");
	bcm2711_gic_validate_cpu();
	ml_install_interrupt_handler(NULL, 0, NULL, bcm2711_gic_handle_irq, NULL);
}

void
bcm2711_gic_send_ipi(uint32_t physical_cpu)
{
	int logical_cpu = ml_get_cpu_number(physical_cpu);
	if (logical_cpu < 0 || logical_cpu >= BCM2711_GIC_MAX_CPUS) {
		panic("BCM2711: invalid IPI target %u", physical_cpu);
	}
	uint32_t target = os_atomic_load(&gic_targets[logical_cpu], acquire);
	if (!target) {
		panic("BCM2711: IPI to uninitialized CPU %u", physical_cpu);
	}
	gic_sync();
	gic_write32(gic_distributor_base, GICD_SGIR,
	    (target << 16) | (1U << 15) | BCM2711_GIC_IPI);
	__asm__ volatile ("isb" ::: "memory");
}

void
bcm2711_gic_init(vm_offset_t distributor_base, vm_offset_t cpu_interface_base)
{
	gic_distributor_base = distributor_base;
	gic_cpu_interface_base = cpu_interface_base;
	uint32_t typer = gic_read32(gic_distributor_base, GICD_TYPER);
	gic_intids = ((typer & 31U) + 1U) * 32U;
	if (gic_intids > BCM2711_GIC_MAX_INTIDS) {
		gic_intids = BCM2711_GIC_MAX_INTIDS;
	}
	gic_write32(gic_distributor_base, GICD_CTLR, 0);
	for (uint32_t intid = 32; intid < gic_intids; intid += 32) {
		uint32_t offset = (intid / 32) * 4;
		gic_write32(gic_distributor_base, GICD_ICENABLER + offset, 0xffffffffU);
		gic_write32(gic_distributor_base, GICD_ICPENDR + offset, 0xffffffffU);
		gic_write32(gic_distributor_base, GICD_ICACTIVER + offset, 0xffffffffU);
		gic_write32(gic_distributor_base, GICD_IGROUPR + offset, 0xffffffffU);
	}
	bcm2711_gic_init_cpu();
	gic_write32(gic_distributor_base, GICD_CTLR, 3U);
	gic_sync();
}

#if DEVELOPMENT || DEBUG
static void
bcm2711_irq_test_timer_done(timer_call_param_t arg, __unused timer_call_param_t unused)
{
	_Atomic bool *done = arg;
	os_atomic_store(done, true, release);
	thread_wakeup((event_t)done);
}

void
bcm2711_irq_test_local_timer(void)
{
	timer_call_data_t call;
	_Atomic bool done = false;
	uint64_t deadline;
	clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
	timer_call_setup(&call, bcm2711_irq_test_timer_done, &done);
	assert_wait((event_t)&done, THREAD_UNINT);
	timer_call_enter(&call, deadline, TIMER_CALL_LOCAL | TIMER_CALL_SYS_CRITICAL);
	thread_block(THREAD_CONTINUE_NULL);
	assert(os_atomic_load(&done, acquire));
	timer_call_cancel(&call);
}
#endif

#if DEVELOPMENT || DEBUG
void
bcm2711_irq_test_bind_cpu(int cpu)
{
	thread_bind(cpu < 0 ? PROCESSOR_NULL : processor_array[cpu]);
	thread_block(THREAD_CONTINUE_NULL);
}
#endif

void ml_enable_monitor(void) {}
boolean_t ml_device_is_prod_fused(void) { return FALSE; }
