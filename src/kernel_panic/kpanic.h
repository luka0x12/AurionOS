/*
 * AurionOS Kernel Panic Subsystem - public API.
 *
 * I replace src/core/panic.c (244 lines, static strings, no diagnostics)
 * with a Linux-style oops: live CPU state, live RAM/VRAM probes, real
 * EBP-chain backtrace, dual VGA/VESA renderer, serial journal.
 *
 * Every number on the panic screen is read at panic time. No canned
 * registers, no fake uptime, no placeholder backtrace. If a probe cannot
 * run (heap corrupt, GPU dead), the field prints "n/a" instead of lying.
 *
 * Panic path contract: no kmalloc, no lock, no virtual dispatch, no
 * unbounded loop. All buffers are static. All math is checked.
*/

#ifndef AURION_KPANIC_H
#define AURION_KPANIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version + branding */

#define KPANIC_VERSION_MAJOR 2u
#define KPANIC_VERSION_MINOR 0u
#define KPANIC_BANNER "AURION KERNEL PANIC"
#define KPANIC_OOPS_TAG "Oops"

/* Fault domains. Each maps to one kpanic_subsys.c entry point that    */
/* gathers live state for that subsystem, not a canned string.         */

typedef enum kpanic_domain {
    KP_DOMAIN_CPU      = 0,  /* x86 exception, regs come from ISR frame */
    KP_DOMAIN_OOM      = 1,  /* kmalloc failed, stats from mem_get_stats */
    KP_DOMAIN_VRAM     = 2,  /* framebuffer alloc/mode failed, VBE probe */
    KP_DOMAIN_GUI      = 3,  /* window manager / compositor fault */
    KP_DOMAIN_VFS      = 4,  /* FAT32/ISO9660/ATA fault */
    KP_DOMAIN_NET      = 5,  /* TCP/IP, DHCP, TLS, driver fault */
    KP_DOMAIN_SCHED    = 6,  /* scheduler, stack, TSS fault */
    KP_DOMAIN_DRIVER   = 7,  /* PCI/ATA/NE2000/RTL8139/VMware fault */
    KP_DOMAIN_HEAP     = 8,  /* mem_validate_heap reported corruption */
    KP_DOMAIN_ASSERT   = 9,  /* KP_ASSERT / explicit kernel_panic call */
    KP_DOMAIN_UNKNOWN  = 10, /* unclassified, still dumps everything */
    KP_DOMAIN_COUNT    = 11
} kpanic_domain_t;

/* Severity: controls halt policy, not just color.                     */

typedef enum kpanic_severity {
    KP_SEV_OOPS     = 0, /* recoverable complaint, dump + halt cpu */
    KP_SEV_FATAL    = 1, /* subsystem dead, need reboot */
    KP_SEV_CORRUPT  = 2, /* memory corruption, reboot without sync */
    KP_SEV_HANG     = 3  /* watchdog: CPU alive but nothing schedules */
} kpanic_severity_t;

/* ISR register frame. Must match the layout pushed by                 */
/* src/asm/interrupt.asm stubs and read by src/core/handlers.c.        */
/* I do not redefine it there; I mirror it here so the panic           */
/* subsystem compiles without including handlers.c internals.          */

typedef struct kpanic_regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} kpanic_regs_t;

/* Live CPU snapshot. Filled by kpanic_cpu.c at panic time from        */
/* real control registers and timestamp counters.                      */

typedef struct kpanic_cpu_state {
    uint32_t cr0;
    uint32_t cr2;          /* fault linear address, read with mov %cr2 */
    uint32_t cr3;          /* page directory base, mov %cr3 */
    uint32_t eflags_live;  /* pushf at panic time, not just ISR copy */
    uint64_t tsc;          /* rdtsc at panic time */
    uint32_t ticks;        /* get_ticks() at panic time (100 Hz) */
    uint32_t cpuid_max;    /* CPUID.0:EAX when available */
    uint32_t cpu_vendor0;  /* CPUID.0:EBX */
    uint32_t cpu_vendor1;  /* CPUID.0:EDX */
    uint32_t cpu_vendor2;  /* CPUID.0:ECX */
    uint32_t cpu_feat_ecx; /* CPUID.1:ECX */
    uint32_t cpu_feat_edx; /* CPUID.1:EDX */
    bool     has_cpuid;
    bool     has_tsc;
} kpanic_cpu_state_t;

/* Live memory snapshot. Filled by kpanic_mem.c.                       */

