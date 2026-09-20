/* smp.c - BSP detection (CPUID + ACPI MADT), LAPIC IPI boot, band jobs.
 *
 * I keep PIC/PIT routing untouched: all IRQs stay on the BSP. APs run
 * with interrupts disabled and poll a generation-counted band queue,
 * so there are no IPIs after boot and no locks in the render path.
 * Every table read is bounds-checked; any failure degrades to 1 CPU.
 */

 #include "smp.h"

extern void serial_puts(const char *s);
extern uint32_t get_ticks(void);
extern void kpanic_post(uint8_t code);
extern uint8_t smp_tramp_image[];
extern uint8_t smp_tramp_end[];
extern void *smp_gdt;
extern void smp_install_gdt_bsp(void);
extern void smp_ap_lidt(void);
extern void smp_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi);
extern void smp_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi);

#define IA32_APIC_BASE 0x1Bu
#define LAPIC_SPURIOUS 0xF0u
#define LAPIC_ID_REG 0x20u
#define LAPIC_TPR 0x80u
#define LAPIC_ICR_LO 0x300u
#define LAPIC_ICR_HI 0x310u
#define LAPIC_ICR_IDLE 0x1000u

static uint32_t lapic_base = 0xFEE00000u;
static uint32_t cpu_lapic_id[SMP_MAX_CPUS];
static volatile uint32_t ap_ready[SMP_MAX_CPUS];
static int online_count = 1;
static bool available = false;
static bool has_msr = false;

static uint8_t ap_stack[SMP_MAX_CPUS][16384] __attribute__((aligned(16)));

static smp_band_fn_t job_fn = 0;
static void *job_ctx = 0;
static int job_nb = 0;
static volatile uint32_t job_seq = 0;
static volatile uint32_t job_next = 0;
static volatile uint32_t job_done = 0;
static uint32_t ap_seen[SMP_MAX_CPUS];
static bool serial_fallback = false;
static bool watchdog_on = false;

static inline uint8_t p_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void delay_us(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p_inb(0x80);
}

static void log_msg(const char *s) { serial_puts(s); }

static void log_hex(uint32_t v) {
    static const char d[16] = "0123456789ABCDEF";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = d[(v >> (28 - i * 4)) & 0xF];
    b[10] = 0;
    serial_puts(b);
}

static void log_dec(int v) {
    char b[12];
    int i = 0;
    bool neg = false;
    if (v < 0) { neg = true; v = -v; }
    if (v == 0) b[i++] = '0';
    char t[12];
    int n = 0;
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg) b[i++] = '-';
    while (n > 0) b[i++] = t[--n];
    b[i] = 0;
    serial_puts(b);
}

static bool cpu_has_cpuid(void) {
    uint32_t a, b;
    __asm__ volatile("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; "
                     "pushl %0; popfl; pushfl; popl %0"
                     : "=r"(a), "=r"(b) :: "cc");
    return ((a ^ b) & 0x200000u) != 0;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}

static bool mem_eq8(const void *p, const char *s) {
    const uint8_t *b = (const uint8_t *)p;
    for (int i = 0; i < 8; i++) if (b[i] != (uint8_t)s[i]) return false;
    return true;
}

static bool mem_eq4(const void *p, const char *s) {
    const uint8_t *b = (const uint8_t *)p;
    for (int i = 0; i < 4; i++) if (b[i] != (uint8_t)s[i]) return false;
    return true;
}

static uint8_t table_sum(const uint8_t *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    return s;
}

static uint32_t rd32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void wr32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

