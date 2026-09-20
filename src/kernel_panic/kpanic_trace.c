/*
 * kpanic_trace.c - real backtrace from the live EBP chain.
 *
 * I walk [EBP] -> prev EBP / [EBP+4] -> return EIP exactly like the
 * CPU does on `leave; ret`. Every frame passes a sanity gate (higher
 * address, in readable RAM, aligned, bounded steps) before I trust
 * it. Corrupt chains terminate with "n/a" instead of page-faulting
 * inside the dumper. Stack + code bytes are read from live memory
 * with the same readability gate.
*/

#include "kpanic.h"

uint32_t kpanic_read_ebp(void) {
    uint32_t v;
    __asm__ volatile("mov %%ebp, %0" : "=r"(v));
    return v;
}

uint32_t kpanic_read_esp(void) {
    uint32_t v;
    __asm__ volatile("mov %%esp, %0" : "=r"(v));
    return v;
}

bool kpanic_frame_sane(uint32_t ebp, uint32_t prev_ebp) {
    /* Must be 4-aligned: the compiler only emits aligned frames. */
    if (ebp & 0x03u) return false;
    /* Must move up the stack (higher addresses). Equal or lower   */
    /* means a loop or garbage. The first frame has prev=0.        */
    if (prev_ebp && ebp <= prev_ebp) return false;
    /* Must be readable for at least the two words I dereference.  */
    if (!kpanic_is_readable(ebp, 8)) return false;
    /* Keep the walk inside the RAM window; wild EBP from a smashed */
    /* stack would otherwise wander into MMIO.                      */
    if (ebp < 0x10000u) return false;
    return true;
}

static uint32_t frame_prev_ebp(uint32_t ebp) {
    return *(volatile uint32_t *)ebp;
}

static uint32_t frame_ret_eip(uint32_t ebp) {
    return *(volatile uint32_t *)(ebp + 4);
}

static bool eip_plausible(uint32_t eip) {
    /* Kernel text lives at 1MB+ (loaded there by the boot sector).   */
    /* Anything below is BIOS/NULL and anything absurdly              */
    /* high is a garbage word off the stack.                          */
    if (eip < 0x1000u) return false;
    if (eip > 0x2000000u && (eip < 0xC0000000u || eip > 0xC1000000u))
        return false;
    return true;
}

