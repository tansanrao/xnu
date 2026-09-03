/* Private glue for the explicitly selected standalone crypto provider. */
#ifndef _LIBKERN_BEARSSL_BACKEND_H_
#define _LIBKERN_BEARSSL_BACKEND_H_

#if !CONFIG_CRYPTO_BEARSSL
#error "BearSSL adapters require CONFIG_CRYPTO_BEARSSL"
#endif
#if CONFIG_KEC_FIPS
#error "BearSSL and the corecrypto KEC are mutually exclusive providers"
#endif
#if CRYPTO
#error "BearSSL currently provides digests and randomness, not the full CRYPTO KPI"
#endif
#if !defined(CRYPTO_SHA2)
#error "The BearSSL provider requires CRYPTO_SHA2"
#endif

#include <bearssl_hash.h>
#include <bearssl_rand.h>

/* HMAC-DRBG limits are enforced by our callers, not by BearSSL itself. */
#define BEARSSL_DRBG_MAX_REQUEST ((size_t)65536)
#define BEARSSL_DRBG_RESEED_INTERVAL (UINT64_C(1) << 48)

struct bearssl_drbg {
	br_hmac_drbg_context context;
	uint64_t requests;
};

#endif /* _LIBKERN_BEARSSL_BACKEND_H_ */
