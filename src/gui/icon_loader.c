/*
 * icon_loader.c - Load 128x128 PNG icons from disk
 *
 * Icons are 128x128 RGBA PNG files in /icons/, decoded once at boot via
 * png_decode() and box-downsampled into 96x96 ARGB dock buffers. The dock
 * draws them at 64px, so edges stay supersampled; PNG carries real alpha,
 * so no background color-keying is needed.
*/

#include "icon_loader.h"
#include "png.h"
#include <stdint.h>
#include <stdbool.h>

extern void c_puts(const char *s);
extern void *kmalloc(uint32_t size);
extern void kfree(void *ptr);
extern void gpu_draw_pixel(int x, int y, uint32_t color);
extern uint32_t *gpu_get_backbuffer(void);
extern int gpu_get_width(void);
extern int gpu_get_height(void);
extern int load_file_content(const char *filename, char *buffer, int max_len);
extern int iso9660_read_file(const char *filename, void *buffer, uint32_t max_size);
extern bool iso9660_find_file(const char *filename, uint32_t *out_lba, uint32_t *out_size);

#define ICON_SIZE 96
#define ICON_PIXELS (ICON_SIZE * ICON_SIZE)
/* On-screen dock glyph size. Buffers stay larger so drawn edges are
   supersampled instead of nearest-neighbor staircases. */
#define ICON_DRAWN 64

/* Largest icon PNG we accept. Masters ship at ~8-15 KB; 256 KB leaves
   headroom without letting a corrupt size field exhaust the heap. */
#define ICON_PNG_MAX_BYTES (256U * 1024U)
/* Raw-sector fallback slot stride: icons live at LBA 60000 + index*2000. */
#define ICON_RAW_LBA_BASE 60000U
#define ICON_RAW_LBA_STRIDE 2000U

static unsigned int buf_terminal[ICON_PIXELS];
static unsigned int buf_browser[ICON_PIXELS];
static unsigned int buf_notepad[ICON_PIXELS];
static unsigned int buf_calculator[ICON_PIXELS];
static unsigned int buf_files[ICON_PIXELS];
static unsigned int buf_clock[ICON_PIXELS];
static unsigned int buf_paint[ICON_PIXELS];
static unsigned int buf_sysinfo[ICON_PIXELS];
static unsigned int buf_settings[ICON_PIXELS];
static unsigned int buf_folder[ICON_PIXELS];
static unsigned int buf_file[ICON_PIXELS];
static unsigned int buf_snake[ICON_PIXELS];
static unsigned int buf_3d_demo[ICON_PIXELS];
static unsigned int buf_blockforge[ICON_PIXELS];
static unsigned int buf_taskmgr[ICON_PIXELS];
static unsigned int buf_audiomgr[ICON_PIXELS];

/* Scratch file buffer in BSS: reused per icon, never held across calls. */
static unsigned char png_loader_data[ICON_PNG_MAX_BYTES];

/* Read a whole PNG file into png_loader_data. Returns byte count or <0. */
static int read_png_file(const char *path, int index, uint32_t *out_size) {
    int bytes = load_file_content(path, (char *)png_loader_data,
                                  sizeof(png_loader_data));
    if (bytes <= 0) {
        bytes = iso9660_read_file(path, png_loader_data,
                                  sizeof(png_loader_data));
        if (bytes <= 0) {
            const char *filename = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\')
                    filename = p + 1;
            bytes = iso9660_read_file(filename, png_loader_data,
                                      sizeof(png_loader_data));
        }
    }
    if (bytes <= 0 && index >= 0) {
        extern int disk_read_lba_hdd(uint32_t lba, uint32_t count, void *buffer);
        extern int disk_read_lba_cdrom(uint32_t lba, uint32_t count, void *buffer);
        uint32_t raw_lba = ICON_RAW_LBA_BASE + (uint32_t)index * ICON_RAW_LBA_STRIDE;
        static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        bool sig_ok = false;
        if (disk_read_lba_hdd(raw_lba, 1, png_loader_data) == 0) {
            sig_ok = true;
            for (int k = 0; k < 8; k++)
                if (png_loader_data[k] != png_sig[k]) { sig_ok = false; break; }
            if (sig_ok) {
                uint32_t n = sizeof(png_loader_data) / 512;
                if (disk_read_lba_hdd(raw_lba, n, png_loader_data) == 0)
                    bytes = (int)sizeof(png_loader_data);
                else
                    bytes = -1;
            }
        }
        if (!sig_ok) {
            if (disk_read_lba_cdrom(raw_lba, 1, png_loader_data) == 0) {
                sig_ok = true;
                for (int k = 0; k < 8; k++)
                    if (png_loader_data[k] != png_sig[k]) { sig_ok = false; break; }
                if (sig_ok) {
                    uint32_t n = sizeof(png_loader_data) / 512;
                    if (disk_read_lba_cdrom(raw_lba, n, png_loader_data) == 0)
                        bytes = (int)sizeof(png_loader_data);
                    else
                        bytes = -1;
                } else {
                    bytes = -1;
                }
            } else {
                bytes = -1;
            }
        }
    }
    if (bytes <= 0) {
        extern bool ata_cdrom_available(void);
        if (!ata_cdrom_available())
            return -5;
        return -1;
    }
    if (out_size) *out_size = (uint32_t)bytes;
    return bytes;
}