void kpanic_trace_capture(kpanic_trace_state_t *out, uint32_t ebp,
                          uint32_t esp, uint32_t eip) {
    uint32_t prev = 0;
    uint32_t n = 0;
    if (!out) return;
    for (uint32_t i = 0; i < KP_MAX_FRAMES; i++) {
        out->frames[i].ebp = 0;
        out->frames[i].eip = 0;
        out->frames[i].eip_readable = false;
    }
    for (uint32_t i = 0; i < KP_STACK_WORDS; i++) out->stack_dump[i] = 0;
    for (uint32_t i = 0; i < sizeof(out->code_bytes); i++) out->code_bytes[i] = 0;
    out->frame_count = 0;
    out->stack_base = esp;
    out->stack_words = 0;
    out->code_base = eip;
    out->code_len = 0;
    out->ebp_valid = false;

    /* Frame 0 is the faulting EIP when we have an ISR frame. It is   */
    /* real even when the EBP chain is already smashed.               */
    if (eip && eip_plausible(eip) && n < KP_MAX_FRAMES) {
        out->frames[n].ebp = ebp;
        out->frames[n].eip = eip;
        out->frames[n].eip_readable = true;
        n++;
    }

    /* Walk the chain. Cap at KP_MAX_FRAMES so a loop cannot wedge us. */
    {
        uint32_t cur = ebp;
        uint32_t steps = 0;
        /* First-frame gate: if the incoming EBP itself is insane,    */
        /* report it and stop instead of dereferencing it.            */
        if (cur == 0) {
            out->ebp_valid = false;
        } else if (!kpanic_frame_sane(cur, 0)) {
            out->ebp_valid = false;
        } else {
            out->ebp_valid = true;
        }
        while (cur && steps < KP_MAX_FRAMES && n < KP_MAX_FRAMES) {
            uint32_t next_ebp;
            uint32_t ret;
            steps++;
            if (!kpanic_frame_sane(cur, prev)) break;
            /* Read the two words. They are inside readable RAM per   */
            /* the gate above, so no fault can escape here.           */
            next_ebp = frame_prev_ebp(cur);
            ret = frame_ret_eip(cur);
            /* Skip the duplicate when frame 0 already logged EIP and */
            /* the first chain entry points at the same return.       */
            if (!(n == 1 && ret == eip)) {
                out->frames[n].ebp = cur;
                out->frames[n].eip = ret;
                out->frames[n].eip_readable = eip_plausible(ret);
                n++;
            }
            /* Termination: NULL ends the chain; non-sane ends it.    */
            if (next_ebp == 0) break;
            if (next_ebp == cur) break; /* cycle */
            if (!kpanic_frame_sane(next_ebp, cur)) break;
            /* Bound the stride: a frame more than 64KB above the     */
            /* previous one is a smashed word, not a real frame.      */
            if (next_ebp - cur > 0x10000u) break;
            prev = cur;
            cur = next_ebp;
        }
    }
    out->frame_count = n;

    /* Live stack dump: 32 dwords from ESP, word by word so a partially */
    /* mapped tail still yields its readable prefix.                    */
    if (esp && !(esp & 0x03u)) {
        uint32_t got = 0;
        for (uint32_t i = 0; i < KP_STACK_WORDS; i++) {
            uint32_t addr;
            /* Checked add: ESP near 0xFFFFFFFF must not wrap. */
            if (esp > 0xFFFFFFFFu - i * 4u) break;
            addr = esp + i * 4u;
            if (!kpanic_is_readable(addr, 4)) break;
            out->stack_dump[i] = *(volatile uint32_t *)addr;
            got++;
        }
        out->stack_words = got;
    }

    /* Code bytes around EIP: 16 before + 16 after when readable. This */
    /* shows the faulting instruction without a disassembler.         */
    if (eip && eip_plausible(eip)) {
        uint32_t base;
        uint32_t want = 32;
        /* Prefer centered: EIP-16, clamped so base never wraps. */
        if (eip >= 16) base = eip - 16;
        else base = eip;
        if (kpanic_is_readable(base, want)) {
            volatile uint8_t *p = (volatile uint8_t *)base;
            for (uint32_t i = 0; i < want; i++)
                out->code_bytes[i] = p[i];
            out->code_base = base;
            out->code_len = want;
        } else if (kpanic_is_readable(eip, 8)) {
            volatile uint8_t *p = (volatile uint8_t *)eip;
            uint32_t got = 0;
            for (uint32_t i = 0; i < 8; i++) {
                if (!kpanic_is_readable(eip + i, 1)) break;
                out->code_bytes[i] = p[i];
                got++;
            }
            out->code_base = eip;
            out->code_len = got;
        }
    }
}

/* Address region classifier. Names where a faulting pointer lives so */
/* the dump reads "heap?" "stack?" "VGA?" instead of a bare hex value.*/
/* Ranges are the AurionOS 32-bit memory map (see link.ld + boot).    */

const char *kpanic_addr_region(uint32_t addr) {
    if (addr < 0x1000u) return "NULL-page";
    if (addr < 0x7C00u) return "low-BIOS";
    if (addr < 0x7E00u) return "boot-sector";
    if (addr < 0x9000u) return "low-mem";
    if (addr < 0xA000u) return "boot-info";
    if (addr < 0x10000u) return "BIOS-area";
    if (addr < 0x80000u) return "kernel-text?";
    if (addr < 0xA0000u) return "kernel/heap?";
    if (addr < 0xB8000u) return "VGA-gfx-mem";
    if (addr < 0xB9000u) return "VGA-text";
    if (addr < 0xC0000u) return "VGA-bios";
    if (addr < 0x100000u) return "BIOS-ROM";
    if (addr < 0x2000000u) return "heap/stack?";
    if (addr >= 0xE0000000u) return "MMIO/high?";
    return "unknown-RAM?";
}

