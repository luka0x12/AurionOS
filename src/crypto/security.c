/*
 * security.c — AurionOS Security Primitives Implementation
 * CSPRNG, PBKDF2-HMAC-SHA256, safe arithmetic, pointer validation.
 * AUR-01, AUR-02, AUR-04, AUR-05
*/

#include "security.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Hardware entropy helpers */

static inline bool cpu_has_rdrand(void) {
    uint32_t ecx;
    __asm__ volatile("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
    return (ecx & (1U << 30)) != 0;
}

static inline bool cpu_has_rdseed(void) {
    uint32_t ebx;
    __asm__ volatile("cpuid" : "=b"(ebx) : "a"(7), "c"(0) : "edx");
    return (ebx & (1U << 18)) != 0;
}

static inline uint32_t rdrand32(void) {
    uint32_t val;
    __asm__ volatile("rdrand %0" : "=r"(val));
    return val;
}

static inline uint32_t rdseed32(void) {
    uint32_t val;
    __asm__ volatile("rdseed %0" : "=r"(val));
    return val;
}

/* TSC for jitter entropy */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* PIT timer for additional entropy */
extern uint32_t get_ticks(void);

/* CSPRNG — ChaCha20 (AUR-01)
 * Replaces the LCG at tls12_client.c:36. */

csprng_t g_csprng;

static uint32_t csprng_rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void csprng_quarter_round(uint32_t *s, int a, int b, int c, int d) {
    s[a] += s[b]; s[d] ^= s[a]; s[d] = csprng_rotl32(s[d], 16);
    s[c] += s[d]; s[b] ^= s[c]; s[b] = csprng_rotl32(s[b], 12);
    s[a] += s[b]; s[d] ^= s[a]; s[d] = csprng_rotl32(s[d], 8);
    s[c] += s[d]; s[b] ^= s[c]; s[b] = csprng_rotl32(s[b], 7);
}

static void csprng_block(csprng_t *rng) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = rng->state[i];

    for (int i = 0; i < 10; i++) {
        /* Column rounds */
        csprng_quarter_round(x, 0, 4, 8, 12);
        csprng_quarter_round(x, 1, 5, 9, 13);
        csprng_quarter_round(x, 2, 6, 10, 14);
        csprng_quarter_round(x, 3, 7, 11, 15);
        /* Diagonal rounds */
        csprng_quarter_round(x, 0, 5, 10, 15);
        csprng_quarter_round(x, 1, 6, 11, 12);
        csprng_quarter_round(x, 2, 7, 8, 13);
        csprng_quarter_round(x, 3, 4, 9, 14);
    }

    for (int i = 0; i < 16; i++) {
        rng->output[i] = x[i] + rng->state[i];
    }

    /* Increment 64-bit counter in state[12], state[13] */
    rng->state[12]++;
    if (rng->state[12] == 0) rng->state[13]++;
    rng->output_pos = 0;

    /* Periodic re-seeding from hardware entropy.
       Every reseed_interval blocks, XOR fresh RDRAND/RDSEED bytes
       into the nonce slots (state[14], state[15]). This strengthens
       forward secrecy without replacing the full state. */
    if (rng->reseed_interval > 0) {
        rng->reseed_ctr++;
        if (rng->reseed_ctr >= rng->reseed_interval) {
            rng->reseed_ctr = 0;
            csprng_reseed_from_hardware(rng);
        }
    }
}

/* Mix fresh hardware entropy into the CSPRNG state.
   Uses RDRAND/RDSEED to generate 8 bytes and XOR them into
   the nonce portion of the ChaCha20 state (state[14..15]).
   This is a hedge — not a full re-seed — so even if RDRAND
   is weak, the existing state remains dominant. */
