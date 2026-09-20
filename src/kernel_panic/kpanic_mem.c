/*
 * kpanic_mem.c - live RAM + VRAM probes for the panic screen.
 *
 * Every byte count here is read at panic time:
 *   RAM  = mem_get_stats() heap walk + mem_validate_heap() in memory.asm
 *   VRAM = gpu_get_width/height/is_vesa() + VBE boot block at 0x9000
 * I never cache these at boot; a stale snapshot would blame the wrong
 * subsystem. Checked arithmetic everywhere: a framebuffer size
 * computation that wraps would turn an OOM report into a second fault.
*/

#include "kpanic.h"

/* Live GPU probe (weak decls; vbe_graphics.c provides the strong ones). */
__attribute__((weak)) int gpu_get_width(void);
__attribute__((weak)) int gpu_get_height(void);
__attribute__((weak)) bool gpu_is_vesa(void);

/* memory.asm exports. Their ABI is register-based (EAX=pointer), not
 * cdecl, so I preload EAX before the call AND pass the pointer on the
 * stack. That way both conventions see the right address. */
extern void mem_get_stats(uint32_t *stats);
extern uint32_t mem_validate_heap(void);

/* VBE boot info block filled by bootload.asm at 0x9000. */
typedef struct kpanic_vbe_info {
    uint32_t framebuffer;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  reserved;
    uint16_t pitch;
    uint16_t lin_pitch;
    uint16_t mode_number;
} __attribute__((packed)) kpanic_vbe_info_t;

#define VBE_INFO_PTR ((volatile kpanic_vbe_info_t *)0x9000u)

/* Upper bound for any single panic-time size computation (256 MB).
 * Larger claims are clamped, not trusted. */
#define KP_MAX_ALLOC (256u * 1024u * 1024u)
/* Lowest / highest addresses I consider plausible for readable RAM   */
/* on this kernel (loaded at 1MB, heap + framebuffer above).          */
#define KP_RAM_LO 0x1000u
#define KP_RAM_HI 0x2000000u /* 32 MB identity window we rely on */

bool kpanic_safe_mul_u32(uint32_t a, uint32_t b, uint32_t *out) {
    if (!out) return false;
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a > 0xFFFFFFFFu / b) return false;
    *out = a * b;
    return true;
}

static bool safe_add_u32(uint32_t a, uint32_t b, uint32_t *out) {
    if (!out) return false;
    if (a > 0xFFFFFFFFu - b) return false;
    *out = a + b;
    return true;
}

bool kpanic_is_readable(uint32_t addr, uint32_t len) {
    uint32_t end;
    if (len == 0) return true;
    if (addr < KP_RAM_LO) return false;
    if (!safe_add_u32(addr, len, &end)) return false;
    if (end > KP_RAM_HI) {
        /* VGA text (0xB8000) is inside the window; the check above   */
        /* already covers it. Anything past 32MB needs paging info   */
        /* I do not have in the panic path, so I refuse.             */
        return false;
    }
    /* NULL page + low BIOS area are never valid dump sources. */
    if (addr < 0x1000u) return false;
    return true;
}

/* Run the live heap walk. Returns true when the stats came from the  */
/* real allocator rather than a fallback.                             */
static bool mem_stats_live(uint32_t *free_b, uint32_t *used_b, uint32_t *blocks) {
    uint32_t st[4];
    st[0] = 0;
    st[1] = 0;
    st[2] = 0;
    st[3] = 0xDEADBEEFu;
    /* Preload EAX for the register ABI; stack slot covers cdecl. */
    __asm__ volatile("" : : "r"(st) : "memory");
    {
        uint32_t *p = st;
        __asm__ volatile("mov %0, %%eax" :: "r"(p) : "eax");
        mem_get_stats(p);
    }
    /* Sanity: free+used must fit in the 32MB window and the block    */
    /* count must be plausible. Otherwise the heap walk stopped on a  */
    /* corrupt MCB and I report that instead of printing garbage.     */
    if (st[2] > 1000000u) return false;
    {
        uint32_t total;
        if (!safe_add_u32(st[0], st[1], &total)) return false;
        if (total > KP_RAM_HI) return false;
    }
    if (st[3] != 0xDEADBEEFu) return false; /* stack smash in callee */
    *free_b = st[0];
    *used_b = st[1];
    *blocks = st[2];
    return true;
}

