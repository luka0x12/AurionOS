/*
 * kpanic_compat.c - legacy panic API on top of the new subsystem.
 *
 * Old call sites (handlers.c isr_handler, PANIC()/PANIC_CODE macros)
 * keep compiling unchanged. Behavior changes: instead of the static
 * blue screen they get the full Linux-style dump + journal + reboot
 * policy. I classify the legacy code the same way the old handler
 * did: a CPU vector stays CPU-FAULT, everything else is ASSERT.
*/

#include "kpanic.h"

/* Heuristic: legacy callers passed the ISR vector as error_code from */
/* isr_handler ("CPU exception occurred", code=int_no). When the code */
/* is <32 and the message names the CPU, route to the CPU decoder so  */
/* the dump shows the fault class instead of a bare ASSERT.           */
static bool looks_like_cpu_vector(const char *message, uint32_t code) {
    if (code >= 32) return false;
    if (!message || !message[0]) return true;
    /* isr_handler passes exactly this string. */
    if (kpanic_streq(message, "CPU exception occurred")) return true;
    if (kpanic_streq(message, "CPU exception")) return true;
    return false;
}

void kernel_panic(const char *message, const char *file, int line, uint32_t error_code) {
    uint32_t ln = (line < 0) ? 0u : (uint32_t)line;
    /* No ISR frame is available on this path (legacy signature has no */
    /* regs pointer), so I capture CPU/VRAM/stack live inside enter.   */
    if (looks_like_cpu_vector(message, error_code)) {
        /* Synthesize the classification without faking registers:    */
        /* has_regs=false prints "no ISR frame" honestly.             */
        kpanic_enter(KP_DOMAIN_CPU, KP_SEV_FATAL,
                     message && message[0] ? message : "CPU exception",
                     file, ln, error_code, NULL);
    } else {
        kpanic_domain_t d = kpanic_classify_legacy(message, error_code);
        kpanic_severity_t s = (d == KP_DOMAIN_HEAP) ? KP_SEV_CORRUPT
            : (d == KP_DOMAIN_SCHED)                ? KP_SEV_HANG
                                                    : KP_SEV_FATAL;
        kpanic_enter(d, s,
                     message && message[0] ? message : "kernel panic",
                     file, ln, error_code, NULL);
    }
}

void panic(const char *message) {
    kernel_panic(message, NULL, 0, 0);
}

void panic_code(const char *message, uint32_t code) {
    kernel_panic(message, NULL, 0, code);
}

/* ISR hook for handlers.c. Pass the raw ISR frame straight through so */
/* registers are the faulting ones, not the dumper's.                  */
void kpanic_isr_entry(const void *regs_ptr, const char *file, uint32_t line) {
    const kpanic_regs_t *r = (const kpanic_regs_t *)regs_ptr;
    if (!r) {
        kernel_panic("CPU exception occurred (null frame)", file, (int)line, 0);
        return;
    }
    kpanic_cpu_fault(r, file, line);
}

/* Legacy message classifier. Old code panics with free-form strings; */
/* this table routes known ones to the right domain so the AUX line   */
/* shows live subsystem state instead of a bare ASSERT. Unknown       */
/* strings stay ASSERT (honest) rather than guessing.                 */

typedef struct kpanic_legacy_rule {
    const char *substr;
    kpanic_domain_t domain;
} kpanic_legacy_rule_t;

static const kpanic_legacy_rule_t k_legacy_rules[] = {
    { "CPU exception", KP_DOMAIN_CPU },
    { "out of memory", KP_DOMAIN_OOM },
    { "Out of memory", KP_DOMAIN_OOM },
    { "kmalloc", KP_DOMAIN_OOM },
    { "VRAM", KP_DOMAIN_VRAM },
    { "framebuffer", KP_DOMAIN_VRAM },
    { "VBE", KP_DOMAIN_VRAM },
    { "window", KP_DOMAIN_GUI },
    { "compositor", KP_DOMAIN_GUI },
    { "desktop", KP_DOMAIN_GUI },
    { "FAT", KP_DOMAIN_VFS },
    { "ISO", KP_DOMAIN_VFS },
    { "ATA", KP_DOMAIN_VFS },
    { "disk", KP_DOMAIN_VFS },
    { "TCP", KP_DOMAIN_NET },
    { "DHCP", KP_DOMAIN_NET },
    { "TLS", KP_DOMAIN_NET },
    { "DNS", KP_DOMAIN_NET },
    { "ARP", KP_DOMAIN_NET },
    { "sched", KP_DOMAIN_SCHED },
    { "stack overflow", KP_DOMAIN_SCHED },
    { "TSS", KP_DOMAIN_SCHED },
    { "PCI", KP_DOMAIN_DRIVER },
    { "NE2000", KP_DOMAIN_DRIVER },
    { "RTL8139", KP_DOMAIN_DRIVER },
    { "virtio", KP_DOMAIN_DRIVER },
    { "SVGA", KP_DOMAIN_DRIVER },
    { "heap", KP_DOMAIN_HEAP },
    { "corrupt", KP_DOMAIN_HEAP },
    { "assert", KP_DOMAIN_ASSERT },
};

static bool substr_match(const char *hay, const char *needle) {
    uint32_t hn;
    uint32_t nn;
    uint32_t i, j;
    if (!hay || !needle || !needle[0]) return false;
    hn = kpanic_strlen(hay);
    nn = kpanic_strlen(needle);
    if (nn > hn) return false;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++)
            if (hay[i + j] != needle[j]) break;
        if (j == nn) return true;
    }
    return false;
}

