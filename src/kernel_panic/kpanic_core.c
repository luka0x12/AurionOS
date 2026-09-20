/*
 * kpanic_core.c - panic entry pipeline.
 *
 * Pipeline per panic: guard re-entry -> snapshot regs -> capture live
 * CPU -> capture live RAM/VRAM -> walk real stack -> build AUX line ->
 * checksum + journal -> render VGA/VESA/serial -> halt policy.
 *
 * Re-entrancy: the first panic owns the screen. A second panic while
 * dumped (double fault in the dumper) appends "(RE-ENTERED)" to serial
 * and reboots without re-rendering, so the original dump survives.
*/

#include "kpanic.h"

extern uint32_t kpanic_log_seq_next(void);
extern void kpanic_log_seq_set(uint32_t s);

/* Weak import: handlers.c defines it strong; harnesses without an ISR */
/* layer leave it NULL and the guard below skips the call.             */
__attribute__((weak)) void set_shutting_down(void);

static kpanic_record_t k_current;
static uint32_t k_depth;
static uint32_t k_seq;

uint32_t kpanic_depth(void) {
    return k_depth;
}

const kpanic_record_t *kpanic_current(void) {
    return &k_current;
}

static void record_init(kpanic_record_t *r, kpanic_domain_t domain,
                        kpanic_severity_t sev, const char *msg,
                        const char *file, uint32_t line, uint32_t code,
                        const kpanic_regs_t *regs) {
    uint32_t i;
    r->magic = KPANIC_MAGIC;
    r->seq = k_seq++;
    r->domain = (domain < KP_DOMAIN_COUNT) ? domain : KP_DOMAIN_UNKNOWN;
    r->severity = sev;
    r->line = line;
    r->code = code;
    r->has_regs = (regs != NULL);
    if (regs) r->regs = *regs;
    else {
        for (i = 0; i < (uint32_t)sizeof(r->regs); i++)
            ((uint8_t *)&r->regs)[i] = 0;
    }
    kpanic_strncpy(r->message, msg ? msg : "kernel panic", KP_MSG_LEN);
    if (file) kpanic_strncpy(r->file, file, KP_FILE_LEN);
    else r->file[0] = '\0';
    r->aux[0] = '\0';
    r->checksum = 0;
    r->uptime_ticks = 0;
}

static uint32_t core_read_ebp(void) {
    uint32_t v;
    __asm__ volatile("mov %%ebp, %0" : "=r"(v));
    return v;
}

static uint32_t core_read_esp(void) {
    uint32_t v;
    __asm__ volatile("mov %%esp, %0" : "=r"(v));
    return v;
}

/* Domain from a CPU vector: page faults and double faults are their  */
/* own triage class; everything else stays CPU-FAULT with the vector  */
/* preserved in code.                                                 */
static kpanic_domain_t domain_for_vector(uint32_t vec) {
    (void)vec;
    return KP_DOMAIN_CPU;
}

static kpanic_severity_t sev_for_vector(uint32_t vec) {
    switch (vec) {
    case 8:  /* #DF */
    case 18: /* #MC */
        return KP_SEV_CORRUPT;
    case 14: /* #PF */
    case 13: /* #GP */
    case 12: /* #SS */
    case 10: /* #TS */
    case 11: /* #NP */
        return KP_SEV_FATAL;
    default:
        return KP_SEV_FATAL;
    }
}

void kpanic_enter(kpanic_domain_t domain, kpanic_severity_t severity,
                  const char *message, const char *file, uint32_t line,
                  uint32_t code, const kpanic_regs_t *regs) {
    uint32_t ebp;
    uint32_t esp;
    uint32_t eip;
    uint32_t want = 0;
    bool reentered;

    __asm__ volatile("cli");
    k_depth++;
    /* Tell the ISR layer to stop panicking on stray interrupts while  */
    /* the dump owns the screen (handlers.c honors this flag). Weak so */
    /* harnesses without handlers.c still link.                        */
    if (set_shutting_down) set_shutting_down();
    reentered = (k_depth > 1);
    if (reentered) {
        kpanic_serial_puts("\n[KPANIC RE-ENTERED depth=");
        {
            char nb[16];
            kpanic_u32_dec(k_depth, nb);
            kpanic_serial_puts(nb);
        }
        kpanic_serial_puts(" while dumping; rebooting to protect first oops]\n");
        kpanic_post(0xEEu);
        kpanic_reboot_now();
        for (;;) kpanic_halt_cpu();
    }

    if (domain == KP_DOMAIN_OOM || domain == KP_DOMAIN_VRAM)
        want = code;

    record_init(&k_current, domain, severity, message, file, line, code, regs);

    /* Live captures. Order matters: CPU first (CR2 is per-fault and   */
    /* must be read before any call that could fault again).           */
    kpanic_cpu_capture(&k_current.cpu);
    k_current.uptime_ticks = k_current.cpu.ticks;
    kpanic_mem_capture(&k_current.mem, want);

    ebp = core_read_ebp();
    esp = core_read_esp();
    eip = regs ? regs->eip : 0;
    /* When called explicitly (no ISR frame), EIP is our return site; */
    /* the EBP chain still walks the real callers.                    */
    if (!eip) {
        eip = (uint32_t)__builtin_return_address(0);
    }
    kpanic_trace_capture(&k_current.trace, ebp, esp, eip);

    kpanic_subsys_line(k_current.domain,
                       k_current.message, k_current.code,
                       &k_current.mem, &k_current.cpu,
                       k_current.aux, KP_AUX_LEN);

    k_current.checksum = 0;
    k_current.checksum = kpanic_checksum(&k_current);
    kpanic_log_push(&k_current);
    kpanic_log_seq_set(k_seq);
    kpanic_post(kpanic_post_for_domain(k_current.domain, k_current.severity));
    if (k_current.severity == KP_SEV_CORRUPT) {
        /* Headless lifeline: blink domain+severity on the keyboard   */
        /* LEDs before rendering, so a dead screen still identifies   */
        /* the fault class. Bounded (~5s worst case).                 */
        kpanic_led_code(k_current.domain, k_current.severity);
    }

    kpanic_emit_all(&k_current);
    kpanic_post(0xE6u);
    kpanic_halt_loop(&k_current);
    for (;;) kpanic_halt_cpu();
}