/* Box-downsample decoded ARGB into the 96x96 dock buffer.
 * Premultiplied alpha: transparent texels contribute no color, so
 * edges stay the glyph's hue instead of darkening to a sloppy outline.
*/

static void scale_to_icon(const uint32_t *src, uint32_t sw, uint32_t sh,
                          unsigned int *dst) {
    for (int dy = 0; dy < ICON_SIZE; dy++) {
        uint32_t y0 = (uint32_t)((uint64_t)dy * sh / ICON_SIZE);
        uint32_t y1 = (uint32_t)((uint64_t)(dy + 1) * sh / ICON_SIZE);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int dx = 0; dx < ICON_SIZE; dx++) {
            uint32_t x0 = (uint32_t)((uint64_t)dx * sw / ICON_SIZE);
            uint32_t x1 = (uint32_t)((uint64_t)(dx + 1) * sw / ICON_SIZE);
            uint64_t a_sum = 0, r_sum = 0, g_sum = 0, b_sum = 0, n = 0;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            for (uint32_t sy = y0; sy < y1; sy++) {
                for (uint32_t sx = x0; sx < x1; sx++) {
                    uint32_t px = src[sy * sw + sx];
                    uint32_t a = (px >> 24) & 0xFF;
                    uint32_t r = (px >> 16) & 0xFF;
                    uint32_t g = (px >> 8) & 0xFF;
                    uint32_t b = px & 0xFF;
                    a_sum += a;
                    r_sum += r * a;
                    g_sum += g * a;
                    b_sum += b * a;
                    n++;
                }
            }
            if (!n || a_sum == 0) {
                dst[dy * ICON_SIZE + dx] = 0;
                continue;
            }
            uint32_t a = (uint32_t)(a_sum / n);
            uint32_t r = (uint32_t)(r_sum / a_sum);
            uint32_t g = (uint32_t)(g_sum / a_sum);
            uint32_t b = (uint32_t)(b_sum / a_sum);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            dst[dy * ICON_SIZE + dx] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* Load one PNG icon and decode it into a 96x96 ARGB buffer. */
static int load_png_icon(const char *path, unsigned int *dst, int index) {
    uint32_t size = 0;
    int bytes = read_png_file(path, index, &size);
    PngImage img;
    int rc;
    if (bytes <= 0)
        return bytes;
    if (size < 8)
        return -2;
    rc = png_decode(png_loader_data, size, &img);
    if (rc != 0) {
        c_puts("[ICON] png_decode failed: ");
        c_puts(path);
        c_puts(" (");
        c_puts(png_error_string(rc));
        c_puts(")\n");
        return -4;
    }
    if (img.width == 0 || img.height == 0 || !img.pixels) {
        png_free(&img);
        return -4;
    }
    scale_to_icon(img.pixels, img.width, img.height, dst);
    png_free(&img);
    return 1;
}

static void create_fallback_icon(unsigned int *dst, uint32_t color) {
    for (int i = 0; i < ICON_PIXELS; i++)
        dst[i] = color;
}

static int try_load_icon(const char *path, unsigned int *dst,
                           uint32_t def_color, int index) {
    int res = load_png_icon(path, dst, index);
    if (res == 1)
        return 1;
    if (res == -5)
        create_fallback_icon(dst, 0xFFAA00AA);
    else if (res == -1)
        create_fallback_icon(dst, 0xFFFF00FF);
    else if (res == -2)
        create_fallback_icon(dst, 0xFFFFFF00);
    else if (res == -4)
        create_fallback_icon(dst, 0xFFFF8800);
    else
        create_fallback_icon(dst, def_color);
    return 0;
}

void icons_load_all(void) {
    try_load_icon("/icons/terminal.png", buf_terminal, 0xFF333333, 0);
    try_load_icon("/icons/browser.png", buf_browser, 0xFF4488FF, 1);
    try_load_icon("/icons/notepad.png", buf_notepad, 0xFFFFFFFF, 2);
    try_load_icon("/icons/calculator.png", buf_calculator, 0xFF444444, 3);
    try_load_icon("/icons/files.png", buf_files, 0xFF888888, 4);
    try_load_icon("/icons/clock.png", buf_clock, 0xFFFFFFFF, 5);
    try_load_icon("/icons/paint.png", buf_paint, 0xFFFF8844, 6);
    try_load_icon("/icons/sys-info.png", buf_sysinfo, 0xFF666666, 7);
    try_load_icon("/icons/settings.png", buf_settings, 0xFF888888, 8);
    try_load_icon("/icons/folder.png", buf_folder, 0xFFFFCC66, 9);
    try_load_icon("/icons/file.png", buf_file, 0xFFBBBBCC, 10);
    try_load_icon("/icons/snake.png", buf_snake, 0xFF44DD44, 11);
    try_load_icon("/icons/3d_demo.png", buf_3d_demo, 0xFF8888FF, 12);
    /* BlockForge ships its own glyph; a missing file falls back to the
       3D-demo art so the dock never shows a diagnostic square. */
    if (!try_load_icon("/icons/blockforge.png", buf_blockforge, 0xFF8888FF, 15)) {
        for (int i = 0; i < ICON_PIXELS; i++)
            buf_blockforge[i] = buf_3d_demo[i];
    }
    try_load_icon("/icons/task_manager.png", buf_taskmgr, 0xFF22CC88, 13);
    try_load_icon("/icons/audio_manager.png", buf_audiomgr, 0xFFFF6B35, 14);
}

/* Draw a 96x96 buffer at the 64px dock size. Premultiplied for
 * crisp Mac-like edges: no dark halo around the glyph. */
static void draw_buf(const unsigned int *buf, int x, int y) {
    uint32_t *bb = gpu_get_backbuffer();
    int sw = gpu_get_width();
    int sh = gpu_get_height();
    for (int dy = 0; dy < ICON_DRAWN; dy++) {
        uint32_t y0 = (uint32_t)((uint64_t)dy * ICON_SIZE / ICON_DRAWN);
        uint32_t y1 = (uint32_t)((uint64_t)(dy + 1) * ICON_SIZE / ICON_DRAWN);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > ICON_SIZE) y1 = ICON_SIZE;
        for (int dx = 0; dx < ICON_DRAWN; dx++) {
            uint32_t x0 = (uint32_t)((uint64_t)dx * ICON_SIZE / ICON_DRAWN);
            uint32_t x1 = (uint32_t)((uint64_t)(dx + 1) * ICON_SIZE / ICON_DRAWN);
            uint64_t a_sum = 0, r_sum = 0, g_sum = 0, b_sum = 0, n = 0;
            int px = x + dx, py = y + dy;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > ICON_SIZE) x1 = ICON_SIZE;
            for (uint32_t sy = y0; sy < y1; sy++) {
                for (uint32_t sx = x0; sx < x1; sx++) {
                    uint32_t c = buf[sy * ICON_SIZE + sx];
                    uint32_t a = (c >> 24) & 0xFF;
                    a_sum += a;
                    r_sum += ((c >> 16) & 0xFF) * a;
                    g_sum += ((c >> 8) & 0xFF) * a;
                    b_sum += (c & 0xFF) * a;
                    n++;
                }
            }
            if (!n || a_sum == 0) continue;
            uint32_t sa = (uint32_t)(a_sum / n);
            if (sa < 8) continue;
            uint32_t sr = (uint32_t)(r_sum / a_sum);
            uint32_t sg = (uint32_t)(g_sum / a_sum);
            uint32_t sb = (uint32_t)(b_sum / a_sum);
            if (sa < 255 && bb && px >= 0 && py >= 0 && px < sw && py < sh) {
                uint32_t inv = 255 - sa;
                uint32_t bg = bb[py * sw + px];
                uint32_t br = (bg >> 16) & 0xFF;
                uint32_t bgc = (bg >> 8) & 0xFF;
                uint32_t bbc = bg & 0xFF;
                sr = (sr * sa + br * inv) / 255;
                sg = (sg * sa + bgc * inv) / 255;
                sb = (sb * sa + bbc * inv) / 255;
            }
            gpu_draw_pixel(px, py, 0xFF000000U | (sr << 16) | (sg << 8) | sb);
        }
    }
}