/* x86 instruction-length decoder (32-bit). I use it to place a '>'   */
/* marker on the faulting instruction inside the code dump. Lengths   */
/* come from the Intel opcode map: prefixes, 0x0F escape, ModRM/SIB,  */
/* displacement, immediate. Unknown/suspicious encodings return 1 so  */
/* the marker loop always terminates.                                 */

static bool is_prefix(uint8_t b) {
    switch (b) {
    case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0xF0: case 0xF2: case 0xF3:
        return true;
    default:
        return false;
    }
}

static bool opcode_has_modrm(uint8_t op) {
    /* 0x00-0x3F group minus a few; 0x60-0x6F partially; 0x80-0x8F all;
     * 0xC0-0xC7, 0xD0-0xD3, 0xF6, 0xF7, 0xFE, 0xFF. Conservative: any
     * byte in these bands is treated as ModRM-present. */
    if (op <= 0x3Fu) {
        if (op == 0x06 || op == 0x07 || op == 0x0E || op == 0x16 ||
            op == 0x17 || op == 0x1E || op == 0x1F || op == 0x27 ||
            op == 0x2F || op == 0x37 || op == 0x3F) return false;
        return true;
    }
    if (op >= 0x60u && op <= 0x6Fu) {
        if (op == 0x60 || op == 0x61 || op == 0x68 || op == 0x69 ||
            op == 0x6A || op == 0x6B || op == 0x6C || op == 0x6D ||
            op == 0x6E || op == 0x6F) return false;
        return true;
    }
    if (op >= 0x80u && op <= 0x8Fu) return true;
    if (op >= 0xC0u && op <= 0xC7u) return true;
    if (op >= 0xD0u && op <= 0xD3u) return true;
    if (op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) return true;
    return false;
}

