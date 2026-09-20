/*
 * kpanic_log.c - panic journal: ring buffer + serial replay.
 *
 * I keep the last 8 panics in .bss so a double-fault still shows the
 * first oops (the one that matters). Each record carries a checksum;
 * replay skips entries that fail it instead of printing corrupt data.
 * No heap, no file I/O: disks may be the thing that crashed.
*/

#include "kpanic.h"

#define KP_LOG_N 8u

static kpanic_record_t k_log[KP_LOG_N];
static uint32_t k_log_n;
static uint32_t k_log_seq;

void kpanic_log_usage_serial(void);
uint32_t kpanic_log_dropped(void);

/* CRC-8/SMBUS (poly 0x07) table. I use it for the journal slot
 * checksum upgrade path: keep the xor checksum on the wire format
 * (compat), verify CRC on replay when present. Table is the standard
 * MSB-first 256-entry expansion of x^8+x^2+x+1. */
static const uint8_t k_crc8_tab[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15,
    0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65,
    0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5,
    0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85,
    0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2,
    0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2,
    0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32,
    0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42,
    0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C,
    0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC,
    0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C,
    0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C,
    0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B,
    0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B,
    0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB,
    0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB,
    0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
};

static uint8_t crc8_record(const kpanic_record_t *r) {
    const uint8_t *b = (const uint8_t *)r;
    uint32_t n = (uint32_t)sizeof(kpanic_record_t) - 1;
    uint8_t c = 0;
    for (uint32_t i = 4; i < n; i++)
        c = k_crc8_tab[c ^ b[i]];
    return c ? c : 0xA5u;
}

uint8_t kpanic_checksum(const kpanic_record_t *r) {
    const uint8_t *b;
    uint32_t n;
    uint8_t x = 0;
    if (!r) return 0;
    b = (const uint8_t *)r;
    /* xor every byte except magic/seq-independent tail: I cover the  */
    /* whole struct minus the checksum field itself.                  */
    n = (uint32_t)sizeof(kpanic_record_t);
    n -= 1; /* last byte is checksum (u8, no padding assumed at tail) */
    /* Defensive: if the compiler padded the tail, the loop still     */
    /* stays in-bounds because n < sizeof.                            */
    for (uint32_t i = 0; i < n; i++) {
        /* Skip the magic word so a zeroed slot checksums to 0 and is */
        /* trivially distinguishable from a real record.              */
        if (i < 4) continue;
        x ^= b[i];
    }
    return x ? x : 0xA5u;
}

static bool record_valid(const kpanic_record_t *r) {
    uint8_t want;
    if (!r) return false;
    if (r->magic != KPANIC_MAGIC) return false;
    if (r->domain >= KP_DOMAIN_COUNT) return false;
    want = kpanic_checksum(r);
    if (want == r->checksum) return true;
    /* Upgrade path: accept CRC8 as well so journals written by a    */
    /* newer build still replay on this one.                        */
    return crc8_record(r) == r->checksum;
}

void kpanic_log_push(const kpanic_record_t *rec) {
    kpanic_record_t *slot;
    if (!rec) return;
    slot = &k_log[k_log_n % KP_LOG_N];
    /* Bounded copy: struct assignment, no strcpy anywhere near here. */
    *slot = *rec;
    slot->checksum = 0;
    slot->checksum = kpanic_checksum(slot);
    k_log_n++;
    if (rec->seq >= k_log_seq) k_log_seq = rec->seq + 1;
}

uint32_t kpanic_log_seq_next(void) {
    return k_log_seq;
}

void kpanic_log_seq_set(uint32_t s) {
    k_log_seq = s;
}

uint32_t kpanic_log_count(void) {
    return k_log_n < KP_LOG_N ? k_log_n : KP_LOG_N;
}

const kpanic_record_t *kpanic_log_get(uint32_t index) {
    uint32_t total = kpanic_log_count();
    uint32_t base;
    if (index >= total) return NULL;
    /* Oldest first: base is the slot after the newest when wrapped. */
    base = (k_log_n < KP_LOG_N) ? 0 : (k_log_n % KP_LOG_N);
    {
        const kpanic_record_t *r = &k_log[(base + index) % KP_LOG_N];
        return record_valid(r) ? r : NULL;
    }
}

