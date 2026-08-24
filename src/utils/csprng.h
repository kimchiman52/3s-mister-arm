/*
 * csprng.h — S4 of docs/plan-netplay-connection.md: cryptographically
 * secure random bytes for the netplay connection path.
 *
 * Why not SDL_rand: SDL 3.4.4's SDL_rand IS auto-seeded (from the
 * performance counter at first use) — the problem is the generator
 * itself. It is a 64-bit LCG (xorshift-star style mixing of a linear
 * state), so its internal state is recoverable from a handful of
 * observed outputs, after which every future output is predictable.
 * STUN transaction IDs, punch-token nonces and room-code nonces exist
 * precisely to be unpredictable to an off-path attacker, so they must
 * come from the OS CSPRNG instead.
 *
 * Implementation: /dev/urandom on POSIX (macOS + MiSTer ARM Linux),
 * rand_s (the CRT's RtlGenRandom wrapper) on Windows. No fallback to a
 * weak generator here — callers decide whether a failure is fatal
 * (e.g. the room-code nonce must NOT silently degrade) or can degrade
 * with a logged warning (STUN transaction IDs).
 */
#ifndef UTILS_CSPRNG_H
#define UTILS_CSPRNG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fill `out` with `len` cryptographically secure random bytes from the
 * platform CSPRNG. Thread-safe (no shared state). Returns false when
 * the platform source is unavailable or short-reads; on false the
 * buffer contents are unspecified and MUST NOT be used.
 */
bool Csprng_Bytes(void* out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_CSPRNG_H */