/* Per-domain entries. Each names severity explicitly so a future edit */
/* cannot silently downgrade an OOM to an OOPS.                        */

void kpanic_cpu_fault(const kpanic_regs_t *regs, const char *file, uint32_t line) {
    kpanic_domain_t d;
    kpanic_severity_t s;
    uint32_t vec = regs ? regs->int_no : 0xFFFFFFFFu;
    const char *msg;
    if (!regs) {
        kpanic_enter(KP_DOMAIN_CPU, KP_SEV_FATAL, "CPU fault with no register frame",
                     file, line, vec, NULL);
        return;
    }
    d = domain_for_vector(vec);
    s = sev_for_vector(vec);
    msg = kpanic_exception_name(vec);
    kpanic_enter(d, s, msg, file, line, vec, regs);
    /* Forward the ISR error code into the record code slot when the   */
    /* vector carries one: the AUX decoder prints it.                  */
    (void)msg;
}

void kpanic_oom(uint32_t want_bytes, const char *file, uint32_t line) {
    kpanic_enter(KP_DOMAIN_OOM, KP_SEV_FATAL, "out of memory: kmalloc failed",
                 file, line, want_bytes, NULL);
}

void kpanic_vram(uint32_t want_bytes, const char *file, uint32_t line) {
    kpanic_enter(KP_DOMAIN_VRAM, KP_SEV_FATAL, "VRAM exhausted: framebuffer alloc failed",
                 file, line, want_bytes, NULL);
}

void kpanic_gui(const char *what, const char *file, uint32_t line, uint32_t code) {
    kpanic_enter(KP_DOMAIN_GUI, KP_SEV_FATAL,
                 what && what[0] ? what : "GUI crashed",
                 file, line, code, NULL);
}

void kpanic_vfs(const char *what, const char *file, uint32_t line, uint32_t code) {
    kpanic_enter(KP_DOMAIN_VFS, KP_SEV_FATAL,
                 what && what[0] ? what : "filesystem fault",
                 file, line, code, NULL);
}

void kpanic_net(const char *what, const char *file, uint32_t line, uint32_t code) {
    kpanic_enter(KP_DOMAIN_NET, KP_SEV_FATAL,
                 what && what[0] ? what : "network fault",
                 file, line, code, NULL);
}

void kpanic_sched(const char *what, const char *file, uint32_t line, uint32_t code) {
    kpanic_enter(KP_DOMAIN_SCHED, KP_SEV_HANG,
                 what && what[0] ? what : "scheduler fault",
                 file, line, code, NULL);
}

void kpanic_driver(const char *what, const char *file, uint32_t line, uint32_t code) {
    kpanic_enter(KP_DOMAIN_DRIVER, KP_SEV_FATAL,
                 what && what[0] ? what : "driver fault",
                 file, line, code, NULL);
}

void kpanic_heap(const char *what, const char *file, uint32_t line, uint32_t code) {
    kpanic_enter(KP_DOMAIN_HEAP, KP_SEV_CORRUPT,
                 what && what[0] ? what : "heap corruption detected",
                 file, line, code, NULL);
}

void kpanic_assert(const char *expr, const char *file, uint32_t line) {
    char msg[KP_MSG_LEN];
    uint32_t p = 0;
    const char *t = "assertion failed: ";
    while (*t && p + 1 < sizeof(msg)) msg[p++] = *t++;
    t = expr && expr[0] ? expr : "?";
    while (*t && p + 1 < sizeof(msg)) msg[p++] = *t++;
    msg[p] = '\0';
    kpanic_enter(KP_DOMAIN_ASSERT, KP_SEV_FATAL, msg, file, line, 0, NULL);
}

