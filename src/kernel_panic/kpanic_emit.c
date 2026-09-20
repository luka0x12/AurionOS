/*
 * kpanic_emit.c - panic-time output with zero allocation.
 *
 * I write to three sinks at once: VGA text at 0xB8000 (always works,
 * even when the VESA driver is the thing that crashed), VESA graphics
 * via gpu_* (readable at 1920x1080), and COM1 serial (the headless
 * log QEMU captures). No kmalloc, no printf, no cursor ioctl that can
 * fault. Direct port + memory-mapped I/O only.
*/

#include "kpanic.h"

/* Tiny string helpers. I do not link libc string.o in the panic path */
/* so a libc regression cannot take the dumper down with it.          */

uint32_t kpanic_strlen(const char *s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

void kpanic_strncpy(char *dst, const char *src, uint32_t dst_len) {
    uint32_t i = 0;
    if (!dst || !dst_len) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_len && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

bool kpanic_streq(const char *a, const char *b) {
    uint32_t i = 0;
    if (!a || !b) return a == b;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

void kpanic_u32_hex(uint32_t v, char *out) {
    static const char dig[] = "0123456789ABCDEF";
    if (!out) return;
    out[0] = '0';
    out[1] = 'x';
    for (int32_t s = 28; s >= 0; s -= 4)
        out[2 + (28 - s) / 4] = dig[(v >> s) & 0xFu];
    out[10] = '\0';
}

void kpanic_u32_dec(uint32_t v, char *out) {
    char rev[12];
    uint32_t n = 0;
    if (!out) return;
    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v && n < sizeof(rev)) {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (uint32_t i = 0; i < n; i++)
        out[i] = rev[n - 1 - i];
    out[n] = '\0';
}

void kpanic_u64_hex(uint64_t v, char *out) {
    static const char dig[] = "0123456789ABCDEF";
    if (!out) return;
    out[0] = '0';
    out[1] = 'x';
    for (int32_t s = 60; s >= 0; s -= 4)
        out[2 + (60 - s) / 4] = dig[(uint32_t)((v >> s) & 0xFu)];
    out[18] = '\0';
}

void kpanic_u32_hex8(uint32_t v, char *out) {
    static const char dig[] = "0123456789ABCDEF";
    if (!out) return;
    for (int32_t s = 28; s >= 0; s -= 4)
        out[(28 - s) / 4] = dig[(v >> s) & 0xFu];
    out[8] = '\0';
}

/* COM1 serial. Polled, bounded, no interrupt dependency. */

static inline void emit_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t emit_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void serial_raw(char c) {
    /* Bounded TX wait: 100k polls, then give up so a dead UART cannot
     * wedge the panic dumper. Real hardware answers in <10 polls. */
    for (uint32_t i = 0; i < 100000u; i++) {
        if (emit_inb(0x3FDu) & 0x20u) break;
    }
    emit_outb(0x3F8u, (uint8_t)c);
}

void kpanic_serial_puts(const char *s) {
    if (!s) return;
    while (*s) {
        char c = *s++;
        if (c == '\n') serial_raw('\r');
        serial_raw(c);
    }
}

/* Externals for the VESA path. Weak so a harness without the GPU     */
/* driver still links; I probe them at runtime anyway.                */
__attribute__((weak)) int gpu_get_width(void) { return 0; }
__attribute__((weak)) int gpu_get_height(void) { return 0; }
__attribute__((weak)) bool gpu_is_vesa(void) { return false; }
__attribute__((weak)) void gpu_fill_rect(int x, int y, int w, int h, uint32_t c) {
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
__attribute__((weak)) void gpu_draw_string(int x, int y, const uint8_t *s, uint32_t fg, uint32_t bg) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
__attribute__((weak)) int gpu_flush(void) { return -1; }
__attribute__((weak)) void c_putc(char c) { (void)c; }
__attribute__((weak)) void c_puts(const char *s) { (void)s; }
__attribute__((weak)) void set_attr(uint8_t a) { (void)a; }

/* VGA text backend. Writes 0xB8000 directly so a broken console_shim */
/* hook or terminal redirect cannot silence the panic.                */

#define VGA_BASE ((volatile uint16_t *)0xB8000u)
#define VGA_COLS 80u
#define VGA_ROWS 25u

static uint32_t vga_row;
static uint32_t vga_col;
static uint8_t vga_attr = 0x07;

static void vga_set_hw_cursor(uint32_t pos) {
    emit_outb(0x3D4u, 0x0Fu);
    emit_outb(0x3D5u, (uint8_t)(pos & 0xFFu));
    emit_outb(0x3D4u, 0x0Eu);
    emit_outb(0x3D5u, (uint8_t)((pos >> 8) & 0xFFu));
}

static void vga_clear(uint8_t attr) {
    for (uint32_t i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_BASE[i] = (uint16_t)(((uint16_t)attr << 8) | 0x20u);
    vga_row = 0;
    vga_col = 0;
    vga_set_hw_cursor(0);
}

static void vga_scroll(void) {
    for (uint32_t r = 0; r + 1 < VGA_ROWS; r++)
        for (uint32_t c = 0; c < VGA_COLS; c++)
            VGA_BASE[r * VGA_COLS + c] = VGA_BASE[(r + 1) * VGA_COLS + c];
    for (uint32_t c = 0; c < VGA_COLS; c++)
        VGA_BASE[(VGA_ROWS - 1) * VGA_COLS + c] = (uint16_t)(((uint16_t)vga_attr << 8) | 0x20u);
    vga_row = VGA_ROWS - 1;
}

static void vga_ch(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_ROWS) vga_scroll();
        vga_set_hw_cursor(vga_row * VGA_COLS + vga_col);
        return;
    }
    if (c == '\r') {
        vga_col = 0;
        return;
    }
    if (vga_col >= VGA_COLS) {
        vga_col = 0;
        vga_row++;
    }
    if (vga_row >= VGA_ROWS) vga_scroll();
    VGA_BASE[vga_row * VGA_COLS + vga_col] =
        (uint16_t)(((uint16_t)vga_attr << 8) | (uint8_t)c);
    vga_col++;
    vga_set_hw_cursor(vga_row * VGA_COLS + vga_col);
}

static void vga_str(const char *s) {
    if (!s) return;
    while (*s) vga_ch(*s++);
}

/* VESA backend. Draws the same lines with gpu_draw_string so the     */
/* panic is readable without switching back to text mode (which may   */
/* itself be broken). I clamp to screen size read live.               */

#define KP_GFX_BG 0xFF0A0A14u
#define KP_GFX_FG 0xFFE8E8F0u
#define KP_GFX_ACC 0xFFFFB000u
#define KP_GFX_DIM 0xFF9A9AA8u
#define KP_GFX_RED 0xFFFF5555u

static int gfx_cursor_y;
static int gfx_w;
static int gfx_h;

static void gfx_begin(void) {
    gfx_w = gpu_get_width();
    gfx_h = gpu_get_height();
    if (gfx_w <= 0 || gfx_w > 4096) gfx_w = 0;
    if (gfx_h <= 0 || gfx_h > 2160) gfx_h = 0;
    if (gfx_w == 0 || gfx_h == 0) return;
    gpu_fill_rect(0, 0, gfx_w, gfx_h, KP_GFX_BG);
    /* Amber rule under the banner. */
    gpu_fill_rect(24, 52, gfx_w - 48, 3, KP_GFX_ACC);
    gfx_cursor_y = 72;
}

static void gfx_line(const char *s, uint32_t color) {
    char clipped[100];
    uint32_t i = 0;
    if (!gfx_w || !gfx_h) return;
    if (gfx_cursor_y + 16 > gfx_h - 40) return; /* keep footer visible */
    if (!s) s = "";
    while (s[i] && i + 1 < sizeof(clipped)) {
        clipped[i] = s[i];
        i++;
    }
    clipped[i] = '\0';
    /* Hard clip to ~112 columns at 8px on 1024+ wide modes. */
    if (gfx_w < 900 && i > 80) clipped[80] = '\0';
    gpu_draw_string(24, gfx_cursor_y, (const uint8_t *)clipped, color, 0);
    gfx_cursor_y += 16;
}

static void gfx_footer(void) {
    if (!gfx_w || !gfx_h) return;
    gpu_draw_string(24, gfx_h - 32,
        (const uint8_t *)"ENTER=reboot S=serial H=help P=off X=hex auto 10s",
        KP_GFX_DIM, 0);
    gpu_flush();
}

/* Help screen: what each dump section means + what to send with the  */
/* bug report. Rendered on demand (H key) over the same VESA/VGA      */
/* backends; serial gets the text form. Bounded, static strings.      */
void kpanic_emit_help_serial(void) {
    kpanic_serial_puts("--- panic help ---\n");
    kpanic_serial_puts("VEC: x86 vector. 0-31=CPU fault (see cause table), 32-47=IRQ.\n");
    kpanic_serial_puts("ERR: #PF=page-fault bits, #GP=selector. CR2=fault address.\n");
    kpanic_serial_puts("REGS: faulting context from the ISR frame (not the dumper).\n");
    kpanic_serial_puts("HEAP: live kmalloc stats. CORRUPT=MCB magic mismatch.\n");
    kpanic_serial_puts("VRAM: live mode + framebuffer phys addr from 0x9000 block.\n");
    kpanic_serial_puts("BACKTRACE: EBP chain, word tags C=code S=stack H=heap A=ascii.\n");
    kpanic_serial_puts("STACK: 32 live dwords from ESP. CODE: bytes around EIP.\n");
    kpanic_serial_puts("Report: Oops # + PANIC: line + serial journal + photo.\n");
    kpanic_serial_puts("Keys: ENTER=reboot S=serial D=re-dump H=help P=off X=hex.\n");
    kpanic_domain_hint_serial();
    kpanic_serial_puts("--- end help ---\n");
}

void kpanic_emit_help_vga(void) {
    __asm__ volatile("cli");
    vga_attr = 0x1F;
    vga_clear(0x1F);
    vga_attr = 0x1F;
    vga_str("PANIC HELP -- what the dump means");
    vga_ch('\n');
    vga_attr = 0x07;
    vga_str("VEC 0-31 CPU fault, 32-47 IRQ. ERR #PF/#GP bits.");
    vga_ch('\n');
    vga_str("CR2 fault addr. REGS from ISR frame, not dumper.");
    vga_ch('\n');
    vga_str("HEAP live kmalloc stats. CORRUPT = MCB mismatch.");
    vga_ch('\n');
    vga_str("VRAM live mode + fb addr from 0x9000 block.");
    vga_ch('\n');
    vga_str("TRACE EBP chain. Tags C code S stack H heap A ascii.");
    vga_ch('\n');
    vga_str("Report Oops# + PANIC: line + journal + photo.");
    vga_ch('\n');
    vga_attr = 0x1E;
    vga_str("Any key returns to the dump. ENTER reboots.");
    vga_ch('\n');
    vga_str("S serial D redump P poweroff X hexexport.");
    vga_ch('\n');
}

/* Shared line formatters. All bounded, all static buffers. */

static void fmt_reg_line(const char *name, uint32_t v, char *out, uint32_t len) {
    char hx[16];
    uint32_t p = 0;
    kpanic_u32_hex(v, hx);
    while (*name && p + 1 < len) out[p++] = *name++;
    while (p + 1 < len && p < 6) out[p++] = ' ';
    {
        const char *h = hx;
        while (*h && p + 1 < len) out[p++] = *h++;
    }
    out[p] = '\0';
}

/* Full record emitters. */

static void emit_header_serial(const kpanic_record_t *r) {
    char nb[24];
    char hx[24];
    kpanic_serial_puts("\n========================================\n");
    kpanic_serial_puts(KPANIC_BANNER);
    kpanic_serial_puts("  [");
    kpanic_serial_puts(kpanic_domain_name(r->domain));
    kpanic_serial_puts(" / ");
    kpanic_serial_puts(kpanic_severity_name(r->severity));
    kpanic_serial_puts("]\n");
    kpanic_u32_dec(r->seq, nb);
    kpanic_serial_puts("Oops #");
    kpanic_serial_puts(nb);
    kpanic_serial_puts("  uptime=");
    kpanic_u32_dec(r->uptime_ticks, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" ticks (100Hz)  TSC=");
    kpanic_u64_hex(r->cpu.tsc, hx);
    kpanic_serial_puts(hx);
    {
        char up[32];
        kpanic_format_uptime(r->uptime_ticks, up, sizeof(up));
        kpanic_serial_puts("UPTIME: ");
        kpanic_serial_puts(up);
        kpanic_serial_puts("\n");
    }
    kpanic_serial_puts("\nFAULT: ");
    kpanic_serial_puts(r->message[0] ? r->message : "(no message)");
    kpanic_serial_puts("\nAT: ");
    kpanic_serial_puts(r->file[0] ? r->file : "?");
    kpanic_serial_puts(":");
    kpanic_u32_dec(r->line, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts("  code=");
    kpanic_u32_hex(r->code, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts("\n");
    if (r->aux[0]) {
        kpanic_serial_puts("INFO: ");
        kpanic_serial_puts(r->aux);
        kpanic_serial_puts("\n");
    }
}

static void emit_regs_serial(const kpanic_record_t *r) {
    char hx[24];
    char fl[96];
    if (!r->has_regs) {
        kpanic_serial_puts("REGS: n/a (explicit panic, no ISR frame)\n");
        return;
    }
    kpanic_serial_puts("VEC ");
    {
        char nb[16];
        kpanic_u32_dec(r->regs.int_no, nb);
        kpanic_serial_puts(nb);
    }
    kpanic_serial_puts(" (");
    kpanic_serial_puts(kpanic_exception_name(r->regs.int_no));
    kpanic_serial_puts(")  ERR=");
    kpanic_u32_hex(r->regs.err_code, hx);
    kpanic_serial_puts(hx);
    if (r->regs.int_no == 14) {
        char pf[96];
        kpanic_decode_pf_error(r->regs.err_code, pf, sizeof(pf));
        kpanic_serial_puts(" [");
        kpanic_serial_puts(pf);
        kpanic_serial_puts("]");
    } else if (r->regs.int_no == 13) {
        char gp[96];
        kpanic_decode_gpf_error(r->regs.err_code, gp, sizeof(gp));
        kpanic_serial_puts(" [");
        kpanic_serial_puts(gp);
        kpanic_serial_puts("]");
    }
    kpanic_serial_puts("\nEIP=");
    kpanic_u32_hex(r->regs.eip, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(" CS=");
    kpanic_u32_hex(r->regs.cs, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(" EFLAGS=");
    kpanic_u32_hex(r->regs.eflags, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts("  CR2=");
    kpanic_u32_hex(r->cpu.cr2, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(" CR3=");
    kpanic_u32_hex(r->cpu.cr3, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts("\n");
    kpanic_decode_eflags(r->regs.eflags, fl, sizeof(fl));
    kpanic_serial_puts("FLAGS: ");
    kpanic_serial_puts(fl);
    kpanic_serial_puts("\n");
    kpanic_serial_puts("EAX="); kpanic_u32_hex(r->regs.eax, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" EBX="); kpanic_u32_hex(r->regs.ebx, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" ECX="); kpanic_u32_hex(r->regs.ecx, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" EDX="); kpanic_u32_hex(r->regs.edx, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts("\nESI="); kpanic_u32_hex(r->regs.esi, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" EDI="); kpanic_u32_hex(r->regs.edi, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" EBP="); kpanic_u32_hex(r->regs.ebp, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" ESP="); kpanic_u32_hex(r->regs.esp, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts("\nDS="); kpanic_u32_hex(r->regs.ds, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" ES="); kpanic_u32_hex(r->regs.es, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" FS="); kpanic_u32_hex(r->regs.fs, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts(" GS="); kpanic_u32_hex(r->regs.gs, hx); kpanic_serial_puts(hx);
    kpanic_serial_puts("\n");
    {
        char vn[20];
        kpanic_format_vendor(&r->cpu, vn, sizeof(vn));
        kpanic_serial_puts("CPU: ");
        kpanic_serial_puts(vn);
        kpanic_serial_puts(" CR0=");
        kpanic_u32_hex(r->cpu.cr0, hx);
        kpanic_serial_puts(hx);
        kpanic_serial_puts("\n");
    }
}

static void emit_mem_serial(const kpanic_record_t *r) {
    char line[128];
    kpanic_format_heap_line(&r->mem, line, sizeof(line));
    kpanic_serial_puts("HEAP: ");
    kpanic_serial_puts(line);
    kpanic_serial_puts("\n");
    kpanic_format_vram_line(&r->mem, line, sizeof(line));
    kpanic_serial_puts("VRAM: ");
    kpanic_serial_puts(line);
    kpanic_serial_puts("\n");
}

static void emit_trace_serial(const kpanic_record_t *r) {
    char hx[24];
    char nb[16];
    kpanic_serial_puts("BACKTRACE (real EBP chain):\n");
    if (!r->trace.ebp_valid && r->trace.frame_count == 0) {
        kpanic_serial_puts("  <no valid frames: EBP failed sanity gate>\n");
    }
    for (uint32_t i = 0; i < r->trace.frame_count; i++) {
        kpanic_serial_puts("  #");
        kpanic_u32_dec(i, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts(" EBP=");
        kpanic_u32_hex(r->trace.frames[i].ebp, hx);
        kpanic_serial_puts(hx);
        kpanic_serial_puts(" EIP=");
        if (r->trace.frames[i].eip_readable) {
            kpanic_u32_hex(r->trace.frames[i].eip, hx);
            kpanic_serial_puts(hx);
        } else {
            kpanic_serial_puts("??????????");
        }
        kpanic_serial_puts("\n");
    }
    kpanic_serial_puts("STACK @");
    kpanic_u32_hex(r->trace.stack_base, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(":\n");
    for (uint32_t i = 0; i < r->trace.stack_words; i++) {
        if ((i % 4) == 0) {
            kpanic_serial_puts("  ");
            kpanic_u32_hex(r->trace.stack_base + i * 4, hx);
            kpanic_serial_puts(hx);
            kpanic_serial_puts(": ");
        }
        kpanic_u32_hex(r->trace.stack_dump[i], hx);
        kpanic_serial_puts(hx + 2);
        if ((i % 4) == 3 || i + 1 == r->trace.stack_words)
            kpanic_serial_puts("\n");
        else
            kpanic_serial_puts(" ");
    }
    if (r->trace.code_len) {
        kpanic_serial_puts("CODE @");
        kpanic_u32_hex(r->trace.code_base, hx);
        kpanic_serial_puts(hx);
        kpanic_serial_puts(": ");
        for (uint32_t i = 0; i < r->trace.code_len; i++) {
            static const char dig[] = "0123456789ABCDEF";
            char b[3];
            b[0] = dig[(r->trace.code_bytes[i] >> 4) & 0xF];
            b[1] = dig[r->trace.code_bytes[i] & 0xF];
            b[2] = '\0';
            kpanic_serial_puts(b);
            if (i + 1 < r->trace.code_len) kpanic_serial_puts(" ");
        }
        kpanic_serial_puts("\n");
        kpanic_code_marker_serial(&r->trace);
    }
}

void kpanic_emit_serial(const kpanic_record_t *rec) {
    if (!rec) return;
    emit_header_serial(rec);
    emit_regs_serial(rec);
    emit_mem_serial(rec);
    emit_trace_serial(rec);
    kpanic_serial_puts("ACTION: ");
    kpanic_serial_puts(kpanic_action_line(rec->severity));
    kpanic_serial_puts("\n========================================\n");
    kpanic_emit_machine_line(rec);
    kpanic_emit_sysinfo_serial(rec);
}

/* Machine-parseable one-liner for log scrapers and CI boot checks.
 * Format: PANIC: seq=<n> domain=<name> sev=<name> eip=<hex> cr2=<hex>
 * code=<hex> free=<dec> msg="<message>" — no spaces inside fields
 * except the quoted message tail, so grep/cut keep working.
*/
void kpanic_emit_machine_line(const kpanic_record_t *rec) {
    char nb[16];
    char hx[24];
    if (!rec) return;
    kpanic_serial_puts("PANIC: seq=");
    kpanic_u32_dec(rec->seq, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" domain=");
    kpanic_serial_puts(kpanic_domain_name(rec->domain));
    kpanic_serial_puts(" sev=");
    kpanic_serial_puts(kpanic_severity_name(rec->severity));
    kpanic_serial_puts(" eip=");
    kpanic_u32_hex(rec->has_regs ? rec->regs.eip : 0, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(" cr2=");
    kpanic_u32_hex(rec->cpu.cr2, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(" code=");
    kpanic_u32_hex(rec->code, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(" free=");
    kpanic_u32_dec(rec->mem.stats_ok ? rec->mem.heap_free : 0xFFFFFFFFu, nb);
    kpanic_serial_puts(nb);
    kpanic_serial_puts(" msg=\"");
    kpanic_serial_puts(rec->message[0] ? rec->message : "-");
    kpanic_serial_puts("\"\n");
}

/* VGA full-screen renderer. Mirrors the serial content in a denser   */
/* layout that fits 80x25.                                            */

void kpanic_emit_all(const kpanic_record_t *rec) {
    char line[128];
    char hx[24];
    char nb[16];
    char fl[96];
    if (!rec) return;

    /* Serial first: if the screen path triple-faults, the log host   */
    /* already has the dump.                                          */
    kpanic_emit_serial(rec);

    /* VGA text */
    __asm__ volatile("cli");
    vga_attr = 0x1F;
    vga_clear(0x1F);
    vga_attr = 0x1F;
    vga_str(KPANIC_BANNER);
    vga_str(" -- ");
    vga_str(kpanic_domain_name(rec->domain));
    vga_ch('\n');
    vga_attr = 0x07;
    vga_str("Oops #");
    kpanic_u32_dec(rec->seq, nb);
    vga_str(nb);
    vga_str("  ");
    vga_str(rec->message[0] ? rec->message : "(no message)");
    vga_ch('\n');
    vga_str("at ");
    vga_str(rec->file[0] ? rec->file : "?");
    vga_str(":");
    kpanic_u32_dec(rec->line, nb);
    vga_str(nb);
    vga_str("  code=");
    kpanic_u32_hex(rec->code, hx);
    vga_str(hx);
    vga_ch('\n');
    if (rec->aux[0]) {
        vga_attr = 0x0E;
        vga_str(rec->aux);
        vga_ch('\n');
        vga_attr = 0x07;
    }
    if (rec->has_regs) {
        vga_str("VEC ");
        kpanic_u32_dec(rec->regs.int_no, nb);
        vga_str(nb);
        vga_str(" ");
        vga_str(kpanic_exception_name(rec->regs.int_no));
        vga_ch('\n');
        fmt_reg_line("EIP", rec->regs.eip, line, sizeof(line));
        vga_str(line);
        vga_str("  CR2=");
        kpanic_u32_hex(rec->cpu.cr2, hx);
        vga_str(hx);
        vga_ch('\n');
        fmt_reg_line("EAX", rec->regs.eax, line, sizeof(line));
        vga_str(line);
        vga_str(" ");
        fmt_reg_line("EBX", rec->regs.ebx, line, sizeof(line));
        vga_str(line);
        vga_ch('\n');
        fmt_reg_line("ECX", rec->regs.ecx, line, sizeof(line));
        vga_str(line);
        vga_str(" ");
        fmt_reg_line("EDX", rec->regs.edx, line, sizeof(line));
        vga_str(line);
        vga_ch('\n');
        fmt_reg_line("ESI", rec->regs.esi, line, sizeof(line));
        vga_str(line);
        vga_str(" ");
        fmt_reg_line("EDI", rec->regs.edi, line, sizeof(line));
        vga_str(line);
        vga_ch('\n');
        fmt_reg_line("EBP", rec->regs.ebp, line, sizeof(line));
        vga_str(line);
        vga_str(" ");
        fmt_reg_line("ESP", rec->regs.esp, line, sizeof(line));
        vga_str(line);
        vga_ch('\n');
        kpanic_decode_eflags(rec->regs.eflags, fl, sizeof(fl));
        vga_str("FLAGS ");
        vga_str(fl);
        vga_ch('\n');
    } else {
        vga_str("(no ISR frame: explicit panic)");
        vga_ch('\n');
    }
    kpanic_format_heap_line(&rec->mem, line, sizeof(line));
    vga_str("HEAP ");
    vga_str(line);
    vga_ch('\n');
    kpanic_format_vram_line(&rec->mem, line, sizeof(line));
    vga_str("VRAM ");
    vga_str(line);
    vga_ch('\n');
    vga_str("TRACE ");
    if (rec->trace.frame_count == 0) {
        vga_str("<no valid frames>");
    } else {
        for (uint32_t i = 0; i < rec->trace.frame_count && i < 4; i++) {
            kpanic_u32_hex(rec->trace.frames[i].eip, hx);
            vga_str(hx);
            vga_str(" ");
        }
    }
    vga_ch('\n');
    {
        char up[32];
        char pv[64];
        char tags[40];
        kpanic_format_uptime(rec->uptime_ticks, up, sizeof(up));
        vga_str("UP ");
        vga_str(up);
        vga_ch('\n');
        kpanic_format_priv(rec->has_regs ? rec->regs.cs : 0,
                           rec->has_regs ? rec->regs.eflags : rec->cpu.eflags_live,
                           pv, sizeof(pv));
        vga_str(pv);
        vga_ch('\n');
        kpanic_annotate_stack(&rec->trace, tags, sizeof(tags));
        vga_str("TAGS ");
        vga_str(tags[0] ? tags : "-");
        vga_ch('\n');
    }
    vga_attr = 0x1E;
    vga_str(kpanic_action_line(rec->severity));
    vga_ch('\n');
    vga_attr = 0x07;
    vga_str("ENTER=reboot S=ser D=dump H=help P=off auto 10s");
    vga_ch('\n');

    /* VESA overlay (only when the GPU probe says a mode is up). */
    /* If the GPU is what crashed, gpu_is_vesa() is false and I skip  */
    /* it instead of hanging inside the driver.                       */
    if (rec->mem.gpu_vesa) {
        gfx_begin();
        if (gfx_w && gfx_h) {
            gfx_line(KPANIC_BANNER, KP_GFX_ACC);
            gfx_line(rec->message[0] ? rec->message : "(no message)", KP_GFX_FG);
            line[0] = '\0';
            {
                uint32_t p = 0;
                const char *t = "Oops #";
                while (*t && p + 1 < sizeof(line)) line[p++] = *t++;
                kpanic_u32_dec(rec->seq, nb);
                t = nb;
                while (*t && p + 1 < sizeof(line)) line[p++] = *t++;
                t = "  ";
                while (*t && p + 1 < sizeof(line)) line[p++] = *t++;
                t = kpanic_domain_name(rec->domain);
                while (*t && p + 1 < sizeof(line)) line[p++] = *t++;
                line[p] = '\0';
            }
            gfx_line(line, KP_GFX_DIM);
            if (rec->has_regs) {
                char rl[110];
                uint32_t p = 0;
                const char *t = "VEC ";
                while (*t && p + 1 < sizeof(rl)) rl[p++] = *t++;
                kpanic_u32_dec(rec->regs.int_no, nb);
                t = nb;
                while (*t && p + 1 < sizeof(rl)) rl[p++] = *t++;
                t = "  EIP ";
                while (*t && p + 1 < sizeof(rl)) rl[p++] = *t++;
                kpanic_u32_hex(rec->regs.eip, hx);
                t = hx;
                while (*t && p + 1 < sizeof(rl)) rl[p++] = *t++;
                t = "  CR2 ";
                while (*t && p + 1 < sizeof(rl)) rl[p++] = *t++;
                kpanic_u32_hex(rec->cpu.cr2, hx);
                t = hx;
                while (*t && p + 1 < sizeof(rl)) rl[p++] = *t++;
                rl[p] = '\0';
                gfx_line(rl, KP_GFX_FG);
                fmt_reg_line("EAX", rec->regs.eax, line, sizeof(line));
                gfx_line(line, KP_GFX_FG);
                fmt_reg_line("EBP", rec->regs.ebp, line, sizeof(line));
                gfx_line(line, KP_GFX_FG);
            }
            kpanic_format_heap_line(&rec->mem, line, sizeof(line));
            gfx_line(line, KP_GFX_DIM);
            {
                char up[32];
                uint32_t q = 0;
                const char *t = "up ";
                kpanic_format_uptime(rec->uptime_ticks, up, sizeof(up));
                while (*t && q + 1 < sizeof(line)) line[q++] = *t++;
                t = up;
                while (*t && q + 1 < sizeof(line)) line[q++] = *t++;
                line[q] = '\0';
            }
            gfx_line(line, KP_GFX_DIM);
            kpanic_format_vram_line(&rec->mem, line, sizeof(line));
            gfx_line(line, KP_GFX_DIM);
            if (rec->aux[0]) gfx_line(rec->aux, KP_GFX_RED);
            gfx_line(kpanic_action_line(rec->severity), KP_GFX_ACC);
            gfx_footer();
        }
    }
}

/* Extended sysinfo block: taint, BIOS RAM, APIC, CR0, features, heap */
/* grade, vector detail. Runs after the machine line on serial; VGA   */
/* keeps the dense subset (space is 80x25). All values live.          */
void kpanic_emit_sysinfo_serial(const kpanic_record_t *rec) {
    char line[160];
    char hx[24];
    if (!rec) return;
    /* Taint. */
    {
        char t[64];
        kpanic_taint_flags(t, sizeof(t));
        kpanic_serial_puts("TAINT: ");
        kpanic_serial_puts(t);
        kpanic_serial_puts("\n");
    }
    /* BIOS RAM. */
    kpanic_format_ram_line(line, sizeof(line));
    kpanic_serial_puts("RAM: ");
    kpanic_serial_puts(line);
    kpanic_serial_puts("\n");
    /* VBE mode note + CR0 bit glossary. */
    if (rec->mem.vbe_info_ok) {
        kpanic_serial_puts("VBEMODE: ");
        {
            char nb[16];
            kpanic_u32_dec(rec->mem.vbe_mode, nb);
            kpanic_serial_puts(nb);
        }
        kpanic_serial_puts(" ");
        kpanic_serial_puts(kpanic_vbe_mode_note(rec->mem.vbe_mode));
        kpanic_serial_puts("\n");
    }
    kpanic_serial_puts("CR0-PG: ");
    kpanic_serial_puts(kpanic_cr0_bit_meaning(31));
    kpanic_serial_puts(" / CR0-WP: ");
    kpanic_serial_puts(kpanic_cr0_bit_meaning(16));
    kpanic_serial_puts("\n");
    /* APIC base when MSRs are readable. */
    {
        bool ok = false;
        uint64_t base = kpanic_rdmsr_safe(0x1Bu, &ok, &rec->cpu);
        if (ok) {
            kpanic_format_apic(base, line, sizeof(line));
            kpanic_serial_puts("APIC: ");
            kpanic_serial_puts(line);
            kpanic_serial_puts("\n");
        } else {
            kpanic_serial_puts("APIC: n/a (no MSR)\n");
        }
    }
    /* CPU summary line (ISR EIP + live CR2 + vector). */
    {
        char sum[96];
        kpanic_cpu_summary(rec->has_regs ? &rec->regs : NULL, &rec->cpu,
                           sum, sizeof(sum));
        kpanic_serial_puts("SUMMARY: ");
        kpanic_serial_puts(sum);
        kpanic_serial_puts("\n");
        {
            char pv[96];
            kpanic_format_priv(rec->has_regs ? rec->regs.cs : 0,
                               rec->has_regs ? rec->regs.eflags : rec->cpu.eflags_live,
                               pv, sizeof(pv));
            kpanic_serial_puts("PRIV: ");
            kpanic_serial_puts(pv);
            kpanic_serial_puts("\n");
        }
    }
    /* CR0 headline bits. */
    kpanic_decode_cr0(rec->cpu.cr0, line, sizeof(line));
    kpanic_serial_puts("CR0: ");
    kpanic_serial_puts(line);
    kpanic_serial_puts(" (");
    kpanic_u32_hex(rec->cpu.cr0, hx);
    kpanic_serial_puts(hx);
    kpanic_serial_puts(")\n");
    /* CPU features. */
    kpanic_decode_features(rec->cpu.cpu_feat_ecx, rec->cpu.cpu_feat_edx,
                           line, sizeof(line));
    kpanic_serial_puts("FEAT: ");
    kpanic_serial_puts(line);
    kpanic_serial_puts("\n");
    /* Heap grade for OOM, heap lines otherwise already shown. */
    if (rec->domain == KP_DOMAIN_OOM || rec->domain == KP_DOMAIN_HEAP) {
        kpanic_heap_grade(&rec->mem, line, sizeof(line));
        kpanic_serial_puts("GRADE: ");
        kpanic_serial_puts(line);
        kpanic_serial_puts("\n");
    }
    /* Mode budget for VRAM/OOM: which modes fit in free bytes. */
    if (rec->domain == KP_DOMAIN_VRAM || rec->domain == KP_DOMAIN_OOM) {
        kpanic_vram_budget_serial(&rec->mem);
    }
    /* Vector long detail for CPU faults. */
    if (rec->domain == KP_DOMAIN_CPU) {
        char cause[128], code[128], check[128];
        kpanic_vector_detail(rec->code, cause, sizeof(cause),
                             code, sizeof(code), check, sizeof(check));
        kpanic_serial_puts("CAUSE: ");
        kpanic_serial_puts(cause);
        kpanic_serial_puts("\nCODE-MEANING: ");
        kpanic_serial_puts(code);
        kpanic_serial_puts("\nCHECK: ");
        kpanic_serial_puts(check);
        kpanic_serial_puts("\nHINT: ");
        kpanic_serial_puts(kpanic_exception_hint(rec->code));
        kpanic_serial_puts("\n");
        if (rec->code < 32) {
            /* Reserved-vector sanity: these should never fire. */
            if (rec->code == 15 || (rec->code >= 22 && rec->code <= 27) ||
                rec->code == 31) {
                kpanic_serial_puts("NOTE: reserved vector fired; suspect PIC/APIC misroute.\n");
            }
        } else if (rec->code < 48) {
            kpanic_serial_puts("IRQ: ");
            kpanic_serial_puts(kpanic_irq_name(rec->code));
            kpanic_serial_puts("\n");
        }
    }
    /* errno decode for VFS with numeric codes. */
    if (rec->domain == KP_DOMAIN_VFS && rec->code < 512) {
        int ec = (int)rec->code;
        if (ec > 0 && ec < 200) {
            kpanic_serial_puts("ERRNO: ");
            kpanic_serial_puts(kpanic_errno_name(ec));
            kpanic_serial_puts(" - ");
            kpanic_serial_puts(kpanic_errno_fix(ec));
            kpanic_serial_puts("\n");
        }
    }
    /* Stack annotations. */
    {
        char tags[40];
        kpanic_annotate_stack(&rec->trace, tags, sizeof(tags));
        kpanic_serial_puts("TAGS: ");
        kpanic_serial_puts(tags[0] ? tags : "-");
        kpanic_serial_puts(" (C=code S=stack H=heap A=ascii .=other)\n");
    }
    /* Cause-table detail for classified domains. */
    if (rec->domain == KP_DOMAIN_GUI || rec->domain == KP_DOMAIN_VFS ||
        rec->domain == KP_DOMAIN_NET || rec->domain == KP_DOMAIN_SCHED ||
        rec->domain == KP_DOMAIN_DRIVER || rec->domain == KP_DOMAIN_HEAP) {
        kpanic_subsys_code_detail(rec->domain, rec->code, line, sizeof(line));
        kpanic_serial_puts("DETAIL: ");
        kpanic_serial_puts(line);
        kpanic_serial_puts("\n");
    }
}