static int madt_collect(const uint8_t *madt, uint32_t mlen,
                        uint32_t *ids, uint32_t *lapic_addr) {
    if (mlen < 44 || mlen > 4096) return 0;
    *lapic_addr = rd32(madt + 36);
    uint32_t off = 44;
    int n = 0;
    while (off + 2 <= mlen && n < SMP_MAX_CPUS) {
        uint8_t type = madt[off];
        uint8_t len = madt[off + 1];
        if (len < 2 || off + len > mlen) break;
        if (type == 0 && len >= 8) {
            uint32_t flags = rd32(madt + off + 4);
            if (flags & 0x3u) {
                ids[n++] = madt[off + 3];
            }
        } else if (type == 9 && len >= 16) {
            uint32_t flags = rd32(madt + off + 8);
            if (flags & 0x3u) {
                ids[n++] = rd32(madt + off + 4) & 0xFFu;
            }
        }
        off += len;
    }
    return n;
}

static int acpi_find_cpus(uint32_t *ids, uint32_t *lapic_addr) {
    uint32_t ebda = 0;
    volatile const uint8_t *bda = (volatile const uint8_t *)0x400u;
    uint16_t ebda_seg = (uint16_t)((uint16_t)bda[0x0E] | ((uint16_t)bda[0x0F] << 8));
    ebda = (uint32_t)ebda_seg << 4;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t base, limit;
        if (pass == 0) {
            if (ebda < 0x40000u || ebda > 0xA0000u) continue;
            base = ebda;
            limit = ebda + 1024;
        } else {
            base = 0xE0000u;
            limit = 0x100000u;
        }
        for (uint32_t p = base; p + 20 <= limit; p += 16) {
            const uint8_t *q = (const uint8_t *)p;
            if (!mem_eq8(q, "RSD PTR ")) continue;
            if (table_sum(q, 20) != 0) continue;
            uint32_t rsdt = rd32(q + 16);
            if (rsdt < 0x1000u || rsdt > 0xFFF00000u) return 0;
            const uint8_t *r = (const uint8_t *)rsdt;
            if (!mem_eq4(r, "RSDT")) continue;
            uint32_t rlen = rd32(r + 4);
            if (rlen < 36 || rlen > 4096) continue;
            if (table_sum(r, rlen) != 0) continue;
            for (uint32_t e = 36; e + 4 <= rlen; e += 4) {
                uint32_t t = rd32(r + e);
                if (t < 0x1000u || t > 0xFFF00000u) continue;
                const uint8_t *m = (const uint8_t *)t;
                if (!mem_eq4(m, "APIC")) continue;
                uint32_t mlen = rd32(m + 4);
                if (mlen < 44 || mlen > 4096) continue;
                if (table_sum(m, mlen) != 0) continue;
                int n = madt_collect(m, mlen, ids, lapic_addr);
                if (n > 0) return n;
                return 0;
            }
            return 0;
        }
    }
    return 0;
}

static uint32_t lapic_read(uint32_t off) {
    return *(volatile uint32_t *)(lapic_base + off);
}

static void lapic_write(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(lapic_base + off) = v;
}

static void lapic_enable(void) {
    if (has_msr) {
        uint32_t lo = 0, hi = 0;
        smp_rdmsr(IA32_APIC_BASE, &lo, &hi);
        lapic_base = lo & 0xFFFFF000u;
        if (lapic_base == 0) lapic_base = 0xFEE00000u;
        smp_wrmsr(IA32_APIC_BASE, lo | 0x800u, hi);
    }
    lapic_write(LAPIC_TPR, 0);
    uint32_t sp = lapic_read(LAPIC_SPURIOUS);
    lapic_write(LAPIC_SPURIOUS, (sp & ~0xFFu) | 0xFFu | 0x100u);
}

static bool icr_idle(void) {
    for (int i = 0; i < 100000; i++) {
        if ((lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_IDLE) == 0) return true;
        smp_cpu_pause();
    }
    return false;
}

static void send_ipi(uint32_t dest, uint32_t lo) {
    icr_idle();
    lapic_write(LAPIC_ICR_HI, dest << 24);
    lapic_write(LAPIC_ICR_LO, lo);
}

static uint32_t this_lapic_id(void) {
    return (lapic_read(LAPIC_ID_REG) >> 24) & 0xFFu;
}

static void drain_bands(void) {
    for (;;) {
        uint32_t m = smp_atomic_xadd(&job_next, 1);
        if (m >= (uint32_t)job_nb) break;
        job_fn((int)m, job_nb, job_ctx);
    }
}