void icon_draw_png_terminal(int x, int y) { draw_buf(buf_terminal, x, y); }
void icon_draw_png_browser(int x, int y) { draw_buf(buf_browser, x, y); }
void icon_draw_png_notepad(int x, int y) { draw_buf(buf_notepad, x, y); }
void icon_draw_png_calculator(int x, int y) { draw_buf(buf_calculator, x, y); }
void icon_draw_png_files(int x, int y) { draw_buf(buf_files, x, y); }
void icon_draw_png_clock(int x, int y) { draw_buf(buf_clock, x, y); }
void icon_draw_png_paint(int x, int y) { draw_buf(buf_paint, x, y); }
void icon_draw_png_sysinfo(int x, int y) { draw_buf(buf_sysinfo, x, y); }
void icon_draw_png_settings(int x, int y) { draw_buf(buf_settings, x, y); }
void icon_draw_png_folder(int x, int y) { draw_buf(buf_folder, x, y); }
void icon_draw_png_file(int x, int y) { draw_buf(buf_file, x, y); }
void icon_draw_png_snake(int x, int y) { draw_buf(buf_snake, x, y); }
void icon_draw_png_3d_demo(int x, int y) { draw_buf(buf_3d_demo, x, y); }

void icon_draw_png_cube(int x, int y) {
    icon_draw_png_3d_demo(x, y);
}
void icon_draw_png_blockforge(int x, int y) { draw_buf(buf_blockforge, x, y); }
void icon_draw_png_taskmgr(int x, int y) { draw_buf(buf_taskmgr, x, y); }
void icon_draw_png_audiomgr(int x, int y) { draw_buf(buf_audiomgr, x, y); }
