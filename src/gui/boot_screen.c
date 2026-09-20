// src/boot_screen.c
/*
 * Amber-phosphor splash, final: CRT power-on sweep, staggered fades,
 * ink-centered ring monogram, 12-dot spinner with trail, segmented bar with
 * right-aligned percent and fixed-anchor status line, gap-free annulus ring,
 * 1-alpha-step center lift, row/col vignette, fade-to-black handoff.
 * 200 ticks at 100 Hz PIT. Integer math only, no per-frame allocation.
*/

#include "boot_screen.h"
#include <stdint.h>
#include <stdbool.h>

extern int gpu_get_width(void);
extern int gpu_get_height(void);
extern void gpu_fill_rect(int x, int y, int w, int h, uint32_t color);
extern void gpu_fill_rect_blend(int x, int y, int w, int h, uint32_t c, uint8_t a);
extern void gpu_draw_pixel(int x, int y, uint32_t color);
extern int gpu_flush(void);
extern uint32_t get_ticks(void);
extern void io_wait(void);
extern int load_file_content(const char *filename, char *buffer, int max_len);
extern void *kmalloc(uint32_t size);
extern void audio_boot_jingle_start(void);
extern void audio_boot_jingle_poll(void);
extern void audio_boot_jingle_stop(void);
extern void c_puts(const char *);
extern void c_putc(char);

extern const uint8_t font_8x8[128][8];

/* Warm phosphor palette: charcoal glass, amber beam, bone text.
 * No blue channel above 0x4E anywhere.
*/
#define BG_COLOR        0xFF0B0908
#define WARM_WHITE      0xFFF4EEE2
#define SUB_COLOR       0xFFDDD4C2
#define TEXT_MID        0xFFC9C0B0
#define STATUS_COLOR    0xFFB4AA96
#define RULE_COLOR      0xFF3A332A
#define TRACK_COLOR     0xFF221D17
#define RING_COLOR      0xFF6E6659
#define AMBER           0xFFFFB000
#define AMBER_HI        0xFFFFC94E
#define AMBER_LO        0xFFFF8C1A
#define SHADOW_COLOR    0xFF080605
#define LIFT_COLOR      0xFF241A10

#define BOOT_TICKS          200
#define FADEOUT_TICK        192
#define FRAME_DELAY_ITERS   120

#define FONT_W 8
#define LOGO_MAX_BYTES 32768
#define LOGO_MAX_DIM   256
#define LOGO_DRAW_SIZE 56
#define RING_RADIUS    28
#define RING_THICK     2
#define SPINNER_POINTS 12

#define BAR_SEGS      24
#define BAR_SEG_W     10
#define BAR_SEG_GAP   3
#define BAR_W         (BAR_SEGS * (BAR_SEG_W + BAR_SEG_GAP) - BAR_SEG_GAP)
#define RULE_W        360

/* Vertical rhythm, one place to tune. */
#define L_LOGO_H        56
#define L_GAP_LOGO_T    22
#define L_TITLE_H       32
#define L_GAP_TITLE_S   14
#define L_SUB_H         16
#define L_GAP_SUB_RULE  20
#define L_RULE_H        2
#define L_GAP_RULE_SPIN 30
#define L_SPIN_D        52
#define L_GAP_SPIN_LOAD 26
#define L_LOAD_H        8
#define L_GAP_LOAD_STAT 10
#define L_STATUS_H      16

/* Status/percent drop to scale 1 below this width so nothing overflows. */
#define WIDE_SCREEN     700

static int boot_strlen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void boot_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++))
        ;
}

static int fmt_uint(char *buf, int v)
{
    char rev[12];
    int k = 0, n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v > 0 && k < 11) { rev[k++] = (char)('0' + v % 10); v /= 10; }
    while (k > 0) buf[n++] = rev[--k];
    buf[n] = 0;
    return n;
}

static void draw_pixel_safe(int x, int y, uint32_t color)
{
    int sw = gpu_get_width();
    int sh = gpu_get_height();
    if (x < 0 || y < 0 || x >= sw || y >= sh) return;
    gpu_draw_pixel(x, y, color);
}

