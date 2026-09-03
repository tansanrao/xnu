/* XNU's digest ABI, backed by unmodified BearSSL hash implementations. */
#include <stddef.h>
#include <libkern/crypto/sha1.h>
#include <libkern/crypto/sha2.h>
#include <corecrypto/cc.h>
#include <string.h>
#include "bearssl_backend.h"

/* Preserve the legacy SHA-1 field layout, including its byte count. */
static void
sha1_import(br_sha1_context *bear, const SHA1_CTX *ctx)
{
	br_sha1_init(bear);
	memcpy(bear->val, ctx->h.b32, sizeof(bear->val));
	bear->count = ctx->c.b64[0];
	memcpy(bear->buf, ctx->m.b8, sizeof(bear->buf));
}

static void
sha1_export(SHA1_CTX *ctx, const br_sha1_context *bear)
{
	memcpy(ctx->h.b32, bear->val, sizeof(bear->val));
	ctx->c.b64[0] = bear->count;
	memcpy(ctx->m.b8, bear->buf, sizeof(bear->buf));
}

void
SHA1Init(SHA1_CTX *ctx)
{
	br_sha1_context bear = {0};
	br_sha1_init(&bear);
	memset(ctx, 0, sizeof(*ctx));
	sha1_export(ctx, &bear);
	cc_clear(sizeof(bear), &bear);
}

void
SHA1Update(SHA1_CTX *ctx, const void *data, size_t len)
{
	br_sha1_context bear;
	sha1_import(&bear, ctx);
	br_sha1_update(&bear, data, len);
	sha1_export(ctx, &bear);
	cc_clear(sizeof(bear), &bear);
}

void
SHA1Final(void *digest, SHA1_CTX *ctx)
{
	br_sha1_context bear;
	sha1_import(&bear, ctx);
	br_sha1_out(&bear, digest);
	cc_clear(sizeof(bear), &bear);
}

void SHA1Final_r(SHA1_CTX *ctx, void *digest);
void
SHA1Final_r(SHA1_CTX *ctx, void *digest)
{
	SHA1Final(digest, ctx);
}

/*
 * SHA-2 contexts are opaque storage in XNU's public ABI. Copy through aligned
 * local objects rather than aliasing that storage as a BearSSL structure.
 * Do not change the public size/alignment or depend on a context cast.
 */
#define BEARSSL_SHA2(bits) \
	_Static_assert(sizeof(br_sha##bits##_context) <= sizeof(SHA##bits##_CTX), \
	    "BearSSL SHA context exceeds the XNU ABI"); \
	void SHA##bits##_Init(SHA##bits##_CTX *ctx) \
	{ \
		br_sha##bits##_context bear = {0}; \
		br_sha##bits##_init(&bear); \
		memset(ctx, 0, sizeof(*ctx)); \
		memcpy(ctx, &bear, sizeof(bear)); \
		cc_clear(sizeof(bear), &bear); \
	} \
	void SHA##bits##_Update(SHA##bits##_CTX *ctx, const void *data, size_t len) \
	{ \
		br_sha##bits##_context bear; \
		memcpy(&bear, ctx, sizeof(bear)); \
		br_sha##bits##_update(&bear, data, len); \
		memcpy(ctx, &bear, sizeof(bear)); \
		cc_clear(sizeof(bear), &bear); \
	} \
	void SHA##bits##_Final(void *digest, SHA##bits##_CTX *ctx) \
	{ \
		br_sha##bits##_context bear; \
		memcpy(&bear, ctx, sizeof(bear)); \
		br_sha##bits##_out(&bear, digest); \
		cc_clear(sizeof(bear), &bear); \
	}

BEARSSL_SHA2(256)
BEARSSL_SHA2(384)
BEARSSL_SHA2(512)

#undef BEARSSL_SHA2
