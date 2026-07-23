/*
 * Raspberry Pi 4 GIC-400/GICv2 support.
 *
 * This backend is deliberately narrow: CPU 0, Group 0/FIQ delivery, and the
 * ARM virtual timer PPI are the only Phase 6 requirements.
 */

#include <stddef.h>
#include <stdint.h>
#include <arm64/proc_reg.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/arm64/board_config.h>
#include <pexpert/arm/protos.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>

#if HAS_GIC_V2

enum {
	GICD_CTLR = 0x000,
	GICD_TYPER = 0x004,
	GICD_IIDR = 0x008,
	GICD_IGROUPR0 = 0x080,
	GICD_ISENABLER0 = 0x100,
	GICD_ICENABLER0 = 0x180,
	GICD_ICPENDR0 = 0x280,
	GICD_ISACTIVER0 = 0x300,
	GICD_ICACTIVER0 = 0x380,
	GICD_IPRIORITYR = 0x400,
	GICD_ICFGR1 = 0xc04,
	GICD_PIDR2 = 0xfe8,

	GICC_CTLR = 0x000,
	GICC_PMR = 0x004,
	GICC_BPR = 0x008,
	GICC_IAR = 0x00c,
	GICC_EOIR = 0x010,
	GICC_IIDR = 0x0fc,

	GICD_CTLR_ENABLEGRP0 = 1u << 0,
	GICC_CTLR_ENABLEGRP0 = 1u << 0,
	GICC_CTLR_FIQEN = 1u << 3,
	GICD_TYPER_SECURITY_EXTN = 1u << 10,
	GIC_INTID_MASK = 0x3ff,
	GIC_SPURIOUS_INTID = 1023,
	GIC_TIMER_INTID = 27,
	GIC_TIMER_BIT = 1u << GIC_TIMER_INTID,
	GIC_TIMER_CONFIG_BIT = 1u << (((GIC_TIMER_INTID - 16u) * 2u) + 1u),
	GIC_PRIORITY = 0x80,
};

static vm_offset_t gicd_base;
static vm_offset_t gicc_base;
static uint64_t gicd_phys;
static uint64_t gicc_phys;
static volatile struct pe_gicv2_stats gicv2_stats;

static size_t
string_length(const char *value)
{
	size_t length = 0;

	while (value[length] != '\0') {
		length++;
	}
	return length;
}

static int
bytes_equal(const void *left_value, const void *right_value, size_t length)
{
	const uint8_t *left = left_value;
	const uint8_t *right = right_value;
	uint8_t difference = 0;

	for (size_t index = 0; index < length; ++index) {
		difference |= left[index] ^ right[index];
	}
	return difference == 0;
}

static uint32_t
gicd_read32(uint32_t offset)
{
	return *(volatile uint32_t *)(gicd_base + offset);
}

static void
gicd_write32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(gicd_base + offset) = value;
}

static uint32_t
gicc_read32(uint32_t offset)
{
	return *(volatile uint32_t *)(gicc_base + offset);
}

static void
gicc_write32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(gicc_base + offset) = value;
}

static void
gicv2_sync(void)
{
	__builtin_arm_dmb(DMB_SY);
	__builtin_arm_isb(ISB_SY);
}

static const void *
required_property(
	DTEntry entry,
	const char *name,
	unsigned int expected_size)
{
	const void *property;
	unsigned int size;

	if (SecureDTGetProperty(entry, name, &property, &size) != kSuccess ||
	    size != expected_size) {
		panic("RPI4: GICV2 invalid %s property", name);
	}
	return property;
}

static void
require_string(DTEntry entry, const char *name, const char *expected)
{
	const char *property;
	unsigned int size;
	size_t expected_size = string_length(expected) + 1;

	if (SecureDTGetProperty(entry, name, (const void **)&property, &size) !=
	    kSuccess || size != expected_size ||
	    !bytes_equal(property, expected, expected_size)) {
		panic("RPI4: GICV2 invalid %s string", name);
	}
}

