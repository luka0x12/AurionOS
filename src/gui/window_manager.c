#include "window_manager.h"
#include "mouse.h"
#include "fs.h"
#include "scheduler.h"
#include "clipboard.h"
#include "png.h"

/* External GPU */
extern void   gpu_clear(uint32_t color);
extern void   gpu_fill_rect(int x, int y, int w, int h, uint32_t color);
extern void   gpu_fill_rect_blend(int x, int y, int w, int h,
                                  uint32_t color, uint8_t alpha);
extern void   gpu_blur_rect(int x, int y, int w, int h);
extern void   gpu_draw_pixel(int x, int y, uint32_t color);
extern void   gpu_draw_char(int x, int y, uint8_t c,
                            uint32_t fg, uint32_t bg);
extern void   gpu_draw_string(int x, int y, const uint8_t *str,
                              uint32_t fg, uint32_t bg);
extern int    gpu_flush(void);
extern int    gpu_get_width(void);
extern int    gpu_get_height(void);
extern uint32_t* gpu_get_backbuffer(void);

/* PREMIUM COLOR PALETTE — macOS Sequoia inspired */
#define ACCENT_BLUE       0xFF0A84FF  /* macOS accent */
#define ACCENT_BLUE_DIM   0xFF0066CC
#define ACCENT_GLOW       0xFF4DA6FF

#define MAT_DARK_BASE     0xFF1C1C1E  /* material base */
#define MAT_DARK_TINT     0xFF2C2C2E  /* material tint */
#define MAT_DARK_ELEVATED 0xFF3A3A3C  /* raised surfaces */
#define MAT_SEPARATOR     0xFF48484A

#define TL_CLOSE_TOP      0xFFFF6159
#define TL_CLOSE_BOT      0xFFE8453E
#define TL_MIN_TOP        0xFFFFBD2E
#define TL_MIN_BOT        0xFFE8A317
#define TL_MAX_TOP        0xFF28C93F
#define TL_MAX_BOT        0xFF1FA82F
#define TL_INACTIVE       0xFF4A4A4C

#define TEXT_PRIMARY      0xFFF5F5F7
#define TEXT_SECONDARY    0xFFAEAEB2
#define TEXT_TERTIARY     0xFF6E6E73

/* PERFORMANCE OPTIMIZATIONS - Dirty flag system */

static bool inst_mode = false;
static int scr_w = 1024, scr_h = 768;

bool wm_bg_dirty = true;
bool wm_dock_dirty = true;
bool wm_windows_dirty = true;
bool wm_cursor_dirty = true;
bool wm_full_redraw = true;

static int wm_prev_cursor_x = -1;
static int wm_prev_cursor_y = -1;

/* Animation counter for smooth effects */
static uint32_t wm_anim_tick = 0;

/* WALLPAPER SUPPORT (PNG)
 *
 * Raw-sector layout for wallpaper PNGs (8192 sectors = 4 MB per slot):
 *   LBA 10000: Night_ridge.png (desktop slot 0)
 *   LBA 20000: Misty_conife.png (desktop slot 1, default)
 *   LBA 40000: Gate_B14_217_AM.png (desktop slot 2)
 *   LBA 50000: Sunday_4_47_PM.png (desktop slot 3)
 *   LBA 30000: ins.png (installer background)
 * Icons follow at LBA 60000 + index*2000 (see icon_loader.c).
 *
 * The ISO carries the same files under /Wallpaper/. Installed HDD boots
 * without a CD fall back to the raw slots. The 8 KB RAM-FS object cap
 * rules the filesystem out for multi-MB PNGs, so no FS path exists here
 * by design: ISO file first, raw slot second.
 *
 * Each desktop wallpaper ships as a full-size PNG plus a 144x81
 * thumbnail (thumb_*.png) used for the Settings preview rows.
*/

extern void c_puts(const char *s);

#define WALLPAPER_COUNT 4
#define WALLPAPER_LBA0 10000U
#define WALLPAPER_LBA1 20000U
#define WALLPAPER_LBA2 40000U
#define WALLPAPER_LBA3 50000U
#define INSTALLER_WALLPAPER_LBA 30000U
#define WALLPAPER_SLOT_SECTORS 8192U
#define WALLPAPER_MAX_BYTES (WALLPAPER_SLOT_SECTORS * 512U)

static const char *const g_wallpaper_iso_names[WALLPAPER_COUNT] = {
    "Wallpaper/Night_ridge.png",
    "Wallpaper/Misty_conife.png",
    "Wallpaper/Gate_B14_217_AM.png",
    "Wallpaper/Sunday_4_47_PM.png"
};
static const uint32_t g_wallpaper_lbas[WALLPAPER_COUNT] = {
    WALLPAPER_LBA0, WALLPAPER_LBA1, WALLPAPER_LBA2, WALLPAPER_LBA3
};
#define INSTALLER_WALLPAPER_ISO "Wallpaper/ins.png"

static int g_wallpaper_idx = 1;

extern bool iso9660_find_file(const char *filename, uint32_t *out_lba, uint32_t *out_size);
extern int  iso9660_read_file(const char *filename, void *buffer, uint32_t max_size);
extern int  disk_read_lba_cdrom(uint32_t lba, uint32_t count, void *buffer);
extern int  disk_read_lba_hdd(uint32_t lba, uint32_t count, void *buffer);

/* Read one ISO file with exact size. Returns heap buffer or 0. */
static uint8_t *wm_read_iso_file(const char *name, uint32_t *out_len) {
    extern void *kmalloc(uint32_t size);
    extern void kfree(void *ptr);
    uint32_t lba = 0, size = 0;
    uint32_t alloc;
    uint8_t *buf;
    int got;
    if (!iso9660_find_file(name, &lba, &size))
        return 0;
    if (size <= 8 || size > WALLPAPER_MAX_BYTES)
        return 0;
    /* iso9660_read_file lands whole 512-byte sectors: round the heap
       block up, or the tail sector stomps the next block's header and
       every later multi-MB kmalloc fails on the broken chain. */
    alloc = (size + 511) & ~(uint32_t)511;
    buf = (uint8_t *)kmalloc(alloc);
    if (!buf)
        return 0;
    got = iso9660_read_file(name, buf, size);
    if (got != (int)size) {
        kfree(buf);
        return 0;
    }
    *out_len = size;
    return buf;
}

/* Load the active wallpaper PNG into a heap buffer. Installer mode always
   uses ins.png; desktop uses g_wallpaper_idx. Caller frees the buffer.
   Returns 0 with *out_len = 0 when nothing readable exists. */
static uint8_t *wm_read_wallpaper_file(uint32_t *out_len) {
    extern void *kmalloc(uint32_t size);
    extern void kfree(void *ptr);
    const char *iso_name;
    uint32_t slot_lba;
    uint8_t *buf;
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    *out_len = 0;
    if (inst_mode) {
        iso_name = INSTALLER_WALLPAPER_ISO;
        slot_lba = INSTALLER_WALLPAPER_LBA;
    } else {
        int idx = g_wallpaper_idx;
        if (idx < 0 || idx >= WALLPAPER_COUNT)
            idx = 0;
        iso_name = g_wallpaper_iso_names[idx];
        slot_lba = g_wallpaper_lbas[idx];
    }
    buf = wm_read_iso_file(iso_name, out_len);
    if (buf) {
        c_puts("[WM] Wallpaper loaded from ISO\n");
        return buf;
    }
    /* Basename fallback: mkiso also stages bare names at the ISO root. */
    {
        const char *base = iso_name;
        for (const char *p = iso_name; *p; p++)
            if (*p == '/')
                base = p + 1;
        if (base != iso_name) {
            buf = wm_read_iso_file(base, out_len);
            if (buf) {
                c_puts("[WM] Wallpaper loaded from ISO root\n");
                return buf;
            }
        }
    }
    /* Raw slot fallback for installed HDD boots without a CD. The slot is
       read whole; png_decode stops at IEND and ignores zero padding. */
    buf = (uint8_t *)kmalloc(WALLPAPER_MAX_BYTES);
    if (!buf)
        return 0;
    if (disk_read_lba_hdd(slot_lba, WALLPAPER_SLOT_SECTORS, buf) != 0 ||
        buf[0] != png_sig[0] || buf[1] != png_sig[1] ||
        buf[2] != png_sig[2] || buf[3] != png_sig[3] ||
        buf[4] != png_sig[4] || buf[5] != png_sig[5] ||
        buf[6] != png_sig[6] || buf[7] != png_sig[7]) {
        if (disk_read_lba_cdrom(slot_lba, WALLPAPER_SLOT_SECTORS, buf) != 0) {
            kfree(buf);
            return 0;
        }
        for (int k = 0; k < 8; k++) {
            if (buf[k] != png_sig[k]) {
                kfree(buf);
                return 0;
            }
        }
        c_puts("[WM] Wallpaper loaded from CD raw slot\n");
    } else {
        c_puts("[WM] Wallpaper loaded from HDD raw slot\n");
    }
    *out_len = WALLPAPER_MAX_BYTES;
    return buf;
}

/* Drop the cached wallpaper so the next desktop draw decodes the active
   selection again. Used on wallpaper switch, mode change, and resize. */
static void wm_drop_wallpaper_cache(void);

/* WALLPAPER render */

bool wallpaper_rendered = false;
static uint32_t *g_wallpaper_cache = 0;
static int g_wallpaper_cache_w = 0;
static int g_wallpaper_cache_h = 0;
static bool g_wallpaper_has_image = false;