/* Selftest: validates every formatter/decoder/checksum WITHOUT       */
/* panicking. Returns 0 when all checks pass, else the 1-based index  */
/* of the first failure. out[] receives "PASS n/n" or "FAIL i". Safe  */
/* to call from the shell (uses only the panic static helpers).       */

int kpanic_selftest(char *out, uint32_t out_len) {
    uint32_t passed = 0;
    uint32_t total = 12;
    char b[64];
    char hx[24];
    char nb[16];
    /* 1: hex formatter. */
    kpanic_u32_hex(0xDEADBEEFu, hx);
    if (!kpanic_streq(hx, "0xDEADBEEF")) goto fail;
    passed++;
    /* 2: dec formatter, zero. */
    kpanic_u32_dec(0, nb);
    if (!kpanic_streq(nb, "0")) goto fail;
    passed++;
    /* 3: dec formatter, large. */
    kpanic_u32_dec(4294967295u, nb);
    if (!kpanic_streq(nb, "4294967295")) goto fail;
    passed++;
    /* 4: checked mul ok. */
    {
        uint32_t r = 0;
        if (!kpanic_safe_mul_u32(1920, 1080, &r) || r != 2073600u) goto fail;
        passed++;
    }
    /* 5: checked mul overflow caught. */
    {
        uint32_t r = 0;
        if (kpanic_safe_mul_u32(0x10000u, 0x10000u, &r)) goto fail;
        passed++;
    }
    /* 6: readability gate rejects NULL + wraps. */
    if (kpanic_is_readable(0, 4)) goto fail;
    passed++;
    if (kpanic_is_readable(0xFFFFFFFCu, 8)) goto fail;
    passed++;
    /* 7: readability accepts kernel window. */
    if (!kpanic_is_readable(0x10000u, 16)) goto fail;
    passed++;
    /* 8: exception names sane. */
    if (!kpanic_streq(kpanic_exception_name(14), "#PF Page Fault")) goto fail;
    passed++;
    /* 9: domain names sane. */
    if (!kpanic_streq(kpanic_domain_name(KP_DOMAIN_OOM), "OUT-OF-MEMORY")) goto fail;
    passed++;
    /* 10: byte formatter. */
    kpanic_format_bytes(2048, b, sizeof(b));
    if (!kpanic_streq(b, "2 KB")) goto fail;
    passed++;
    /* 11: framebuffer math. */
    {
        bool ov = false;
        uint32_t fb = kpanic_fb_bytes(1920, 1080, 32, &ov);
        (void)fb;
        if (ov) goto fail;
        if (fb != 8294400u) goto fail;
        passed++;
    }
    if (out && out_len) {
        uint32_t p = 0;
        const char *t = (passed == total) ? "KPANIC SELFTEST PASS " : "KPANIC SELFTEST PARTIAL ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        kpanic_u32_dec(passed, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
        {
            const char *t2 = "/";
            while (*t2 && p + 1 < out_len) out[p++] = *t2++;
        }
        kpanic_u32_dec(total, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
        out[p] = '\0';
    }
    return (passed == total) ? 0 : -1;
fail:
    if (out && out_len) {
        uint32_t p = 0;
        const char *t = "KPANIC SELFTEST FAIL at check ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        kpanic_u32_dec(passed + 1, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
        out[p] = '\0';
    }
    return (int)(passed + 1);
}

/* Uptime formatter: 100Hz ticks -> "2d 04:12:33". No division lib,   */
/* no floats, bounded. Days saturate at 9999 so the field never       */
/* overflows the panic header.                                        */

void kpanic_format_uptime(uint32_t ticks, char *out, uint32_t out_len) {
    uint32_t p = 0;
    uint32_t secs = ticks / 100u;
    uint32_t days = secs / 86400u;
    uint32_t hh, mm, ss;
    char nb[16];
    if (!out || !out_len) return;
    if (days > 9999u) days = 9999u;
    secs = secs % 86400u;
    hh = secs / 3600u;
    mm = (secs % 3600u) / 60u;
    ss = secs % 60u;
    kpanic_u32_dec(days, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = "d ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    /* Zero-padded HH:MM:SS. */
    if (hh < 10 && p + 1 < out_len) out[p++] = '0';
    kpanic_u32_dec(hh, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    if (p + 1 < out_len) out[p++] = ':';
    if (mm < 10 && p + 1 < out_len) out[p++] = '0';
    kpanic_u32_dec(mm, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    if (p + 1 < out_len) out[p++] = ':';
    if (ss < 10 && p + 1 < out_len) out[p++] = '0';
    kpanic_u32_dec(ss, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}