void csprng_reseed_from_hardware(csprng_t *rng) {
    uint8_t fresh[8];
    for (int i = 0; i < 8; i++) fresh[i] = 0;

    if (cpu_has_rdrand()) {
        uint32_t r = rdrand32();
        fresh[0] ^= (uint8_t)(r);
        fresh[1] ^= (uint8_t)(r >> 8);
        fresh[2] ^= (uint8_t)(r >> 16);
        fresh[3] ^= (uint8_t)(r >> 24);
        r = rdrand32();
        fresh[4] ^= (uint8_t)(r);
        fresh[5] ^= (uint8_t)(r >> 8);
        fresh[6] ^= (uint8_t)(r >> 16);
        fresh[7] ^= (uint8_t)(r >> 24);
    } else if (cpu_has_rdseed()) {
        uint32_t r = rdseed32();
        fresh[0] ^= (uint8_t)(r);
        fresh[1] ^= (uint8_t)(r >> 8);
        fresh[2] ^= (uint8_t)(r >> 16);
        fresh[3] ^= (uint8_t)(r >> 24);
        r = rdseed32();
        fresh[4] ^= (uint8_t)(r);
        fresh[5] ^= (uint8_t)(r >> 8);
        fresh[6] ^= (uint8_t)(r >> 16);
        fresh[7] ^= (uint8_t)(r >> 24);
    } else {
        /* No hardware entropy — fall back to TSC jitter */
        uint64_t t = rdtsc();
        fresh[0] ^= (uint8_t)(t);
        fresh[1] ^= (uint8_t)(t >> 8);
        fresh[2] ^= (uint8_t)(t >> 16);
        fresh[3] ^= (uint8_t)(t >> 24);
        fresh[4] ^= (uint8_t)(t >> 32);
        fresh[5] ^= (uint8_t)(t >> 40);
        fresh[6] ^= (uint8_t)(t >> 48);
        fresh[7] ^= (uint8_t)(t >> 56);
    }

    /* XOR fresh entropy into the nonce slots */
    uint32_t f0 = ((uint32_t)fresh[0]) | ((uint32_t)fresh[1] << 8) |
                  ((uint32_t)fresh[2] << 16) | ((uint32_t)fresh[3] << 24);
    uint32_t f1 = ((uint32_t)fresh[4]) | ((uint32_t)fresh[5] << 8) |
                  ((uint32_t)fresh[6] << 16) | ((uint32_t)fresh[7] << 24);
    rng->state[14] ^= f0;
    rng->state[15] ^= f1;
}

void csprng_init(csprng_t *rng) {
    for (int i = 0; i < 16; i++) rng->state[i] = 0;
    for (int i = 0; i < 16; i++) rng->output[i] = 0;
    rng->output_pos = 0;
    rng->seeded = false;
    rng->reseed_ctr = 0;
    rng->reseed_interval = 1000; /* Re-seed every 1000 blocks (~64KB) */

    /* Mix hardware entropy sources */
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = 0;

    /* Source 1: RDRAND if available */
    if (cpu_has_rdrand()) {
        for (int i = 0; i < 8; i++) {
            uint32_t r = rdrand32();
            seed[i * 4 + 0] ^= (uint8_t)(r);
            seed[i * 4 + 1] ^= (uint8_t)(r >> 8);
            seed[i * 4 + 2] ^= (uint8_t)(r >> 16);
            seed[i * 4 + 3] ^= (uint8_t)(r >> 24);
        }
    }

    /* Source 2: RDSEED if available */
    if (cpu_has_rdseed()) {
        for (int i = 0; i < 4; i++) {
            uint32_t r = rdseed32();
            seed[i * 4 + 0] ^= (uint8_t)(r);
            seed[i * 4 + 1] ^= (uint8_t)(r >> 8);
            seed[i * 4 + 2] ^= (uint8_t)(r >> 16);
            seed[i * 4 + 3] ^= (uint8_t)(r >> 24);
        }
    }

    /* Source 3: TSC jitter (timing skew between instructions) */
    for (int i = 0; i < 16; i++) {
        uint64_t t1 = rdtsc();
        __asm__ volatile("nop; nop; nop");
        uint64_t t2 = rdtsc();
        uint32_t jitter = (uint32_t)(t2 - t1);
        seed[(i * 2) % 32] ^= (uint8_t)(jitter);
        seed[(i * 2 + 1) % 32] ^= (uint8_t)(jitter >> 8);
    }

    /* Source 4: PIT timer ticks */
    {
        uint32_t t = get_ticks();
        seed[24] ^= (uint8_t)(t);
        seed[25] ^= (uint8_t)(t >> 8);
        seed[26] ^= (uint8_t)(t >> 16);
        seed[27] ^= (uint8_t)(t >> 24);
    }

    csprng_seed(rng, seed);
}

