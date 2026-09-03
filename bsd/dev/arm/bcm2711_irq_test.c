#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_BCM2711) && (DEVELOPMENT || DEBUG)
#include <arm64/bcm2711_interrupt.h>
#include <console/serial_protos.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <pexpert/arm/protos.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>

void bcm2711_irq_test_start(void);
extern void bcm2711_irq_controller_test(void);
extern int cnopen(dev_t, int, int, proc_t);
extern int cnclose(dev_t, int, int, proc_t);
extern int cnread(dev_t, struct uio *, int);

#define CHECK(x) do { if (!(x)) panic("BCM2711 IRQ test: %s", #x); } while (0)
static thread_t reader;
static _Atomic unsigned int read_phase;
static _Atomic unsigned int idle_phase;

static void
sleep_ms(unsigned int ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	assert_wait_deadline((event_t)sleep_ms, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
runtime_us(thread_t thread)
{
	time_value_t user, system;
	thread_read_times(thread, &user, &system, NULL);
	return (uint64_t)system.seconds * 1000000 + system.microseconds;
}

static void
validate_cpu(__unused void *arg)
{
	bcm2711_gic_validate_cpu();
}

static void
monitor(__unused void *arg, __unused wait_result_t wait_result)
{
	for (unsigned int phase = 1; phase <= 2; phase++) {
		while (os_atomic_load(&read_phase, acquire) != phase) {
			sleep_ms(10);
		}
		sleep_ms(100); /* Let both reader and keyboard reach their wait. */
		uint64_t polls0, polls1, keyboard0, keyboard1;
		serial_keyboard_stats(&polls0, &keyboard0);
		uint64_t reader0 = runtime_us(reader);
		struct bcm2711_gic_stats before, after;
		bcm2711_gic_get_stats(&before);
		sleep_ms(2000);
		serial_keyboard_stats(&polls1, &keyboard1);
		uint64_t used = runtime_us(reader) - reader0;
		CHECK(os_atomic_load(&read_phase, acquire) == phase);
		thread_basic_info_data_t info;
		mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
		CHECK(thread_info(reader, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS);
		CHECK(info.run_state == TH_STATE_WAITING);
		CHECK(polls1 == polls0 && keyboard1 - keyboard0 < 1000 && used < 1000);
		/* Force cross-CPU traffic while the read remains asleep. */
		cpu_broadcast_xcall_simple(TRUE, validate_cpu, &phase);
		bcm2711_gic_get_stats(&after);
		uint64_t timer_delta = 0;
		for (unsigned int cpu = 0; cpu < ml_get_cpu_count(); cpu++) {
			timer_delta += after.timer[cpu] - before.timer[cpu];
			printf("BCM2711 IRQ test: CPU %u timer=%llu IPI=%llu\n",
			    cpu, after.timer[cpu], after.ipi[cpu]);
			CHECK(after.timer[cpu] > 0 && after.ipi[cpu] > 0);
		}
		CHECK(timer_delta > 0 && after.validated_cpus == (1U << ml_get_cpu_count()) - 1);
		os_atomic_store(&idle_phase, phase, release);
		printf("BCM2711 IRQ test: idle %u PASS (2s, reader %llu us, keyboard %llu us, polls %llu, timer +%llu, banks 0x%x); send irq-%s\\n\n",
		    phase, used, keyboard1 - keyboard0, polls1 - polls0, timer_delta,
		    after.validated_cpus, phase == 1 ? "one" : "two");
	}
	thread_deallocate(reader);
	thread_terminate(current_thread());
}

static void
read_console(__unused void *arg, __unused wait_result_t wait_result)
{
	bcm2711_irq_controller_test();
	CHECK(cnopen(0, FREAD | FWRITE, 0, current_proc()) == 0);
	reader = current_thread();
	thread_reference(reader);
	thread_t observer;
	CHECK(kernel_thread_start(monitor, NULL, &observer) == KERN_SUCCESS);
	thread_deallocate(observer);
	for (unsigned int phase = 1; phase <= 2; phase++) {
		char buffer[32] = {};
		uio_t uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
		CHECK(uio && uio_addiov(uio, (user_addr_t)buffer, sizeof(buffer) - 1) == 0);
		uint64_t irq0, irq1, errors;
		serial_rx_irq_stats(&irq0, &errors);
		printf("BCM2711 IRQ test: cnread %u blocking\n", phase);
		os_atomic_store(&read_phase, phase, release);
		CHECK(cnread(0, uio, 0) == 0);
		size_t length = sizeof(buffer) - 1 - (size_t)uio_resid(uio);
		uio_free(uio);
		serial_rx_irq_stats(&irq1, &errors);
		CHECK(os_atomic_load(&idle_phase, acquire) == phase && irq1 > irq0);
		const char *expected = phase == 1 ? "irq-one\n" : "irq-two\n";
		CHECK(length == strlen(expected) && memcmp(buffer, expected, length) == 0);
		printf("BCM2711 IRQ test: cnread %u woke, %lu bytes, RX IRQ +%llu, RX errors %llu\n",
		    phase, length, irq1 - irq0, errors);
	}
	CHECK(cnclose(0, FREAD | FWRITE, 0, current_proc()) == 0);
	printf("BCM2711 IRQ test: PASS\n");
	thread_terminate(current_thread());
}

void
bcm2711_irq_test_start(void)
{
	unsigned int enabled = 0;
	if (!PE_parse_boot_argn("bcm2711-irq-test", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	thread_t thread;
	CHECK(kernel_thread_start(read_console, NULL, &thread) == KERN_SUCCESS);
	thread_deallocate(thread);
}
#endif