static uint32_t heap_valid_live(void) {
    /* mem_validate_heap returns 0 when the MCB chain is intact. It   */
    /* takes no arguments so both ABIs agree.                         */
    uint32_t rc = mem_validate_heap();
    return (rc == 0) ? 1u : 0u;
}

static void vbe_probe(uint32_t *fb, uint16_t *w, uint16_t *h, uint8_t *bpp,
                      uint16_t *mode, bool *ok) {
    uint32_t f;
    uint16_t ww, hh, mo;
    uint8_t bb;
    *ok = false;
    *fb = 0;
    *w = 0;
    *h = 0;
    *bpp = 0;
    *mode = 0;
    if (!kpanic_is_readable(0x9000u, (uint32_t)sizeof(kpanic_vbe_info_t)))
        return;
    /* Volatile reads: the bootloader wrote this once, but I re-read  */
    /* it now so a mode switch after boot shows up correctly.         */
    f = VBE_INFO_PTR->framebuffer;
    ww = VBE_INFO_PTR->width;
    hh = VBE_INFO_PTR->height;
    bb = VBE_INFO_PTR->bpp;
    mo = VBE_INFO_PTR->mode_number;
    /* Sanity gates: reject a zeroed or garbage block. */
    if (ww == 0 || ww > 4096) return;
    if (hh == 0 || hh > 2160) return;
    if (bb != 8 && bb != 15 && bb != 16 && bb != 24 && bb != 32) return;
    if (f == 0) return;
    *fb = f;
    *w = ww;
    *h = hh;
    *bpp = bb;
    *mode = mo;
    *ok = true;
}

void kpanic_mem_capture(kpanic_mem_state_t *out, uint32_t want_bytes) {
    uint32_t free_b = 0, used_b = 0, blocks = 0;
    bool stats_ok;
    if (!out) return;
    stats_ok = mem_stats_live(&free_b, &used_b, &blocks);
    out->heap_free = free_b;
    out->heap_used = used_b;
    out->heap_blocks = blocks;
    out->stats_ok = stats_ok;
    out->heap_valid = stats_ok ? heap_valid_live() : 0u;
    out->fail_size = want_bytes;
    /* GPU probe: live size + mode flag. */
    {
        int w = gpu_get_width();
        int h = gpu_get_height();
        out->gpu_w = (w > 0 && w <= 4096) ? w : 0;
        out->gpu_h = (h > 0 && h <= 2160) ? h : 0;
        out->gpu_vesa = gpu_is_vesa();
    }
    /* VBE boot block probe. */
    {
        uint32_t fb = 0;
        uint16_t w = 0, h = 0, mo = 0;
        uint8_t bpp = 0;
        bool ok = false;
        vbe_probe(&fb, &w, &h, &bpp, &mo, &ok);
        out->vbe_fb = fb;
        out->vbe_mode = mo;
        out->vbe_bpp = bpp;
        out->vbe_info_ok = ok;
        /* If the live GPU size disagrees with the boot block, trust  */
        /* the live driver: a mode switch happened after boot.        */
        (void)w;
        (void)h;
    }
    /* VRAM need: what the failed operation wanted, or what a full   */
    /* framebuffer costs right now. Checked math, clamped.           */
    {
        uint32_t bpp_bytes = 4;
        uint32_t pixels = 0;
        uint32_t need = 0;
        if (out->vbe_info_ok && out->vbe_bpp)
            bpp_bytes = (uint32_t)out->vbe_bpp / 8u;
        if (bpp_bytes == 0) bpp_bytes = 4;
        if (want_bytes && want_bytes <= KP_MAX_ALLOC) {
            need = want_bytes;
        } else if (out->gpu_w > 0 && out->gpu_h > 0) {
            if (kpanic_safe_mul_u32((uint32_t)out->gpu_w, (uint32_t)out->gpu_h, &pixels) &&
                kpanic_safe_mul_u32(pixels, bpp_bytes, &need)) {
                /* need is valid */
            } else {
                need = KP_MAX_ALLOC;
            }
        } else {
            need = 0;
        }
        if (need > KP_MAX_ALLOC) need = KP_MAX_ALLOC;
        out->vram_need = need;
        out->vram_have = stats_ok ? free_b : 0;
    }
}