typedef struct kpanic_mem_state {
    uint32_t heap_free;     /* mem_get_stats()[0], live heap walk */
    uint32_t heap_used;     /* mem_get_stats()[1] */
    uint32_t heap_blocks;   /* mem_get_stats()[2] */
    uint32_t heap_valid;    /* mem_validate_heap() == 0 */
    uint32_t fail_size;     /* allocation size that failed (OOM only) */
    uint32_t vram_need;     /* bytes the framebuffer wanted */
    uint32_t vram_have;     /* bytes plausibly free for VRAM */
    int32_t  gpu_w;         /* gpu_get_width() at panic time */
    int32_t  gpu_h;         /* gpu_get_height() at panic time */
    uint32_t vbe_fb;        /* VBE boot info framebuffer phys addr */
    uint16_t vbe_mode;      /* VBE mode number from 0x9000 block */
    uint8_t  vbe_bpp;       /* bits per pixel from 0x9000 block */
    bool     gpu_vesa;      /* gpu_is_vesa() at panic time */
    bool     vbe_info_ok;   /* 0x9000 block looked sane */
    bool     stats_ok;      /* mem_get_stats ran without fault */
} kpanic_mem_state_t;

/* Backtrace snapshot. Filled by kpanic_trace.c by walking the real    */
/* EBP chain with a validation gate on every frame.                    */

#define KP_MAX_FRAMES 16u
#define KP_STACK_WORDS 32u

typedef struct kpanic_frame {
    uint32_t ebp;
    uint32_t eip;
    bool     eip_readable;
} kpanic_frame_t;

typedef struct kpanic_trace_state {
    kpanic_frame_t frames[KP_MAX_FRAMES];
    uint32_t       frame_count;
    uint32_t       stack_dump[KP_STACK_WORDS];
    uint32_t       stack_base;   /* ESP at capture */
    uint32_t       stack_words;  /* how many of stack_dump[] are valid */
    uint8_t        code_bytes[32];
    uint32_t       code_base;    /* EIP the code was read from */
    uint32_t       code_len;     /* how many of code_bytes[] are valid */
    bool           ebp_valid;    /* initial EBP passed the sanity gate */
} kpanic_trace_state_t;

/* One complete panic record. Lives in .bss, never on the heap.        */

#define KP_MSG_LEN 128u
#define KP_FILE_LEN 64u
#define KP_AUX_LEN 192u

typedef struct kpanic_record {
    uint32_t          magic;       /* KPANIC_MAGIC while valid */
    uint32_t          seq;         /* oops counter, 0-based */
    kpanic_domain_t   domain;
    kpanic_severity_t severity;
    char              message[KP_MSG_LEN];
    char              file[KP_FILE_LEN];
    uint32_t          line;
    uint32_t          code;        /* error_code / int_no / errno */
    kpanic_regs_t     regs;
    bool              has_regs;
    kpanic_cpu_state_t cpu;
    kpanic_mem_state_t mem;
    kpanic_trace_state_t trace;
    char              aux[KP_AUX_LEN]; /* subsystem line, live values */
    uint32_t          uptime_ticks;
    uint8_t           checksum;    /* xor over record minus this field */
} kpanic_record_t;

#define KPANIC_MAGIC 0x50414E49u /* "PANI" */

/* Panic-time configuration. Tunables live here (not Kconfig): the    */
/* values below are the tested policy.                                */
/*   KP_AUTO_REBOOT_TSC  ~10s of TSC ticks for the halt-loop timeout  */
/*   KP_CORRUPT_GRACE    ~1s of TSC ticks before a CORRUPT reboot     */
/*   KP_WATCHDOG_TICKS   500 ticks (5s at 100Hz) of stalled runqueue  */
/* Halt-loop keys (buffered sys_getkey path + raw 8042 path):         */
/*   ENTER (0x1C) reboot   S (0x1F) serial replay   D (0x20) re-dump  */
/*   H (0x23) help overlay   P (0x19) QEMU poweroff   X (0x2D) hex journal */
/* POST codes (port 0x80) E0-EF are the panic range, see kpanic_halt. */

#define KP_AUTO_REBOOT_TSC 10000000000ull
#define KP_CORRUPT_GRACE_TSC 1000000000ull
#define KP_WATCHDOG_TICKS 500u

