/* Minimal PSA Crypto subset -- ONLY the surface src/utils/sha256.c uses.
 *
 * Why this exists: src/utils/sha256.h embeds psa_hash_operation_t and the repo's
 * PSA provider (third_party/tf-psa-crypto) ships as a prebuilt macOS/arm64 tree
 * with no x86_64-Linux build. Building all of mbedtls inside an emulated x86_64
 * VM to obtain SHA-256 is a poor trade.
 *
 * This shim substitutes only the CRYPTO BACKEND. src/utils/sha256.c and
 * src/utils/sha256.h are compiled UNMODIFIED, so the key-derivation code under
 * test (Rendezvous_DeriveSessionKey / Rendezvous_DerivePunchToken,
 * src/netplay/rendezvous.c:130-149) is the production code path.
 *
 * SHA-256 is a fixed function: a correct implementation is byte-identical to any
 * other. That is not assumed here -- it is verified by
 * tools/netplay/natmatrix/probe/sha256_equiv_test.c, which checks the FIPS 180-4
 * vectors AND cross-checks this backend against the real tf-psa-crypto build on
 * macOS over the exact 10-byte rendezvous derivation payload.
 */
#ifndef S8_PSA_CRYPTO_SHIM_H
#define S8_PSA_CRYPTO_SHIM_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_BAD_STATE ((psa_status_t)-137)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)

/* Value matches the PSA spec's SHA-256 algorithm id. Nothing here depends on
 * the numeric value beyond sha256.c passing it straight back to us. */
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
    int initialized;
} psa_hash_operation_t;

#define PSA_HASH_OPERATION_INIT { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, { 0 }, 0, 0 }

psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t* op, psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t* op, const uint8_t* input, size_t input_length);
psa_status_t psa_hash_finish(psa_hash_operation_t* op, uint8_t* hash, size_t hash_size, size_t* hash_length);
psa_status_t psa_hash_abort(psa_hash_operation_t* op);

#endif /* S8_PSA_CRYPTO_SHIM_H */
