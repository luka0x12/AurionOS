/*
 * kpanic_halt.c - end-of-panic policy: keys, reboot, poweroff.
 *
 * State machine: PANICKED -> (ENTER=reboot | S=serial replay |
 * D=register re-dump | timeout=reboot). Interrupts stay off; I poll
 * the 8042 directly so a dead IDT cannot block the reboot key. The
 * auto-reboot timer uses get_ticks() (IRQ0 keeps ticking only when
 * interrupts are on) with an RDTSC fallback when ticks freeze.
*/

#include "kpanic.h"

__attribute__((weak)) int sys_kb_hit(void) { return 0; }
__attribute__((weak)) uint16_t sys_getkey(void) { return 0; }

static inline void halt_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t halt_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

void kpanic_halt_cpu(void) {
    __asm__ volatile("cli; hlt");
}

void kpanic_reboot_now(void) {
    /* 8042 reset pulse. */
    uint8_t good = 0x02;
    (void)good;
    __asm__ volatile("cli");
    /* PCI reset control (port 0xCF9, Intel chipsets): bit1=reset,   */
    /* bit2+bit3 select hard reset. Try it first on modern boards;   */
    /* harmless where the register does not exist.                    */
    halt_outb(0xCF9u, 0x02u);
    for (uint32_t i = 0; i < 1000u; i++) {
        __asm__ volatile("" ::: "memory");
    }
    halt_outb(0xCF9u, 0x0Eu);
    /* Drain the 8042 input buffer, then pulse reset. */
    for (uint32_t i = 0; i < 10000u; i++) {
        if ((halt_inb(0x64u) & 0x02u) == 0) break;
    }
    halt_outb(0x64u, 0xFEu);
    /* If the 8042 ignored us, triple-fault via a bad IDT. */
    for (;;) {
        __asm__ volatile("lidt 0(%%eax)" :: "a"(0) : "memory");
        __asm__ volatile("int $3");
        __asm__ volatile("hlt");
    }
}

void kpanic_poweroff_qemu(void) {
    /* QEMU debug-exit (isa-debug-exit, port 0x501) and Bochs/older   */
    /* apm poweroff (0xB004). Harmless on real iron: the ports either */
    /* do not exist or ignore the write.                              */
    halt_outb(0xB004u, 0x00u);
    halt_outb(0x501u, 0x31u);
}

/* Direct 8042 poll: scancode set 1, ENTER = 0x1C make. I also accept */
/* the BIOS-buffered path (sys_kb_hit/sys_getkey) when it answers.   */
static bool poll_enter_raw(void) {
    uint8_t st = halt_inb(0x64u);
    if ((st & 0x01u) == 0) return false;
    {
        uint8_t sc = halt_inb(0x60u);
        if (sc == 0x1Cu) return true;   /* ENTER make */
        if (sc == 0x39u) return false;  /* SPACE: ignore */
        /* S make = 0x1F, D make = 0x20. Handled by caller via code. */
    }
    return false;
}

static int poll_key_raw(void) {
    uint8_t st = halt_inb(0x64u);
    if ((st & 0x01u) == 0) return -1;
    return (int)halt_inb(0x60u);
}

static bool k_help_shown;

static void help_show(const kpanic_record_t *rec) {
    kpanic_emit_help_serial();
    kpanic_emit_help_vga();
    (void)rec;
    k_help_shown = true;
}

static void help_maybe_restore(const kpanic_record_t *rec) {
    if (k_help_shown) {
        k_help_shown = false;
        kpanic_emit_all(rec);
    }
}