void
pe_gicv2_init(void)
{
	DTEntry gic;
	DTEntry timer;
	const uint64_t *reg;
	const uint32_t *interrupts;
	uint64_t soc_phys;
	uint64_t gicd_size;
	uint64_t gicc_size;
	uint32_t typer;
	uint32_t pidr2;
	uint32_t interrupt_lines;
	uint32_t cpu_interfaces;
	uint32_t priority_word;
	uint32_t priority_shift;

	if (gicd_base != 0) {
		panic("RPI4: GICV2 initialized more than once on UP target");
	}
	if (SecureDTLookupEntry(NULL, "/arm-io/gic", &gic) != kSuccess ||
	    SecureDTLookupEntry(NULL, "/arm-io/timer", &timer) != kSuccess) {
		panic("RPI4: GICV2 missing ADT nodes");
	}
	require_string(gic, "device_type", "interrupt-controller");
	require_string(gic, "compatible", "arm,gic-400");
	require_string(gic, "interrupt-controller", "master");
	if (*(const uint32_t *)required_property(
		    gic, "#interrupt-cells", sizeof(uint32_t)) != 3 ||
	    *(const uint32_t *)required_property(
		    gic, "AAPL,phandle", sizeof(uint32_t)) != 2) {
		panic("RPI4: GICV2 invalid interrupt-controller metadata");
	}

	reg = required_property(gic, "reg", 4 * sizeof(uint64_t));
	gicd_size = reg[1];
	gicc_size = reg[3];
	if (gicd_size != 0x1000 || gicc_size != 0x2000) {
		panic("RPI4: GICV2 invalid MMIO sizes");
	}

	require_string(timer, "device_type", "timer");
	require_string(timer, "compatible", "arm,armv8-timer");
	if (*(const uint32_t *)required_property(
		    timer, "interrupt-parent", sizeof(uint32_t)) != 2) {
		panic("RPI4: GICV2 invalid timer interrupt parent");
	}
	interrupts = required_property(timer, "interrupts", 3 * sizeof(uint32_t));
	if (interrupts[0] != 1 || interrupts[1] != 11 ||
	    interrupts[2] != 0xf08) {
		panic("RPI4: GICV2 timer is not virtual PPI 27");
	}

	soc_phys = pe_arm_get_soc_base_phys();
	if (soc_phys == 0 ||
	    reg[0] > UINT64_MAX - soc_phys ||
	    reg[2] > UINT64_MAX - soc_phys) {
		panic("RPI4: GICV2 invalid SoC translation");
	}
	gicd_phys = soc_phys + reg[0];
	gicc_phys = soc_phys + reg[2];
	gicd_base = ml_io_map(gicd_phys, gicd_size);
	gicc_base = ml_io_map(gicc_phys, gicc_size);
	if (gicd_base == 0 || gicc_base == 0) {
		panic("RPI4: GICV2 MMIO mapping failed");
	}

	typer = gicd_read32(GICD_TYPER);
	pidr2 = gicd_read32(GICD_PIDR2);
	interrupt_lines = 32u * ((typer & 0x1fu) + 1u);
	cpu_interfaces = ((typer >> 5) & 0x7u) + 1u;
	if (((pidr2 >> 4) & 0xfu) != 2u ||
	    interrupt_lines <= GIC_TIMER_INTID ||
	    cpu_interfaces == 0 ||
	    (typer & GICD_TYPER_SECURITY_EXTN) != 0) {
		panic("RPI4: GICV2 unsupported PIDR2=0x%x TYPER=0x%x",
		    pidr2, typer);
	}

	gicc_write32(GICC_CTLR, 0);
	gicd_write32(GICD_CTLR, 0);
	gicv2_sync();

	gicd_write32(GICD_ICENABLER0, GIC_TIMER_BIT);
	gicd_write32(GICD_ICPENDR0, GIC_TIMER_BIT);
	gicd_write32(GICD_ICACTIVER0, GIC_TIMER_BIT);
	gicd_write32(
	    GICD_IGROUPR0, gicd_read32(GICD_IGROUPR0) & ~GIC_TIMER_BIT);
	gicd_write32(
	    GICD_ICFGR1, gicd_read32(GICD_ICFGR1) & ~GIC_TIMER_CONFIG_BIT);

	priority_shift = (GIC_TIMER_INTID & 3u) * 8u;
	priority_word = gicd_read32(
	    GICD_IPRIORITYR + (GIC_TIMER_INTID & ~3u));
	priority_word &= ~(0xffu << priority_shift);
	priority_word |= GIC_PRIORITY << priority_shift;
	gicd_write32(
	    GICD_IPRIORITYR + (GIC_TIMER_INTID & ~3u), priority_word);
	gicd_write32(GICD_ISENABLER0, GIC_TIMER_BIT);

	gicc_write32(GICC_PMR, 0xff);
	gicc_write32(GICC_BPR, 0);
	gicd_write32(GICD_CTLR, GICD_CTLR_ENABLEGRP0);
	gicc_write32(
	    GICC_CTLR, GICC_CTLR_ENABLEGRP0 | GICC_CTLR_FIQEN);
	gicv2_sync();

	if ((gicd_read32(GICD_ISENABLER0) & GIC_TIMER_BIT) == 0 ||
	    (gicd_read32(GICD_IGROUPR0) & GIC_TIMER_BIT) != 0 ||
	    (gicd_read32(GICD_ICFGR1) & GIC_TIMER_CONFIG_BIT) != 0 ||
	    (gicd_read32(GICD_CTLR) & GICD_CTLR_ENABLEGRP0) == 0 ||
	    (gicc_read32(GICC_CTLR) &
	    (GICC_CTLR_ENABLEGRP0 | GICC_CTLR_FIQEN)) !=
	    (GICC_CTLR_ENABLEGRP0 | GICC_CTLR_FIQEN)) {
		panic("RPI4: GICV2 register readback failed");
	}

	printf("RPI4: GICV2 READY GICD=0x%llx GICC=0x%llx TIMER=27 "
	    "LINES=%u CPUS=%u IIDR=0x%x GICC_IIDR=0x%x PIDR2=0x%x\n",
	    gicd_phys, gicc_phys, interrupt_lines, cpu_interfaces,
	    gicd_read32(GICD_IIDR), gicc_read32(GICC_IIDR), pidr2);
}