/* Decimal-encode v into buf. Returns digit count (no terminator). */
static int wm_u32_to_str(uint32_t v, char *buf) {
    char tmp[11];
    int ti = 0, n = 0;
    if (v == 0) {
        buf[0] = '0';
        return 1;
    }
    while (v > 0 && ti < 10) {
        tmp[ti++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (ti > 0) buf[n++] = tmp[--ti];
    return n;
}

/* Renders the wallpaper into the backbuffer. Returns false when no
   wallpaper file is readable, so the caller falls back to gradient. */
static bool wm_render_wallpaper(int screen_w, int screen_h)
{
    extern void* kmalloc(uint32_t size);
    uint32_t *bb = gpu_get_backbuffer();
    if (!bb) return false;

    if (g_wallpaper_cache) {
        if (g_wallpaper_cache_w != screen_w || g_wallpaper_cache_h != screen_h) {
            extern void kfree(void *ptr);
            kfree(g_wallpaper_cache);
            g_wallpaper_cache = 0;
            g_wallpaper_cache_w = 0;
            g_wallpaper_cache_h = 0;
            wallpaper_rendered = false;
        } else {
            uint32_t *src = g_wallpaper_cache;
            uint32_t *dst = bb;
            uint32_t count = screen_w * screen_h;
            for (uint32_t i = 0; i < count; i++) dst[i] = src[i];
            return g_wallpaper_has_image;
        }
    }

    if (wallpaper_rendered) return g_wallpaper_has_image;
    wallpaper_rendered = true;

    c_puts("[WM] Creating wallpaper cache in RAM...\n");

    /* Read the PNG before the big allocs so the diag line reports it. */
    uint32_t png_len = 0;
    uint8_t *png_file = wm_read_wallpaper_file(&png_len);
    {
        extern void mem_get_stats(uint32_t *stats);
        uint32_t st[3] = {0, 0, 0};
        char line[64];
        int li = 0, k;
        uint32_t vals[3];
        mem_get_stats(st);
        vals[0] = st[0] / (1024 * 1024);
        vals[1] = st[1] / (1024 * 1024);
        vals[2] = png_len / 1024;
        for (k = 0; "[WM] heap free="[k]; k++) line[li++] = "[WM] heap free="[k];
        li += wm_u32_to_str(vals[0], line + li);
        for (k = 0; "MB used="[k]; k++) line[li++] = "MB used="[k];
        li += wm_u32_to_str(vals[1], line + li);
        for (k = 0; "MB png="[k]; k++) line[li++] = "MB png="[k];
        li += wm_u32_to_str(vals[2], line + li);
        for (k = 0; "KB\n"[k]; k++) line[li++] = "KB\n"[k];
        line[li] = '\0';
        c_puts(line);
    }

    /* Decode first, then drop the file buffer BEFORE allocating the
     * screen cache: peak use is file+decode, never file+decode+cache,
     * which keeps repeated switches from fragmenting the heap into
     * unusable slivers. The inflate loop heartbeats (see png.c) and the
     * scale loop below does too, so the 5s scheduler watchdog never
     * mistakes a big PNG for a hung OS.
    */
    
    PngImage wp = {0, 0, 0};
    int32_t img_w = 0, img_h = 0;
    if (png_file && png_len > 8) {
        int prc = png_decode(png_file, png_len, &wp);
        extern void kfree(void *ptr);
        kfree(png_file);
        png_file = 0;
        if (prc != 0) {
            c_puts("[WM] Wallpaper PNG decode failed: ");
            c_puts(png_error_string(prc));
            c_puts("\n");
            png_free(&wp);
            wp.pixels = 0;
        } else {
            img_w = (int32_t)wp.width;
            img_h = (int32_t)wp.height;
        }
    } else if (png_file) {
        extern void kfree(void *ptr);
        kfree(png_file);
        png_file = 0;
    }
    if (wp.pixels && (img_w <= 0 || img_h <= 0 ||
                      img_w > 4096 || img_h > 4096)) {
        c_puts("[WM] Wallpaper dimensions out of range, ignoring\n");
        png_free(&wp);
        wp.pixels = 0;
        img_w = img_h = 0;
    }

    g_wallpaper_cache = (uint32_t*)kmalloc(screen_w * screen_h * 4);
    if (!g_wallpaper_cache) {
        c_puts("[WM] ERROR: Failed to allocate wallpaper cache\n");
        if (wp.pixels) png_free(&wp);
        return false;
    }

    g_wallpaper_cache_w = screen_w;
    g_wallpaper_cache_h = screen_h;

    for (int i = 0; i < screen_w * screen_h; i++) g_wallpaper_cache[i] = 0xFF000000;

    /* Nothing decodable: release the black cache and let the caller
     * fall back to gradient. */
    if (!wp.pixels || img_w <= 0 || img_h <= 0) {
        /* No readable wallpaper: release the black cache and let the
           caller fall back to gradient. wallpaper_rendered stays true so
           a missing file costs one probe, not one per frame.
        */
        extern void kfree(void *ptr);
        kfree(g_wallpaper_cache);
        g_wallpaper_cache = 0;
        g_wallpaper_cache_w = 0;
        g_wallpaper_cache_h = 0;
        g_wallpaper_has_image = false;
        return false;
    }

    int32_t draw_w, draw_h;
    if ((uint64_t)screen_w * (uint64_t)img_h > (uint64_t)screen_h * (uint64_t)img_w) {
        draw_w = screen_w;
        draw_h = (int32_t)(((uint64_t)img_h * (uint64_t)screen_w) / (uint64_t)img_w);
    } else {
        draw_h = screen_h;
        draw_w = (int32_t)(((uint64_t)img_w * (uint64_t)screen_h) / (uint64_t)img_h);
    }

    int32_t offset_x = (screen_w - draw_w) / 2;
    int32_t offset_y = (screen_h - draw_h) / 2;

    /* Cover-fit the decoded image into the cache with bilinear sampling.
       Decoded PNG rows are top-down, so no vertical flip is needed.
       Heartbeat as we go: a 1080p cover-fit is millions of multiply
       ops, and silence longer than 5s trips the scheduler watchdog.
    */
    for (int32_t screen_row = 0; screen_row < screen_h; screen_row++) {
        uint32_t *cache_row = g_wallpaper_cache + screen_row * screen_w;

        if ((screen_row & 15) == 0) {
            extern void sched_heartbeat(void);
            sched_heartbeat();
        }

        int32_t s_off_y = screen_row - offset_y;
        if (s_off_y < 0 || s_off_y >= draw_h) {
            for (int32_t x = 0; x < screen_w; x++) cache_row[x] = 0xFF000000;
            continue;
        }

        uint32_t src_y_fp = (uint32_t)(((uint64_t)(uint32_t)s_off_y *
                                        (uint64_t)(uint32_t)img_h << 16) /
                                       (uint64_t)(uint32_t)draw_h);
        uint32_t y0 = src_y_fp >> 16;
        uint32_t fy = (src_y_fp >> 8) & 0xFF;
        uint32_t y1 = y0 + 1;
        if (y0 >= (uint32_t)img_h) y0 = (uint32_t)img_h - 1;
        if (y1 >= (uint32_t)img_h) y1 = (uint32_t)img_h - 1;

        for (int32_t screen_col = 0; screen_col < screen_w; screen_col++) {
            int32_t s_off_x = screen_col - offset_x;
            uint32_t src_x_fp, x0, fx, x1;
            uint32_t p00, p10, p01, p11;
            uint32_t r, g, b;
            if (s_off_x < 0 || s_off_x >= draw_w) {
                cache_row[screen_col] = 0xFF000000;
                continue;
            }

            src_x_fp = (uint32_t)(((uint64_t)(uint32_t)s_off_x *
                                   (uint64_t)(uint32_t)img_w << 16) /
                                  (uint64_t)(uint32_t)draw_w);
            x0 = src_x_fp >> 16;
            fx = (src_x_fp >> 8) & 0xFF;
            x1 = x0 + 1;
            if (x0 >= (uint32_t)img_w) x0 = (uint32_t)img_w - 1;
            if (x1 >= (uint32_t)img_w) x1 = (uint32_t)img_w - 1;

            p00 = wp.pixels[y0 * (uint32_t)img_w + x0];
            p10 = wp.pixels[y0 * (uint32_t)img_w + x1];
            p01 = wp.pixels[y1 * (uint32_t)img_w + x0];
            p11 = wp.pixels[y1 * (uint32_t)img_w + x1];

            r = ((((p00 >> 16) & 0xFF) * (256 - fx) + ((p10 >> 16) & 0xFF) * fx) *
                 (256 - fy) +
                 (((p01 >> 16) & 0xFF) * (256 - fx) + ((p11 >> 16) & 0xFF) * fx) *
                 fy) >> 16;
            g = ((((p00 >> 8) & 0xFF) * (256 - fx) + ((p10 >> 8) & 0xFF) * fx) *
                 (256 - fy) +
                 (((p01 >> 8) & 0xFF) * (256 - fx) + ((p11 >> 8) & 0xFF) * fx) *
                 fy) >> 16;
            b = ((((p00) & 0xFF) * (256 - fx) + ((p10) & 0xFF) * fx) *
                 (256 - fy) +
                 (((p01) & 0xFF) * (256 - fx) + ((p11) & 0xFF) * fx) *
                 fy) >> 16;

            cache_row[screen_col] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    png_free(&wp);

    c_puts("[WM] Wallpaper cached!\n");
    g_wallpaper_has_image = true;

    uint32_t *src = g_wallpaper_cache;
    uint32_t *dst = bb;
    for (int i = 0; i < screen_w * screen_h; i++) dst[i] = src[i];
    return true;
}

/* Defined here: drops the cache so the next draw decodes the selection. */
static void wm_drop_wallpaper_cache(void) {
    extern void kfree(void *ptr);
    if (g_wallpaper_cache) {
        kfree(g_wallpaper_cache);
        g_wallpaper_cache = 0;
        g_wallpaper_cache_w = 0;
        g_wallpaper_cache_h = 0;
        wallpaper_rendered = false;
        g_wallpaper_has_image = false;
    }
}

/* Switch the desktop wallpaper. 0 = Night Ridge, 1 = Misty Conifer (default),
   2 = Gate B14, 2:17 AM, 3 = Sunday, 4:47 PM.
   Takes effect on the next desktop draw; the caller persists g_settings. */
void wm_set_wallpaper(int idx) {
    if (idx < 0)
        idx = 0;
    if (idx >= WALLPAPER_COUNT)
        idx = WALLPAPER_COUNT - 1;
    if (idx == g_wallpaper_idx && g_wallpaper_cache) {
        wm_invalidate_all();
        return;
    }
    g_wallpaper_idx = idx;
    g_settings.wallpaper = idx;
    wm_drop_wallpaper_cache();
    wm_invalidate_all();
}

int wm_get_wallpaper(void) {
    return g_wallpaper_idx;
}

int wm_wallpaper_count(void) {
    return WALLPAPER_COUNT;
}

static const char *const g_wallpaper_names[] = {
    "Night Ridge",
    "Misty Conifer",
    "Gate B14, 2:17 AM",
    "Sunday, 4:47 PM"
};

const char *wm_wallpaper_name(int idx) {
    if (idx < 0 || idx >= WALLPAPER_COUNT)
        return "Unknown";
    return g_wallpaper_names[idx];
}

/* Session thumbnail cache for the Settings preview rows. */
#define WP_THUMB_W 144
#define WP_THUMB_H 81
static uint32_t *g_wp_thumb[WALLPAPER_COUNT] = {0, 0, 0, 0};

static const char *const g_wallpaper_thumb_iso[WALLPAPER_COUNT] = {
    "Wallpaper/thumb_night.png",
    "Wallpaper/thumb_misty.png",
    "Wallpaper/thumb_gate.png",
    "Wallpaper/thumb_sunday.png"
};

/* Decoded thumbnail pixels for Settings, or 0 when unreadable. The first
   call decodes the ~10 KB file; later calls reuse the cached buffer.
*/
const uint32_t *wm_wallpaper_preview(int idx) {
    extern void *kmalloc(uint32_t size);
    extern void kfree(void *ptr);
    uint32_t len = 0;
    uint8_t *file;
    PngImage img = {0, 0, 0};
    uint32_t *thumb;
    if (idx < 0 || idx >= WALLPAPER_COUNT)
        return 0;
    if (g_wp_thumb[idx])
        return g_wp_thumb[idx];
    file = wm_read_iso_file(g_wallpaper_thumb_iso[idx], &len);
    if (!file || len <= 8) {
        if (file)
            kfree(file);
        return 0;
    }
    if (png_decode(file, len, &img) != 0 || !img.pixels) {
        kfree(file);
        png_free(&img);
        return 0;
    }
    kfree(file);
    thumb = (uint32_t *)kmalloc(WP_THUMB_W * WP_THUMB_H * 4);
    if (!thumb) {
        png_free(&img);
        return 0;
    }
    /* Thumbs ship at exactly 144x81; scale defensively anyway. */
    for (int dy = 0; dy < WP_THUMB_H; dy++) {
        uint32_t sy = (uint32_t)dy * img.height / WP_THUMB_H;
        if (sy >= img.height)
            sy = img.height - 1;
        for (int dx = 0; dx < WP_THUMB_W; dx++) {
            uint32_t sx = (uint32_t)dx * img.width / WP_THUMB_W;
            if (sx >= img.width)
                sx = img.width - 1;
            thumb[dy * WP_THUMB_W + dx] = img.pixels[sy * img.width + sx];
        }
    }
    png_free(&img);
    g_wp_thumb[idx] = thumb;
    return thumb;
}

/* External kernel */
extern uint16_t sys_getkey(void);
extern uint16_t c_getkey_nonblock(void);
extern char     keyboard_remap(char c);
extern int      sys_kb_hit(void);
extern uint32_t get_ticks(void);
extern int      sys_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
extern bool     vmware_svga_available(void);

extern int  load_file_content(const char *fn, char *buf, int max);
extern int  save_file_content(const char *fn, const char *data, int len);
extern int  fs_save_to_disk(void);

/* GLOBAL SETTING */
OSSettings g_settings = {
    .bg_color            = 0,
    .window_style        = 0,
    .startup_app_idx     = 0,
    .dock_magnification  = 1,
    .dock_transparent    = 1,
    .resolution          = 4,
    .wallpaper           = 1,
    .cursor_size         = 0
};

/* MODULE STATE */
volatile int  desktop_exit_reason = -1;
int           dock_clicked_slot   = -1;

static Window  windows[WM_MAX_WINDOWS];
static int     win_count  = 0;
static Window *focus_win  = 0;

static bool    res_changed = false;
static bool    bg_dirty    = true;

static bool    dragging = false;
static int     drag_ox, drag_oy;
static Window *drag_win = 0;
static int     drag_tx, drag_ty;

static bool    resizing  = false;
static int     rsz_edge  = 0;
static Window *rsz_win   = 0;

static bool    prev_lmb = false;
static bool    prev_rmb = false;

static uint32_t dbl_tick = 0;
static Window  *dbl_win  = 0;

static icon_draw_fn dock_fns[WM_MAX_DOCK_SLOTS];
static char         dock_names[WM_MAX_DOCK_SLOTS][64];
static int          dock_cnt   = 0;
static int          dock_hover = -1;
static int          dock_sz[WM_MAX_DOCK_SLOTS];
static int          dock_bounce[WM_MAX_DOCK_SLOTS]; /* NEW: bounce animation */

static int      tip_slot = -1;
static uint32_t tip_tick = 0;
static bool     tip_show = false;

static int mn(int a, int b) { return a < b ? a : b; }
static int mx(int a, int b) { return a > b ? a : b; }
static int cl(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ab(int v) { return v < 0 ? -v : v; }

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, int m) {
    int i = 0;
    while (i < m - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
bool wm_string_compare(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static bool seq(const char *a, const char *b) { return wm_string_compare(a, b); }

/* Color interpolation for gradients */
static uint32_t lerp_color(uint32_t c1, uint32_t c2, uint8_t t) {
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t r = r1 + ((r2 - r1) * t) / 255;
    uint8_t g = g1 + ((g2 - g1) * t) / 255;
    uint8_t b = b1 + ((b2 - b1) * t) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* ROUNDED RECT PRIMITIVES — with better anti-aliasing feel */
static int corner_inset(int r, int row)
{
    int dy = r - row;
    int dx = 0;
    int r2 = r * r;
    while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
    return r - dx;
}

static void rrect(int x, int y, int w, int h, uint32_t c, int r)
{
    if (w <= 0 || h <= 0) return;
    if (r < 1) { gpu_fill_rect(x, y, w, h, c); return; }
    r = mn(r, mn(w / 2, h / 2));
    for (int i = 0; i < r; i++) {
        int ins = corner_inset(r, i);
        int sw  = w - 2 * ins;
        if (sw > 0) {
            gpu_fill_rect(x + ins, y + i,         sw, 1, c);
            gpu_fill_rect(x + ins, y + h - 1 - i, sw, 1, c);
        }
    }
    if (h - 2 * r > 0) gpu_fill_rect(x, y + r, w, h - 2 * r, c);
}

static void rrect_a(int x, int y, int w, int h, uint32_t c, int r, uint8_t a)
{
    if (w <= 0 || h <= 0 || a == 0) return;
    if (r < 1) { gpu_fill_rect_blend(x, y, w, h, c, a); return; }
    r = mn(r, mn(w / 2, h / 2));
    for (int i = 0; i < r; i++) {
        int ins = corner_inset(r, i);
        int sw  = w - 2 * ins;
        if (sw > 0) {
            gpu_fill_rect_blend(x + ins, y + i,         sw, 1, c, a);
            gpu_fill_rect_blend(x + ins, y + h - 1 - i, sw, 1, c, a);
        }
    }
    if (h - 2 * r > 0)
        gpu_fill_rect_blend(x, y + r, w, h - 2 * r, c, a);
}

static void rrect_bot(int x, int y, int w, int h, uint32_t c, int r)
{
    if (w <= 0 || h <= 0) return;
    if (r < 1) { gpu_fill_rect(x, y, w, h, c); return; }
    r = mn(r, mn(w / 2, h));
    if (h - r > 0) gpu_fill_rect(x, y, w, h - r, c);
    for (int i = 0; i < r; i++) {
        int ins = corner_inset(r, i);
        int sw  = w - 2 * ins;
        if (sw > 0) gpu_fill_rect(x + ins, y + h - 1 - i, sw, 1, c);
    }
}

static void rrect_bot_a(int x, int y, int w, int h, uint32_t c, int r, uint8_t a)
{
    if (w <= 0 || h <= 0 || a == 0) return;
    if (r < 1) { gpu_fill_rect_blend(x, y, w, h, c, a); return; }
    r = mn(r, mn(w / 2, h));
    if (h - r > 0)
        gpu_fill_rect_blend(x, y, w, h - r, c, a);
    for (int i = 0; i < r; i++) {
        int ins = corner_inset(r, i);
        int sw  = w - 2 * ins;
        if (sw > 0)
            gpu_fill_rect_blend(x + ins, y + h - 1 - i, sw, 1, c, a);
    }
}

/* Gradient rounded rect - NEW premium effect */
static void rrect_gradient(int x, int y, int w, int h,
                           uint32_t top_color, uint32_t bot_color, int r)
{
    if (w <= 0 || h <= 0) return;
    if (r < 1) r = 0;
    r = mn(r, mn(w / 2, h / 2));

    for (int row = 0; row < h; row++) {
        uint8_t t = (row * 255) / (h > 1 ? h - 1 : 1);
        uint32_t c = lerp_color(top_color, bot_color, t);
        int ins = 0;
        if (row < r) ins = corner_inset(r, row);
        else if (row >= h - r) ins = corner_inset(r, h - 1 - row);
        int sw = w - 2 * ins;
        if (sw > 0) gpu_fill_rect(x + ins, y + row, sw, 1, c);
    }
}

static void circ(int cx, int cy, int r, uint32_t c)
{
    int r2 = r * r + r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
        if (dx > 0)
            gpu_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
        else if (dy * dy <= r2)
            gpu_draw_pixel(cx, cy + dy, c);
    }
}

/* Gradient circle - NEW premium traffic lights */
static void circ_gradient(int cx, int cy, int r, uint32_t top, uint32_t bot)
{
    int r2 = r * r + r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
        uint8_t t = ((dy + r) * 255) / (2 * r);
        uint32_t c = lerp_color(top, bot, t);
        if (dx > 0)
            gpu_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
        else if (dy * dy <= r2)
            gpu_draw_pixel(cx, cy + dy, c);
    }
}

/* Alpha-blended circle */
static void circ_a(int cx, int cy, int r, uint32_t c, uint8_t a)
{
    int r2 = r * r + r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
        if (dx > 0)
            gpu_fill_rect_blend(cx - dx, cy + dy, dx * 2 + 1, 1, c, a);
    }
}

/*PREMIUM SHADOW — Multi-layer soft shadow with offset. Uses alpha blending to create genuine depth */
static void draw_shadow(int x, int y, int w, int h, bool focused, int cr)
{
    if (w <= 0 || h <= 0) return;

    /* A smooth, premium multi-layer shadow using rounded rects for all layers */
    if (focused) {
        /* Layer 1: Ambient wide shadow */
        rrect_a(x - 12, y - 5, w + 24, h + 22, 0xFF000000, cr + 8, 6);
        /* Layer 2: Medium soft shadow */
        rrect_a(x - 8, y - 3,  w + 16, h + 14, 0xFF000000, cr + 6, 10);
        /* Layer 3: Defined shadow */
        rrect_a(x - 4,  y - 1,  w + 8, h + 7, 0xFF000000, cr + 3,  20);
        /* Layer 4: Close contact shadow */
        rrect_a(x - 1,  y,      w + 2,  h + 2,  0xFF000000, cr + 1,  40);
    } else {
        /* Layer 1: Ambient wide shadow */
        rrect_a(x - 6, y - 2, w + 12, h + 10, 0xFF000000, cr + 4, 6);
        /* Layer 2: Medium defined shadow */
        rrect_a(x - 3,  y - 1, w + 6, h + 5,  0xFF000000, cr + 2, 12);
        /* Layer 3: Close contact shadow */
        rrect_a(x - 1,  y,     w + 2,  h + 2,  0xFF000000, cr + 1, 30);
    }
}

static void tl_btn(int cx, int cy, int r, uint32_t top, uint32_t bot,
                   bool show_sym, int sym, bool inactive)
{
    /* Outer subtle rim shadow */
    circ_a(cx, cy + 1, r, 0xFF000000, 80);

    if (inactive) {
        /* Inactive: subtle gray */
        circ(cx, cy, r, 0xFF4A4A4C);
        circ(cx, cy, r - 1, 0xFF3A3A3C);
    } else {
        /* Active: vibrant gradient */
        circ_gradient(cx, cy, r, top, bot);

        /* Top shine - stronger highlight */
        gpu_fill_rect_blend(cx - r + 2, cy - r + 1, r * 2 - 4, 1,
                           0xFFFFFFFF, 120);
        gpu_fill_rect_blend(cx - r + 3, cy - r + 2, r * 2 - 6, 1,
                           0xFFFFFFFF, 60);
        gpu_draw_pixel(cx, cy - r + 1, 0xFFFFFFFF);
        
        /* Bottom shadow for depth */
        gpu_fill_rect_blend(cx - r + 3, cy + r - 2, r * 2 - 6, 1,
                           0xFF000000, 40);
    }

    if (!show_sym) return;

    /* Symbol color - dark for contrast */
    uint32_t sc = inactive ? 0xFF1A1A1C : 0xFF2A0A00;

    switch (sym) {
    case 0: /* × close - thicker X */
        for (int i = -3; i <= 3; i++) {
            gpu_draw_pixel(cx + i, cy + i, sc);
            gpu_draw_pixel(cx + i, cy - i, sc);
            if (i != 0) {
                gpu_draw_pixel(cx + i, cy + i - 1, sc);
                gpu_draw_pixel(cx + i, cy - i + 1, sc);
            }
        }
        break;
    case 1: /* − minimize - thicker line */
        gpu_fill_rect(cx - 4, cy, 9, 2, sc);
        break;
    case 2: /* fullscreen - double arrows */
        /* Top-left arrow */
        gpu_fill_rect(cx - 3, cy - 3, 4, 1, sc);
        gpu_fill_rect(cx - 3, cy - 2, 3, 1, sc);
        gpu_fill_rect(cx - 3, cy - 1, 2, 1, sc);
        gpu_fill_rect(cx - 3, cy,     1, 1, sc);
        /* Bottom-right arrow */
        gpu_fill_rect(cx,     cy + 3, 4, 1, sc);
        gpu_fill_rect(cx + 1, cy + 2, 3, 1, sc);
        gpu_fill_rect(cx + 2, cy + 1, 2, 1, sc);
        gpu_fill_rect(cx + 3, cy,     1, 1, sc);
        break;
    }
}

static void draw_traffic_lights(Window *win, int hmx, int hmy)
{
    int cy  = win->y + WM_TITLEBAR_HEIGHT / 2;
    int cx0 = win->x + WM_TL_LEFT_PAD;
    int cx1 = cx0 + WM_TL_SPACING;
    int cx2 = cx1 + WM_TL_SPACING;
    int hr  = WM_TL_RADIUS + 4;

    bool near = false, h0 = false, h1 = false, h2 = false;
    if (hmx >= 0 && hmy >= 0 &&
        hmx >= cx0 - hr && hmx <= cx2 + hr &&
        hmy >= cy  - hr && hmy <= cy  + hr)
    {
        near = true;
        #define TH(px, py, ccx, ccy) \
            (((px)-(ccx))*((px)-(ccx)) + ((py)-(ccy))*((py)-(ccy)) <= (hr)*(hr))
        h0 = TH(hmx, hmy, cx0, cy);
        h1 = TH(hmx, hmy, cx1, cy);
        h2 = TH(hmx, hmy, cx2, cy);
        #undef TH
    }

    bool colorize = win->focused || near;

    tl_btn(cx0, cy, WM_TL_RADIUS, TL_CLOSE_TOP, TL_CLOSE_BOT,
           near && h0, 0, !colorize);
    tl_btn(cx1, cy, WM_TL_RADIUS, TL_MIN_TOP, TL_MIN_BOT,
           near && h1, 1, !colorize);
    tl_btn(cx2, cy, WM_TL_RADIUS, TL_MAX_TOP, TL_MAX_BOT,
           near && h2, 2, !colorize);
}

static void draw_win_buttons(Window *win)
{
    int bw = WM_WIN_BTN_W, bh = WM_WIN_BTN_H;
    int by = win->y + (WM_TITLEBAR_HEIGHT - bh) / 2;
    int cx = win->x + win->w - bw - 4;

    /* Close button - modern flat red */
    gpu_fill_rect(cx, by, bw, bh, 0xFFE81123);
    gpu_draw_char(cx + 10, by + 7, 'X', 0xFFFFFFFF, 0);
    cx -= bw + 2;

    /* Maximize - flat gray */
    gpu_fill_rect(cx, by, bw, bh, 0xFF2D2D2D);
    gpu_fill_rect(cx + 8, by + 5,     12, 1,       0xFFFFFFFF);
    gpu_fill_rect(cx + 8, by + bh - 6, 12, 1,       0xFFFFFFFF);
    gpu_fill_rect(cx + 8, by + 5,      1,  bh - 10, 0xFFFFFFFF);
    gpu_fill_rect(cx + 19,by + 5,      1,  bh - 10, 0xFFFFFFFF);
    cx -= bw + 2;

    /* Minimize - flat gray */
    gpu_fill_rect(cx, by, bw, bh, 0xFF2D2D2D);
    gpu_fill_rect(cx + 8, by + bh / 2, 12, 2, 0xFFFFFFFF);
}

/* SETTINGS */
void wm_load_settings(void)
{
    OSSettings ld;
    int n = load_file_content("/Settings.dat",
                              (char *)&ld, sizeof(OSSettings));
    if (n == sizeof(OSSettings)) {
        g_settings = ld;
        if (!g_settings.dock_magnification && !g_settings.dock_transparent) {
            g_settings.dock_magnification = 1;
            g_settings.dock_transparent   = 1;
            wm_save_settings();
        }
    } else if (n == (int)(sizeof(OSSettings) - sizeof(int))) {
        /* Previous Settings.dat with wallpaper but without cursor size:
           keep every saved value, default the cursor, re-save upgraded. */
        uint8_t *dst = (uint8_t *)&g_settings;
        uint8_t *src = (uint8_t *)&ld;
        for (uint32_t k = 0; k < sizeof(OSSettings) - sizeof(int); k++)
            dst[k] = src[k];
        g_settings.cursor_size = 0;
        wm_save_settings();
    } else if (n == (int)(sizeof(OSSettings) - 2 * sizeof(int))) {
        /* Legacy Settings.dat without the wallpaper field: keep every
           saved value, default to Misty Conifer, and re-save upgraded. */
        uint8_t *dst = (uint8_t *)&g_settings;
        uint8_t *src = (uint8_t *)&ld;
        for (uint32_t k = 0; k < sizeof(OSSettings) - 2 * sizeof(int); k++)
            dst[k] = src[k];
        g_settings.wallpaper = 1;
        g_settings.cursor_size = 0;
        wm_save_settings();
    } else {
        wm_save_settings();
    }
    if (g_settings.wallpaper < 0 || g_settings.wallpaper >= WALLPAPER_COUNT)
        g_settings.wallpaper = 1;
    if (g_settings.cursor_size < 0 || g_settings.cursor_size > 2)
        g_settings.cursor_size = 0;
    g_wallpaper_idx = g_settings.wallpaper;
}

void wm_save_settings(void)
{
    save_file_content("/Settings.dat",
                      (const char *)&g_settings, sizeof(OSSettings));
    fs_save_to_disk();

    static const uint16_t ws[] = {1280,1280,1440,1600,1920,2560};
    static const uint16_t hs[] = {720,1024,900,900,1080,1440};
    int i = g_settings.resolution;
    if (i >= 0 && i < 6) {
        char b[512];
        for (int j = 0; j < 512; j++) b[j] = 0;
        b[0] = 'B'; b[1] = 'O'; b[2] = 'O'; b[3] = 'T'; b[4] = (char)i;
        *(uint16_t *)(b + 8)  = ws[i];
        *(uint16_t *)(b + 10) = hs[i];
        extern int disk_write_lba(uint32_t, uint32_t, const void *);
        disk_write_lba(999, 1, b);
    }
}

/* MODE */
void wm_set_installer_mode(bool e) {
    if (inst_mode != e) {
        inst_mode = e;
        /* Wallpaper source depends on the mode; the next desktop draw
           decodes ins.png (installer) or the selected desktop PNG. */
        wm_drop_wallpaper_cache();
        wm_invalidate_all();
    }
}
bool wm_is_installer_mode(void) { return inst_mode; }

/* INITIALIZATION */
void wm_init(int sw, int sh)
{
    scr_w = sw;  scr_h = sh;
    win_count = 0;  focus_win = 0;
    dragging = false;  resizing = false;
    bg_dirty = true;
    desktop_exit_reason = -1;
    dock_cnt = 0;  dock_hover = -1;
    tip_slot = -1;  tip_show = false;
    prev_lmb = false;  prev_rmb = false;
    wm_anim_tick = 0;

    /* Wallpaper decodes lazily on the first desktop draw, from the ISO
       file or the raw HDD slot selected by wm_read_wallpaper_file().
    */

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        windows[i].visible      = false;
        windows[i].focused      = false;
        windows[i].state        = WM_STATE_NORMAL;
        windows[i].z_order      = 0;
        windows[i].on_draw      = 0;
        windows[i].on_key       = 0;
        windows[i].on_mouse     = 0;
        windows[i].on_close     = 0;
        windows[i].user_data    = 0;
        windows[i].is_chromeless = false;
        windows[i].needs_redraw = true;
        windows[i].anim_phase   = 0;
        windows[i].opacity      = 255;
    }
    for (int i = 0; i < WM_MAX_DOCK_SLOTS; i++) {
        dock_sz[i] = WM_DOCK_ICON_BASE;
        dock_bounce[i] = 0;
    }
}

void wm_set_screen_size(int sw, int sh)
{
    scr_w = sw;  scr_h = sh;
    res_changed = true;  bg_dirty = true;

    extern void kfree(void *ptr);
    if (g_wallpaper_cache) {
        kfree(g_wallpaper_cache);
        g_wallpaper_cache = 0;
        g_wallpaper_cache_w = 0;
        g_wallpaper_cache_h = 0;
        wallpaper_rendered = false;
        g_wallpaper_has_image = false;
    }

    int mxy = sh - WM_TASKBAR_HEIGHT;
    for (int i = 0; i < win_count; i++) {
        Window *w = &windows[i];
        if (!w->visible) continue;
        if (w->x + w->w > sw) w->x = mx(0, sw - w->w);
        if (w->y + w->h > mxy) w->y = mx(0, mxy - w->h);
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
    }
}

void wm_set_resolution(int w, int h) {
    wm_set_screen_size(w, h);
    mouse_set_bounds(w, h);
}

bool wm_resolution_changed(void)
{
    bool r = res_changed;
    res_changed = false;
    return r;
}

/* WINDOW LIFECYCLE */
Window *wm_create_window(const char *title, int x, int y, int w, int h)
{
    if (win_count >= WM_MAX_WINDOWS) return 0;
    Window *win = &windows[win_count];
    scpy(win->title, title, 64);
    win->x = x;  win->y = y;  win->w = w;  win->h = h;
    win->saved_x = x;  win->saved_y = y;
    win->saved_w = w;  win->saved_h = h;
    win->min_w = 200;  win->min_h = 120;
    win->state = WM_STATE_NORMAL;
    win->visible = true;  win->focused = false;
    win->z_order = win_count;
    win->app_id = win_count;
    {
        /* every window gets a real process entry (task manager shows it).
         * Memory is attributed by heap-owner walk, not estimates.
        */
        sched_create_process(title, PROC_GUI, win);
    }
    win->is_chromeless = false;
    win->needs_redraw = true;
    win->on_draw = 0;  win->on_key = 0;
    win->on_mouse = 0; win->on_close = 0;
    win->user_data = 0;
    win->opacity = 255;
    win->anim_phase = 0;
    win_count++;
    wm_focus_window(win);
    bg_dirty = true;
    wm_invalidate_all();
    return win;
}

void wm_destroy_window(Window *win)
{
    if (!win) return;
    sched_destroy_by_owner(win);
    win->visible = false;
    win->state = WM_STATE_MINIMIZED;
    bg_dirty = true;
    if (focus_win == win) {
        focus_win = 0;
        int bz = -1; Window *bw = 0;
        for (int i = 0; i < win_count; i++) {
            if (windows[i].visible &&
                windows[i].state != WM_STATE_MINIMIZED &&
                &windows[i] != win &&
                windows[i].z_order > bz) {
                bz = windows[i].z_order; bw = &windows[i];
            }
        }
        if (bw) wm_focus_window(bw);
    }
}

/* one lifecycle entry point: GUI procs die with their window (which frees
   the sched entry via sched_destroy_by_owner), the rest via the scheduler.
   task manager calls only this.
*/
void wm_terminate_process(int pid) {
    Proc *p = sched_get(pid);
    if (!p) return;
    if (p->owner) wm_destroy_window((Window *)p->owner);
    else sched_destroy_process(pid);
}

/* desktop.c / login_screen.c owns the real login flow; weak default keeps
   the hotkey a no-op until the strong definition lands there.
*/
__attribute__((weak)) void wm_on_login_hotkey(void) { }

static void wm_toggle_show_desktop(void) {
    bool any_up = false;
    for (int i = 0; i < win_count; i++)
        if (windows[i].visible && windows[i].state != WM_STATE_MINIMIZED)
            any_up = true;
    if (any_up) {
        for (int i = 0; i < win_count; i++)
            if (windows[i].visible && windows[i].state != WM_STATE_MINIMIZED)
                wm_minimize_window(&windows[i]);
    } else {
        for (int i = 0; i < win_count; i++)
            if (windows[i].visible) {
                windows[i].state = WM_STATE_NORMAL;
                windows[i].needs_redraw = true;
            }
        bg_dirty = true;
        wm_invalidate_all();
    }
}

static void wm_paste_to(Window *win) {
    if (!win || !win->on_key) return;
    const char *s = clipboard_get();
    while (*s) {
        uint16_t k = (uint8_t)*s++;
        if (k == '\n') k = 13;
        sched_charge_elapsed();
        sched_set_current_by_owner(win);
        win->on_key(win, k);
        sched_charge_elapsed();
        sched_set_current(1);
    }
    wm_invalidate_windows();
}

// WINDOW MANIPULATION
void wm_focus_window(Window *win)
{
    if (!win || !win->visible) return;
    if (focus_win && focus_win != win) {
        focus_win->focused = false;
        focus_win->needs_redraw = true;
    }
    win->focused = true;
    win->needs_redraw = true;
    focus_win = win;
    wm_bring_to_front(win);
    wm_invalidate_all();
}

void wm_clear_focus(void)
{
    if (focus_win) {
        focus_win->focused = false;
        focus_win->needs_redraw = true;
    }
    focus_win = 0;
}

void wm_minimize_window(Window *win)
{
    if (!win) return;
    win->state = WM_STATE_MINIMIZED;
    bg_dirty = true;
    wm_invalidate_windows();
    if (focus_win == win) {
        focus_win = 0;
        int bz = -1; Window *bw = 0;
        for (int i = 0; i < win_count; i++) {
            if (windows[i].visible &&
                windows[i].state != WM_STATE_MINIMIZED &&
                &windows[i] != win &&
                windows[i].z_order > bz) {
                bz = windows[i].z_order; bw = &windows[i];
            }
        }
        if (bw) wm_focus_window(bw);
    }
}

void wm_maximize_window(Window *win)
{
    if (!win) return;
    if (win->state == WM_STATE_MAXIMIZED) {
        wm_restore_window(win); return;
    }
    win->saved_x = win->x;  win->saved_y = win->y;
    win->saved_w = win->w;  win->saved_h = win->h;
    int top = (g_settings.window_style == 0) ? WM_MENUBAR_HEIGHT : 0;
    win->x = 0;  win->y = top;
    win->w = scr_w;
    win->h = scr_h - WM_TASKBAR_HEIGHT - top;
    win->state = WM_STATE_MAXIMIZED;
    win->needs_redraw = true;
    bg_dirty = true;
    wm_invalidate_all();
}

void wm_restore_window(Window *win)
{
    if (!win) return;
    if (win->state == WM_STATE_MAXIMIZED) {
        win->x = win->saved_x;  win->y = win->saved_y;
        win->w = win->saved_w;  win->h = win->saved_h;
    }
    win->state = WM_STATE_NORMAL;
    win->needs_redraw = true;
    bg_dirty = true;
    wm_invalidate_all();
}

void wm_bring_to_front(Window *win)
{
    if (!win) return;
    int oz = win->z_order;
    for (int i = 0; i < win_count; i++)
        if (windows[i].z_order > oz) windows[i].z_order--;
    win->z_order = win_count - 1;
}

/* AREA GEOMETRY */
int wm_client_x(Window *w) { return w->x; }
int wm_client_y(Window *w) { return w->is_chromeless ? w->y : w->y + WM_TITLEBAR_HEIGHT; }
int wm_client_w(Window *w) { return w->w; }
int wm_client_h(Window *w) { return w->is_chromeless ? w->h : w->h - WM_TITLEBAR_HEIGHT; }

/* HELPERS (client-relative, clipped) */
void wm_draw_pixel(Window *w, int x, int y, uint32_t c)
{
    int sx = wm_client_x(w) + x, sy = wm_client_y(w) + y;
    int cx = wm_client_x(w), cy = wm_client_y(w);
    if (sx >= cx && sx < cx + wm_client_w(w) &&
        sy >= cy && sy < cy + wm_client_h(w))
        gpu_draw_pixel(sx, sy, c);
}

void wm_fill_rect(Window *w, int x, int y, int rw, int rh, uint32_t c)
{
    int cx = wm_client_x(w), cy = wm_client_y(w);
    int cw = wm_client_w(w), ch = wm_client_h(w);
    int sx = cx + x, sy = cy + y, sw = rw, sh = rh;
    if (sx < cx) { sw -= (cx - sx); sx = cx; }
    if (sy < cy) { sh -= (cy - sy); sy = cy; }
    if (sx + sw > cx + cw) sw = cx + cw - sx;
    if (sy + sh > cy + ch) sh = cy + ch - sy;
    if (sw > 0 && sh > 0) gpu_fill_rect(sx, sy, sw, sh, c);
}

void wm_draw_rect(Window *w, int x, int y, int rw, int rh, uint32_t c)
{
    wm_fill_rect(w, x, y, rw, 1, c);
    wm_fill_rect(w, x, y + rh - 1, rw, 1, c);
    wm_fill_rect(w, x, y, 1, rh, c);
    wm_fill_rect(w, x + rw - 1, y, 1, rh, c);
}

void wm_draw_char(Window *w, int x, int y, uint8_t ch,
                  uint32_t fg, uint32_t bg)
{
    int sx = wm_client_x(w) + x, sy = wm_client_y(w) + y;
    int cx = wm_client_x(w), cy = wm_client_y(w);
    if (sx >= cx && sx + 8 <= cx + wm_client_w(w) &&
        sy >= cy && sy + 8 <= cy + wm_client_h(w))
        gpu_draw_char(sx, sy, ch, fg, bg);
}

void wm_draw_string(Window *w, int x, int y, const char *s,
                    uint32_t fg, uint32_t bg)
{
    int px = x;
    while (*s) { wm_draw_char(w, px, y, (uint8_t)*s, fg, bg); px += 8; s++; }
}

void wm_fill_rect_blend(Window *w, int x, int y, int rw, int rh,
                        uint32_t c, uint8_t a)
{
    int cx = wm_client_x(w), cy = wm_client_y(w);
    int cw = wm_client_w(w), ch = wm_client_h(w);
    int sx = cx + x, sy = cy + y, sw = rw, sh = rh;
    if (sx < cx) { sw -= (cx - sx); sx = cx; }
    if (sy < cy) { sh -= (cy - sy); sy = cy; }
    if (sx + sw > cx + cw) sw = cx + cw - sx;
    if (sy + sh > cy + ch) sh = cy + ch - sy;
    if (sw > 0 && sh > 0) gpu_fill_rect_blend(sx, sy, sw, sh, c, a);
}

void wm_draw_rounded_rect(Window *w, int x, int y, int rw, int rh,
                          uint32_t c, int r)
{
    rrect(wm_client_x(w) + x, wm_client_y(w) + y, rw, rh, c, r);
}

/* ORDER / HIT TEST */
static Window *at_z(int z)
{
    for (int i = 0; i < win_count; i++)
        if (windows[i].z_order == z) return &windows[i];
    return 0;
}

static Window *win_at(int px, int py)
{
    Window *r = 0; int bz = -1;
    for (int i = 0; i < win_count; i++) {
        Window *w = &windows[i];
        if (!w->visible || w->state == WM_STATE_MINIMIZED) continue;
        if (px >= w->x && px < w->x + w->w &&
            py >= w->y && py < w->y + w->h && w->z_order > bz) {
            bz = w->z_order; r = w;
        }
    }
    return r;
}

/* PREMIUM WINDOW DRAWING */
static void draw_window(Window *win)
{
    if (!win->visible || win->state == WM_STATE_MINIMIZED)
        return;

    if (win->is_chromeless) {
        if (win->on_draw) {
            sched_charge_elapsed();
            sched_set_current_by_owner(win);
            win->on_draw(win);
            sched_charge_elapsed();   /* the draw itself bills to the window, not idle */
            sched_set_current(0);   /* idle: waiting ticks go to pid 0, not kernel */
        }
        return;
    }

    bool focused = (win == focus_win);
    bool is_max  = (win->state == WM_STATE_MAXIMIZED);
    int  cr      = (g_settings.window_style == 0 && !is_max)
                       ? WM_CORNER_RADIUS : 0;
    bool glass   = g_settings.dock_transparent;

    /* 1. Premium multi-layer shadow */
    if (!is_max)
        draw_shadow(win->x, win->y, win->w, win->h, focused, cr);

    /* 2. Window body with vibrancy */
    if (glass && focused)
    {
        /* Focused: full glass effect with blur */
        gpu_blur_rect(win->x, win->y, win->w, win->h);

        /* Vibrancy tint over blur */
        if (cr > 0)
            rrect_a(win->x, win->y, win->w, win->h,
                    MAT_DARK_BASE, cr, 175);
        else
            gpu_fill_rect_blend(win->x, win->y, win->w, win->h,
                                MAT_DARK_BASE, 175);

        /* Top highlight gradient (frosted shine) */
        for (int i = 0; i < 40 && i < win->h; i++) {
            uint8_t a = 50 - i;
            if (a > 0) {
                int ins = (i < cr) ? corner_inset(cr, i) : 0;
                int sw = win->w - 2 * ins;
                if (sw > 0)
                    gpu_fill_rect_blend(win->x + ins, win->y + i, sw, 1,
                                        0xFFFFFFFF, a);
            }
        }
    }
    else
    {
        /* Unfocused or opaque: solid material with subtle gradient */
        uint32_t body_top = focused ? MAT_DARK_ELEVATED : MAT_DARK_TINT;
        uint32_t body_bot = focused ? MAT_DARK_TINT : MAT_DARK_BASE;

        if (cr > 0) {
            /* Draw gradient body with rounded corners */
            int bh = win->h;
            for (int row = 0; row < bh; row++) {
                uint8_t t = (row * 255) / (bh > 1 ? bh - 1 : 1);
                uint32_t c = lerp_color(body_top, body_bot, t);
                int ins = 0;
                if (row < cr) ins = corner_inset(cr, row);
                else if (row >= bh - cr) ins = corner_inset(cr, bh - 1 - row);
                int sw = win->w - 2 * ins;
                if (sw > 0) gpu_fill_rect(win->x + ins, win->y + row, sw, 1, c);
            }
        } else {
            rrect_gradient(win->x, win->y, win->w, win->h, body_top, body_bot, 0);
        }
    }

    /* 3. Subtle border highlight (1px top rim) */
    if (cr > 0) {
        int ins0 = corner_inset(cr, 0);
        int sw0 = win->w - 2 * ins0;
        if (sw0 > 0)
            gpu_fill_rect_blend(win->x + ins0, win->y, sw0, 1,
                                0xFFFFFFFF, focused ? 60 : 30);
    } else {
        gpu_fill_rect_blend(win->x, win->y, win->w, 1,
                            0xFFFFFFFF, focused ? 60 : 30);
    }

    /* 4. Titlebar (subtle overlay) */
    {
        /* Gradient overlay on titlebar for depth */
        uint32_t tb_top = focused ? 0xFF404044 : 0xFF323236;
        uint32_t tb_bot = focused ? 0xFF353539 : 0xFF2A2A2E;
        uint8_t tb_alpha = glass && focused ? 100 : 230;

        if (cr > 0) {
            int tbh = WM_TITLEBAR_HEIGHT;
            for (int row = 0; row < tbh; row++) {
                uint8_t t = (row * 255) / (tbh - 1);
                uint32_t c = lerp_color(tb_top, tb_bot, t);
                int ins = (row < cr) ? corner_inset(cr, row) : 0;
                int sw = win->w - 2 * ins;
                if (sw > 0) {
                    if (glass && focused)
                        gpu_fill_rect_blend(win->x + ins, win->y + row, sw, 1, c, tb_alpha);
                    else
                        gpu_fill_rect(win->x + ins, win->y + row, sw, 1, c);
                }
            }
        } else {
            rrect_gradient(win->x, win->y, win->w, WM_TITLEBAR_HEIGHT, tb_top, tb_bot, 0);
        }
    }

    /* 5. Separator line (subtle) */
    gpu_fill_rect_blend(win->x, win->y + WM_TITLEBAR_HEIGHT - 1,
                        win->w, 1, 0xFF000000, 90);
    gpu_fill_rect_blend(win->x, win->y + WM_TITLEBAR_HEIGHT,
                        win->w, 1, 0xFFFFFFFF, focused ? 25 : 12);

    /* 6. Title text with shadow */
    {
        int tlen = slen(win->title);
        int tx = win->x + (win->w - tlen * 8) / 2;
        int ty = win->y + (WM_TITLEBAR_HEIGHT - 8) / 2;

        if (g_settings.window_style == 0) {
            int min_tx = win->x + WM_TL_LEFT_PAD + WM_TL_SPACING * 3 + 12;
            if (tx < min_tx) tx = min_tx;
        }

        /* Subtle text shadow for depth */
        if (focused)
            gpu_draw_string(tx, ty + 1, (const uint8_t *)win->title,
                            0xFF000000, 0);

        uint32_t tfg = focused ? TEXT_PRIMARY : TEXT_SECONDARY;
        gpu_draw_string(tx, ty, (const uint8_t *)win->title, tfg, 0);
    }

    /* 7. Window buttons */
    if (g_settings.window_style == 0)
        draw_traffic_lights(win, mouse_get_x(), mouse_get_y());
    else
        draw_win_buttons(win);

    /* 8. Client area */
    {
        int cx = wm_client_x(win);
        int cy = wm_client_y(win);
        int cw = wm_client_w(win);
        int ch = wm_client_h(win);

        if (glass && focused)
        {
            /* Translucent client over blurred bg */
            uint32_t ct = WM_COLOR_WINDOW_BG;
            uint8_t  ca = 210;
            if (cr > 0 && ch > cr) {
                gpu_fill_rect_blend(cx, cy, cw, ch - cr, ct, ca);
                rrect_bot_a(cx, cy + ch - cr, cw, cr, ct, cr, ca);
            } else {
                gpu_fill_rect_blend(cx, cy, cw, ch, ct, ca);
            }
        }
        else
        {
            if (cr > 0 && ch > cr) {
                gpu_fill_rect(cx, cy, cw, ch - cr, WM_COLOR_WINDOW_BG);
                rrect_bot(cx, cy + ch - cr, cw, cr, WM_COLOR_WINDOW_BG, cr);
            } else {
                gpu_fill_rect(cx, cy, cw, ch, WM_COLOR_WINDOW_BG);
            }
        }
    }

    /* 9. App content */
    if (win->on_draw) {
        sched_charge_elapsed();
        sched_set_current_by_owner(win);
        win->on_draw(win);
        sched_charge_elapsed();   /* same fix: close the window's interval before idling */
        sched_set_current(0);   /* idle: waiting ticks go to pid 0, not kernel */
    }

    win->needs_redraw = false;
}

/* DESKTOP BACKGROUND */
void wm_mark_dirty(void) { wm_full_redraw = true; }

void wm_invalidate_bg(void) {
    wm_mark_dirty();
    bg_dirty = true;
    wm_bg_dirty = true;
    wm_full_redraw = true;
}

void wm_invalidate_dock(void) { wm_mark_dirty(); wm_dock_dirty = true; }
void wm_invalidate_windows(void) { wm_mark_dirty(); wm_windows_dirty = true; }
void wm_invalidate_cursor(void) { wm_mark_dirty(); wm_cursor_dirty = true; }

void wm_invalidate_all(void) {
    wm_mark_dirty();
    wm_full_redraw = true;
    wm_bg_dirty = true;
    wm_dock_dirty = true;
    wm_windows_dirty = true;
    wm_cursor_dirty = true;
}

void wm_draw_desktop_bg(void)
{
    if (!wm_bg_dirty && !wm_full_redraw && !g_settings.dock_transparent) return;

    bg_dirty = false;
    wm_bg_dirty = false;
    wm_windows_dirty = true;
    if (!wm_full_redraw) wm_dock_dirty = true;

    /* Wallpaper PNG first; gradient when no file is readable. */
    if (wm_render_wallpaper(scr_w, scr_h))
        return;


    /* PREMIUM gradient background with color richness */
    int dh = scr_h;
    int bands = 32;
    int bh = dh / bands;
    if (bh < 1) bh = 1;

    /* Rich dark gradient (deep navy to purple-black) */
    uint32_t top_color = 0xFF1A1A2E;
    uint32_t mid_color = 0xFF16213E;
    uint32_t bot_color = 0xFF0F0F1E;

    for (int i = 0; i < bands; i++) {
        int y = i * bh;
        int h = (i == bands - 1) ? (dh - y) : bh;
        uint8_t t = (i * 255) / bands;
        uint32_t c;
        if (t < 128) c = lerp_color(top_color, mid_color, t * 2);
        else c = lerp_color(mid_color, bot_color, (t - 128) * 2);
        gpu_fill_rect(0, y, scr_w, h, c);
    }

    /* Subtle radial glow center-top */
    int gx = scr_w / 2;
    int gy = scr_h / 3;
    for (int r = 400; r > 0; r -= 40) {
        uint8_t a = 3 + (400 - r) / 20;
        gpu_fill_rect_blend(gx - r, gy - r/2, r * 2, r, ACCENT_BLUE, a);
    }

    /* Centered premium branding */
    const char *label = "Aurion OS";
    int lx = (scr_w - slen(label) * 8) / 2;
    int ly = dh / 2 - 20;
    gpu_draw_string(lx + 1, ly + 1, (const uint8_t *)label, 0xFF000000, 0);
    gpu_draw_string(lx,     ly,     (const uint8_t *)label, TEXT_PRIMARY, 0);
}

/* MENU BAR */
void wm_draw_menubar(void)
{
    extern void menubar_draw(void);
    menubar_draw();
}

/* PREMIUM DOCK — with glow, reflection, and smooth animations */
void wm_dock_add(const char *name, icon_draw_fn fn)
{
    if (dock_cnt >= WM_MAX_DOCK_SLOTS) return;
    int i = 0;
    while (name[i] && i < 63) { dock_names[dock_cnt][i] = name[i]; i++; }
    dock_names[dock_cnt][i] = 0;
    dock_fns[dock_cnt] = fn;
    dock_sz[dock_cnt] = WM_DOCK_ICON_BASE;
    dock_bounce[dock_cnt] = 0;
    dock_cnt++;
}

void wm_dock_clear(void) { dock_cnt = 0; dock_hover = -1; }

int wm_dock_slot_count(void) { return dock_cnt; }

static bool dock_is_open(const char *n)
{
    for (int i = 0; i < win_count; i++) {
        if (!windows[i].visible ||
            windows[i].state == WM_STATE_MINIMIZED) continue;
        if (seq(windows[i].title, n)) return true;
    }
    return false;
}

static void dock_layout(int *out_w, int *out_h, int *out_x, int *out_y)
{
    int tw = 0;
    for (int i = 0; i < dock_cnt; i++) tw += dock_sz[i];
    tw += (dock_cnt - 1) * WM_DOCK_PAD + 32;
    int dh = WM_DOCK_PILL_H;
    int dx = (scr_w - tw) / 2;
    int dy = scr_h - dh - WM_DOCK_BOTTOM_GAP;
    *out_w = tw;  *out_h = dh;  *out_x = dx;  *out_y = dy;
}

static void dock_animate(void)
{
    if (dock_hover < 0 && !g_settings.dock_magnification) {
        for (int i = 0; i < dock_cnt; i++)
            dock_sz[i] = WM_DOCK_ICON_BASE;
        return;
    }

    for (int i = 0; i < dock_cnt; i++) {
        int target = WM_DOCK_ICON_BASE;
        if (g_settings.dock_magnification && dock_hover >= 0) {
            int d = ab(i - dock_hover);
            if (d == 0)
                target = WM_DOCK_ICON_MAG;
            else if (d == 1)
                target = WM_DOCK_ICON_BASE +
                         (WM_DOCK_ICON_MAG - WM_DOCK_ICON_BASE) * 7 / 10;
            else if (d == 2)
                target = WM_DOCK_ICON_BASE +
                         (WM_DOCK_ICON_MAG - WM_DOCK_ICON_BASE) * 3 / 10;
        }
        int diff = target - dock_sz[i];
        if (diff) {
            /* Smoother easing */
            int step = diff / 4;
            if (!step) step = diff > 0 ? 1 : -1;
            dock_sz[i] += step;
            wm_dock_dirty = true;
        }
    }
}

static void dock_slot(int sx, int sy, int sz, icon_draw_fn fn,
                      bool open, bool hov)
{
    /* macOS: magnification is the dock_sz scale itself; the only hover
     * chrome is a single soft under-glow, not a stacked white plate.
    */
    if (hov) {
        rrect_a(sx - 4, sy - 4, sz + 8, sz + 8, 0xFFFFFFFF, 14, 14);
    }

    if (fn) {
        int icon_sz = WM_DOCK_ICON_BASE;
        int ix = sx + (sz - icon_sz) / 2;
        int iy = sy + (sz - icon_sz) / 2;
        fn(ix, iy);
    }

    /* Premium indicator dot for running apps */
    if (open) {
        int ix = sx + sz / 2;
        int iy = sy + sz + 6;
        /* Glow */
        circ_a(ix, iy, 3, ACCENT_GLOW, 80);
        /* Core dot */
        circ(ix, iy, 2, 0xFFFFFFFF);
    }
}

static void dock_tooltip(int cx, int top_y, const char *name)
{
    int len = slen(name);
    int tw  = len * 8 + 20;
    int th  = 26;
    int tx  = cl(cx - tw / 2, 2, scr_w - tw - 2);
    int ty  = top_y - th - 14;

    /* Shadow below tooltip */
    rrect_a(tx + 2, ty + 3, tw, th, 0xFF000000, 13, 100);

    /* Tooltip body - very dark with subtle border */
    rrect_a(tx, ty, tw, th, 0xFF0A0A0F, 13, 245);

    /* Top shine */
    gpu_fill_rect_blend(tx + 10, ty, tw - 20, 1, 0xFFFFFFFF, 40);

    /* Border */
    rrect_a(tx, ty, tw, th, 0xFFFFFFFF, 13, 15);

    gpu_draw_string(tx + 10, ty + 9,
                    (const uint8_t *)name, TEXT_PRIMARY, 0);
}

void wm_draw_taskbar(void)
{
    if (focus_win && focus_win->is_chromeless) return;
    if (dock_cnt == 0) return;

    dock_animate();
    wm_anim_tick++;

    if (!wm_dock_dirty && !wm_full_redraw) return;

    int tw, dh, dx, dy;
    dock_layout(&tw, &dh, &dx, &dy);

    int radius = WM_DOCK_CORNER;

    /* PREMIUM DOCK PILL */

    /* Shadow beneath dock */
    gpu_fill_rect_blend(dx - 6, dy + dh - 4, tw + 12, 12, 0xFF000000, 40);
    gpu_fill_rect_blend(dx - 2, dy + dh, tw + 4, 8, 0xFF000000, 60);

    if (g_settings.dock_transparent)
    {
        if (vmware_svga_available()) {
            /* Full glass with blur */
            gpu_blur_rect(dx, dy, tw, dh);

            /* Gradient glass tint */
            uint32_t glass_top = 0xFF2A2A30;
            uint32_t glass_bot = 0xFF18181C;
            for (int row = 0; row < dh; row++) {
                uint8_t t = (row * 255) / (dh - 1);
                uint32_t c = lerp_color(glass_top, glass_bot, t);
                int ins = 0;
                if (row < radius) ins = corner_inset(radius, row);
                else if (row >= dh - radius) ins = corner_inset(radius, dh - 1 - row);
                int sw = tw - 2 * ins;
                if (sw > 0)
                    gpu_fill_rect_blend(dx + ins, dy + row, sw, 1, c, 180);
            }

            /* Top shine */
            gpu_fill_rect_blend(dx + radius, dy,
                                tw - 2 * radius, 1, 0xFFFFFFFF, 60);
            gpu_fill_rect_blend(dx + radius, dy + 1,
                                tw - 2 * radius, 1, 0xFFFFFFFF, 25);

            /* Side highlights */
            gpu_fill_rect_blend(dx, dy + radius, 1,
                                dh - 2 * radius, 0xFFFFFFFF, 20);
            gpu_fill_rect_blend(dx + tw - 1, dy + radius, 1,
                                dh - 2 * radius, 0xFFFFFFFF, 20);

            /* Bottom shadow inside */
            gpu_fill_rect_blend(dx + radius, dy + dh - 1,
                                tw - 2 * radius, 1, 0xFF000000, 100);
        } else {
            /* QEMU-safe path with gradient */
            uint32_t glass_top = 0xFF22222A;
            uint32_t glass_bot = 0xFF0E0E14;
            for (int row = 0; row < dh; row++) {
                uint8_t t = (row * 255) / (dh - 1);
                uint32_t c = lerp_color(glass_top, glass_bot, t);
                int ins = 0;
                if (row < radius) ins = corner_inset(radius, row);
                else if (row >= dh - radius) ins = corner_inset(radius, dh - 1 - row);
                int sw = tw - 2 * ins;
                if (sw > 0)
                    gpu_fill_rect_blend(dx + ins, dy + row, sw, 1, c, 215);
            }
            gpu_fill_rect_blend(dx + radius, dy,
                                tw - 2 * radius, 1, 0xFFFFFFFF, 50);
        }
    }
    else
    {
        /* Opaque gradient dock */
        rrect_gradient(dx, dy, tw, dh, 0xFF1E1E24, 0xFF0A0A0E, radius);
        gpu_fill_rect(dx + radius, dy, tw - 2 * radius, 1, 0xFF333338);
    }

    /* Icons with premium hover effects */
    int sx = dx + 16;
    for (int i = 0; i < dock_cnt; i++) {
        int sz = dock_sz[i];
        int sy = dy + (dh - sz) / 2;
        if (sy < 0) sy = 0;
        if (sy + sz > scr_h) sy = scr_h - sz;
        bool hov = (i == dock_hover);
        bool op  = dock_is_open(dock_names[i]);

        dock_slot(sx, sy, sz, dock_fns[i], op, hov);

        if (hov && tip_show)
            dock_tooltip(sx + sz / 2, dy, dock_names[i]);

        sx += sz + WM_DOCK_PAD;
    }

    /* Clock (Windows-style only) */
    if (g_settings.window_style != 0) {
        uint8_t hh, mm, ss;
        sys_get_time(&hh, &mm, &ss);
        char clk[9];
        clk[0] = '0' + hh / 10; clk[1] = '0' + hh % 10; clk[2] = ':';
        clk[3] = '0' + mm / 10; clk[4] = '0' + mm % 10; clk[5] = ':';
        clk[6] = '0' + ss / 10; clk[7] = '0' + ss % 10; clk[8] = 0;
        rrect_a(scr_w - 104, 6, 98, 26, 0xFF0A0A12, 13, 200);
        gpu_fill_rect_blend(scr_w - 100, 6, 90, 1, 0xFFFFFFFF, 30);
        gpu_draw_string(scr_w - 94, 13,
                        (const uint8_t *)clk, TEXT_PRIMARY, 0);
    }

    wm_dock_dirty = false;
}

bool wm_dock_handle(int mmx, int mmy, bool click)
{
    if (dock_cnt == 0) return false;

    int tw, dh, dx, dy;
    dock_layout(&tw, &dh, &dx, &dy);

    int ext = g_settings.dock_magnification ? 20 : 4;
    if (mmx < dx - ext || mmx > dx + tw + ext ||
        mmy < dy - ext || mmy > dy + dh + ext) {
        if (dock_hover != -1) {
            dock_hover = -1; tip_show = false; tip_slot = -1;
            wm_dock_dirty = true;
        }
        return false;
    }

    int sx = dx + 16;
    int found = -1;
    for (int i = 0; i < dock_cnt; i++) {
        int sz = dock_sz[i];
        int sy = dy + (dh - sz) / 2;
        if (mmx >= sx && mmx < sx + sz && mmy >= sy && mmy < sy + sz) {
            found = i;
            break;
        }
        sx += sz + WM_DOCK_PAD;
    }

    if (found >= 0) {
        if (dock_hover != found) {
            wm_dock_dirty = true;
            dock_hover = found;
        }
        if (found != tip_slot) {
            tip_slot = found; tip_tick = get_ticks(); tip_show = false;
        } else if (!tip_show && (get_ticks() - tip_tick) > 30) {
            tip_show = true;
            wm_dock_dirty = true;
        }
        if (click) dock_clicked_slot = found;
        return true;
    }

    if (dock_hover != -1) {
        dock_hover = -1; tip_show = false; tip_slot = -1;
        wm_dock_dirty = true;
    }
    return false;
}

/* PREMIUM CURSOR */
void wm_draw_cursor(int mmx, int mmy)
{
    if (mmx != wm_prev_cursor_x || mmy != wm_prev_cursor_y) {
        wm_cursor_dirty = true;
        wm_prev_cursor_x = mmx;
        wm_prev_cursor_y = mmy;
    }
    /* Arrow silhouette in half-pixel units, scaled by the Settings
     * multiplier. A single tip and parallel diagonal stem edges keep
     * the shape consistent at every size. The outer white ring is
     * separate from the black fill so it cannot cut into the stem.
    */

    static const int16_t pts[] = {
         0, 0,   0,30,   8,22,  14,36,  20,34,  14,20,  24,20
    };
#define CUR_NPTS ((int)(sizeof(pts) / sizeof(pts[0]) / 2))
#define CUR_MAXW 43   /* 24 half-units * 3 / 2 + 2*3 ring cols + 1 */
#define CUR_MAXH 61   /* 36 half-units * 3 / 2 + 2*3 ring rows + 1 */
    static uint8_t mask[CUR_MAXW * CUR_MAXH];   /* one cursor, never reentrant */
    int cs = g_settings.cursor_size + 1;
    if (cs < 1 || cs > 3)
        cs = 1;

    int sx[CUR_NPTS], sy[CUR_NPTS];
    int minx = 0x7FFFFFFF, maxx = -0x7FFFFFFF;
    int miny = 0x7FFFFFFF, maxy = -0x7FFFFFFF;
    for (int i = 0; i < CUR_NPTS; i++) {
        sx[i] = pts[i * 2] * cs;
        sy[i] = pts[i * 2 + 1] * cs;
        if (sx[i] < minx) minx = sx[i];
        if (sx[i] > maxx) maxx = sx[i];
        if (sy[i] < miny) miny = sy[i];
        if (sy[i] > maxy) maxy = sy[i];
    }
    /* Bbox in output px, cs ring pixels of margin on every side. */
    int x0 = mmx + minx / 2 - cs;
    int x1 = mmx + (maxx + 1) / 2 + cs;
    int y0 = mmy + miny / 2 - cs;
    int y1 = mmy + (maxy + 1) / 2 + cs;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w > CUR_MAXW || h > CUR_MAXH)
        return;   /* unreachable at cs<=3; guards the mask index math */
    for (int i = 0; i < w * h; i++)
        mask[i] = 0;

    /* Even-odd spans per row, in half-units, at row-center y2. */
    for (int ry = y0; ry <= y1; ry++) {
        int y2 = (ry - mmy) * 2 + 1;
        int xs[CUR_NPTS];
        int n = 0;
        for (int i = 0; i < CUR_NPTS; i++) {
            int j = (i + 1) % CUR_NPTS;
            int ya = sy[i], yb = sy[j];
            if ((ya <= y2 && yb > y2) || (yb <= y2 && ya > y2)) {
                int xa = sx[i], xb = sx[j];
                xs[n++] = xa + (xb - xa) * (y2 - ya) / (yb - ya);
            }
        }
        for (int i = 1; i < n; i++) {
            int k = xs[i], j = i - 1;
            while (j >= 0 && xs[j] > k) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = k;
        }
        /* Span [xa,xb) half-units covers px whose center (px*2+1) falls
           inside: pa = mmx + xa/2, pb = mmx + xb/2 - 1, both floor. */
        for (int i = 0; i + 1 < n; i += 2) {
            int pa = mmx + xs[i] / 2;
            int pb = mmx + xs[i + 1] / 2 - 1;
            if (pa < x0) pa = x0;
            if (pb > x1) pb = x1;
            for (int px = pa; px <= pb; px++)
                mask[(ry - y0) * w + (px - x0)] = 1;
        }
    }

    /* Ring = outside pixels within Chebyshev distance cs of fill;
     * black fill and white ring are disjoint, order irrelevant. */
    for (int ry = y0; ry <= y1; ry++) {
        if (ry < 0 || ry >= scr_h) continue;
        for (int rx = x0; rx <= x1; rx++) {
            if (rx < 0 || rx >= scr_w) continue;
            int gx = rx - x0, gy = ry - y0;
            uint32_t c;
            if (mask[gy * w + gx]) {
                c = 0xFF000000;
            } else {
                int ring = 0;
                for (int dy = -cs; dy <= cs && !ring; dy++) {
                    int ny = gy + dy;
                    if (ny < 0 || ny >= h) continue;
                    for (int dx = -cs; dx <= cs; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = gx + dx;
                        if (nx >= 0 && nx < w && mask[ny * w + nx]) {
                            ring = 1;
                            break;
                        }
                    }
                }
                if (!ring) continue;
                c = 0xFFFFFFFF;
            }
            gpu_draw_pixel(rx, ry, c);
        }
    }
#undef CUR_NPTS
#undef CUR_MAXW
#undef CUR_MAXH

    wm_cursor_dirty = false;
}

/* INPUT HANDLING (unchanged logic, just clean) */
static int tl_hit(Window *w, int px, int py)
{
    int cy  = w->y + WM_TITLEBAR_HEIGHT / 2;
    int cx0 = w->x + WM_TL_LEFT_PAD;
    int cx1 = cx0 + WM_TL_SPACING;
    int cx2 = cx1 + WM_TL_SPACING;
    int hr2 = (WM_TL_RADIUS + 4) * (WM_TL_RADIUS + 4);
    int d;
    d = (px-cx0)*(px-cx0) + (py-cy)*(py-cy); if (d <= hr2) return 0;
    d = (px-cx1)*(px-cx1) + (py-cy)*(py-cy); if (d <= hr2) return 1;
    d = (px-cx2)*(px-cx2) + (py-cy)*(py-cy); if (d <= hr2) return 2;
    return -1;
}

static int wb_hit(Window *w, int px, int py)
{
    int bh = WM_WIN_BTN_H, bw = WM_WIN_BTN_W;
    int by = w->y + (WM_TITLEBAR_HEIGHT - bh) / 2;
    if (py < by || py >= by + bh) return -1;
    int cx = w->x + w->w - bw - 4;
    if (px >= cx && px < cx + bw) return 0;
    cx -= bw + 2;
    if (px >= cx && px < cx + bw) return 2;
    cx -= bw + 2;
    if (px >= cx && px < cx + bw) return 1;
    return -1;
}

void wm_handle_input(void)
{
    int  mmx     = mouse_get_x();
    int  mmy     = mouse_get_y();
    bool lmb     = mouse_get_left();
    bool rmb     = mouse_get_right();
    bool click   = lmb && !prev_lmb;
    bool release = !lmb && prev_lmb;
    bool rclick  = rmb && !prev_rmb;

    int tbar_y = scr_h - WM_TASKBAR_HEIGHT;
    int min_wy = (g_settings.window_style == 0) ? WM_MENUBAR_HEIGHT : 0;

    if (dragging && drag_win) {
        if (lmb) {
            drag_tx = mmx - drag_ox;
            drag_ty = cl(mmy - drag_oy, min_wy,
                         tbar_y - WM_TITLEBAR_HEIGHT);
            int dx = drag_tx - drag_win->x;
            int dy = drag_ty - drag_win->y;
            int sx = dx * 3 / 5;
            int sy = dy * 3 / 5;
            if (!sx && dx) sx = dx > 0 ? 1 : -1;
            if (!sy && dy) sy = dy > 0 ? 1 : -1;
            drag_win->x += sx;
            drag_win->y += sy;
            if (ab(drag_win->x - drag_tx) <= 1) drag_win->x = drag_tx;
            if (ab(drag_win->y - drag_ty) <= 1) drag_win->y = drag_ty;
            wm_bg_dirty = true;
            bg_dirty = true;
            wm_invalidate_windows();
        } else {
            drag_win->x = drag_tx;
            drag_win->y = drag_ty;
            dragging = false; drag_win = 0;
        }
        prev_lmb = lmb; prev_rmb = rmb; return;
    }

    if (resizing && rsz_win) {
        if (lmb) {
            if (rsz_edge & 1) {
                int nw = mmx - rsz_win->x;
                if (nw >= rsz_win->min_w) rsz_win->w = nw;
            }
            if (rsz_edge & 2) {
                int nh = mmy - rsz_win->y;
                if (nh >= rsz_win->min_h &&
                    rsz_win->y + nh < tbar_y) rsz_win->h = nh;
            }
            rsz_win->needs_redraw = true;
            bg_dirty = true;
            wm_invalidate_windows();
        } else {
            resizing = false; rsz_win = 0;
        }
        prev_lmb = lmb; prev_rmb = rmb; return;
    }

    if (click) {
        if (mmy >= tbar_y) {
            prev_lmb = lmb; prev_rmb = rmb; return;
        }
        if (g_settings.window_style == 0 && mmy < WM_MENUBAR_HEIGHT) {
            prev_lmb = lmb; prev_rmb = rmb; return;
        }

        Window *cw = win_at(mmx, mmy);
        if (cw) {
            wm_focus_window(cw);

            uint32_t now = get_ticks();
            bool dbl = (cw == dbl_win && (now - dbl_tick) < 40);
            dbl_tick = now; dbl_win = cw;

            if (mmy >= cw->y && mmy < cw->y + WM_TITLEBAR_HEIGHT) {
                if (dbl) {
                    if (cw->state == WM_STATE_MAXIMIZED)
                        wm_restore_window(cw);
                    else
                        wm_maximize_window(cw);
                    dbl_tick = 0;
                    prev_lmb = lmb; prev_rmb = rmb; return;
                }

                int btn = g_settings.window_style == 0
                              ? tl_hit(cw, mmx, mmy)
                              : wb_hit(cw, mmx, mmy);
                if (btn >= 0) {
                    if (btn == 0) {
                        if (cw->on_close) cw->on_close(cw);
                        wm_destroy_window(cw);
                    } else if (btn == 1) {
                        wm_minimize_window(cw);
                    } else if (btn == 2) {
                        wm_maximize_window(cw);
                    }
                    prev_lmb = lmb; prev_rmb = rmb; return;
                }

                if (cw->state != WM_STATE_MAXIMIZED) {
                    dragging = true; drag_win = cw;
                    drag_ox = mmx - cw->x; drag_oy = mmy - cw->y;
                    drag_tx = cw->x; drag_ty = cw->y;
                    prev_lmb = lmb; prev_rmb = rmb; return;
                }
            }

            if (cw->state != WM_STATE_MAXIMIZED) {
                int edge = 0;
                if (mmx >= cw->x + cw->w - 6) edge |= 1;
                if (mmy >= cw->y + cw->h - 6) edge |= 2;
                if (edge) {
                    resizing = true; rsz_edge = edge; rsz_win = cw;
                    prev_lmb = lmb; prev_rmb = rmb; return;
                }
            }

            if (cw->on_mouse)
                cw->on_mouse(cw, mmx - wm_client_x(cw),
                             mmy - wm_client_y(cw), true, false);
            wm_invalidate_windows();
        }
    }

    if (rclick) {
        Window *rw = win_at(mmx, mmy);
        if (rw && rw->on_mouse && mmy >= wm_client_y(rw)) {
            wm_focus_window(rw);
            rw->on_mouse(rw, mmx - wm_client_x(rw),
                         mmy - wm_client_y(rw), false, true);
            wm_invalidate_windows();
        }
    }

    if (lmb && focus_win && focus_win->on_mouse &&
        !dragging && !resizing) {
        int lx = mmx - wm_client_x(focus_win);
        int ly = mmy - wm_client_y(focus_win);
        if (lx >= 0 && ly >= 0 &&
            lx < wm_client_w(focus_win) &&
            ly < wm_client_h(focus_win))
            focus_win->on_mouse(focus_win, lx, ly,
                                lmb, mouse_get_right());
        wm_invalidate_windows();
    }

    if (!lmb && !rmb && focus_win && focus_win->on_mouse &&
        !dragging && !resizing) {
        int lx = mmx - wm_client_x(focus_win);
        int ly = mmy - wm_client_y(focus_win);
        if (lx >= 0 && ly >= 0 &&
            lx < wm_client_w(focus_win) &&
            ly < wm_client_h(focus_win))
            focus_win->on_mouse(focus_win, lx, ly, false, false);
        wm_invalidate_windows();
    }

    if (release && focus_win && focus_win->on_mouse) {
        focus_win->on_mouse(focus_win,
                            mmx - wm_client_x(focus_win),
                            mmy - wm_client_y(focus_win),
                            false, false);
        wm_invalidate_windows();
    }

    while (sys_kb_hit() && focus_win) {
        uint16_t key = sys_getkey();
        if ((key & 0xFF) != 0) {
            char m = keyboard_remap((char)(key & 0xFF));
            key = (key & 0xFF00) | (uint8_t)m;
        }
        uint8_t ascii = key & 0xFF;
        if (ascii == 4)  { wm_toggle_show_desktop(); continue; }  /* Ctrl+D */
        if (ascii == 12) { wm_on_login_hotkey();     continue; }  /* Ctrl+L */
        if (ascii == 22) { wm_paste_to(focus_win);   continue; }  /* Ctrl+V */
        /* Ctrl+C (3) deliberately NOT intercepted: it reaches on_key so the
           terminal can copy when idle, and busy loops drain it via
           kb_poll_abort() themselves. One consumer at a time, no races. */
        if (focus_win->on_key) {
            sched_charge_elapsed();
            sched_set_current_by_owner(focus_win);
            focus_win->on_key(focus_win, key);
            sched_charge_elapsed();   /* key handling bills to the app */
            sched_set_current(1);
        }
        wm_invalidate_windows();
    }

    int zd = mouse_get_z_delta();
    if (zd && focus_win && focus_win->on_key) {
        uint16_t vk = (zd < 0) ? (0x49 << 8) : (0x51 << 8);
        sched_charge_elapsed();
        sched_set_current_by_owner(focus_win);
        focus_win->on_key(focus_win, vk);
        sched_charge_elapsed();   /* wheel handling bills to the app */
        sched_set_current(1);
        wm_invalidate_windows();
    }

    prev_lmb = lmb; prev_rmb = rmb;
}

void wm_draw_all(void)
{
    for (int z = 0; z < win_count; z++) {
        Window *w = at_z(z);
        if (w && w->on_tick) w->on_tick(w);
        if (w) draw_window(w);
    }
    if (!wm_is_installer_mode())
        wm_draw_menubar();
}

void wm_update_light(void)
{
    for (int i = 0; i < 5; i++) mouse_poll();

    while (sys_kb_hit() && focus_win) {
        uint16_t key = sys_getkey();
        if ((key & 0xFF) != 0) {
            char m = keyboard_remap((char)(key & 0xFF));
            key = (key & 0xFF00) | (uint8_t)m;
        }
        uint8_t ascii = key & 0xFF;
        if (ascii == 4)  { wm_toggle_show_desktop(); continue; }  /* Ctrl+D */
        if (ascii == 12) { wm_on_login_hotkey();     continue; }  /* Ctrl+L */
        if (ascii == 22) { wm_paste_to(focus_win);   continue; }  /* Ctrl+V */
        if (focus_win->on_key) {
            sched_charge_elapsed();
            sched_set_current_by_owner(focus_win);
            focus_win->on_key(focus_win, key);
            sched_charge_elapsed();   /* key handling bills to the app */
            sched_set_current(1);
        }
    }

    int mmx = mouse_get_x(), mmy = mouse_get_y();
    mouse_get_z_delta();

    wm_draw_desktop_bg();
    wm_draw_all();
    wm_draw_taskbar();
    wm_draw_cursor(mmx, mmy);
    gpu_flush();
    wm_full_redraw = false;
}

int     wm_get_screen_w(void)     { return scr_w; }
int     wm_get_screen_h(void)     { return scr_h; }
int     wm_get_window_count(void) { return win_count; }
Window *wm_get_focused(void)      { return focus_win; }
Window *wm_get_window(int i)
{
    return (i >= 0 && i < win_count) ? &windows[i] : 0;
}