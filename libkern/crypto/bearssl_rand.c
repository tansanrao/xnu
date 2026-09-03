/* The existing libkern random interfaces, without a corecrypto dispatch table. */
#include <sys/types.h>
#include <libkern/crypto/rand.h>
#include <sys/random.h>
#include <corecrypto/cc.h>
#include <kern/debug.h>
#include <string.h>
#include "bearssl_backend.h"

_Static_assert(sizeof(struct bearssl_drbg) <= CRYPTO_RANDOM_MAX_CTX_SIZE,
    "BearSSL kmem context exceeds the libkern ABI");

int
cc_rand_generate(void *out, size_t outlen)
{
	if (!crypto_init) {
		return -1;
	}
	uint8_t *bytes = out;
	while (outlen != 0) {
		unsigned int count = (unsigned int)(outlen > UINT32_MAX ? UINT32_MAX : outlen);
		read_random(bytes, count);
		bytes += count;
		outlen -= count;
	}
	return 0;
}

int
random_buf(void *buf, size_t buflen)
{
	return cc_rand_generate(buf, buflen);
}

size_t
crypto_random_kmem_ctx_size(void)
{
	return sizeof(struct bearssl_drbg);
}

void
crypto_random_kmem_init(crypto_random_ctx_t ctx)
{
	struct bearssl_drbg *drbg = ctx;
	uint8_t seed[32];
	read_random(seed, sizeof(seed));
	memset(drbg, 0, sizeof(*drbg));
	br_hmac_drbg_init(&drbg->context, &br_sha256_vtable, seed, sizeof(seed));
	cc_clear(sizeof(seed), seed);
}

/*
 * kmem owns one context per CPU and serializes its use. Its contract forbids
 * acquiring locks here: only initialization calls the locked kernel PRNG.
 */
void
crypto_random_generate(crypto_random_ctx_t ctx, void *out, size_t length)
{
	struct bearssl_drbg *drbg = ctx;
	uint8_t *bytes = out;
	while (length != 0) {
		size_t count = length > BEARSSL_DRBG_MAX_REQUEST ? BEARSSL_DRBG_MAX_REQUEST : length;
		if (drbg->requests == BEARSSL_DRBG_RESEED_INTERVAL) {
			panic("BearSSL kmem DRBG exhausted its request limit");
		}
		br_hmac_drbg_generate(&drbg->context, bytes, count);
		drbg->requests++;
		bytes += count;
		length -= count;
	}
}

void
crypto_random_uniform(crypto_random_ctx_t ctx, uint64_t bound, uint64_t *out)
{
	if (bound == 0) {
		panic("crypto_random_uniform requires a nonzero bound");
	}
	uint64_t threshold = -bound % bound;
	uint64_t value;
	do {
		crypto_random_generate(ctx, &value, sizeof(value));
	} while (value < threshold);
	*out = value % bound;
}