/* Panic-time hardware contract (ports the dumper touches directly):  */
/*   0x3F8  COM1 data / 0x3FD line-status (bit5 = TX empty)           */
/*   0x60   8042 data (scancodes, LED command payload)                */
/*   0x64   8042 status (bit0 = output ready, bit1 = input busy)      */
/*   0x80   POST diagnostic (E0-EF = panic range)                     */
/*   0xCF9  PCI reset control (Intel: 02 soft, 0E hard)               */
/*   0xB004 Bochs/QEMU APM poweroff (harmless elsewhere)              */
/*   0x0501 QEMU isa-debug-exit (harmless elsewhere)                  */
/*   0xB8000 VGA text framebuffer (always written, even in VESA mode) */
/*   0x09000 VBE boot info block (fb, w/h, bpp, mode)                 */
/*   0x09010 BIOS RAM total in KB (INT15/E801, gated 1MB..4GB)        */
/* File map (one job per file, see section 4):                        */
/*   kpanic.h      public API + tunables + port contract (this file)  */
/*   kpanic_core   entry pipeline + domain entries + selftest         */
/*   kpanic_cpu    CRx/TSC/CPUID capture + fault decoders + reference */
/*   kpanic_mem    heap/VRAM/VBE/BIOS probes + budget advisor         */
/*   kpanic_trace  EBP walk + stack/code dump + insn length + tags    */
/*   kpanic_emit   VGA/VESA/serial renderers + machine line + help    */
/*   kpanic_subsys domain names + cause/PCI/ATA/TLS/errno tables      */
/*   kpanic_log    journal ring + CRC8 + replay + verify               */
/*   kpanic_halt   POST/LED diagnostics + reboot/poweroff + key loop  */
/*   kpanic_compat legacy kernel_panic API + ISR entry + watchdog     */

/* Core entry points (kpanic_core.c).                                  */

void kpanic_cpu_fault(const kpanic_regs_t *regs, const char *file, uint32_t line);
void kpanic_oom(uint32_t want_bytes, const char *file, uint32_t line);
void kpanic_vram(uint32_t want_bytes, const char *file, uint32_t line);
void kpanic_gui(const char *what, const char *file, uint32_t line, uint32_t code);
void kpanic_vfs(const char *what, const char *file, uint32_t line, uint32_t code);
void kpanic_net(const char *what, const char *file, uint32_t line, uint32_t code);
void kpanic_sched(const char *what, const char *file, uint32_t line, uint32_t code);
void kpanic_driver(const char *what, const char *file, uint32_t line, uint32_t code);
void kpanic_heap(const char *what, const char *file, uint32_t line, uint32_t code);
void kpanic_assert(const char *expr, const char *file, uint32_t line);

/* Drop-in used by the legacy kernel_panic() path and by hand-rolled    */
/* call sites that already have a message + code.                      */
void kpanic_enter(kpanic_domain_t domain, kpanic_severity_t severity,
                  const char *message, const char *file, uint32_t line,
                  uint32_t code, const kpanic_regs_t *regs);

/* Current record for debuggers and the halt loop. Never NULL after    */
/* the first panic. Points at static storage.                          */
const kpanic_record_t *kpanic_current(void);

/* 0 before the first panic, 1 inside the first panic, 2+ on re-entry. */
uint32_t kpanic_depth(void);

/* Convenience macros. They capture __FILE__/__LINE__ at the call site */
/* so the panic record names the real fault location.                  */

#define KP_CPU_FAULT(r)        kpanic_cpu_fault((const kpanic_regs_t *)(r), __FILE__, __LINE__)
#define KP_OOM(n)              kpanic_oom((uint32_t)(n), __FILE__, __LINE__)
#define KP_VRAM(n)             kpanic_vram((uint32_t)(n), __FILE__, __LINE__)
#define KP_GUI(w, c)           kpanic_gui((w), __FILE__, __LINE__, (uint32_t)(c))
#define KP_VFS(w, c)           kpanic_vfs((w), __FILE__, __LINE__, (uint32_t)(c))
#define KP_NET(w, c)           kpanic_net((w), __FILE__, __LINE__, (uint32_t)(c))
#define KP_SCHED(w, c)         kpanic_sched((w), __FILE__, __LINE__, (uint32_t)(c))
#define KP_DRIVER(w, c)        kpanic_driver((w), __FILE__, __LINE__, (uint32_t)(c))
#define KP_HEAP(w, c)          kpanic_heap((w), __FILE__, __LINE__, (uint32_t)(c))
#define KP_ASSERT(e)           kpanic_assert((e), __FILE__, __LINE__)
#define KP_FATAL(m, c)         kpanic_enter(KP_DOMAIN_ASSERT, KP_SEV_FATAL, (m), __FILE__, __LINE__, (uint32_t)(c), NULL)

