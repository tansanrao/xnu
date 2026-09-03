/* BearSSL implementation of XNU's registered kernel-PRNG contract. */
#include <kern/locks.h>
#include <prng/random.h>
#include <kern/startup.h>
#include <kern/debug.h>
#include <libkern/crypto/crypto.h>
#include <libkern/section_keywords.h>
#include <corecrypto/cc.h>
#include "bearssl_backend.h"

/* XNU only forwards this cookie; no corecrypto PRNG fields are used. */
static struct cckprng_ctx registration_context;
static struct bearssl_drbg root_generator;
static struct bearssl_drbg generators[MAX_CPUS];
static bool generator_ready[MAX_CPUS];
static unsigned int generator_count;
static cckprng_getentropy get_entropy;
static void *get_entropy_arg;
static bool initialized;

LCK_GRP_DECLARE(bearssl_prng_group, "bearssl-prng");
static lck_mtx_t prng_lock;

SECURITY_READ_ONLY_LATE(bool) crypto_init;

static void
check_context(struct cckprng_ctx *ctx)
{
	if (ctx != &registration_context || !initialized) {
		panic("BearSSL PRNG used before registration");
	}
}

static void
generate_locked(struct bearssl_drbg *drbg, size_t nbytes, void *out)
{
	if (nbytes > CCKPRNG_GENERATE_MAX_NBYTES ||
	    drbg->requests == BEARSSL_DRBG_RESEED_INTERVAL) {
		panic("BearSSL PRNG request/reseed limit exceeded");
	}
	br_hmac_drbg_generate(&drbg->context, out, nbytes);
	drbg->requests++;
}

static void
init_with_entropy(struct cckprng_ctx *ctx, unsigned int max_ngens,
    size_t seed_size, const void *seed, size_t nonce_size, const void *nonce,
    cckprng_getentropy getentropy, void *getentropy_arg)
{
	if (ctx != &registration_context || initialized || max_ngens == 0 ||
	    max_ngens > MAX_CPUS || seed_size < 32 || getentropy == NULL) {
		panic("Invalid BearSSL PRNG registration");
	}
	lck_mtx_init(&prng_lock, &bearssl_prng_group, LCK_ATTR_NULL);
	br_hmac_drbg_init(&root_generator.context, &br_sha256_vtable, seed, seed_size);
	br_hmac_drbg_update(&root_generator.context, nonce, nonce_size);
	static const char label[] = "xnu bearssl kernel prng";
	br_hmac_drbg_update(&root_generator.context, label, sizeof(label));
	generator_count = max_ngens;
	get_entropy = getentropy;
	get_entropy_arg = getentropy_arg;
	initialized = true;
}

static void
init_legacy(struct cckprng_ctx *ctx, size_t seed_size, const void *seed,
    size_t nonce_size, const void *nonce, cckprng_getentropy getentropy,
    void *getentropy_arg)
{
	init_with_entropy(ctx, MAX_CPUS, seed_size, seed, nonce_size, nonce,
	    getentropy, getentropy_arg);
}

static void
init_generator(struct cckprng_ctx *ctx, unsigned int index)
{
	check_context(ctx);
	lck_mtx_lock(&prng_lock);
	if (index >= generator_count || generator_ready[index]) {
		panic("Invalid/duplicate BearSSL PRNG generator %u", index);
	}
	uint8_t seed[32];
	generate_locked(&root_generator, sizeof(seed), seed);
	br_hmac_drbg_init(&generators[index].context, &br_sha256_vtable, seed, sizeof(seed));
	br_hmac_drbg_update(&generators[index].context, &index, sizeof(index));
	generator_ready[index] = true;
	cc_clear(sizeof(seed), seed);
	lck_mtx_unlock(&prng_lock);
}

static void
reseed_locked(size_t nbytes, const void *seed)
{
	if (nbytes == 0) {
		return;
	}
	br_hmac_drbg_update(&root_generator.context, seed, nbytes);
	root_generator.requests = 0;
	for (unsigned int i = 0; i < generator_count; i++) {
		if (generator_ready[i]) {
			br_hmac_drbg_update(&generators[i].context, seed, nbytes);
			generators[i].requests = 0;
		}
	}
}

static void
reseed(struct cckprng_ctx *ctx, size_t nbytes, const void *seed)
{
	check_context(ctx);
	lck_mtx_lock(&prng_lock);
	reseed_locked(nbytes, seed);
	lck_mtx_unlock(&prng_lock);
}

static void
refresh(struct cckprng_ctx *ctx)
{
	check_context(ctx);
	/* Like cckprng_refresh, contention must not block the caller. */
	if (!lck_mtx_try_lock(&prng_lock)) {
		return;
	}
	uint8_t seed[64];
	size_t nbytes = sizeof(seed);
	int32_t samples = get_entropy(&nbytes, seed, get_entropy_arg);
	if (nbytes > sizeof(seed)) {
		panic("BearSSL entropy source exceeded its buffer");
	}
	if (samples > 0 && nbytes != 0) {
		reseed_locked(nbytes, seed);
	}
	/* Zero/negative means no healthy fresh entropy; retain existing state. */
	cc_clear(sizeof(seed), seed);
	lck_mtx_unlock(&prng_lock);
}

static void
generate(struct cckprng_ctx *ctx, unsigned int index, size_t nbytes, void *out)
{
	check_context(ctx);
	lck_mtx_lock(&prng_lock);
	if (index >= generator_count || !generator_ready[index]) {
		panic("Uninitialized BearSSL PRNG generator %u", index);
	}
	generate_locked(&generators[index], nbytes, out);
	lck_mtx_unlock(&prng_lock);
}

static const struct cckprng_funcs functions = {
	.init = init_legacy,
	.initgen = init_generator,
	.reseed = reseed,
	.refresh = refresh,
	.generate = generate,
	.init_with_getentropy = init_with_entropy,
};

__startup_func
static void
bearssl_prng_start(void)
{
	/* The upstream handoff seeds us, initializes entropy, and erases erandom. */
	register_and_init_prng(&registration_context, &functions);
	crypto_init = true;
}
STARTUP(EARLY_BOOT, STARTUP_RANK_SECOND, bearssl_prng_start);