uint32_t
pe_gicv2_acknowledge_timer(void)
{
	uint32_t iar = gicc_read32(GICC_IAR);
	uint32_t intid = iar & GIC_INTID_MASK;

	if (gicv2_stats.active_interrupt != 0) {
		gicv2_stats.nested_interrupts++;
		panic("RPI4: GICV2 nested FIQ active=0x%x iar=0x%x",
		    gicv2_stats.active_interrupt, iar);
	}
	if (intid == GIC_SPURIOUS_INTID) {
		gicv2_stats.spurious_interrupts++;
		panic("RPI4: GICV2 spurious FIQ");
	}
	if (intid != GIC_TIMER_INTID || !ml_get_timer_pending()) {
		gicv2_stats.unexpected_interrupts++;
		panic("RPI4: GICV2 unexpected FIQ iar=0x%x timer_pending=%u",
		    iar, ml_get_timer_pending());
	}

	gicv2_stats.active_interrupt = iar;
	gicv2_stats.acknowledgements++;
	gicv2_stats.timer_interrupts++;
	return iar;
}

void
pe_gicv2_end_of_interrupt(uint32_t iar)
{
	if (gicv2_stats.active_interrupt != iar) {
		gicv2_stats.unexpected_interrupts++;
		panic("RPI4: GICV2 EOI mismatch active=0x%x iar=0x%x",
		    gicv2_stats.active_interrupt, iar);
	}

	gicc_write32(GICC_EOIR, iar);
	gicv2_sync();
	gicv2_stats.end_of_interrupts++;
	gicv2_stats.active_interrupt = 0;
	if ((gicd_read32(GICD_ISACTIVER0) & GIC_TIMER_BIT) != 0) {
		gicv2_stats.stuck_active_interrupts++;
		panic("RPI4: GICV2 timer remained active after EOI");
	}
}

void
pe_gicv2_get_stats(struct pe_gicv2_stats *stats)
{
	boolean_t interrupts_enabled;

	if (stats == NULL) {
		panic("RPI4: GICV2 NULL stats output");
	}
	interrupts_enabled = ml_set_interrupts_enabled(FALSE);
	stats->acknowledgements = gicv2_stats.acknowledgements;
	stats->end_of_interrupts = gicv2_stats.end_of_interrupts;
	stats->timer_interrupts = gicv2_stats.timer_interrupts;
	stats->nested_interrupts = gicv2_stats.nested_interrupts;
	stats->spurious_interrupts = gicv2_stats.spurious_interrupts;
	stats->unexpected_interrupts = gicv2_stats.unexpected_interrupts;
	stats->stuck_active_interrupts = gicv2_stats.stuck_active_interrupts;
	stats->active_interrupt = gicv2_stats.active_interrupt;
	(void)ml_set_interrupts_enabled(interrupts_enabled);
}

#endif /* HAS_GIC_V2 */