/* Legacy compatibility: old call sites keep compiling and get the new */
/* renderer. Implemented in kpanic_compat.c.                           */
void kernel_panic(const char *message, const char *file, int line, uint32_t error_code);
void panic(const char *message);
void panic_code(const char *message, uint32_t code);
#define PANIC(msg) kernel_panic(msg, __FILE__, __LINE__, 0)
#define PANIC_CODE(msg, code) kernel_panic(msg, __FILE__, __LINE__, code)

/* Sub-module API used internally. Public so unit harnesses and the    */
/* ISR hook can call one stage without running the whole pipeline.     */

/* kpanic_cpu.c */
void     kpanic_cpu_capture(kpanic_cpu_state_t *out);
const char *kpanic_exception_name(uint32_t int_no);
const char *kpanic_exception_class(uint32_t int_no);
bool     kpanic_exception_has_error(uint32_t int_no);
void     kpanic_decode_pf_error(uint32_t err, char *out, uint32_t out_len);
void     kpanic_decode_gpf_error(uint32_t err, char *out, uint32_t out_len);
void     kpanic_decode_eflags(uint32_t eflags, char *out, uint32_t out_len);
void     kpanic_format_vendor(const kpanic_cpu_state_t *cpu, char *out, uint32_t out_len);

/* kpanic_mem.c */
void     kpanic_mem_capture(kpanic_mem_state_t *out, uint32_t want_bytes);
bool     kpanic_safe_mul_u32(uint32_t a, uint32_t b, uint32_t *out);
bool     kpanic_is_readable(uint32_t addr, uint32_t len);
void     kpanic_format_bytes(uint32_t bytes, char *out, uint32_t out_len);
void     kpanic_format_vram_line(const kpanic_mem_state_t *m, char *out, uint32_t out_len);
void     kpanic_format_heap_line(const kpanic_mem_state_t *m, char *out, uint32_t out_len);

/* kpanic_trace.c */
void     kpanic_trace_capture(kpanic_trace_state_t *out, uint32_t ebp, uint32_t esp, uint32_t eip);
bool     kpanic_frame_sane(uint32_t ebp, uint32_t prev_ebp);
uint32_t kpanic_read_ebp(void);
uint32_t kpanic_read_esp(void);

/* kpanic_emit.c */
void     kpanic_emit_all(const kpanic_record_t *rec);
void     kpanic_emit_serial(const kpanic_record_t *rec);
void     kpanic_serial_puts(const char *s);
void     kpanic_u32_hex(uint32_t v, char *out);
void     kpanic_u32_dec(uint32_t v, char *out);
void     kpanic_u64_hex(uint64_t v, char *out);
uint32_t kpanic_strlen(const char *s);
void     kpanic_strncpy(char *dst, const char *src, uint32_t dst_len);
bool     kpanic_streq(const char *a, const char *b);

/* kpanic_subsys.c */
void     kpanic_subsys_line(kpanic_domain_t domain, const char *message,
                            uint32_t code, const kpanic_mem_state_t *mem,
                            const kpanic_cpu_state_t *cpu,
                            char *out, uint32_t out_len);
const char *kpanic_domain_name(kpanic_domain_t domain);
const char *kpanic_severity_name(kpanic_severity_t severity);
const char *kpanic_action_line(kpanic_severity_t severity);

/* kpanic_log.c */
void     kpanic_log_push(const kpanic_record_t *rec);
uint32_t kpanic_log_count(void);
const kpanic_record_t *kpanic_log_get(uint32_t index);
uint8_t  kpanic_checksum(const kpanic_record_t *rec);
void     kpanic_log_replay_serial(void);
void     kpanic_log_verify_all_serial(void);
void     kpanic_log_usage_serial(void);
uint32_t kpanic_log_dropped(void);
uint32_t kpanic_log_capacity(void);
void     kpanic_log_export_hex(uint32_t slot);
const kpanic_record_t *kpanic_log_last(void);

/* kpanic_halt.c */
void     kpanic_halt_loop(const kpanic_record_t *rec);
void     kpanic_halt_announced(const kpanic_record_t *rec);
void     kpanic_reboot_now(void);
void     kpanic_halt_cpu(void);
void     kpanic_poweroff_qemu(void);

