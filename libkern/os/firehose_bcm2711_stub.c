/*
 * Local BCM2711 firehose shim.
 *
 * Apple's prebuilt libfirehose_kernel.a is built with ARMv8.1 LSE atomics.
 * QEMU raspi4b and the Raspberry Pi 5 Cortex-A72 cores only support ARMv8.0,
 * so this target keeps the kernel logging ABI satisfied without linking that
 * archive.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mach/vm_types.h>
#include <firehose/firehose_types_private.h>
#include <firehose/tracepoint_private.h>
#include <os/firehose_buffer_private.h>

extern vm_offset_t kernel_firehose_addr;

firehose_buffer_t
__firehose_buffer_create(size_t *size)
{
	(void)size;

	return (firehose_buffer_t)kernel_firehose_addr;
}

void
__firehose_buffer_tracepoint_flush(firehose_tracepoint_t vat,
    firehose_tracepoint_id_u vatid)
{
	(void)vat;
	(void)vatid;
}

firehose_tracepoint_t
__firehose_buffer_tracepoint_reserve(uint64_t stamp, firehose_stream_t stream,
    uint16_t pubsize, uint16_t privsize, uint8_t **privptr)
{
	(void)stamp;
	(void)stream;
	(void)pubsize;
	(void)privsize;

	if (privptr != NULL) {
		*privptr = NULL;
	}
	return NULL;
}

bool
__firehose_merge_updates(firehose_push_reply_t update)
{
	(void)update;

	return false;
}

int
__firehose_kernel_configuration_valid(uint8_t chunk_count, uint8_t io_pages)
{
	return chunk_count >= FIREHOSE_BUFFER_KERNEL_MIN_CHUNK_COUNT &&
	       chunk_count <= FIREHOSE_BUFFER_KERNEL_MAX_CHUNK_COUNT &&
	       io_pages > 0 &&
	       io_pages < chunk_count;
}
