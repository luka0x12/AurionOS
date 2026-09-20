#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * security.h — AurionOS Security Primitives
 * PBKDF2-HMAC-SHA256 password hashing, CSPRNG, safe arithmetic, pointer validation.
 * Zero dependencies beyond freestanding libc.
*/

/* Safe Arithmetic (AUR-05) */

static inline bool safe_mul_u32(uint32_t a, uint32_t b, uint32_t *out) {
    uint64_t r = (uint64_t)a * (uint64_t)b;
    if (r > 0xFFFFFFFFULL) return false;
    *out = (uint32_t)r;
    return true;
}

static inline bool safe_add_u32(uint32_t a, uint32_t b, uint32_t *out) {
    if (a > 0xFFFFFFFFU - b) return false;
    *out = a + b;
    return true;
}

static inline bool safe_mul_add_u32(uint32_t a, uint32_t b, uint32_t c, uint32_t *out) {
    uint64_t r = (uint64_t)a * (uint64_t)b + (uint64_t)c;
    if (r > 0xFFFFFFFFULL) return false;
    *out = (uint32_t)r;
    return true;
}

/* CSPRNG (AUR-01)
 * ChaCha20-based CSPRNG seeded from RDRAND + timing jitter.
 * Replaces the LCG at tls12_client.c:36.
*/

typedef struct {
    uint32_t state[16];   /* ChaCha20 state */
    uint32_t output[16];  /* Keystream buffer */
    int      output_pos;  /* Position in output buffer (0..15) */
    bool     seeded;
    uint32_t reseed_ctr;  /* Blocks generated since last re-seed */
    uint32_t reseed_interval; /* Re-seed after this many blocks (0 = never) */
} csprng_t;

void csprng_init(csprng_t *rng);
void csprng_seed(csprng_t *rng, const uint8_t seed[32]);
void csprng_bytes(csprng_t *rng, uint8_t *out, uint32_t n);
uint32_t csprng_u32(csprng_t *rng);

/* Periodic re-seeding from hardware entropy (RDRAND/RDSEED).
   Strengthens forward secrecy by mixing fresh entropy into the state
   without replacing it entirely (XOR-mix approach).
*/

void csprng_reseed_from_hardware(csprng_t *rng);

/* Global CSPRNG instance (for TLS, DHCP, etc.) */
extern csprng_t g_csprng;

/* PBKDF2-HMAC-SHA256 (AUR-02) */

/* SHA-256 output size */
#define SHA256_DIGEST_SIZE 32

/* PBKDF2 parameters */
#define PBKDF2_SALT_SIZE  16
#define PBKDF2_HASH_SIZE  32
#define PBKDF2_ITERATIONS 100000

/* User entry with salted hash */
typedef struct {
    char     username[32];
    uint8_t  salt[PBKDF2_SALT_SIZE];
    uint8_t  derived_key[PBKDF2_HASH_SIZE];
    uint32_t iterations;
} SecureUserEntry;

/* SHA-256 standalone */
void sha256_raw(const uint8_t *data, uint32_t len, uint8_t out[32]);

/* HMAC-SHA256 */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *msg, uint32_t msg_len,
                 uint8_t mac[32]);

/* PBKDF2-HMAC-SHA256 */
void pbkdf2_hmac_sha256(const uint8_t *password, uint32_t pass_len,
                         const uint8_t *salt, uint32_t salt_len,
                         uint32_t iterations,
                         uint8_t *out, uint32_t out_len);

/* Derive a key from password + salt (convenience) */
void password_derive_key(const char *password,
                          const uint8_t salt[16],
                          uint32_t iterations,
                          uint8_t out_key[32]);

/* Constant-time comparison */
bool secure_memcmp(const uint8_t *a, const uint8_t *b, uint32_t len);

/* Syscall Pointer Validation (AUR-04) */

/* Access type flags */
#define ACCESS_READ   0x01
#define ACCESS_WRITE  0x02
#define ACCESS_RW     (ACCESS_READ | ACCESS_WRITE)

/* Memory zone boundaries (32-bit flat model, no paging) */
#define KERNEL_BASE       0x00000000U
#define VGA_TEXT_BASE     0x000B8000U
#define VGA_TEXT_END      0x000B8FA0U
#define VGA_GRAPHIC_BASE  0x000A0000U
#define VGA_GRAPHIC_END   0x000C0000U
#define USER_PTR_MIN      0x00100000U  /* 1MB — above BIOS/VGA */
#define USER_PTR_MAX      0x7FFFFFFFU  /* 2GB — below kernel area */

/* Validate a user-supplied pointer for safety.
 * Returns true if safe to dereference, false otherwise.
 * Checks: non-null, within user address space, no overflow on ptr+len,
 *         does not overlap kernel/VGA zones. */
bool validate_user_ptr(const void *ptr, size_t len, int access_type);

/* Constants */

#define E_FAULT  6  /* Bad address / segfault equivalent */

#endif /* SECURITY_H */