void kpanic_log_replay_serial(void) {
    uint32_t total = kpanic_log_count();
    char nb[16];
    if (!total) {
        kpanic_serial_puts("(panic journal empty)\n");
        return;
    }
    kpanic_serial_puts("--- panic journal replay (oldest first) ---\n");
    {
        char nb[16];
        kpanic_serial_puts("journal seq base=");
        kpanic_u32_dec(kpanic_log_seq_next(), nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts("\n");
    }
    for (uint32_t i = 0; i < total; i++) {
        const kpanic_record_t *r = kpanic_log_get(i);
        if (!r) {
            kpanic_serial_puts("#");
            kpanic_u32_dec(i, nb);
            kpanic_serial_puts(nb);
            kpanic_serial_puts(": <checksum fail, skipped>\n");
            continue;
        }
        kpanic_serial_puts("#");
        kpanic_u32_dec(i, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts(" Oops #");
        kpanic_u32_dec(r->seq, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts(" [");
        kpanic_serial_puts(kpanic_domain_name(r->domain));
        kpanic_serial_puts("] ");
        kpanic_serial_puts(r->message[0] ? r->message : "(no message)");
        kpanic_serial_puts(" @");
        kpanic_serial_puts(r->file[0] ? r->file : "?");
        kpanic_serial_puts(":");
        kpanic_u32_dec(r->line, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts("\n");
    }
    kpanic_serial_puts("--- end journal ---\n");
    kpanic_log_verify_all_serial();
}

/* Verify every journal slot and report PASS/FAIL with the stored vs  */
/* recomputed checksum in hex8. Corrupt slots are listed, never       */
/* printed: printing them would leak stale pointers as if live.       */
void kpanic_log_verify_all_serial(void) {
    char nb[16];
    char h1[16];
    char h2[16];
    kpanic_serial_puts("--- journal verify ---\n");
    for (uint32_t s = 0; s < KP_LOG_N; s++) {
        const kpanic_record_t *r = &k_log[s];
        uint8_t want;
        if (r->magic != KPANIC_MAGIC) {
            kpanic_serial_puts("slot ");
            kpanic_u32_dec(s, nb);
            kpanic_serial_puts(nb);
            kpanic_serial_puts(": empty\n");
            continue;
        }
        want = kpanic_checksum(r);
        kpanic_u32_hex8(r->checksum, h1);
        kpanic_u32_hex8(want, h2);
        kpanic_serial_puts("slot ");
        kpanic_u32_dec(s, nb);
        kpanic_serial_puts(nb);
        if (want == r->checksum || crc8_record(r) == r->checksum) {
            kpanic_serial_puts(": PASS ck=");
            kpanic_serial_puts(h1);
            kpanic_serial_puts("\n");
        } else {
            kpanic_serial_puts(": FAIL stored=");
            kpanic_serial_puts(h1);
            kpanic_serial_puts(" recomputed=");
            kpanic_serial_puts(h2);
            kpanic_serial_puts(" (not printed)\n");
        }
    }
    kpanic_serial_puts("--- end verify ---\n");
    kpanic_log_usage_serial();
}

const kpanic_record_t *kpanic_log_last(void) {
    uint32_t total = kpanic_log_count();
    if (!total) return NULL;
    return kpanic_log_get(total - 1);
}

void kpanic_log_usage_serial(void) {
    char nb[16];
    kpanic_serial_puts("journal slots used ");
    kpanic_u32_dec(kpanic_log_count(), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts("/");
    kpanic_u32_dec(KP_LOG_N, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" total pushes ");
    kpanic_u32_dec(k_log_n, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" dropped ");
    kpanic_u32_dec(kpanic_log_dropped(), nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts("\n");
}

/* Ring capacity, for the usage line and for harnesses that size     */
/* their overlap math against the journal.                            */
uint32_t kpanic_log_capacity(void) {
    return KP_LOG_N;
}

/* Pushes that no longer fit the ring. The journal keeps the newest  */
/* KP_LOG_N; dropped() tells the bug report how many oldest oopses   */
/* scrolled off (usually 0: two panics in a row is already rare).     */
uint32_t kpanic_log_dropped(void) {
    uint32_t kept = kpanic_log_count();
    return (k_log_n > kept) ? (k_log_n - kept) : 0;
}

/* Hex export of one journal slot: 16 bytes per line, offset prefix. */
/* For capturing the raw record over a serial line when the pretty   */
/* printer itself is suspected (self-hosting check). Bounded.        */
void kpanic_log_export_hex(uint32_t slot) {
    static const char dig[] = "0123456789ABCDEF";
    const uint8_t *b;
    uint32_t n;
    char nb[16];
    if (slot >= KP_LOG_N) {
        kpanic_serial_puts("export: slot out of range\n");
        return;
    }
    b = (const uint8_t *)&k_log[slot];
    n = (uint32_t)sizeof(kpanic_record_t);
    kpanic_serial_puts("export slot ");
    kpanic_u32_dec(slot, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" bytes ");
    kpanic_u32_dec(n, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts("\n");
    for (uint32_t off = 0; off < n; off += 16) {
        char h4[8];
        uint32_t ln = (n - off > 16) ? 16 : (n - off);
        /* Offset as 4 hex digits (records are <64KB). */
        h4[0] = dig[(off >> 12) & 0xF];
        h4[1] = dig[(off >> 8) & 0xF];
        h4[2] = dig[(off >> 4) & 0xF];
        h4[3] = dig[off & 0xF];
        h4[4] = '\0';
        kpanic_serial_puts(h4);
        kpanic_serial_puts(": ");
        for (uint32_t i = 0; i < ln; i++) {
            char bb[3];
            bb[0] = dig[(b[off + i] >> 4) & 0xF];
            bb[1] = dig[b[off + i] & 0xF];
            bb[2] = '\0';
            kpanic_serial_puts(bb);
            kpanic_serial_puts(" ");
        }
        kpanic_serial_puts("\n");
    }
    kpanic_serial_puts("export done\n");
}