void csprng_seed(csprng_t *rng, const uint8_t seed[32]) {
    /* ChaCha20 constants: "expand 32-byte k" */
    rng->state[0] = 0x61707865U;
    rng->state[1] = 0x3320646EU;
    rng->state[2] = 0x79622D32U;
    rng->state[3] = 0x6B206574U;

    /* Key (256 bits) */
    for (int i = 0; i < 8; i++) {
        rng->state[4 + i] = ((uint32_t)seed[i * 4 + 0]) |
                             ((uint32_t)seed[i * 4 + 1] << 8) |
                             ((uint32_t)seed[i * 4 + 2] << 16) |
                             ((uint32_t)seed[i * 4 + 3] << 24);
    }

    /* Counter */
    rng->state[12] = 0;
    rng->state[13] = 0;

    /* Nonce (from TSC for uniqueness) */
    uint64_t t = rdtsc();
    rng->state[14] = (uint32_t)t;
    rng->state[15] = (uint32_t)(t >> 32) ^ get_ticks();

    rng->output_pos = 0;
    rng->seeded = true;

    /* Generate first block */
    csprng_block(rng);
}

void csprng_bytes(csprng_t *rng, uint8_t *out, uint32_t n) {
    if (!rng->seeded) csprng_init(rng);

    for (uint32_t i = 0; i < n; i++) {
        if (rng->output_pos >= 16) {
            csprng_block(rng);
        }
        out[i] = (uint8_t)(rng->output[rng->output_pos] & 0xFF);
        rng->output[rng->output_pos] >>= 8;
        rng->output_pos++;
    }
}