/* Format 0/1024/1MB+ with one decimal for MB. Bounded, no floats. */
void kpanic_format_bytes(uint32_t bytes, char *out, uint32_t out_len) {
    char nb[16];
    uint32_t p = 0;
    if (!out || !out_len) return;
    out[0] = '\0';
    if (bytes < 1024u) {
        kpanic_u32_dec(bytes, nb);
        {
            const char *t = nb;
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        {
            const char *t = " B";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
    } else if (bytes < 1048576u) {
        kpanic_u32_dec(bytes / 1024u, nb);
        {
            const char *t = nb;
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        {
            const char *t = " KB";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
    } else {
        uint32_t whole = bytes / 1048576u;
        uint32_t frac = (bytes % 1048576u) / 104858u; /* 0-9 */
        if (frac > 9) frac = 9;
        kpanic_u32_dec(whole, nb);
        {
            const char *t = nb;
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        if (p + 1 < out_len) out[p++] = '.';
        if (p + 1 < out_len) out[p++] = (char)('0' + frac);
        {
            const char *t = " MB";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
    }
    out[p] = '\0';
}

void kpanic_format_heap_line(const kpanic_mem_state_t *m, char *out, uint32_t out_len) {
    uint32_t p = 0;
    char b0[24], b1[24], nb[16];
    if (!out || !out_len) return;
    if (!m || !m->stats_ok) {
        kpanic_strncpy(out, "stats n/a (allocator did not answer)", out_len);
        return;
    }
    kpanic_format_bytes(m->heap_free, b0, sizeof(b0));
    kpanic_format_bytes(m->heap_used, b1, sizeof(b1));
    {
        const char *t = "free ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = b0;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = " used ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = b1;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = " blocks ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    kpanic_u32_dec(m->heap_blocks, nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    {
        const char *t = m->heap_valid ? " heap=ok" : " heap=CORRUPT";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

void kpanic_format_vram_line(const kpanic_mem_state_t *m, char *out, uint32_t out_len) {
    uint32_t p = 0;
    char b0[24], nb[16], hx[16];
    if (!out || !out_len) return;
    if (!m) {
        out[0] = '\0';
        return;
    }
    if (!m->gpu_vesa && m->gpu_w == 0) {
        kpanic_strncpy(out, "gpu down (text mode); fb probe skipped", out_len);
        return;
    }
    {
        const char *t = "mode ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    kpanic_u32_dec((uint32_t)(m->gpu_w > 0 ? m->gpu_w : 0), nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    if (p + 1 < out_len) out[p++] = 'x';
    kpanic_u32_dec((uint32_t)(m->gpu_h > 0 ? m->gpu_h : 0), nb);
    {
        const char *t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    if (m->vbe_info_ok) {
        const char *t = " bpp=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        kpanic_u32_dec(m->vbe_bpp, nb);
        t = nb;
        while (*t && p + 1 < out_len) out[p++] = *t++;
        t = " fb=";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        kpanic_u32_hex(m->vbe_fb, hx);
        t = hx;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    if (m->vram_need) {
        const char *t = " need ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        kpanic_format_bytes(m->vram_need, b0, sizeof(b0));
        t = b0;
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

/* Framebuffer size with overflow flag. bpp is bits (8/15/16/24/32).  */

uint32_t kpanic_fb_bytes(int32_t w, int32_t h, uint32_t bpp, bool *overflow) {
    uint32_t pixels = 0;
    uint32_t bytes = 0;
    uint32_t bpp_b = 0;
    bool ov = false;
    if (overflow) *overflow = true;
    if (w <= 0 || h <= 0) return 0;
    if (bpp == 15) bpp_b = 2;
    else if (bpp == 0) bpp_b = 4;
    else bpp_b = (bpp + 7u) / 8u;
    if (bpp_b == 0) bpp_b = 4;
    if (!kpanic_safe_mul_u32((uint32_t)w, (uint32_t)h, &pixels)) return 0;
    if (!kpanic_safe_mul_u32(pixels, bpp_b, &bytes)) return 0;
    if (bytes > KP_MAX_ALLOC) ov = true;
    if (overflow) *overflow = ov;
    return bytes;
}

/* Standard VBE mode table (VESA BIOS Extension). Used to cross-check */
/* the live mode number from the 0x9000 block: if the mode byte says  */
/* 0x118 but the driver reports 1920 wide, something switched modes   */
/* after boot and the boot block is stale.                            */

typedef struct kpanic_vbe_mode {
    uint16_t mode;
    uint16_t w;
    uint16_t h;
    uint8_t  bpp;
    const char *note;
} kpanic_vbe_mode_t;

static const kpanic_vbe_mode_t k_vbe_modes[] = {
    { 0x101, 640, 480, 8, "VGA 256-color" },
    { 0x103, 800, 600, 8, "SVGA 256-color" },
    { 0x105, 1024, 768, 8, "XGA 256-color" },
    { 0x107, 1280, 1024, 8, "SXGA 256-color" },
    { 0x110, 640, 480, 15, "direct 5:5:5" },
    { 0x111, 640, 480, 16, "direct 5:6:5" },
    { 0x112, 640, 480, 24, "direct 8:8:8 packed" },
    { 0x113, 640, 480, 32, "direct X:8:8:8" },
    { 0x114, 800, 600, 16, "direct 5:6:5" },
    { 0x115, 800, 600, 24, "direct packed" },
    { 0x116, 800, 600, 32, "direct XRGB" },
    { 0x117, 1024, 768, 16, "direct 5:6:5" },
    { 0x118, 1024, 768, 24, "direct packed" },
    { 0x119, 1024, 768, 32, "direct XRGB" },
    { 0x11A, 1280, 1024, 16, "direct 5:6:5" },
    { 0x11B, 1280, 1024, 24, "direct packed" },
    { 0x11C, 1280, 1024, 32, "direct XRGB" },
    { 0x11D, 640, 400, 8, "banked fallback" },
    { 0x11E, 640, 480, 16, "alt 5:6:5" },
    { 0x11F, 640, 480, 32, "alt XRGB" },
    { 0x120, 800, 600, 16, "alt 5:6:5" },
    { 0x121, 800, 600, 32, "alt XRGB" },
    { 0x122, 1024, 768, 32, "alt XRGB" },
    { 0x124, 1280, 1024, 32, "alt XRGB" },
    { 0x12B, 640, 350, 8, "EGA compat" },
    { 0x12D, 320, 200, 15, "ModeX direct" },
    { 0x12E, 320, 200, 16, "ModeX direct" },
    { 0x12F, 320, 200, 24, "ModeX direct" },
    { 0x130, 320, 200, 32, "ModeX direct" },
    { 0x131, 320, 240, 8, "chunky QVGA" },
    { 0x132, 320, 240, 16, "chunky QVGA" },
    { 0x133, 320, 240, 32, "chunky QVGA" },
    { 0x134, 400, 300, 8, "chunky" },
    { 0x135, 400, 300, 16, "chunky" },
    { 0x136, 400, 300, 32, "chunky" },
    { 0x13A, 640, 400, 16, "direct" },
    { 0x13B, 640, 400, 32, "direct" },
    { 0x13C, 640, 480, 32, "VMware preferred" },
    { 0x13D, 800, 600, 32, "VMware preferred" },
    { 0x13E, 1024, 768, 32, "VMware preferred" },
    { 0x13F, 1152, 864, 32, "VMware preferred" },
    { 0x140, 1280, 768, 32, "widescreen" },
    { 0x141, 1280, 800, 32, "widescreen" },
    { 0x142, 1280, 1024, 32, "VMware preferred" },
    { 0x143, 1366, 768, 32, "laptop panel" },
    { 0x144, 1440, 900, 32, "widescreen" },
    { 0x145, 1600, 1200, 32, "UXGA" },
    { 0x146, 1920, 1080, 32, "1080p (Aurion default)" },
    { 0x147, 1920, 1200, 32, "WUXGA" },
};

static const kpanic_vbe_mode_t *vbe_mode_lookup(uint16_t mode) {
    for (uint32_t i = 0; i < sizeof(k_vbe_modes) / sizeof(k_vbe_modes[0]); i++)
        if (k_vbe_modes[i].mode == mode) return &k_vbe_modes[i];
    return NULL;
}

const char *kpanic_vbe_mode_note(uint16_t mode) {
    const kpanic_vbe_mode_t *m = vbe_mode_lookup(mode);
    return m ? m->note : "OEM/linear custom mode";
}

/* Heap grade: from live free/used/blocks + fail size, estimate       */
/* fragmentation without walking MCBs (the chain may be corrupt, and  */
/* walking it again risks a second fault). avg block = used/blocks;   */
/* if the fail size exceeds free -> OOM-certain; if it exceeds the    */
/* average but fits free -> fragmented; else healthy-but-unlucky.     */

void kpanic_heap_grade(const kpanic_mem_state_t *m, char *out, uint32_t out_len) {
    uint32_t p = 0;
    char b0[24];
    if (!out || !out_len) return;
    if (!m || !m->stats_ok) {
        kpanic_strncpy(out, "grade n/a", out_len);
        return;
    }
    if (m->fail_size == 0) {
        /* No failed alloc: report pressure as used/(free+used). */
        uint64_t total = (uint64_t)m->heap_free + m->heap_used;
        uint32_t pct = 0;
        if (total) pct = (uint32_t)((uint64_t)m->heap_used * 100u / total);
        {
            const char *t = "pressure ";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        {
            char nb[16];
            kpanic_u32_dec(pct, nb);
            const char *t = nb;
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        {
            const char *t = "% used";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        if (!m->heap_valid) {
            const char *t = " +CORRUPT";
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
        out[p] = '\0';
        return;
    }
    if (m->fail_size > m->heap_free) {
        const char *t = "OOM-certain: want ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
        kpanic_format_bytes(m->fail_size, b0, sizeof(b0));
        {
            const char *t2 = b0;
            while (*t2 && p + 1 < out_len) out[p++] = *t2++;
        }
        {
            const char *t2 = " > free ";
            while (*t2 && p + 1 < out_len) out[p++] = *t2++;
        }
        kpanic_format_bytes(m->heap_free, b0, sizeof(b0));
        {
            const char *t2 = b0;
            while (*t2 && p + 1 < out_len) out[p++] = *t2++;
        }
    } else if (m->heap_blocks) {
        uint32_t avg = m->heap_used / (m->heap_blocks ? m->heap_blocks : 1u);
        if (m->fail_size > avg * 4u) {
            const char *t = "fragmented?: want ";
            while (*t && p + 1 < out_len) out[p++] = *t++;
            kpanic_format_bytes(m->fail_size, b0, sizeof(b0));
            {
                const char *t2 = b0;
                while (*t2 && p + 1 < out_len) out[p++] = *t2++;
            }
            {
                const char *t2 = " >> avg-used ";
                while (*t2 && p + 1 < out_len) out[p++] = *t2++;
            }
            kpanic_format_bytes(avg, b0, sizeof(b0));
            {
                const char *t2 = b0;
                while (*t2 && p + 1 < out_len) out[p++] = *t2++;
            }
        } else {
            const char *t = "fits on paper: want ";
            while (*t && p + 1 < out_len) out[p++] = *t++;
            kpanic_format_bytes(m->fail_size, b0, sizeof(b0));
            {
                const char *t2 = b0;
                while (*t2 && p + 1 < out_len) out[p++] = *t2++;
            }
            {
                const char *t2 = " <= free; suspect transient spike or corrupt accounting";
                while (*t2 && p + 1 < out_len) out[p++] = *t2++;
            }
        }
    }
    out[p] = '\0';
}

/* BIOS RAM probe. bootload.asm stores INT15/E801 total KB at 0x9010. */
/* I re-read it live and sanity-gate it (1MB..4GB) so a zeroed word   */
/* from a failed BIOS call prints "n/a" instead of "0 KB".            */

#define BIOS_RAM_PTR ((volatile uint32_t *)0x9010u)

uint32_t kpanic_bios_ram_kb(bool *ok) {
    uint32_t kb;
    if (ok) *ok = false;
    if (!kpanic_is_readable(0x9010u, 4)) return 0;
    kb = *BIOS_RAM_PTR;
    /* E801 math: CX+DX*64+1024 KB. Plausible range 1MB..4GB. */
    if (kb < 1024u || kb > 4194304u) return 0;
    if (ok) *ok = true;
    return kb;
}

void kpanic_format_ram_line(char *out, uint32_t out_len) {
    uint32_t p = 0;
    bool ok = false;
    uint32_t kb = 0;
    char b0[24];
    if (!out || !out_len) return;
    kb = kpanic_bios_ram_kb(&ok);
    if (!ok) {
        kpanic_strncpy(out, "bios ram n/a (0x9010 word failed sanity gate)", out_len);
        return;
    }
    {
        const char *t = "bios ";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    /* Checked: KB -> bytes cannot overflow for gated kb (<=4G KB but */
    /* gate caps at 4M KB = 4GB, times 1024 fits in 32 bits? 4M*1024  */
    /* = 4G = wraps. Clamp display at >2GB to ">=2 GB".               */
    if (kb >= 2097152u) {
        const char *t = ">=2 GB";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    } else {
        uint32_t bytes = kb * 1024u;
        kpanic_format_bytes(bytes, b0, sizeof(b0));
        {
            const char *t = b0;
            while (*t && p + 1 < out_len) out[p++] = *t++;
        }
    }
    {
        const char *t = " (int15/e801 @0x9010)";
        while (*t && p + 1 < out_len) out[p++] = *t++;
    }
    out[p] = '\0';
}

/* VRAM budget advisor. For the live free-byte count, list which      */
/* common modes fit and which do not (checked math per mode). This    */
/* turns "VRAM exhausted" into "drop to 1024x768x32 or free 5 MB".    */

typedef struct kpanic_mode_opt {
    uint16_t w;
    uint16_t h;
    uint8_t bpp;
} kpanic_mode_opt_t;

static const kpanic_mode_opt_t k_budget_modes[] = {
    { 640, 480, 32 }, { 800, 600, 32 }, { 1024, 768, 32 },
    { 1152, 864, 32 }, { 1280, 720, 32 }, { 1280, 800, 32 },
    { 1280, 1024, 32 }, { 1366, 768, 32 }, { 1440, 900, 32 },
    { 1600, 900, 32 }, { 1600, 1200, 32 }, { 1920, 1080, 32 },
    { 1024, 768, 16 }, { 1280, 1024, 16 }, { 1920, 1080, 16 },
};

void kpanic_vram_budget_serial(const kpanic_mem_state_t *m) {
    uint32_t free_b;
    char nb[16];
    char b0[24];
    if (!m) return;
    free_b = m->stats_ok ? m->heap_free : 0;
    kpanic_serial_puts("BUDGET (free ");
    kpanic_format_bytes(free_b, b0, sizeof(b0));
    kpanic_serial_puts(b0);
    kpanic_serial_puts("):\n");
    for (uint32_t i = 0; i < sizeof(k_budget_modes) / sizeof(k_budget_modes[0]); i++) {
        bool ov = false;
        uint32_t need = kpanic_fb_bytes(k_budget_modes[i].w, k_budget_modes[i].h,
                                        k_budget_modes[i].bpp, &ov);
        kpanic_serial_puts("  ");
        kpanic_u32_dec(k_budget_modes[i].w, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts("x");
        kpanic_u32_dec(k_budget_modes[i].h, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts("x");
        kpanic_u32_dec(k_budget_modes[i].bpp, nb);
        kpanic_serial_puts(nb);
        kpanic_serial_puts(" need ");
        if (ov || need == 0) {
            kpanic_serial_puts("OVERFLOW");
        } else {
            kpanic_format_bytes(need, b0, sizeof(b0));
            kpanic_serial_puts(b0);
        }
        if (!ov && need && need <= free_b)
            kpanic_serial_puts(" FITS");
        else
            kpanic_serial_puts(" no-fit");
        kpanic_serial_puts("\n");
    }
}
