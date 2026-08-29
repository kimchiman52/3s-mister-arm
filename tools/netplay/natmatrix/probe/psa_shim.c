/* SHA-256 backend for the PSA subset declared in psa_shim/psa/crypto.h.
 * FIPS 180-4. Verified by sha256_equiv_test.c against both the published test
 * vectors and the real tf-psa-crypto implementation. */
#include <psa/crypto.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

psa_status_t psa_hash_setup(psa_hash_operation_t* op, psa_algorithm_t alg) {
    if (!op) return PSA_ERROR_BAD_STATE;
    if (alg != PSA_ALG_SHA_256) return PSA_ERROR_NOT_SUPPORTED;
    op->state[0] = 0x6a09e667u; op->state[1] = 0xbb67ae85u;
    op->state[2] = 0x3c6ef372u; op->state[3] = 0xa54ff53au;
    op->state[4] = 0x510e527fu; op->state[5] = 0x9b05688cu;
    op->state[6] = 0x1f83d9abu; op->state[7] = 0x5be0cd19u;
    op->bitlen = 0;
    op->buflen = 0;
    op->initialized = 1;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_update(psa_hash_operation_t* op, const uint8_t* input, size_t input_length) {
    if (!op || !op->initialized) return PSA_ERROR_BAD_STATE;
    if (input_length == 0) return PSA_SUCCESS;
    if (!input) return PSA_ERROR_BAD_STATE;
    op->bitlen += (uint64_t)input_length * 8u;
    size_t i = 0;
    if (op->buflen) {
        size_t need = 64 - op->buflen;
        size_t take = input_length < need ? input_length : need;
        memcpy(op->buf + op->buflen, input, take);
        op->buflen += take;
        i += take;
        if (op->buflen == 64) { sha256_compress(op->state, op->buf); op->buflen = 0; }
    }
    for (; i + 64 <= input_length; i += 64) sha256_compress(op->state, input + i);
    if (i < input_length) {
        op->buflen = input_length - i;
        memcpy(op->buf, input + i, op->buflen);
    }
    return PSA_SUCCESS;
}

psa_status_t psa_hash_finish(psa_hash_operation_t* op, uint8_t* hash, size_t hash_size, size_t* hash_length) {
    if (!op || !op->initialized) return PSA_ERROR_BAD_STATE;
    if (hash_size < 32) return PSA_ERROR_BUFFER_TOO_SMALL;
    uint64_t bitlen = op->bitlen;
    uint8_t pad = 0x80;
    psa_hash_update(op, &pad, 1);
    op->bitlen = bitlen; /* padding must not count toward the length */
    uint8_t zero = 0x00;
    while (op->buflen != 56) { psa_hash_update(op, &zero, 1); op->bitlen = bitlen; }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[7 - i] = (uint8_t)((bitlen >> (i * 8)) & 0xff);
    memcpy(op->buf + 56, len_be, 8);
    sha256_compress(op->state, op->buf);
    op->buflen = 0;
    for (int i = 0; i < 8; i++) {
        hash[i * 4]     = (uint8_t)(op->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(op->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(op->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(op->state[i]);
    }
    if (hash_length) *hash_length = 32;
    op->initialized = 0;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_abort(psa_hash_operation_t* op) {
    if (op) { op->initialized = 0; op->buflen = 0; }
    return PSA_SUCCESS;
}