uint32_t csprng_u32(csprng_t *rng) {
    uint8_t buf[4];
    csprng_bytes(rng, buf, 4);
    return ((uint32_t)buf[0]) | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* SHA-256 (AUR-02) */

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_transform(uint32_t st[8], const uint8_t block[64]) {
    uint32_t W[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        W[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        W[i] = SIG1(W[i - 2]) + W[i - 7] + SIG0(W[i - 15]) + W[i - 16];
    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + W[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void mem_cpy_local(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void mem_set_local(uint8_t *d, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

void sha256_raw(const uint8_t *data, uint32_t len, uint8_t out[32]) {
    uint32_t st[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    uint8_t buf[64];
    uint32_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_transform(st, data + i);
    uint32_t rem = len - i;
    mem_set_local(buf, 0, 64);
    mem_cpy_local(buf, data + i, rem);
    buf[rem] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8u;
    if (rem + 1 > 56) {
        sha256_transform(st, buf);
        mem_set_local(buf, 0, 64);
    }
    for (int j = 0; j < 8; j++) buf[63 - j] = (uint8_t)(bitlen >> (8 * j));
    sha256_transform(st, buf);
    for (int w = 0; w < 8; w++) {
        out[w * 4 + 0] = (uint8_t)(st[w] >> 24);
        out[w * 4 + 1] = (uint8_t)(st[w] >> 16);
        out[w * 4 + 2] = (uint8_t)(st[w] >> 8);
        out[w * 4 + 3] = (uint8_t)(st[w]);
    }
}

/* HMAC-SHA256 (AUR-02) */

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *msg, uint32_t msg_len,
                 uint8_t mac[32]) {
    uint8_t kipad[64], kopad[64], tk[32];
    const uint8_t *k = key;
    uint32_t kl = key_len;

    if (kl > 64) {
        sha256_raw(key, kl, tk);
        k = tk;
        kl = 32;
    }

    mem_set_local(kipad, 0x36, 64);
    mem_set_local(kopad, 0x5c, 64);
    for (uint32_t i = 0; i < kl; i++) {
        kipad[i] ^= k[i];
        kopad[i] ^= k[i];
    }

    uint8_t inner[64 + 1024];
    if (64 + msg_len > sizeof(inner)) {
        mem_set_local(mac, 0, 32);
        return;
    }
    mem_cpy_local(inner, kipad, 64);
    mem_cpy_local(inner + 64, msg, msg_len);
    uint8_t ih[32];
    sha256_raw(inner, 64 + msg_len, ih);

    uint8_t outer[64 + 32];
    mem_cpy_local(outer, kopad, 64);
    mem_cpy_local(outer + 64, ih, 32);
    sha256_raw(outer, 96, mac);
}

/* PBKDF2-HMAC-SHA256 (AUR-02) */

void pbkdf2_hmac_sha256(const uint8_t *password, uint32_t pass_len,
                         const uint8_t *salt, uint32_t salt_len,
                         uint32_t iterations,
                         uint8_t *out, uint32_t out_len) {
    uint32_t blocks = (out_len + 31) / 32;
    uint8_t U[32], T[32];
    uint8_t salt_block[256]; /* salt + 4-byte block counter */

    for (uint32_t block = 1; block <= blocks; block++) {
        /* Construct salt || INT(block) */
        if (salt_len + 4 > sizeof(salt_block)) {
            /* Salt too large; truncate */
            salt_len = sizeof(salt_block) - 4;
        }
        mem_cpy_local(salt_block, salt, salt_len);
        salt_block[salt_len + 0] = (uint8_t)(block >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block);

        /* U1 = HMAC(password, salt || INT(block)) */
        hmac_sha256(password, pass_len, salt_block, salt_len + 4, U);
        mem_cpy_local(T, U, 32);

        /* U2..Uiterations */
        for (uint32_t iter = 1; iter < iterations; iter++) {
            hmac_sha256(password, pass_len, U, 32, U);
            for (int j = 0; j < 32; j++) T[j] ^= U[j];
        }

        /* Copy to output */
        uint32_t offset = (block - 1) * 32;
        uint32_t to_copy = out_len - offset;
        if (to_copy > 32) to_copy = 32;
        mem_cpy_local(out + offset, T, to_copy);
    }
}

void password_derive_key(const char *password,
                          const uint8_t salt[16],
                          uint32_t iterations,
                          uint8_t out_key[32]) {
    uint32_t pass_len = 0;
    while (password[pass_len]) pass_len++;

    pbkdf2_hmac_sha256((const uint8_t *)password, pass_len,
                        salt, 16,
                        iterations,
                        out_key, 32);
}

bool secure_memcmp(const uint8_t *a, const uint8_t *b, uint32_t len) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

/* Syscall Pointer Validation (AUR-04) */

bool validate_user_ptr(const void *ptr, size_t len, int access_type) {
    (void)access_type; /* In no-paging mode, we can't check page permissions */

    /* Null pointer is always invalid */
    if (!ptr) return false;

    /* Zero-length is technically valid (no dereference needed) */
    if (len == 0) return true;

    uintptr_t addr = (uintptr_t)ptr;

    /* Check for overflow: addr + len must not wrap around */
    /* On 32-bit: UINTPTR_MAX = 0xFFFFFFFF */
    if (addr > 0xFFFFFFFFUL - len) return false;

    uintptr_t end = addr + len;

    /* Must be above minimum user address (avoid NULL page / BIOS area) */
    if (addr < USER_PTR_MIN) return false;

    /* Must be below maximum user address */
    if (end > USER_PTR_MAX) return false;

    /* Must not overlap VGA text memory */
    if (addr < VGA_TEXT_END && end > VGA_TEXT_BASE) return false;

    /* Must not overlap VGA graphic memory */
    if (addr < VGA_GRAPHIC_END && end > VGA_GRAPHIC_BASE) return false;

    return true;
}