static uint32_t dim_color(uint32_t c, uint8_t brightness)
{
    uint32_t r = ((c >> 16) & 0xFF) * brightness / 255;
    uint32_t g = ((c >> 8) & 0xFF) * brightness / 255;
    uint32_t b = (c & 0xFF) * brightness / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static uint32_t lerp_color(uint32_t c1, uint32_t c2, uint8_t t)
{
    int r1 = (int)((c1 >> 16) & 0xFF), g1 = (int)((c1 >> 8) & 0xFF), b1 = (int)(c1 & 0xFF);
    int r2 = (int)((c2 >> 16) & 0xFF), g2 = (int)((c2 >> 8) & 0xFF), b2 = (int)(c2 & 0xFF);
    uint32_t r = (uint32_t)(r1 + ((r2 - r1) * t) / 255);
    uint32_t g = (uint32_t)(g1 + ((g2 - g1) * t) / 255);
    uint32_t b = (uint32_t)(b1 + ((b2 - b1) * t) / 255);
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static uint8_t ease_out_quad(uint8_t t)
{
    uint32_t inv = 255 - t;
    return (uint8_t)(255 - (inv * inv) / 255);
}

static uint8_t ease_out_cubic(uint8_t t)
{
    uint32_t inv = 255 - t;
    uint32_t inv2 = (inv * inv) / 255;
    return (uint8_t)(255 - (inv2 * inv) / 255);
}

/* 0 before t0, 255 after t1, eased between. */
static uint8_t fade_alpha(uint32_t tick, uint32_t t0, uint32_t t1)
{
    if (tick <= t0) return 0;
    if (tick >= t1) return 255;
    return ease_out_quad((uint8_t)(((tick - t0) * 255) / (t1 - t0)));
}

static void draw_text_scaled(int x, int y, const char *text, uint32_t color, int scale)
{
    if (scale < 1) scale = 1;
    while (*text)
    {
        int idx = (uint8_t)*text;
        if (idx > 127) idx = '?';
        const uint8_t *glyph = font_8x8[idx];
        for (int row = 0; row < 8; row++)
        {
            uint8_t bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++)
            {
                if (bits & (0x80 >> col))
                    gpu_fill_rect(x + col * scale, y + row * scale,
                                  scale, scale, color);
            }
        }
        x += FONT_W * scale;
        text++;
    }
}

static void draw_text_centered(int cx, int y, const char *text,
                               uint32_t color, int scale)
{
    int x = cx - (boot_strlen(text) * FONT_W * scale) / 2;
    draw_text_scaled(x, y, text, color, scale);
}

/* Center a single glyph on its ink bounds, not its 8x8 cell: fonts carry
 * empty margin rows/cols and cell-centering reads as visibly off-center. */
static void draw_glyph_ink_centered(int cx, int cy, char ch, uint32_t color, int scale)
{
    int idx = (uint8_t)ch;
    if (idx > 127) idx = '?';
    const uint8_t *glyph = font_8x8[idx];
    int minx = 8, maxx = -1, miny = 8, maxy = -1;
    for (int row = 0; row < 8; row++)
    {
        uint8_t bits = glyph[row];
        if (!bits) continue;
        if (row < miny) miny = row;
        if (row > maxy) maxy = row;
        for (int col = 0; col < 8; col++)
        {
            if (bits & (0x80 >> col))
            {
                if (col < minx) minx = col;
                if (col > maxx) maxx = col;
            }
        }
    }
    if (maxx < 0) return;
    int ink_w = (maxx - minx + 1) * scale;
    int ink_h = (maxy - miny + 1) * scale;
    char s[2] = { ch, 0 };
    draw_text_scaled(cx - ink_w / 2 - minx * scale,
                     cy - ink_h / 2 - miny * scale, s, color, scale);
}

/* Logo: parsed once, cached, nearest-neighbor into the 56px slot. */
static uint8_t logo_buf[LOGO_MAX_BYTES];
static bool logo_attempted = false;
static int logo_w = 0;
static int logo_h = 0;
static uint32_t *logo_px = 0; /* boot-only, intentionally never freed */

static void load_logo(void)
{
    if (logo_attempted) return;
    logo_attempted = true;

    int bytes = load_file_content("/logo.bmp", (char *)logo_buf, LOGO_MAX_BYTES);
    if (bytes < 54) return;

    int32_t w = *(int32_t *)(logo_buf + 18);
    int32_t h = *(int32_t *)(logo_buf + 22);
    uint16_t bpp = *(uint16_t *)(logo_buf + 28);
    uint32_t pix_off = *(uint32_t *)(logo_buf + 10);

    if (w <= 0 || h <= 0 || w > LOGO_MAX_DIM || h > LOGO_MAX_DIM) return;
    if (bpp != 24 && bpp != 32) return;
    if (pix_off >= (uint32_t)bytes) return;

    int stride = ((w * (bpp / 8)) + 3) & ~3;
    if (pix_off + (uint32_t)(stride * h) > (uint32_t)bytes) return;

    logo_px = (uint32_t *)kmalloc((uint32_t)(w * h) * 4);
    if (!logo_px) return;

    uint8_t *src = logo_buf + pix_off;
    int bppx = bpp / 8;
    for (int row = 0; row < h; row++)
    {
        uint8_t *rp = src + (h - 1 - row) * stride; /* BMP is bottom-up */
        for (int col = 0; col < w; col++)
        {
            uint8_t b = rp[col * bppx + 0];
            uint8_t g = rp[col * bppx + 1];
            uint8_t r = rp[col * bppx + 2];
            uint8_t a = (bpp == 32) ? rp[col * bppx + 3] : 0xFF;
            logo_px[row * w + col] =
                ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    logo_w = w;
    logo_h = h;
}

/* Gap-free ring: annulus test per pixel in the bbox. The old y-scan with
 * 3px stamps left holes where the circle runs horizontal (top and bottom). */
static void draw_ring(int cx, int cy, uint32_t color)
{
    int r_out = RING_RADIUS;
    int r_in = RING_RADIUS - RING_THICK;
    int out2 = r_out * r_out;
    int in2 = r_in * r_in;
    for (int dy = -r_out; dy <= r_out; dy++)
    {
        for (int dx = -r_out; dx <= r_out; dx++)
        {
            int d2 = dx * dx + dy * dy;
            if (d2 <= out2 && d2 >= in2)
                draw_pixel_safe(cx + dx, cy + dy, color);
        }
    }
}

static void draw_logo(int cx, int cy, uint8_t a)
{
    load_logo();
    if (!logo_px || logo_w <= 0 || logo_h <= 0)
    {
        draw_ring(cx, cy, dim_color(RING_COLOR, a));
        draw_glyph_ink_centered(cx, cy, 'A', dim_color(AMBER, a), 4);
        return;
    }

    int dw = logo_w, dh = logo_h;
    if (dw > LOGO_DRAW_SIZE || dh > LOGO_DRAW_SIZE)
    {
        if (dw > dh) { dh = (dh * LOGO_DRAW_SIZE) / dw; dw = LOGO_DRAW_SIZE; }
        else { dw = (dw * LOGO_DRAW_SIZE) / dh; dh = LOGO_DRAW_SIZE; }
    }
    int ox = cx - dw / 2;
    int oy = cy - dh / 2;
    for (int y = 0; y < dh; y++)
    {
        int sy = (y * logo_h) / dh;
        for (int x = 0; x < dw; x++)
        {
            int sx = (x * logo_w) / dw;
            uint32_t px = logo_px[sy * logo_w + sx];
            uint32_t pa = (px >> 24) & 0xFF;
            if (pa == 0) continue;
            uint32_t final_a = (pa * a) / 255;
            if (final_a == 0) continue;
            if (final_a >= 255) draw_pixel_safe(ox + x, oy + y, px);
            else gpu_fill_rect_blend(ox + x, oy + y, 1, 1, px, (uint8_t)final_a);
        }
    }
}

static const int8_t spinner_offsets[SPINNER_POINTS][2] = {
    { 26, 0 }, { 23, 13 }, { 13, 23 }, { 0, 26 },
    {-13, 23 }, {-23, 13 }, {-26, 0 }, {-23, -13 },
    {-13, -23 }, { 0, -26 }, { 13, -23 }, { 23, -13 }
};

static void draw_spinner(int cx, int cy, uint32_t tick, uint8_t a)
{
    for (int i = 0; i < SPINNER_POINTS; i++)
        gpu_fill_rect(cx + spinner_offsets[i][0] - 2,
                      cy + spinner_offsets[i][1] - 2, 4, 4,
                      dim_color(TRACK_COLOR, a));

    int active = (int)((tick / 5) % SPINNER_POINTS);
    for (int i = SPINNER_POINTS - 1; i >= 0; i--)
    {
        int idx = (active - i + SPINNER_POINTS * 2) % SPINNER_POINTS;
        int x = cx + spinner_offsets[idx][0] - 2;
        int y = cy + spinner_offsets[idx][1] - 2;
        if (i == 0)
        {
            gpu_fill_rect_blend(x - 2, y - 2, 8, 8, AMBER, (uint8_t)((40 * a) / 255));
            gpu_fill_rect(x, y, 4, 4, dim_color(AMBER_HI, a));
        }
        else
        {
            uint32_t b = 210 - (uint32_t)i * 16;
            gpu_fill_rect(x, y, 4, 4, dim_color(TEXT_MID, (uint8_t)((b * a) / 255)));
        }
    }
}

typedef struct
{
    int cx;
    int logo_cy;
    int title_y;
    int sub_y;
    int rule_y;
    int spin_cy;
    int load_y;
    int status_y;
} BootLayout;

static void compute_layout(int sw, int sh, BootLayout *L)
{
    int total = L_LOGO_H + L_GAP_LOGO_T + L_TITLE_H + L_GAP_TITLE_S +
                L_SUB_H + L_GAP_SUB_RULE + L_RULE_H + L_GAP_RULE_SPIN +
                L_SPIN_D + L_GAP_SPIN_LOAD + L_LOAD_H + L_GAP_LOAD_STAT +
                L_STATUS_H;
    int top = (sh - total) / 2;

    L->cx = sw / 2;
    L->logo_cy = top + L_LOGO_H / 2;
    L->title_y = top + L_LOGO_H + L_GAP_LOGO_T;
    L->sub_y = L->title_y + L_TITLE_H + L_GAP_TITLE_S;
    L->rule_y = L->sub_y + L_SUB_H + L_GAP_SUB_RULE;
    L->spin_cy = L->rule_y + L_RULE_H + L_GAP_RULE_SPIN + L_SPIN_D / 2;
    L->load_y = L->rule_y + L_RULE_H + L_GAP_RULE_SPIN + L_SPIN_D + L_GAP_SPIN_LOAD;
    L->status_y = L->load_y + L_LOAD_H + L_GAP_LOAD_STAT;
}

static void draw_backdrop(int sw, int sh)
{
    gpu_fill_rect(0, 0, sw, sh, BG_COLOR);

    /* Phosphor lift: 4px steps, outermost layer at alpha 1 so the union
     * boundary is a 1/255 step -- invisible, no rectangle seam. */
    int cx = sw / 2, cy = sh / 2;
    for (int r = 0; r < 200; r += 4)
    {
        uint8_t a = (uint8_t)(4 - (r * 4) / 200);
        if (a == 0) continue;
        int wr = 560 - r * 2;
        int hr = 360 - r;
        gpu_fill_rect_blend(cx - wr / 2, cy - hr / 2, wr, hr, LIFT_COLOR, a);
    }

    for (int y = 0; y < sh; y += 3)
        gpu_fill_rect_blend(0, y, sw, 1, 0xFF000000, 6);
}

/* CRT glass: 1px-step row/column ramps after content, corners fall off. */
static void draw_vignette(int sw, int sh)
{
    int vh = sh / 6;
    for (int y = 0; y < vh; y++)
    {
        uint8_t a = (uint8_t)(50 - (y * 50) / vh);
        if (a == 0) break;
        gpu_fill_rect_blend(0, y, sw, 1, 0xFF000000, a);
        gpu_fill_rect_blend(0, sh - 1 - y, sw, 1, 0xFF000000, a);
    }
    int vw = sw / 8;
    for (int x = 0; x < vw; x++)
    {
        uint8_t a = (uint8_t)(40 - (x * 40) / vw);
        if (a == 0) break;
        gpu_fill_rect_blend(x, 0, 1, sh, 0xFF000000, a);
        gpu_fill_rect_blend(sw - 1 - x, 0, 1, sh, 0xFF000000, a);
    }
}

static const char *status_for_frac(int frac_pct)
{
    if (frac_pct < 25) return "Initializing hardware";
    if (frac_pct < 50) return "Mounting root filesystem";
    if (frac_pct < 75) return "Starting core services";
    return "Loading user session";
}

void boot_screen_show(void)
{
    int sw = gpu_get_width();
    int sh = gpu_get_height();
    BootLayout L;
    compute_layout(sw, sh, &L);

    int small_scale = (sw >= WIDE_SCREEN) ? 2 : 1;

    char num[12];
    c_puts("[BOOT] splash ");
    fmt_uint(num, sw); c_puts(num);
    c_putc('x');
    fmt_uint(num, sh); c_puts(num);
    c_putc('\n');

    audio_boot_jingle_start();

    char status_msg[48];
    uint32_t start = get_ticks();

    while (1)
    {
        uint32_t tick = get_ticks() - start;
        if (tick >= BOOT_TICKS)
            break;

        audio_boot_jingle_poll();
        draw_backdrop(sw, sh);

        /* CRT power-on: beam line expands, then dims as content takes over */
        if (tick < 10)
        {
            uint8_t la;
            int lw;
            if (tick < 6)
            {
                la = 255;
                lw = (sw * ease_out_cubic((uint8_t)(tick * 255 / 5))) / 255;
            }
            else
            {
                la = (uint8_t)(255 - (tick - 6) * 64);
                lw = sw;
            }
            if (lw > 0 && la > 0)
                gpu_fill_rect(L.cx - lw / 2, sh / 2 - 1, lw, 2,
                              dim_color(WARM_WHITE, la));
        }

        uint8_t a_logo = fade_alpha(tick, 6, 36);
        if (a_logo) draw_logo(L.cx, L.logo_cy, a_logo);

        uint8_t a_title = fade_alpha(tick, 16, 46);
        if (a_title)
        {
            draw_text_centered(L.cx + 2, L.title_y + 2, "AurionOS",
                               dim_color(SHADOW_COLOR, a_title), 4);
            draw_text_centered(L.cx, L.title_y, "AurionOS",
                               dim_color(WARM_WHITE, a_title), 4);
        }

        uint8_t a_sub = fade_alpha(tick, 30, 60);
        if (a_sub)
            draw_text_centered(L.cx, L.sub_y, "Pure Speed. Zero Compromise.",
                               dim_color(SUB_COLOR, a_sub), 2);

        uint8_t a_rule = fade_alpha(tick, 40, 64);
        if (a_rule)
        {
            int rw = (RULE_W * ease_out_cubic(a_rule)) / 255;
            gpu_fill_rect(L.cx - rw / 2, L.rule_y, rw, L_RULE_H,
                          dim_color(RULE_COLOR, a_rule));
        }

        uint8_t a_spin = fade_alpha(tick, 48, 60);
        if (a_spin) draw_spinner(L.cx, L.spin_cy, tick, a_spin);

        uint8_t a_bar = fade_alpha(tick, 54, 66);
        if (a_bar)
        {
            int lx = L.cx - BAR_W / 2;
            uint32_t fill_ticks = (tick > 54) ? (tick - 54) : 0;
            uint32_t fill_span = BOOT_TICKS - 54;
            int filled = (int)((fill_ticks * BAR_SEGS) / fill_span);
            if (filled > BAR_SEGS) filled = BAR_SEGS;
            int pct = (int)((fill_ticks * 100) / fill_span);
            if (pct > 100) pct = 100;

            for (int i = 0; i < BAR_SEGS; i++)
            {
                int sx = lx + i * (BAR_SEG_W + BAR_SEG_GAP);
                uint32_t c;
                if (i < filled)
                    c = (i == filled - 1)
                        ? AMBER_HI
                        : lerp_color(AMBER_LO, AMBER, (uint8_t)((i * 255) / (BAR_SEGS - 1)));
                else
                    c = TRACK_COLOR;
                gpu_fill_rect(sx, L.load_y, BAR_SEG_W, L_LOAD_H, dim_color(c, a_bar));
            }

            /* Percent: right-aligned in a reserved 4-char slot so the digits
             * grow leftward and never shift the label or collide with the
             * bar. Vertically centered on the bar row. */
            char pbuf[8];
            int n = fmt_uint(pbuf, pct);
            pbuf[n++] = '%'; pbuf[n] = 0;
            int slot_right = lx + BAR_W + 12 + 4 * FONT_W * small_scale;
            int pw = boot_strlen(pbuf) * FONT_W * small_scale;
            int py = L.load_y + L_LOAD_H / 2 - (FONT_W * small_scale) / 2;
            draw_text_scaled(slot_right - pw, py, pbuf,
                             dim_color(AMBER, a_bar), small_scale);

            /* Status: centered under the bar; dots grow symmetrically but
             * jitter is tiny and far less jarring than a bar-left anchor
             * that reads as "too right" on wide glyphs. */
            boot_strcpy(status_msg, status_for_frac(pct));
            int len = boot_strlen(status_msg);
            int dots = (int)((tick / 12) % 4);
            for (int i = 0; i < dots && len + 1 < (int)sizeof(status_msg); i++)
                status_msg[len++] = '.';
            status_msg[len] = 0;
            draw_text_centered(L.cx, L.status_y, status_msg,
                               dim_color(STATUS_COLOR, a_bar), small_scale);
        }

        draw_vignette(sw, sh);

        if (tick >= FADEOUT_TICK)
        {
            uint8_t a = (uint8_t)((tick - FADEOUT_TICK) * 32);
            gpu_fill_rect_blend(0, 0, sw, sh, 0xFF000000, a);
        }

        gpu_flush();

        for (int i = 0; i < FRAME_DELAY_ITERS; i++)
            io_wait();
    }

    audio_boot_jingle_stop();
    gpu_fill_rect(0, 0, sw, sh, BG_COLOR);
    gpu_flush();
}