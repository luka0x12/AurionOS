/*
 * kpanic_cpu.c - live CPU state capture + x86 fault decoders.
 *
 * I read control registers at panic time (CR2/CR3/CR0/EFLAGS/TSC/CPUID)
 * so the dump shows the faulting state, not a canned example. The
 * exception table covers vectors 0-31 with Linux-style names plus the
 * Aurion-specific hint for what to check next.
*/

#include "kpanic.h"

/* Low-level readers. Inline asm, no library, no allocation.           */

static uint32_t cpu_read_cr0(void) {
    uint32_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static uint32_t cpu_read_cr2(void) {
    uint32_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static uint32_t cpu_read_cr3(void) {
    uint32_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static uint32_t cpu_read_eflags(void) {
    uint32_t v;
    __asm__ volatile("pushf; popl %0" : "=r"(v));
    return v;
}

static uint64_t cpu_read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID wrapper. Returns false when the instruction faults or when the
 * ID flag in EFLAGS cannot be toggled (pre-486). I probe the ID flag
 * first so a #UD never escapes the panic path. */
static bool cpu_has_cpuid(void) {
    uint32_t a, b;
    __asm__ volatile(
        "pushf; popl %0; mov %0, %1; xor $0x200000, %1; push %1; popf;"
        " pushf; popl %1; xor %0, %1; push %0; popf"
        : "=&r"(a), "=&r"(b) :: "cc");
    return (b & 0x200000u) != 0;
}

static void cpu_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                      uint32_t *ecx, uint32_t *edx) {
    uint32_t a = leaf, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid"
                     : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

/* get_ticks lives in handlers.c (IRQ0, 100 Hz). Weak-linked so the
 * panic subsystem still links in freestanding test harnesses. */
__attribute__((weak)) uint32_t get_ticks(void) {
    return 0;
}

/* Public capture.                                                     */

void kpanic_cpu_capture(kpanic_cpu_state_t *out) {
    if (!out) return;
    out->cr0 = cpu_read_cr0();
    out->cr2 = cpu_read_cr2();
    out->cr3 = cpu_read_cr3();
    out->eflags_live = cpu_read_eflags();
    out->ticks = get_ticks();
    out->has_cpuid = cpu_has_cpuid();
    out->cpuid_max = 0;
    out->cpu_vendor0 = 0;
    out->cpu_vendor1 = 0;
    out->cpu_vendor2 = 0;
    out->cpu_feat_ecx = 0;
    out->cpu_feat_edx = 0;
    out->has_tsc = false;
    out->tsc = 0;
    if (out->has_cpuid) {
        uint32_t mx, b, c, d;
        cpu_cpuid(0, &mx, &b, &c, &d);
        out->cpuid_max = mx;
        out->cpu_vendor0 = b;
        out->cpu_vendor1 = d;
        out->cpu_vendor2 = c;
        if (mx >= 1) {
            uint32_t a1, b1, c1, d1;
            cpu_cpuid(1, &a1, &b1, &c1, &d1);
            out->cpu_feat_ecx = c1;
            out->cpu_feat_edx = d1;
            out->has_tsc = (d1 & (1u << 4)) != 0;
        }
    } else {
        /* No CPUID leaf to consult; assume TSC exists on 586+ and try
         * it. If the CPU is older the rdtsc faults, but that CPU cannot
         * run this kernel anyway (we need 32-bit + VBE). */
        out->has_tsc = true;
    }
    if (out->has_tsc) {
        /* rdtsc is unprivileged; safe on all CPUs we boot on. */
        out->tsc = cpu_read_tsc();
    }
}

void kpanic_format_vendor(const kpanic_cpu_state_t *cpu, char *out, uint32_t out_len) {
    uint32_t i = 0;
    const char *src;
    if (!cpu || !out || out_len < 16) {
        if (out && out_len) out[0] = '\0';
        return;
    }
    if (!cpu->has_cpuid) {
        kpanic_strncpy(out, "unknown(no-cpuid)", out_len);
        return;
    }
    /* EBX, EDX, ECX order per the CPUID spec. */
    src = (const char *)&cpu->cpu_vendor0;
    for (uint32_t k = 0; k < 4 && i + 1 < out_len; k++) out[i++] = src[k];
    src = (const char *)&cpu->cpu_vendor1;
    for (uint32_t k = 0; k < 4 && i + 1 < out_len; k++) out[i++] = src[k];
    src = (const char *)&cpu->cpu_vendor2;
    for (uint32_t k = 0; k < 4 && i + 1 < out_len; k++) out[i++] = src[k];
    out[i] = '\0';
}

/* Exception table 0-31. Names follow Intel SDM vol 3 + Linux arch/    */
/* x86 naming so developers can cross-reference both.                  */

typedef struct kpanic_exc {
    uint32_t    vec;
    const char *name;    /* short, Linux-style */
    const char *cls;     /* fault / trap / abort / interrupt / reserved */
    bool        has_err; /* CPU pushes an error code */
    const char *hint;    /* first thing to check on AurionOS */
} kpanic_exc_t;

static const kpanic_exc_t k_exc_table[32] = {
    { 0,  "#DE Divide Error",            "fault",    false, "DIV/IDIV denominator was zero; check divisor register" },
    { 1,  "#DB Debug",                   "trap",     false, "DR6 single-step or breakpoint; check debug registers" },
    { 2,  "NMI Non-Maskable Interrupt",  "interrupt",false, "RAM parity or watchdog NMI; check port 0x61" },
    { 3,  "#BP Breakpoint",              "trap",     false, "INT3 hit outside debugger; stray breakpoint left in code" },
    { 4,  "#OF Overflow",                "trap",     false, "INTO with OF set; arithmetic overflowed" },
    { 5,  "#BR Bound Range",             "fault",    false, "BOUND check failed; index outside [low,high]" },
    { 6,  "#UD Invalid Opcode",          "fault",    false, "bad opcode or SSE used with -mno-sse build flags" },
    { 7,  "#NM Device Not Available",    "fault",    false, "FPU/MMX without CR0.TS clear; check clts on task switch" },
    { 8,  "#DF Double Fault",            "abort",    true,  "exception inside handler; IST1 stack or IDT entry first" },
    { 9,  "Coprocessor Segment Overrun", "abort",    false, "legacy 386 FPU fault; should not happen on 486+" },
    { 10, "#TS Invalid TSS",             "fault",    true,  "TSS selector bad on task switch or IRET; check GDT 0x28" },
    { 11, "#NP Segment Not Present",     "fault",    true,  "segment P=0 loaded; check GDT/LDT present bits" },
    { 12, "#SS Stack Fault",             "fault",    true,  "SS limit or canonical stack overflow; check ESP/RSP0" },
    { 13, "#GP General Protection",      "fault",    true,  "privilege or segment violation; decode err_code below" },
    { 14, "#PF Page Fault",              "fault",    true,  "CR2 holds fault addr; decode P/W/U/RSVD/I-D below" },
    { 15, "Reserved (vec 15)",           "reserved", false, "Intel reserved; spurious vector means PIC/APIC misroute" },
    { 16, "#MF x87 FPU Error",           "fault",    false, "unmasked x87 exception; check FPU status word" },
    { 17, "#AC Alignment Check",         "fault",    true,  "misaligned access with CR0.AM+CPL3; check packed structs" },
    { 18, "#MC Machine Check",           "abort",    false, "hardware error; check MCA banks, RAM, thermals" },
    { 19, "#XM SIMD FP Exception",       "fault",    false, "SSE numeric error; masked-bit config in MXCSR" },
    { 20, "#VE Virtualization",          "fault",    false, "EPT violation; only under hypervisor with #VE enabled" },
    { 21, "#CP Control Protection",      "fault",    true,  "CET shadow-stack violation; check SSP and PL3_SSP" },
    { 22, "Reserved (vec 22)",           "reserved", false, "Intel reserved; treat like vec 15" },
    { 23, "Reserved (vec 23)",           "reserved", false, "Intel reserved; treat like vec 15" },
    { 24, "Reserved (vec 24)",           "reserved", false, "Intel reserved; treat like vec 15" },
    { 25, "Reserved (vec 25)",           "reserved", false, "Intel reserved; treat like vec 15" },
    { 26, "Reserved (vec 26)",           "reserved", false, "Intel reserved; treat like vec 15" },
    { 27, "Reserved (vec 27)",           "reserved", false, "Intel reserved; treat like vec 15" },
    { 28, "#HV Hypervisor Injection",    "fault",    false, "hypervisor exception; check VMM event log" },
    { 29, "#VC VMM Communication",       "fault",    true,  "SEV-SNP VC; GHCB protocol violation" },
    { 30, "#SX Security Exception",      "fault",    true,  "CET/SEP integrity; check IA32_U_CET state" },
    { 31, "Reserved (vec 31)",           "reserved", false, "Intel reserved; treat like vec 15" },
};

const char *kpanic_exception_name(uint32_t int_no) {    if (int_no < 32) return k_exc_table[int_no].name;
    if (int_no >= 32 && int_no < 48) return "IRQ (PIC remapped 0x20-0x2F)";
    if (int_no == 128) return "SYSCALL (INT 0x80)";
    return "Unknown vector";
}

const char *kpanic_exception_class(uint32_t int_no) {
    if (int_no < 32) return k_exc_table[int_no].cls;
    if (int_no >= 32 && int_no < 48) return "interrupt";
    return "unknown";
}

bool kpanic_exception_has_error(uint32_t int_no) {
    if (int_no < 32) return k_exc_table[int_no].has_err;
    return false;
}

const char *kpanic_exception_hint(uint32_t int_no) {
    if (int_no < 32) return k_exc_table[int_no].hint;
    if (int_no >= 32 && int_no < 48) return "IRQ fired inside cli region or EOI missing; check PIC mask";
    return "no hint; capture full regs + backtrace";
}

/* Page-fault error decoder. Bits per Intel SDM: 0=P 1=W/R 2=U/S      */
/* 3=RSVD 4=I/D 5=PK 15=SGX.                                          */

void kpanic_decode_pf_error(uint32_t err, char *out, uint32_t out_len) {
    uint32_t p = 0;
    const char *w[8];
    if (!out || !out_len) return;
    out[0] = '\0';
    /* I build the token list first so the join loop stays trivial. */
    w[0] = (err & 0x01u) ? "protection-violation" : "non-present-page";
    w[1] = (err & 0x02u) ? "write" : "read";
    w[2] = (err & 0x04u) ? "user" : "supervisor";
    w[3] = (err & 0x08u) ? "rsvd-bit" : NULL;
    w[4] = (err & 0x10u) ? "instr-fetch" : NULL;
    w[5] = (err & 0x20u) ? "protection-key" : NULL;
    w[6] = (err & 0x8000u) ? "sgx" : NULL;
    w[7] = NULL;
    for (uint32_t i = 0; i < 7 && w[i]; i++) {
        const char *t = w[i];
        if (p > 0 && p + 1 < out_len) out[p++] = ',';
        if (p > 0 && p + 1 < out_len && i > 0) out[p++] = ' ';
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
    /* Append the raw bits so the line stays greppable. */
    {
        char hx[16];
        kpanic_u32_hex(err, hx);
        uint32_t n = kpanic_strlen(out);
        const char *sep = " [";
        while (*sep && n + 1 < out_len) out[n++] = *sep++;
        const char *h = hx;
        while (*h && n + 1 < out_len) out[n++] = *h++;
        if (n + 1 < out_len) out[n++] = ']';
        out[n] = '\0';
    }
}

/* #GP error decoder. When EXT=1 the index refers to IDT/LDT/GDT.      */

void kpanic_decode_gpf_error(uint32_t err, char *out, uint32_t out_len) {
    uint32_t p = 0;
    char nb[16];
    if (!out || !out_len) return;
    if (err == 0) {
        kpanic_strncpy(out, "no segment (NULL selector or non-segment cause)", out_len);
        return;
    }
    out[0] = '\0';
    if (err & 0x01u) {
        const char *t = "external";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    } else {
        const char *t = "internal";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = " sel=0x";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    kpanic_u32_dec((err >> 3) & 0x1FFFu, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = " tbl=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        uint32_t ti = (err >> 1) & 0x03u;
        const char *t = (ti == 0) ? "GDT" : (ti == 1) ? "IDT" : (ti == 2) ? "LDT" : "IDT?";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = (err & 0x01u) ? " (from HW)" : "";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

/* EFLAGS decoder. I print the live flags that matter for fault        */
/* triage: interrupt state, privilege, direction, Resume, VM.          */

typedef struct kpanic_flag {
    uint32_t bit;
    char     on;
    char     off;
    const char *label;
} kpanic_flag_t;

static const kpanic_flag_t k_flags[] = {
    { 0,  'C', 'c', "carry" },
    { 2,  'P', 'p', "parity" },
    { 4,  'A', 'a', "adjust" },
    { 6,  'Z', 'z', "zero" },
    { 7,  'S', 's', "sign" },
    { 8,  'T', 't', "trap" },
    { 9,  'I', 'i', "irq-enable" },
    { 10, 'D', 'd', "direction" },
    { 11, 'O', 'o', "overflow" },
    { 12, 'P', 'p', "iopl0" },
    { 13, 'P', 'p', "iopl1" },
    { 14, 'N', 'n', "nested" },
    { 16, 'R', 'r', "resume" },
    { 17, 'V', 'v', "vm86" },
};

void kpanic_decode_eflags(uint32_t eflags, char *out, uint32_t out_len) {
    uint32_t p = 0;
    if (!out || !out_len) return;
    for (uint32_t i = 0; i < sizeof(k_flags) / sizeof(k_flags[0]); i++) {
        bool set = (eflags & (1u << k_flags[i].bit)) != 0;
        char c = set ? k_flags[i].on : k_flags[i].off;
        if (p + 2 < out_len) {
            if (p > 0) out[p++] = ' ';
            out[p++] = c;
        }
    }
    /* IOPL as a number since two bits encode it. */
    {
        uint32_t iopl = (eflags >> 12) & 0x03u;
        const char *t = " iopl=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        if (p + 1 < out_len) out[p++] = (char)('0' + iopl);
    }
    /* CPL lives in CS.RPL on this kernel (no separate SS read here). */
    out[p] = '\0';
}

/* One-line CPU summary for the serial journal header.                 */

void kpanic_cpu_summary(const kpanic_regs_t *regs, const kpanic_cpu_state_t *cpu,
                        char *out, uint32_t out_len) {
    uint32_t p = 0;
    char nb[16];
    const char *t;
    if (!out || !out_len) return;
    out[0] = '\0';
    t = "EIP=";
    while (*t && p + 1 < out_len) out[p++] = *t++;
    if (regs) {
        kpanic_u32_hex(regs->eip, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    } else {
        t = "n/a";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    t = " CR2=";
    while (*t && p + 1 < out_len) out[p++] = *t++;
    if (cpu) {
        kpanic_u32_hex(cpu->cr2, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    t = " vec=";
    while (*t && p + 1 < out_len) out[p++] = *t++;
    if (regs) {
        kpanic_u32_dec(regs->int_no, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

/* CR0 decoder. Bits per Intel SDM vol 3, section 2.5. The panic shows */
/* which protection/paging/WP/AM flags were live at the fault.        */

typedef struct kpanic_crbit {
    uint32_t bit;
    const char *name;
    const char *meaning;
} kpanic_crbit_t;

static const kpanic_crbit_t k_cr0_bits[] = {
    { 0,  "PE", "protection-enable: 0=real mode, 1=protected" },
    { 1,  "MP", "monitor-coprocessor: x87 wait behavior" },
    { 2,  "EM", "emulation: 1=trap all x87 (no FPU present)" },
    { 3,  "TS", "task-switched: 1=#NM on next FPU use until clts" },
    { 4,  "ET", "extension-type: 387 vs 287 (reserved on 486+)" },
    { 5,  "NE", "numeric-error: 1=#MF via INT16, 0=via IRQ13" },
    { 16, "WP", "write-protect: 1=supervisor writes to RO pages fault" },
    { 18, "AM", "alignment-mask: 1=#AC on misaligned CPL3 access" },
    { 29, "NW", "not-write-through: cache policy for WT memory" },
    { 30, "CD", "cache-disable: 1=all caching off (debug/slow)" },
    { 31, "PG", "paging-enable: 0=physical addressing only" },
};

void kpanic_decode_cr0(uint32_t cr0, char *out, uint32_t out_len) {
    uint32_t p = 0;
    if (!out || !out_len) return;
    out[0] = '\0';
    for (uint32_t i = 0; i < sizeof(k_cr0_bits) / sizeof(k_cr0_bits[0]); i++) {
        bool set = (cr0 & (1u << k_cr0_bits[i].bit)) != 0;
        const char *t = k_cr0_bits[i].name;
        if (p > 0 && p + 1 < out_len) out[p++] = ' ';
        if (!set && p + 1 < out_len) out[p++] = '~';
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

const char *kpanic_cr0_bit_meaning(uint32_t bit) {
    for (uint32_t i = 0; i < sizeof(k_cr0_bits) / sizeof(k_cr0_bits[0]); i++)
        if (k_cr0_bits[i].bit == bit) return k_cr0_bits[i].meaning;
    return "reserved";
}

/* CPUID feature names. CPUID.1:EDX then ECX, bit order per Intel SDM  */
/* vol 2A. I print only bits that change triage (FPU/TSC/MSR/PAE,     */
/* hypervisor-present, RDRAND, SMEP/SMAP when visible).               */

static const char *k_edx_names[32] = {
    "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce",
    "cx8", "apic", "res10", "sep", "mtrr", "pge", "mca", "cmov",
    "pat", "pse36", "psn", "clfsh", "res20", "ds", "acpi", "mmx",
    "fxsr", "sse", "sse2", "ss", "htt", "tm", "res30", "pbe",
};

static const char *k_ecx_names[32] = {
    "sse3", "pclmul", "dtes64", "monitor", "ds-cpl", "vmx", "smx", "est",
    "tm2", "ssse3", "cnxt-id", "sdbg", "fma", "cx16", "xtpr", "pdcm",
    "res16", "pcid", "dca", "sse41", "sse42", "x2apic", "movbe", "popcnt",
    "tsc-dl", "aes", "xsave", "osxsave", "avx", "f16c", "rdrand", "hypervisor",
};

void kpanic_decode_features(uint32_t ecx, uint32_t edx, char *out, uint32_t out_len) {
    uint32_t p = 0;
    bool first = true;
    if (!out || !out_len) return;
    out[0] = '\0';
    /* EDX headline bits first: the ones a kernel dev checks at a glance. */
    {
        static const uint8_t headline[] = { 0, 4, 5, 6, 9, 23, 24, 25, 26, 28 };
        for (uint32_t i = 0; i < sizeof(headline); i++) {
            uint8_t b = headline[i];
            const char *t = k_edx_names[b];
            if (!(edx & (1u << b))) continue;
            if (!first && p + 1 < out_len) out[p++] = ' ';
            if (t[0] == 'r' && t[1] == 'e' && t[2] == 's') continue;
            while (*t && p + 1 < out_len) out[p++] = *t++;
            first = false;
        }
    }
    /* ECX bits that matter for panic triage. */
    {
        static const uint8_t ecx_bits[] = { 0, 9, 12, 19, 20, 25, 26, 28, 30, 31 };
        for (uint32_t i = 0; i < sizeof(ecx_bits); i++) {
            uint8_t b = ecx_bits[i];
            const char *t = k_ecx_names[b];
            if (!(ecx & (1u << b))) continue;
            if (!first && p + 1 < out_len) out[p++] = ' ';
            while (*t && p + 1 < out_len) out[p++] = *t++;
            first = false;
        }
    }
    if (first) {
        const char *t = "(no headline features)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

/* Per-vector long reference. Three lines per vector: what happened,  */
/* what the error code / CR2 mean for THIS vector, and the Aurion     */
/* check order. This is the "crazy ai" part that saves a reboot cycle */
/* per bug: the answer is on the screen, not in the manual.           */

typedef struct kpanic_vec_detail {
    uint32_t vec;
    const char *cause;
    const char *code_meaning;
    const char *check;
} kpanic_vec_detail_t;

static const kpanic_vec_detail_t k_vec_detail[32] = {
    { 0, "DIV/IDIV with divisor 0, or quotient too large for the result register.",
         "No error code. EIP points at the DIV/IDIV instruction.",
         "1) divisor reg/mem 2) dividend bounds 3) compiler emit for / and %" },
    { 1, "Debug trap: single-step (TF), data/code breakpoint (DR0-3), or ICE.",
         "No error code. DR6 tells which condition fired.",
         "1) DR6 bits 2) stray int1 3) debugger left TF set" },
    { 2, "NMI from RAM parity, IOCHK, or watchdog.",
         "No error code. Port 0x61 bit 6/7 latch the source.",
         "1) inb(0x61) 2) RAM seating 3) watchdog timeout value" },
    { 3, "INT3 breakpoint executed outside a debugger.",
         "No error code. EIP-1 is the 0xCC byte.",
         "1) leftover breakpoint 2) selftest table 3) code patch race" },
    { 4, "INTO fired with OF=1 (signed overflow).",
         "No error code. EFLAGS.OF was set at the INTO.",
         "1) signed arithmetic range 2) replace INTO with jo path" },
    { 5, "BOUND range check failed: index outside segment bounds.",
         "No error code. The checked index is in the operand reg.",
         "1) index value 2) bounds table 3) 64-bit index truncation" },
    { 6, "#UD: undefined opcode, bad ModRM, or SSE op with SSE disabled.",
         "No error code. EIP points at the bad byte; code dump shows it.",
         "1) -mno-sse build using xmm 2) jumped into data 3) truncated insn" },
    { 7, "#NM: FPU/MMX/SSE used while CR0.TS=1 (after task switch).",
         "No error code. CR0.TS is set; clts clears it.",
         "1) context switch clts/fxsave pair 2) lazy-FPU state machine" },
    { 8, "#DF: exception while delivering a prior exception (no IST).",
         "Error code is 0. Double fault uses IST1; without it we triple-fault.",
         "1) IDT IST1 for vec 8 2) handler stack overflow 3) GDT TSS RSP0" },
    { 9, "Coprocessor segment overrun (386 legacy; reserved on 486+).",
         "No error code. Should never fire on this kernel.",
         "1) CPU erratum 2) spurious vector routing" },
    { 10, "Invalid TSS: bad selector, limit, or busy bit on task switch/IRET.",
         "Err = selector index. EXT/TI bits name GDT vs LDT.",
         "1) GDT 0x28 TSS descriptor 2) tr load 3) busy bit on re-entry" },
    { 11, "Segment not present: P=0 in GDT/LDT entry that was loaded.",
         "Err = selector index of the missing segment.",
         "1) GDT present bits 2) LDT teardown race 3) far jmp target" },
    { 12, "Stack fault: SS limit violated or canonical-address failure.",
         "Err decodes like #GP. ESP at fault is in the regs dump.",
         "1) kernel stack size 2) RSP0 in TSS 3) guard page mapping" },
    { 13, "General protection: privilege, segment, or alignment violation.",
         "Err = selector or 0. Decode EXT/TI/INDEX on the panic screen.",
         "1) CPL vs DPL 2) NULL segment load 3) MSR/cr access at CPL3" },
    { 14, "Page fault: address in CR2 did not translate with given rights.",
         "Err = P/W/U/RSVD/I-D/PK. CR2 holds the fault address.",
         "1) CR2 vs heap_end 2) present vs protection 3) use-after-kfree" },
    { 15, "Reserved vector: Intel guarantees it never fires.",
         "No architected meaning; a fire means misrouted IRQ/APIC.",
         "1) PIC remap 0x20/0x28 2) APIC spurious 3) IDT init loop" },
    { 16, "x87 math fault: unmasked IE/DE/ZE/OE/UE/PE in the FPU.",
         "No error code. FNSTSW/FNSTCW name the masked-off condition.",
         "1) divide by zero 2) denormal handling 3) mask word init" },
    { 17, "Alignment check: misaligned access with CR0.AM=1 at CPL3.",
         "Err code is 0. Fault address is the instruction, not data.",
         "1) packed struct + -mpreferred-stack-boundary 2) SIMD alignment" },
    { 18, "Machine check: hardware detected an uncorrectable error.",
         "No error code. MCA banks (IA32_MCi_STATUS) name the FRU.",
         "1) RAM MCA bank 2) thermals 3) bus parity; Corner = hw, not sw" },
    { 19, "SIMD FP exception: unmasked SSE numeric error (MXCSR).",
         "No error code. MXCSR exception + mask bits name it.",
         "1) sse divide-by-zero mask 2) denormals-are-zero flag" },
    { 20, "Virtualization exception: EPT violation with #VE enabled.",
         "No error code. Only fires under a hypervisor using #VE.",
         "1) hypervisor EPT map 2) VE info block" },
    { 21, "Control-protection: CET shadow-stack or indirect-branch fault.",
         "Err = CET cause. SSP at fault is the audit trail.",
         "1) SSP mismatch 2) missing ENDBRANCH 3) PL3_SSP init" },
    { 22, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
    { 23, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
    { 24, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
    { 25, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
    { 26, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
    { 27, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
    { 28, "Hypervisor injection exception (AMD SVM).",
         "No error code; event injection state in VMCB.",
         "1) VMM event log 2) nested paging" },
    { 29, "VMM communication exception (AMD SEV-SNP VC).",
         "Err = GHCB exit code.", "1) GHCB protocol 2) SNP attestation" },
    { 30, "Security exception (CET/SEP integrity).",
         "Err = defender cause.", "1) IA32_U_CET 2) key locker state" },
    { 31, "Reserved vector (same handling as vec 15).",
         "No architected meaning.", "1) PIC/APIC routing" },
};

void kpanic_vector_detail(uint32_t vec, char *cause_out, uint32_t cause_len,
                          char *code_out, uint32_t code_len,
                          char *check_out, uint32_t check_len) {
    if (cause_out && cause_len) {
        if (vec < 32) kpanic_strncpy(cause_out, k_vec_detail[vec].cause, cause_len);
        else if (vec < 48) kpanic_strncpy(cause_out, "Hardware IRQ while panicked or inside cli.", cause_len);
        else kpanic_strncpy(cause_out, "Unknown vector.", cause_len);
    }
    if (code_out && code_len) {
        if (vec < 32) kpanic_strncpy(code_out, k_vec_detail[vec].code_meaning, code_len);
        else kpanic_strncpy(code_out, "IRQ number = vec-32.", code_len);
    }
    if (check_out && check_len) {
        if (vec < 32) kpanic_strncpy(check_out, k_vec_detail[vec].check, check_len);
        else kpanic_strncpy(check_out, "Check PIC mask + EOI.", check_len);
    }
}

/* IRQ share table: which PIC line maps to which vector after remap.  */
/* Remap is 0x20/0x28 in handlers.c pic_remap().                      */

const char *kpanic_irq_name(uint32_t vec) {
    switch (vec) {
    case 32: return "IRQ0 timer (100Hz)";
    case 33: return "IRQ1 keyboard";
    case 34: return "IRQ2 cascade";
    case 35: return "IRQ3 COM2";
    case 36: return "IRQ4 COM1";
    case 37: return "IRQ5 LPT2/sound";
    case 38: return "IRQ6 floppy";
    case 39: return "IRQ7 LPT1";
    case 40: return "IRQ8 RTC";
    case 41: return "IRQ9 ACPI/virtio";
    case 42: return "IRQ10 free/virtio-net";
    case 43: return "IRQ11 free/VMware-SVGA";
    case 44: return "IRQ12 PS/2 mouse";
    case 45: return "IRQ13 FPU";
    case 46: return "IRQ14 primary ATA";
    case 47: return "IRQ15 secondary ATA";
    default: return "not-an-IRQ";
    }
}

/* MSR + APIC probes. rdmsr is CPL0-only; the panic runs at CPL0 so it */
/* is safe. I gate on CPUID first: no CPUID/MSR bit, no read. APIC    */
/* base comes from IA32_APIC_BASE (0x1B); microcode from 0x8B.        */

static uint64_t cpu_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t kpanic_rdmsr_safe(uint32_t msr, bool *ok,
                           const kpanic_cpu_state_t *cpu) {
    bool has_msr = false;
    if (ok) *ok = false;
    if (cpu && cpu->has_cpuid) has_msr = (cpu->cpu_feat_edx & (1u << 5)) != 0;
    else has_msr = true; /* pre-CPUID 586 always has MSRs */
    if (!has_msr) return 0;
    /* Only the two MSRs I name below are ever read: allowlist, not an */
    /* arbitrary reader gadget.                                        */
    if (msr != 0x1Bu && msr != 0x8Bu) return 0;
    {
        uint64_t v = cpu_rdmsr(msr);
        if (ok) *ok = true;
        return v;
    }
}

void kpanic_format_apic(uint64_t apic_base, char *out, uint32_t out_len) {
    uint32_t p = 0;
    char hx[24];
    if (!out || !out_len) return;
    out[0] = '\0';
    {
        const char *t = "apic-base=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    kpanic_u64_hex(apic_base & 0xFFFFF000ull, hx);
    {
        const char *t = hx;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = (apic_base & (1ull << 11)) ? " enabled" : " disabled";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = (apic_base & (1ull << 10)) ? " x2apic" : " xapic";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = " bsp=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p++] = (apic_base & (1ull << 8)) ? '1' : '0';
    out[p] = '\0';
}

/* Privilege note: CPL = CS.RPL on this kernel (flat segments, no     */
/* call gates in the panic path). 0 = the fault happened in kernel    */
/* code, 3 = it came from userspace through the syscall/IRQ gate.     */

uint32_t kpanic_cpl_of_cs(uint32_t cs) {
    return cs & 0x03u;
}

void kpanic_format_priv(uint32_t cs, uint32_t eflags, char *out, uint32_t out_len) {
    uint32_t p = 0;
    uint32_t cpl = kpanic_cpl_of_cs(cs);
    uint32_t iopl = (eflags >> 12) & 0x03u;
    char nb[16];
    if (!out || !out_len) return;
    {
        const char *t = "cpl=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    kpanic_u32_dec(cpl, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = (cpl == 0) ? " (kernel)" : (cpl == 3) ? " (user)" : " (weird: cpl 1/2 unused here)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = " iopl=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    kpanic_u32_dec(iopl, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    if (cpl > iopl) {
        const char *t = " (cpl>iopl: cli/sti faulted as expected)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}