static uint32_t modrm_extra(uint8_t modrm, bool addr16, uint8_t op, uint8_t op2,
                            bool is_0f) {
    uint32_t mod = (modrm >> 6) & 0x3u;
    uint32_t rm = modrm & 0x7u;
    uint32_t n = 0;
    (void)op;
    (void)op2;
    (void)is_0f;
    if (mod == 3) return 0; /* register-direct: no memory bytes */
    if (addr16) {
        if (mod == 0 && rm == 6) n += 2;
        else if (mod == 1) n += 1;
        else if (mod == 2) n += 2;
        return n;
    }
    if (rm == 4) {
        /* SIB byte present. Read base/index from the SIB when we have */
        /* it; the caller guarantees avail covers what we touch.       */
        n += 1; /* SIB itself */
    }
    if (mod == 0 && rm == 5) n += 4;
    else if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

uint32_t kpanic_insn_len(const uint8_t *code, uint32_t avail) {
    uint32_t i = 0;
    bool addr16 = false;
    bool op16 = false;
    uint8_t op;
    if (!code || !avail) return 0;
    /* Prefixes: max 4 honored (Intel limit is 4 groups + REX). */
    {
        uint32_t npfx = 0;
        while (i < avail && is_prefix(code[i]) && npfx < 4) {
            if (code[i] == 0x67) addr16 = true;
            if (code[i] == 0x66) op16 = true;
            i++;
            npfx++;
        }
    }
    if (i >= avail) return i ? i : 1;
    op = code[i++];
    /* Two-byte escape. */
    if (op == 0x0F) {
        uint8_t op2;
        if (i >= avail) return i;
        op2 = code[i++];
        /* 0F 38 / 0F 3A three-byte opcodes. */
        if ((op2 == 0x38 || op2 == 0x3A) && i < avail) i++;
        if (i >= avail) return i;
        /* Most 0F opcodes take ModRM. */
        if (op2 >= 0x10 && op2 != 0xA8 && op2 != 0xA9) {
            if (i >= avail) return i;
            {
                uint8_t md = code[i++];
                uint32_t extra = 0;
                /* SIB peek for disp calc. */
                if ((((md >> 6) & 3) != 3) && ((md & 7) == 4) && !addr16) {
                    if (i < avail) {
                        uint8_t sib = code[i];
                        uint32_t base = sib & 7;
                        uint32_t mod = (md >> 6) & 3;
                        (void)base;
                        if (mod == 0 && base == 5) extra += 4;
                        else if (mod == 1) extra += 1;
                        else if (mod == 2) extra += 4;
                    }
                    i++; /* SIB */
                } else {
                    extra = modrm_extra(md, addr16, op, op2, true);
                }
                i += extra;
            }
        }
        /* Jcc near (0F 80-8F) has 4/2-byte displacement. */
        if (op2 >= 0x80 && op2 <= 0x8F) i += op16 ? 2 : 4;
        if (i > avail) i = avail;
        return i ? i : 1;
    }
    /* One-byte opcodes with immediates. */
    switch (op) {
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C:
        return i + 1 <= avail ? i + 1 : avail; /* AL, imm8 */
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D:
        i += op16 ? 2 : 4; /* EAX, imm */
        return i <= avail ? i : avail;
    case 0x68:
        i += op16 ? 2 : 4; /* push imm */
        return i <= avail ? i : avail;
    case 0x6A: case 0x70: case 0x71:
    case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79:
    case 0x7A: case 0x7B: case 0x7C: case 0x7D:
    case 0x7E: case 0x7F: case 0xB0:
    case 0xB1: case 0xB2: case 0xB3: case 0xB4:
    case 0xB5: case 0xB6: case 0xB7: case 0xCD:
    case 0xD4: case 0xD5: case 0xE0: case 0xE1:
    case 0xE2: case 0xE3: case 0xEB:
        return i + 1 <= avail ? i + 1 : avail; /* imm8 forms */
    case 0x69: /* imul r, r/m, imm16/32 */
        if (i < avail) {
            uint8_t md = code[i++];
            i += modrm_extra(md, addr16, op, 0, false);
            i += op16 ? 2 : 4;
        }
        return i <= avail ? i : avail;
    case 0x6B: /* imul r, r/m, imm8 (also 0x6B above? kept explicit) */
        if (i < avail) {
            uint8_t md = code[i++];
            i += modrm_extra(md, addr16, op, 0, false);
            i += 1;
        }
        return i <= avail ? i : avail;
    case 0x80: case 0x82: case 0x83: case 0xC0:
    case 0xC1: case 0xC6: case 0xF6:
        if (i < avail) {
            uint8_t md = code[i++];
            i += modrm_extra(md, addr16, op, 0, false);
            if (op == 0xC6 || op == 0xF6) {
                /* /0 of C6/F6 carries imm8; over-approx as 1. */
                i += 1;
            } else {
                i += 1;
            }
        }
        return i <= avail ? i : avail;
    case 0x81: case 0xC7:
        if (i < avail) {
            uint8_t md = code[i++];
            i += modrm_extra(md, addr16, op, 0, false);
            i += op16 ? 2 : 4;
        }
        return i <= avail ? i : avail;
    case 0x8D: /* lea */
    case 0x8E: case 0x8F:
    case 0xC4: case 0xC5: /* les/lds */
        if (i < avail) {
            uint8_t md = code[i++];
            i += modrm_extra(md, addr16, op, 0, false);
        }
        return i <= avail ? i : avail;
    case 0x9A: /* call ptr16:32 */
        i += op16 ? 4 : 6;
        return i <= avail ? i : avail;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        i += addr16 ? 2 : 4; /* moffs */
        return i <= avail ? i : avail;
    case 0xA8:
        return i + 1 <= avail ? i + 1 : avail;
    case 0xA9:
        i += op16 ? 2 : 4;
        return i <= avail ? i : avail;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        i += op16 ? 2 : 4; /* mov r, imm */
        return i <= avail ? i : avail;
    case 0xC2: case 0xCA:
        i += 2; /* ret imm16 */
        return i <= avail ? i : avail;
    case 0xC8:
        i += 3; /* enter imm16,imm8 */
        return i <= avail ? i : avail;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        return i + 1 <= avail ? i + 1 : avail; /* in/out imm8 */
    case 0xE8: case 0xE9:
        i += op16 ? 2 : 4; /* call/jmp rel */
        return i <= avail ? i : avail;
    case 0xF7:
        if (i < avail) {
            uint8_t md = code[i++];
            uint32_t reg = (md >> 3) & 7;
            i += modrm_extra(md, addr16, op, 0, false);
            if (reg == 0 || reg == 1) i += op16 ? 2 : 4; /* test/div imm */
        }
        return i <= avail ? i : avail;
    default:
        break;
    }
    /* Generic ModRM-bearing opcode? */
    if (opcode_has_modrm(op) && i < avail) {
        uint8_t md = code[i++];
        i += modrm_extra(md, addr16, op, 0, false);
        return i <= avail ? i : avail;
    }
    return i ? i : 1;
}

/* Stack-word annotator. Each dumped dword gets a one-glyph tag:      */
/*   C = plausible code pointer (in kernel-text range)                */
/*   S = plausible stack pointer (near ESP, higher addresses)         */
/*   H = plausible heap pointer (low RAM, aligned)                    */
/*   A = printable ASCII run (4 chars)                                */
/*   . = none of the above                                            */
/* All predicates use live ESP + the same readability gate as the     */
/* dump, so annotations never fault.                                  */

static bool word_is_ascii(uint32_t w) {
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)(w >> (i * 8));
        if (c < 0x20u || c > 0x7Eu) {
            if (c != 0) return false;
        }
    }
    return true;
}