/* Extended CPU reference (kpanic_cpu.c, second half). */
void     kpanic_decode_cr0(uint32_t cr0, char *out, uint32_t out_len);
const char *kpanic_cr0_bit_meaning(uint32_t bit);
void     kpanic_decode_features(uint32_t ecx, uint32_t edx, char *out, uint32_t out_len);
void     kpanic_vector_detail(uint32_t vec, char *cause_out, uint32_t cause_len,
                              char *code_out, uint32_t code_len,
                              char *check_out, uint32_t check_len);
const char *kpanic_irq_name(uint32_t vec);
const char *kpanic_exception_hint(uint32_t int_no);
void     kpanic_cpu_summary(const kpanic_regs_t *regs, const kpanic_cpu_state_t *cpu,
                            char *out, uint32_t out_len);
uint64_t kpanic_rdmsr_safe(uint32_t msr, bool *ok, const kpanic_cpu_state_t *cpu);
void     kpanic_format_apic(uint64_t apic_base, char *out, uint32_t out_len);
uint32_t kpanic_cpl_of_cs(uint32_t cs);
void     kpanic_format_priv(uint32_t cs, uint32_t eflags, char *out, uint32_t out_len);

/* Extended emit helpers. */
void     kpanic_u32_hex8(uint32_t v, char *out);
void     kpanic_emit_machine_line(const kpanic_record_t *rec);
void     kpanic_emit_sysinfo_serial(const kpanic_record_t *rec);
void     kpanic_emit_help_serial(void);
void     kpanic_emit_help_vga(void);

/* Extended memory analysis. */
void     kpanic_heap_grade(const kpanic_mem_state_t *m, char *out, uint32_t out_len);
uint32_t kpanic_fb_bytes(int32_t w, int32_t h, uint32_t bpp, bool *overflow);
uint32_t kpanic_bios_ram_kb(bool *ok);
void     kpanic_format_ram_line(char *out, uint32_t out_len);

/* Extended trace analysis. */
const char *kpanic_addr_region(uint32_t addr);
uint32_t kpanic_insn_len(const uint8_t *code, uint32_t avail);
char     kpanic_annotate_word(uint32_t word, uint32_t esp);
void     kpanic_annotate_stack(const kpanic_trace_state_t *t, char *out, uint32_t out_len);

/* Journal sequence + ISR entry + selftest. */
uint32_t kpanic_log_seq_next(void);
void     kpanic_log_seq_set(uint32_t s);
void     kpanic_isr_entry(const void *regs_ptr, const char *file, uint32_t line);
int      kpanic_selftest(char *out, uint32_t out_len);

/* Extended subsystem reference. */
const char *kpanic_pci_class_name(uint8_t cls, uint8_t sub);
void     kpanic_decode_ata_status(uint8_t st, char *out, uint32_t out_len);
void     kpanic_decode_ata_error(uint8_t er, char *out, uint32_t out_len);
const char *kpanic_tls_alert_name(uint8_t code);
const char *kpanic_tls_alert_fix(uint8_t code);
const char *kpanic_vbe_mode_note(uint16_t mode);
const char *kpanic_errno_name(int code);
const char *kpanic_errno_fix(int code);
void     kpanic_subsys_code_detail(kpanic_domain_t domain, uint32_t code,
                                   char *out, uint32_t out_len);
void     kpanic_domain_hint_serial(void);

/* Headless diagnostics: POST card + keyboard LEDs. */
void     kpanic_post(uint8_t code);
uint8_t  kpanic_post_for_domain(kpanic_domain_t d, kpanic_severity_t s);
void     kpanic_led_code(kpanic_domain_t d, kpanic_severity_t s);

/* Taint + watchdog. */
uint32_t kpanic_taint_flags(char *out, uint32_t out_len);
void     kpanic_watchdog(const char *file, uint32_t line);
kpanic_domain_t kpanic_classify_legacy(const char *message, uint32_t code);
void     kpanic_panic_with_regs(const void *regs_ptr, const char *file, uint32_t line);
void     kpanic_format_uptime(uint32_t ticks, char *out, uint32_t out_len);
void     kpanic_vram_budget_serial(const kpanic_mem_state_t *m);
void     kpanic_code_marker_serial(const kpanic_trace_state_t *t);
void     kpanic_cause_count_serial(void);
const kpanic_record_t *kpanic_log_last(void);

#ifdef __cplusplus
}
#endif

#endif