kpanic_domain_t kpanic_classify_legacy(const char *message, uint32_t code) {
    (void)code;
    if (!message) return KP_DOMAIN_ASSERT;
    for (uint32_t i = 0; i < sizeof(k_legacy_rules) / sizeof(k_legacy_rules[0]); i++)
        if (substr_match(message, k_legacy_rules[i].substr))
            return k_legacy_rules[i].domain;
    return KP_DOMAIN_ASSERT;
}

/* Register-aware entry for the new ISR path in handlers.c. The ISR   */
/* stub pushes the raw frame; this validates the pointer (must be in  */
/* readable RAM, aligned) before trusting a single word of it, then   */
/* routes to the CPU decoder with the real file/line of the fault.    */

void kpanic_panic_with_regs(const void *regs_ptr, const char *file, uint32_t line) {
    const kpanic_regs_t *r = (const kpanic_regs_t *)regs_ptr;
    uint32_t addr = (uint32_t)regs_ptr;
    if (!regs_ptr) {
        kernel_panic("CPU exception (null register frame)", file, (int)line, 0);
        return;
    }
    if (addr & 0x03u) {
        kernel_panic("CPU exception (misaligned register frame)", file, (int)line,
                     (uint32_t)addr);
        return;
    }
    if (!kpanic_is_readable(addr, (uint32_t)sizeof(kpanic_regs_t))) {
        kernel_panic("CPU exception (register frame outside RAM window)", file,
                     (int)line, (uint32_t)addr);
        return;
    }
    /* Sanity on the two fields the decoder trusts most. */
    if (r->int_no > 255) {
        kernel_panic("CPU exception (wild vector in register frame)", file,
                     (int)line, r->int_no);
        return;
    }
    kpanic_cpu_fault(r, file, line);
}

/* Taint flags. Linux marks kernels running proprietary/out-of-tree   */
/* code; I mark which optional drivers were linked so a vendor bug is */
/* visible on the panic screen. Weak symbols: present=linked.         */

__attribute__((weak)) bool vmware_svga_available(void) { return false; }
__attribute__((weak)) int rtl8139_present(void) { return 0; }
__attribute__((weak)) int ne2000_present(void) { return 0; }

uint32_t kpanic_taint_flags(char *out, uint32_t out_len) {
    uint32_t p = 0;
    uint32_t flags = 0;
    if (!out || !out_len) return 0;
    out[0] = '\0';
    /* Bit 0: VMware SVGA active (out-of-tree GPU path). The weak def  */
    /* below returns false; the real driver overrides it at link.     */
    if (vmware_svga_available()) {
        flags |= 0x01u;
        {
            const char *t = "SVGA ";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
    }
    if (p == 0) {
        const char *t = "clean (no taint sources linked)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    } else if (p > 0 && out[p - 1] == ' ') {
        out[p - 1] = '\0';
        return flags;
    }
    out[p] = '\0';
    return flags;
}

/* Watchdog entry. Call from the timer tick when the runqueue stops   */
/* making progress: if ticks advance but the scheduler epoch does     */
/* not, the CPU is alive and nothing schedules -> HANG panic with the */
/* live tick count as evidence. The epoch pointer is weak so builds   */
/* without the scheduler hook link and simply never fire.             */

__attribute__((weak)) uint32_t sched_epoch(void) { return 0; }

/* Preemption provider. The cooperative scheduler has no preempt tick  */
/* yet, so this stays false and the watchdog only re-baselines (never */
/* fires). When the scheduler gains preemption it defines the strong  */
/* symbol returning true and the watchdog arms automatically.         */
__attribute__((weak)) bool sched_preempt_active(void) { return false; }

void kpanic_watchdog(const char *file, uint32_t line) {
    static uint32_t last_epoch;
    static uint32_t last_ticks;
    static uint32_t stalls;
    static bool init;
    uint32_t ep = sched_epoch();
    uint32_t tk = 0;
    bool armed = sched_preempt_active();
    {
        extern uint32_t get_ticks(void);
        tk = get_ticks();
    }
    if (!init) {
        last_epoch = ep;
        last_ticks = tk;
        stalls = 0;
        init = true;
        return;
    }
    if (ep != last_epoch) {
        /* Progress: reset. */
        last_epoch = ep;
        last_ticks = tk;
        stalls = 0;
        return;
    }
    if (!armed) {
        /* No preempt source: re-baseline ticks and wait. An idle      */
        /* cooperative system looks exactly like a hung one here, so   */
        /* firing would reboot healthy machines. Never fire dormant.   */
        last_epoch = ep;
        last_ticks = tk;
        stalls = 0;
        return;
    }
    /* No progress. Only fire when ticks themselves advance (proves   */
    /* IRQs work and the hang is in scheduling, not a dead CPU).      */
    if (tk != last_ticks) {
        last_ticks = tk;
        stalls++;
        /* 500 ticks = 5s at 100Hz (KP_WATCHDOG_TICKS). */
        if (stalls >= KP_WATCHDOG_TICKS) {
            char msg[96];
            uint32_t p = 0;
            const char *t = "watchdog: no sched progress for 5s (epoch ";
            while (*t && p + 1 < sizeof(msg)) msg[p++] = *t++;
            {
                char nb[16];
                kpanic_u32_dec(ep, nb);
                t = nb;
                while (*t && p + 1 < sizeof(msg)) msg[p++] = *t++;
            }
            {
                const char *t2 = ")";
                while (*t2 && p + 1 < sizeof(msg)) msg[p++] = *t2++;
            }
            msg[p] = '\0';
            kpanic_enter(KP_DOMAIN_SCHED, KP_SEV_HANG, msg, file, line, ep, NULL);
        }
    }
}