static void worker_poll(int idx) {
    uint32_t s = job_seq;
    if (s == ap_seen[idx]) return;
    if (s & 1u) {
        drain_bands();
        smp_atomic_xadd(&job_done, 1);
    }
    ap_seen[idx] = s;
}

static volatile uint32_t selftest_cell[64];

static void selftest_band(int band, int nbands, void *ctx) {
    (void)nbands;
    (void)ctx;
    for (int k = 0; k < 1000; k++) selftest_cell[band]++;
}

static bool smp_selftest(void) {
    for (int i = 0; i < 64; i++) selftest_cell[i] = 0;
    __asm__ volatile("" ::: "memory");
    smp_parallel_bands(selftest_band, 0, 64);
    for (int i = 0; i < 64; i++) {
        if (selftest_cell[i] != 1000) return false;
    }
    return true;
}

void smp_ap_entry(void) {
    smp_ap_lidt();
    lapic_enable();
    uint32_t id = this_lapic_id();
    int idx = -1;
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        if (cpu_lapic_id[i] == id) { idx = i; break; }
    }
    if (idx <= 0) {
        for (int i = 1; i < SMP_MAX_CPUS; i++) {
            if (ap_ready[i] == 0) { idx = i; break; }
        }
        if (idx < 0) idx = 1;
        cpu_lapic_id[idx] = id;
    }
    __asm__ volatile("" ::: "memory");
    ap_ready[idx] = 1;
    __asm__ volatile("" ::: "memory");
    for (;;) {
        worker_poll(idx);
        smp_cpu_pause();
    }
}

static void boot_one_ap(uint32_t dest_id, uint32_t stack_top) {
    wr32((void *)SMP_MBOX_STACK, stack_top);
    __asm__ volatile("" ::: "memory");
    ap_ready[online_count] = 0;
    send_ipi(dest_id, 0x4500u);
    delay_us(10000);
    send_ipi(dest_id, 0x4600u | 0x08u);
    delay_us(1000);
    send_ipi(dest_id, 0x4600u | 0x08u);
    delay_us(1000);
    for (uint32_t t = 0; t < 300000; t += 1000) {
        if (ap_ready[online_count]) return;
        delay_us(1000);
    }
}

