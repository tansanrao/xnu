#include <mach/boolean.h>
#include <arm/cpu_data_internal.h>
#include <arm/rtclock.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>

#include <IOKit/IOInterrupts.h>

#define GICD_CTLR       0x000
#define GICD_TYPER      0x004
#define GICD_IIDR       0x008
#define GICD_IGROUPR0   0x080
#define GICD_ISENABLER0 0x100
#define GICD_ICENABLER0 0x180
#define GICD_ICPENDR0   0x280
#define GICD_ICACTIVER0 0x380
#define GICD_SGIR       0xf00

#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_BPR        0x008
#define GICC_IAR        0x00c
#define GICC_EOIR       0x010
#define GICC_IIDR       0x0fc

#define GIC_IPI_SGI             0U
#define GIC_TIMER_VIRTUAL_PPI 27U
#define GIC_SPECIAL_INTID_MIN 1020U

void bcm2711_gic_init(vm_offset_t distributor_base,
    vm_offset_t cpu_interface_base);
void bcm2711_gic_init_cpu(void);
void bcm2711_gic_set_ipi_handler(ipi_handler_t handler);
void bcm2711_gic_send_ipi(uint32_t physical_cpu);
uint32_t bcm2711_gic_ipi_seen_mask(void);

static vm_offset_t gic_distributor_base;
static vm_offset_t gic_cpu_interface_base;
static ipi_handler_t gic_ipi_handler;
static _Atomic uint32_t gic_ipi_seen;

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

static void
bcm2711_gic_handle_irq(__unused void *target, __unused void *refCon,
    __unused void *nub, __unused int source)
{
	uint32_t iar = gic_read32(gic_cpu_interface_base, GICC_IAR);
	uint32_t intid = iar & 0x3ffU;

	if (intid >= GIC_SPECIAL_INTID_MIN) {
		return;
	}
	if (intid == GIC_TIMER_VIRTUAL_PPI) {
		getCpuDatap()->cpu_decrementer = (uint32_t)-1;
		rtclock_intr(TRUE);
	} else if (intid == GIC_IPI_SGI) {
		if (gic_ipi_handler == NULL) {
			panic("BCM2711: IPI arrived before handler registration");
		}
		os_atomic_or(&gic_ipi_seen,
		    1U << getCpuDatap()->cpu_number, relaxed);
		gic_ipi_handler();
	} else {
		panic("BCM2711: unhandled GIC interrupt %u", intid);
	}

	__asm__ volatile ("dsb sy" ::: "memory");
	gic_write32(gic_cpu_interface_base, GICC_EOIR, iar);
}

uint32_t
bcm2711_gic_ipi_seen_mask(void)
{
	return os_atomic_load(&gic_ipi_seen, relaxed);
}

void
bcm2711_gic_set_ipi_handler(ipi_handler_t handler)
{
	if (gic_ipi_handler != NULL && gic_ipi_handler != handler) {
		panic("BCM2711: inconsistent IPI handlers");
	}
	gic_ipi_handler = handler;
}

void
bcm2711_gic_init_cpu(void)
{
	const uint32_t private_group1 =
	    (1U << GIC_IPI_SGI) | (1U << GIC_TIMER_VIRTUAL_PPI);
	const uint32_t timer_bit = 1U << GIC_TIMER_VIRTUAL_PPI;

	gic_write32(gic_distributor_base, GICD_ICENABLER0, 0xffff0000U);
	gic_write32(gic_distributor_base, GICD_ICPENDR0, 0xffff0000U);
	gic_write32(gic_distributor_base, GICD_ICACTIVER0, 0xffff0000U);
	gic_write32(gic_distributor_base, GICD_IGROUPR0,
	    gic_read32(gic_distributor_base, GICD_IGROUPR0) | private_group1);
	gic_write32(gic_distributor_base, GICD_ISENABLER0, timer_bit);

	gic_write32(gic_cpu_interface_base, GICC_PMR, 0xffU);
	gic_write32(gic_cpu_interface_base, GICC_BPR, 0);
	/* AckCtl lets the common IAR acknowledge Group 1 on QEMU's secure view. */
	gic_write32(gic_cpu_interface_base, GICC_CTLR, 7U);
	__asm__ volatile ("dsb sy\n\tisb" ::: "memory");

	ml_install_interrupt_handler(NULL, 0, NULL,
	    bcm2711_gic_handle_irq, NULL);
}

void
bcm2711_gic_send_ipi(uint32_t physical_cpu)
{
	if (physical_cpu >= 8) {
		panic("BCM2711: GICv2 target CPU %u is out of range", physical_cpu);
	}

	/*
	 * Target-list mode, NSATT set for the Group 1 SGI configured above, and
	 * SGI INTID 0. cpu_signal_internal() supplies the required DSB before it
	 * calls the platform signal operation.
	 */
	uint32_t sgir = (1U << (16 + physical_cpu)) | (1U << 15) |
	    GIC_IPI_SGI;
	gic_write32(gic_distributor_base, GICD_SGIR, sgir);
	__asm__ volatile ("isb" ::: "memory");
}

void
bcm2711_gic_init(vm_offset_t distributor_base,
    vm_offset_t cpu_interface_base)
{
	gic_distributor_base = distributor_base;
	gic_cpu_interface_base = cpu_interface_base;

	printf("BCM2711: GICD typer=0x%x iidr=0x%x GICC iidr=0x%x\n",
	    gic_read32(gic_distributor_base, GICD_TYPER),
	    gic_read32(gic_distributor_base, GICD_IIDR),
	    gic_read32(gic_cpu_interface_base, GICC_IIDR));

	gic_write32(gic_distributor_base, GICD_CTLR, 0);
	bcm2711_gic_init_cpu();
	gic_write32(gic_distributor_base, GICD_CTLR, 3U);
	__asm__ volatile ("dsb sy" ::: "memory");
}

void ml_enable_monitor(void) {}

boolean_t ml_device_is_prod_fused(void) { return FALSE; }