static uint64_t halt_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* POST codes to port 0x80. A POST card (or QEMU -d guest_errors with */
/* a debugcon) shows the last stage reached even when all three       */
/* screens are dead. Codes E0-EF are the panic range.                 */

#define POST_PANIC_ENTER 0xE0u
#define POST_PANIC_CPU 0xE1u
#define POST_PANIC_MEM 0xE2u
#define POST_PANIC_TRACE 0xE3u
#define POST_PANIC_EMIT 0xE4u
#define POST_PANIC_JOURNAL 0xE5u
#define POST_PANIC_HALT 0xE6u
#define POST_PANIC_REBOOT 0xE7u
#define POST_PANIC_REENTER 0xEEu
#define POST_PANIC_OOM 0xE8u
#define POST_PANIC_VRAM 0xE9u
#define POST_PANIC_GUI 0xEAu
#define POST_PANIC_VFS 0xEBu
#define POST_PANIC_NET 0xECu
#define POST_PANIC_CORRUPT 0xEFu

void kpanic_post(uint8_t code) {
    halt_outb(0x80u, code);
}

uint8_t kpanic_post_for_domain(kpanic_domain_t d, kpanic_severity_t s) {
    if (s == KP_SEV_CORRUPT) return POST_PANIC_CORRUPT;
    switch (d) {
    case KP_DOMAIN_CPU: return POST_PANIC_CPU;
    case KP_DOMAIN_OOM: return POST_PANIC_OOM;
    case KP_DOMAIN_VRAM: return POST_PANIC_VRAM;
    case KP_DOMAIN_GUI: return POST_PANIC_GUI;
    case KP_DOMAIN_VFS: return POST_PANIC_VFS;
    case KP_DOMAIN_NET: return POST_PANIC_NET;
    case KP_DOMAIN_HEAP: return POST_PANIC_CORRUPT;
    default: return POST_PANIC_ENTER;
    }
}

/* Keyboard LED blink codes. When video AND serial are both dead, the */
/* LEDs are the last working output. Pattern: blink NumLock <n> times */
/* for the domain, CapsLock <m> times for severity, ScrollLock solid  */
/* while waiting for ENTER. 8042 commands: ED=set LEDs, F4=enable.    */

static void kbd_wait_writable(void) {
    for (uint32_t i = 0; i < 10000u; i++)
        if ((halt_inb(0x64u) & 0x02u) == 0) return;
}

static void kbd_wait_readable(void) {
    for (uint32_t i = 0; i < 10000u; i++)
        if (halt_inb(0x64u) & 0x01u) return;
}

static void kbd_cmd(uint8_t cmd) {
    kbd_wait_writable();
    halt_outb(0x60u, cmd);
}

static void kbd_set_leds(uint8_t mask) {
    kbd_cmd(0xEDu);
    kbd_wait_readable();
    (void)halt_inb(0x60u); /* ACK */
    kbd_wait_writable();
    halt_outb(0x60u, mask);
    kbd_wait_readable();
    (void)halt_inb(0x60u); /* ACK */
}

static void blink_delay(void) {
    uint64_t t0 = halt_rdtsc();
    /* ~150ms at >=1GHz. Fail-safe direction: faster CPUs blink      */
    /* faster, which is still readable as a count.                   */
    while (halt_rdtsc() - t0 < 150000000ull) {
        __asm__ volatile("" ::: "memory");
    }
}

void kpanic_led_code(kpanic_domain_t d, kpanic_severity_t s) {
    uint32_t domain_blinks = (uint32_t)d + 1;
    uint32_t sev_blinks = (uint32_t)s + 1;
    uint32_t i;
    if (domain_blinks > 11) domain_blinks = 11;
    /* NumLock = domain. */
    for (i = 0; i < domain_blinks; i++) {
        kbd_set_leds(0x02u);
        blink_delay();
        kbd_set_leds(0x00u);
        blink_delay();
    }
    blink_delay();
    blink_delay();
    /* CapsLock = severity. */
    for (i = 0; i < sev_blinks; i++) {
        kbd_set_leds(0x04u);
        blink_delay();
        kbd_set_leds(0x00u);
        blink_delay();
    }
    /* ScrollLock solid while waiting. */
    kbd_set_leds(0x01u);
}

void kpanic_halt_loop(const kpanic_record_t *rec) {
    uint32_t start_ticks;
    uint64_t start_tsc;
    bool use_ticks;
    __asm__ volatile("cli");
    if (!rec) {
        kpanic_reboot_now();
        for (;;) kpanic_halt_cpu();
    }
    /* CORRUPT severity: reboot without waiting. Sitting on the prompt */
    /* risks the user pressing S and running more code on a smashed   */
    /* heap. Still honor ENTER instantly, but skip the 10s dwell.     */
    start_ticks = rec->uptime_ticks;
    start_tsc = halt_rdtsc();
    /* Ticks move only with IRQs on. I turned them off, so expect them */
    /* frozen; use TSC (constant on all CPUs we boot) as the clock.   */
    use_ticks = false;
    (void)use_ticks;

    if (rec->severity == KP_SEV_CORRUPT) {
        /* One short grace window (~1s) for ENTER, then reboot. */
        for (uint32_t spins = 0; spins < 2000000u; spins++) {
            int sc;
            if (sys_kb_hit()) {
                uint16_t k = sys_getkey();
                uint8_t lo = (uint8_t)(k & 0xFFu);
                if (lo == 13 || lo == 10) kpanic_reboot_now();
            }
            sc = poll_key_raw();
            if (sc == 0x1C) kpanic_reboot_now();
            if ((spins & 0xFFFu) == 0) {
                if (halt_rdtsc() - start_tsc > KP_CORRUPT_GRACE_TSC) break;
            }
            __asm__ volatile("hlt");
        }
        kpanic_reboot_now();
        for (;;) kpanic_halt_cpu();
    }

    kpanic_halt_announced(rec);
    for (;;) {
        /* Buffered path first (works when the keyboard driver is OK). */
        if (sys_kb_hit()) {
            uint16_t k = sys_getkey();
            uint8_t lo = (uint8_t)(k & 0xFFu);
            if (lo == 13 || lo == 10) kpanic_reboot_now();
            if (k_help_shown && lo != 'h' && lo != 'H') {
                help_maybe_restore(rec);
            }
            if (lo == 's' || lo == 'S') {
                kpanic_log_replay_serial();
                kpanic_emit_serial(rec);
            }
            if (lo == 'd' || lo == 'D') {
                kpanic_emit_serial(rec);
            }
            if (lo == 'h' || lo == 'H') {
                help_show(rec);
            }
            if (lo == 'p' || lo == 'P') {
                kpanic_poweroff_qemu();
            }
            if (lo == 'x' || lo == 'X') {
                for (uint32_t s = 0; s < 8; s++)
                    kpanic_log_export_hex(s);
            }
        }
        /* Raw path: survives a dead IDT/driver. */
        {
            int sc = poll_key_raw();
            if (k_help_shown && sc != -1 && sc != 0x23 && (sc & 0x80) == 0)
                help_maybe_restore(rec);
            if (sc == 0x1C) kpanic_reboot_now();
            else if (sc == 0x1F) { /* S */
                kpanic_log_replay_serial();
                kpanic_emit_serial(rec);
            } else if (sc == 0x20) { /* D */
                kpanic_emit_serial(rec);
            } else if (sc == 0x23) { /* H */
                help_show(rec);
            } else if (sc == 0x19) { /* P */
                kpanic_poweroff_qemu();
            } else if (sc == 0x2D) { /* X */
                for (uint32_t s = 0; s < 8; s++)
                    kpanic_log_export_hex(s);
            }
            (void)poll_enter_raw();
        }
        /* Auto-reboot after ~10s of TSC time. 3GHz assumption is only */
        /* for the timeout, never for any dump content.               */
        {
            uint64_t dt = halt_rdtsc() - start_tsc;
            /* ~10s at >=1GHz. Faster CPUs reboot sooner; that is fine */
            /* for a panic prompt (fail-safe direction).              */
            if (dt > KP_AUTO_REBOOT_TSC) kpanic_reboot_now();
            /* Secondary guard: ticks moved (IRQs somehow on) and 1000 */
            /* ticks (10s at 100Hz) elapsed.                          */
            {
                uint32_t now = 0;
                /* get_ticks may be linked weakly; call only when the */
                /* symbol resolved (nonzero function address check via */
                /* direct call is safe: weak stub returns 0).         */
                extern uint32_t get_ticks(void);
                now = get_ticks();
                if (now - start_ticks > 1000u) kpanic_reboot_now();
            }
        }
        __asm__ volatile("hlt");
    }
}

/* One-line halt announcement on serial: severity, policy, timeout.   */
/* Runs once before the key loop. kpanic_halt_announced() is the      */
/* once-guard so a re-entered halt never double-prints.               */

static bool k_halt_announced;

void kpanic_halt_announced(const kpanic_record_t *rec) {
    if (k_halt_announced) return;
    k_halt_announced = true;
    if (!rec) return;
    kpanic_serial_puts("HALT [");
    kpanic_serial_puts(kpanic_severity_name(rec->severity));
    kpanic_serial_puts("]: ");
    kpanic_serial_puts(kpanic_action_line(rec->severity));
    kpanic_serial_puts(" Keys: ENTER/S/D/H/P/X. Auto-reboot ~10s.\n");
}