void smp_init(void) {
    log_msg("SMP: init\n");
    smp_install_gdt_bsp();
    /* Caches on everywhere: some firmware leaves CD/NW set, and APs
     * inherit the same requirement via the trampoline. */
    __asm__ volatile("mov %%cr0, %%eax; andl $0x9FFFFFFF, %%eax; "
                     "orl $0x2, %%eax; mov %%eax, %%cr0; wbinvd; fninit"
                     ::: "eax", "memory");
    if (!cpu_has_cpuid()) {
        log_msg("SMP: no CPUID, 1 CPU\n");
        available = true;
        return;
    }
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    has_msr = (d & (1u << 5)) != 0;
    uint32_t ids[SMP_MAX_CPUS];
    uint32_t addr = 0xFEE00000u;
    int n = acpi_find_cpus(ids, &addr);
    if (n <= 0) {
        log_msg("SMP: no MADT, 1 CPU\n");
        available = true;
        return;
    }
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;
    if (addr) lapic_base = addr & 0xFFFFF000u;
    lapic_enable();
    uint32_t bsp = this_lapic_id();
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        cpu_lapic_id[i] = 0xFFFFFFFFu;
        ap_ready[i] = 0;
    }
    cpu_lapic_id[0] = bsp;
    ap_ready[0] = 1;
    online_count = 1;
    for (int i = 0; i < n && online_count < SMP_MAX_CPUS; i++) {
        if (ids[i] == bsp) continue;
        bool dup = false;
        for (int k = 0; k < online_count; k++) {
            if (cpu_lapic_id[k] == ids[i]) { dup = true; break; }
        }
        if (!dup) cpu_lapic_id[online_count++] = ids[i];
    }
    int want = online_count;
    log_msg("SMP: MADT reports ");
    log_dec(want);
    log_msg(" CPU(s), BSP id ");
    log_hex(bsp);
    log_msg("\n");
    if (want <= 1) {
        online_count = 1;
        available = true;
        log_msg("SMP: 1 CPU online\n");
        return;
    }
    *(volatile uint16_t *)SMP_MBOX_GDTR = 23;
    wr32((void *)(SMP_MBOX_GDTR + 2), (uint32_t)&smp_gdt);
    wr32((void *)SMP_MBOX_ENTRY, (uint32_t)smp_ap_entry);
    uint32_t tsize = (uint32_t)(smp_tramp_end - smp_tramp_image);
    if (tsize > SMP_TRAMP_SIZE) tsize = SMP_TRAMP_SIZE;
    for (uint32_t i = 0; i < tsize; i++) {
        ((volatile uint8_t *)SMP_TRAMP_ADDR)[i] = smp_tramp_image[i];
    }
    int have = 1;
    kpanic_post(0x20);
    for (int i = 1; i < want; i++) {
        uint32_t top = ((uint32_t)&ap_stack[i][sizeof(ap_stack[i])]) & ~15u;
        online_count = have;
        boot_one_ap(cpu_lapic_id[i], top);
        if (ap_ready[have]) {
            have++;
            online_count = have;
            log_msg("SMP: AP id ");
            log_hex(cpu_lapic_id[i]);
            log_msg(" online\n");
        } else {
            log_msg("SMP: AP id ");
            log_hex(cpu_lapic_id[i]);
            log_msg(" no response\n");
        }
    }
    online_count = have;
    available = true;
    kpanic_post(0x21);
    log_msg("SMP: ");
    log_dec(online_count);
    log_msg(" CPU(s) online\n");
    if (online_count > 1) {
        if (smp_selftest()) {
            log_msg("SMP: band selftest PASS\n");
        } else {
            log_msg("SMP: band selftest FAIL, serial fallback\n");
            online_count = 1;
        }
    }
    watchdog_on = true;
}

int smp_cpu_count(void) { return online_count < 1 ? 1 : online_count; }

int smp_ap_count(void) {
    int n = smp_cpu_count() - 1;
    return n < 0 ? 0 : n;
}

int smp_cpu_id(void) {
    if (!available || online_count <= 1) return 0;
    uint32_t id = this_lapic_id();
    for (int i = 0; i < online_count; i++) {
        if (cpu_lapic_id[i] == id) return i;
    }
    return 0;
}

bool smp_available(void) { return available; }

void smp_parallel_bands(smp_band_fn_t fn, void *ctx, int nbands) {
    int n = serial_fallback ? 1 : smp_cpu_count();
    if (!fn || nbands <= 1 || n <= 1) {
        if (fn && nbands >= 1) fn(0, nbands < 1 ? 1 : nbands, ctx);
        return;
    }
    if (nbands > 256) nbands = 256;
    job_fn = fn;
    job_ctx = ctx;
    job_nb = nbands;
    job_next = 0;
    job_done = 0;
    __asm__ volatile("" ::: "memory");
    job_seq++;
    drain_bands();
    smp_atomic_xadd(&job_done, 1);
    if (watchdog_on) {
        /* A dead AP must degrade to serial, never freeze the BSP. The
         * rerun rewrites identical pixels, so the worst case is one
         * torn frame instead of a hung OS. */
        uint32_t start = get_ticks();
        while (job_done < (uint32_t)n) {
            smp_cpu_pause();
            if ((uint32_t)(get_ticks() - start) > 300) {
                log_msg("SMP: band timeout, serial fallback\n");
                serial_fallback = true;
                for (int b = 0; b < nbands; b++) fn(b, nbands, ctx);
                break;
            }
        }
    } else {
        while (job_done < (uint32_t)n) smp_cpu_pause();
    }
    __asm__ volatile("" ::: "memory");
    job_seq++;
}