char kpanic_annotate_word(uint32_t word, uint32_t esp) {
    /* Code pointer: kernel text window, 4-aligned. The readability  */
    /* gate runs on the page so a wild value never faults here.      */
    if (word >= 0x10000u && word < 0x2000000u && !(word & 0x03u)) {
        if (kpanic_is_readable(word & ~0xFFFu, 0x1000u)) return 'C';
        return '.';
    }
    /* Stack pointer: within 1MB above ESP (stacks grow down, frames */
    /* point up). Checked math so ESP near the top cannot wrap.      */
    if (esp && word >= esp && word <= esp + 0x100000u && !(word & 0x03u))
        return 'S';
    /* Heap pointer: low RAM, aligned, not NULL page. */
    if (word >= 0x10000u && word < 0xA0000u && !(word & 0x03u)) return 'H';
    if (word >= 0x100000u && word < 0x1000000u && !(word & 0x03u)) return 'H';
    /* ASCII: four printable bytes (a message fragment on the stack). */
    if (word_is_ascii(word)) return 'A';
    return '.';
}

void kpanic_annotate_stack(const kpanic_trace_state_t *t, char *out, uint32_t out_len) {
    uint32_t p = 0;
    if (!t || !out || !out_len) return;
    for (uint32_t i = 0; i < t->stack_words && p + 1 < out_len; i++)
        out[p++] = kpanic_annotate_word(t->stack_dump[i], t->stack_base);
    out[p] = '\0';
}

/* Code marker: second serial line under the CODE dump with '^^' at   */
/* the faulting byte. The capture centers EIP at code_base+16 when it */
/* got the full 32 bytes; short reads start at EIP (index 0). The     */
/* prefix "CODE @0x........: " is 19 chars; each byte takes 3 cols.   */

void kpanic_code_marker_serial(const kpanic_trace_state_t *t) {
    uint32_t mark;
    uint32_t i;
    if (!t || !t->code_len) return;
    mark = (t->code_len == 32) ? 16 : 0;
    if (mark >= t->code_len) mark = 0;
    kpanic_serial_puts("                   ");
    for (i = 0; i < mark; i++)
        kpanic_serial_puts("   ");
    kpanic_serial_puts("^^ fault");
    if (t->code_len == 32) {
        kpanic_serial_puts(" (EIP=base+16)");
    } else {
        kpanic_serial_puts(" (EIP=base, short read)");
    }
    kpanic_serial_puts("\n");
}
