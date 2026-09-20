/*
 * installer.c - AurionOS first-boot installer
 *
 * Runs as a fullscreen chromeless window. Walks the user through:
 *   welcome -> keyboard -> credentials -> app selection -> wallpaper
 *   -> install -> done
 *
 * All drawing is immediate-mode into the WM framebuffer. No retained
 * widget tree. State lives in InstallerState, hung off win->user_data.
 *
 * NOTE: progress advancement happens in installer_on_tick(), NOT in
 * the draw callback. Draw functions are pure - they read state, they
 * never mutate it. Learned that the hard way after a double-increment
 * bug when the WM started calling draw at variable rate.
*/

#include "window_manager.h"
#include "fs_maintenance.h"
#include "vanta_examples.h"
#include <stdint.h>
#include <stdbool.h>

/* Kernel / FS imports */

extern uint32_t get_ticks(void);
extern int      save_file_content(const char *filename, const char *data, int len);
extern int      load_file_content(const char *filename, char *buffer, int max_len);
extern int      cmd_mkdir(const char *args);
extern int      str_len(const char *s);
extern void     str_copy(char *dst, const char *src, int max);
extern void     c_puts(const char *s);
extern int      fs_save_to_disk(void);
extern int      fs_verify_on_disk(const char *name, const char *expect, int len);
extern bool     ata_is_available(void);

extern void *kmalloc(uint32_t size);
extern void  kfree(void *ptr);
extern bool  iso9660_find_file(const char *filename, uint32_t *out_lba, uint32_t *out_size);
extern int   disk_read_lba_cdrom(uint32_t lba, uint32_t count, void *buffer);
extern int   disk_read_lba_hdd(uint32_t lba, uint32_t count, void *buffer);

/* WM surface API */
extern void wm_set_installer_mode(bool enabled);
extern void wm_draw_desktop_bg(void);
extern int  wm_get_screen_w(void);
extern int  wm_get_screen_h(void);
extern int  wm_client_w(Window *w);
extern int  wm_client_h(Window *w);
extern void wm_fill_rect(Window *w, int x, int y, int rw, int rh, uint32_t c);
extern void wm_draw_rect(Window *w, int x, int y, int rw, int rh, uint32_t c);
extern void wm_draw_rounded_rect(Window *w, int x, int y, int rw, int rh, uint32_t c, int radius);
extern void wm_draw_string(Window *w, int x, int y, const char *s, uint32_t fg, uint32_t bg);
extern void wm_fill_rect_blend(Window *w, int x, int y, int rw, int rh, uint32_t c, uint8_t a);
extern void wm_destroy_window(Window *w);
extern void wm_focus_window(Window *w);

extern int  keyboard_layout;
extern char keyboard_remap(char c);

/* Wallpaper pick reuses the live desktop thumbnail cache. */
extern const uint32_t *wm_wallpaper_preview(int idx);
extern const char *wm_wallpaper_name(int idx);
extern int  wm_wallpaper_count(void);
extern void wm_draw_icon(void *win, const uint32_t *pixels, int img_w, int img_h,
                         int x, int y, int w, int h);
extern void wm_set_wallpaper(int idx);
extern void wm_save_settings(void);

/* Installer steps */

typedef enum {
    STEP_WELCOME = 0,
    STEP_KEYBOARD,
    STEP_CREDENTIALS,
    STEP_APPS,
    STEP_WALLPAPER,
    STEP_INSTALLING,
    STEP_FINISHED,
    STEP_COUNT
} InstallerStep;

/* Layout constants */

/* Every draw function and the mouse handler reference these .*/
/* Change them here, everything stays in sync. Previously each */
/* function had its own copy of "640, 480, cx+40" and the mouse */
/* handler drifted out of alignment twice. Never again. */

#define PANEL_W         640
#define PANEL_H         480
#define PANEL_PAD       40      /* inner padding from panel edge */
#define PANEL_GLASS_A   180     /* alpha for the glass fill */

#define BTN_W           110
#define BTN_H           36
#define BTN_GAP         15      /* gap between back/next buttons */

#define INPUT_W         200
#define INPUT_H         32
#define LABEL_COL_W     140     /* width reserved for field labels */

#define APP_ROW_H       45
#define APP_CARD_W      450
#define APP_CARD_H      40

/* Wallpaper picker grid (2x2 small previews + names). Draw and mouse
 * share these so tiles and hit rects never drift apart. */
#define WP_TILE_W       150
#define WP_TILE_H       85
#define WP_TILE_DX      290
#define WP_TILE_DY      130
#define WP_AUTO_MS      150     /* auto-cycle period until the user picks */

#define LAYOUT_CARD_W   300
#define LAYOUT_CARD_H   40
#define LAYOUT_GAP      50

#define STEP_DOT_SZ     8
#define STEP_DOT_GAP    50

#define PROGRESS_BAR_H  16

#define FOOTER_H        48

#define CURSOR_BLINK_MS 30
#define ERROR_TIMEOUT_MS 300

/* Colors - dark glass palette                                         */
/*                                                                     */
/* Kept minimal. If a color isn't used in at least two places,         */
/* it doesn't get a define.                                            */

#define C_BG            0xFF0A0A0A
#define C_GLASS         0xFF121212
#define C_HEADER        0xFF161616
#define C_ACCENT        0xFF22C55E
#define C_ACCENT_HI     0xFF4ADE80
#define C_ACCENT_DIM    0xFF14532D
#define C_DANGER        0xFFEF4444
#define C_WARN          0xFFF59E0B
#define C_TEXT          0xFFF8FAFC
#define C_TEXT_DIM      0xFF94A3B8
#define C_TEXT_MUTE     0xFF64748B
#define C_TEXT_INV      0xFF020617   /* dark text on green buttons */
#define C_INPUT_BG      0xFF1E293B
#define C_INPUT_BORDER  0xFF334155
#define C_BTN_SEC       0xFF2B2B30
#define C_BTN_SEC_HI    0xFF3A3A42
#define C_BTN_SEC_TXT   0xFFF1F5F9
#define C_BORDER        0xFF444444
#define C_BORDER_DIM    0xFF333333
#define C_CHECK_BG      0xFF1E293B

/* Static data                                                         */

static const char *app_names[] = {
    "Terminal", "Notepad", "Paint", "Calculator", "Browser"
};
static const char *app_descs[] = {
    "Command-line interface",
    "Text editor",
    "Drawing application",
    "Math utilities",
    "Web browser"
};
/* Internal names used for .hidden_ marker files on the desktop */
static const char *app_internal[] = {
    "Terminal", "Notepad", "Paint", "Calculator", "Blaze"
};
#define APP_COUNT 5

static const char *install_msgs[] = {
    "Copying AurionOS files",
    "Expanding AurionOS files",
    "Installing features",
    "Installing updates",
    "Completing installation"
};
#define INSTALL_MSG_COUNT 5

/* Directories created during install. NULL-terminated. */
static const char *install_dirs[] = {
    "/Code", "/Documents", "/Music", "/Pictures",
    "/Downloads", "/Videos", "/Desktop", "/Desktop/Applications",
    "/System", "/Keyboard", "/Development", "/users",
    NULL
};

/* State */

typedef struct {
    InstallerStep step;

    /* credentials */
    char     username[32];
    char     password[32];
    char     confirm[32];
    int      active_field;       /* 0=user 1=pass 2=confirm */
    bool     account_conflict;
    bool     replace_existing;
    bool     account_prepared;
    int      conflict_choice;    /* 0=change name (default), 1=replace */
    char     approved_username[32];

    /* app selection */
    bool     apps[APP_COUNT];

    /* wallpaper pick: -1 until the user clicks a tile. The highlight
     * auto-cycles (wp_auto) until then; Next appears only on choice. */
    int      wallpaper;
    int      wp_auto;
    uint32_t wp_auto_tick;

    /* install progress */
    int      progress;           /* 0..100 */
    uint32_t last_tick;
    bool     install_ok;
    bool     disk_available;
    int      fail_stage;         /* 0=ok, 1=sync, 2=read, 3=content, 4=verify,
                                   5=marker save, 6=account, 7=config */

    /* keyboard test area */
    char     test_input[64];

    /* input timing */
    uint32_t cursor_tick;
    bool     cursor_on;
    uint32_t step_enter_tick;

    /* mouse */
    bool     prev_left;
    int      hover_btn;          /* -1 none, 0 back, 1 next, 2 continue */
    int      hover_app;          /* -1 none, 0..4 */
    int      hover_layout;       /* -1 none, 0 english, 1 serbian */
    int      hover_wp;           /* -1 none, 0..3 wallpaper tile */

    /* validation error (auto-clears) */
    char     error[64];
    uint32_t error_tick;
} InstallerState;

/* Small helpers */

typedef struct { int x, y, w, h; } Rect;

static bool in_rect(Rect r, int px, int py)
{
    return px >= r.x && px < r.x + r.w &&
           py >= r.y && py < r.y + r.h;
}

static void set_error(InstallerState *s, const char *msg)
{
    str_copy(s->error, msg, 63);
    s->error_tick = get_ticks();
}

/* Returns true if an error message is currently visible. */
static bool error_active(InstallerState *s)
{
    if (s->error[0] == '\0')
        return false;
    if (get_ticks() - s->error_tick > ERROR_TIMEOUT_MS) {
        s->error[0] = '\0';
        return false;
    }
    return true;
}

static bool streq(const char *a, const char *b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++) {
        if (a[i] != b[i]) return false;
    }
    return a[i] == b[i];
}

static bool valid_user_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Panel origin - top-left of the centered glass container. */
static void panel_origin(int cw, int ch, int *ox, int *oy)
{
    *ox = (cw - PANEL_W) / 2;
    *oy = (ch - PANEL_H) / 2;
}

/* Drawing primitives */

static void draw_check(Window *w, int x, int y, uint32_t col)
{
    /* 14x10 pixel checkmark, drawn as 2px blocks */
    wm_fill_rect(w, x,      y + 4, 2, 2, col);
    wm_fill_rect(w, x + 2,  y + 6, 2, 2, col);
    wm_fill_rect(w, x + 4,  y + 8, 2, 2, col);
    wm_fill_rect(w, x + 6,  y + 6, 2, 2, col);
    wm_fill_rect(w, x + 8,  y + 4, 2, 2, col);
    wm_fill_rect(w, x + 10, y + 2, 2, 2, col);
    wm_fill_rect(w, x + 12, y,     2, 2, col);
}

static void draw_button(Window *w, Rect r, const char *label,
                        uint32_t bg, uint32_t fg, bool hover)
{
    uint32_t fill = bg;
    if (hover)
        fill = (bg == C_ACCENT) ? C_ACCENT_HI : C_BTN_SEC_HI;

    wm_draw_rounded_rect(w, r.x, r.y, r.w, r.h, fill, 6);

    if (hover)
        wm_fill_rect(w, r.x + 2, r.y + 1, r.w - 4, 1, 0xFFFFFFFF);

    wm_draw_rect(w, r.x, r.y, r.w, r.h, hover ? 0xFF888888 : 0xFF555555);

    int tlen = str_len(label);
    int tx = r.x + (r.w - tlen * 8) / 2;
    int ty = r.y + (r.h - 8) / 2;
    wm_draw_string(w, tx, ty, label, fg, fill);
}

static void draw_input(Window *w, Rect r, const char *text,
                       bool focused, bool masked, bool show_cursor)
{
    uint32_t border = focused ? C_ACCENT : C_INPUT_BORDER;

    wm_draw_rounded_rect(w, r.x, r.y, r.w, r.h, C_INPUT_BG, 4);
    wm_draw_rect(w, r.x, r.y, r.w, r.h, border);

    int tx = r.x + 8;
    int ty = r.y + (r.h - 8) / 2;
    int len = str_len(text);

    if (masked) {
        char stars[33];
        int i;
        for (i = 0; i < len && i < 32; i++)
            stars[i] = '*';
        stars[i] = '\0';
        wm_draw_string(w, tx, ty, stars, 0xFFFFFFFF, C_INPUT_BG);
    } else {
        wm_draw_string(w, tx, ty, text, 0xFFFFFFFF, C_INPUT_BG);
    }

    if (focused && show_cursor)
        wm_fill_rect(w, tx + len * 8, ty - 2, 2, 12, C_ACCENT);
}

static void draw_checkbox(Window *w, int x, int y, bool checked, bool hover)
{
    uint32_t border = hover ? C_ACCENT : C_INPUT_BORDER;
    wm_draw_rounded_rect(w, x, y, 16, 16, C_CHECK_BG, 3);
    wm_draw_rect(w, x, y, 16, 16, border);
    if (checked)
        draw_check(w, x + 2, y + 3, C_ACCENT);
}

/* Step progress dots (top of panel) */

static void draw_step_dots(Window *w, int px, int py, InstallerStep cur)
{
    int total_w = (STEP_COUNT - 1) * STEP_DOT_GAP;
    int sx = px + (PANEL_W - total_w) / 2;
    int dy = py + 18;

    for (int i = 0; i < STEP_COUNT; i++) {
        int dx = sx + i * STEP_DOT_GAP;

        if (i < (int)cur) {
            wm_fill_rect(w, dx, dy, STEP_DOT_SZ, STEP_DOT_SZ, C_ACCENT);
            draw_check(w, dx + 1, dy + 1, C_TEXT_INV);
        } else if (i == (int)cur) {
            wm_fill_rect_blend(w, dx - 3, dy - 3,
                               STEP_DOT_SZ + 6, STEP_DOT_SZ + 6, C_ACCENT, 50);
            wm_fill_rect(w, dx, dy, STEP_DOT_SZ, STEP_DOT_SZ, C_ACCENT);
            wm_draw_rect(w, dx - 1, dy - 1,
                         STEP_DOT_SZ + 2, STEP_DOT_SZ + 2, 0xFFFFFFFF);
        } else {
            wm_fill_rect(w, dx, dy, STEP_DOT_SZ, STEP_DOT_SZ, C_INPUT_BG);
            wm_draw_rect(w, dx, dy, STEP_DOT_SZ, STEP_DOT_SZ, C_TEXT_MUTE);
        }

        /* connector line between dots */
        if (i < STEP_COUNT - 1) {
            uint32_t lc = (i < (int)cur) ? C_ACCENT : C_BORDER_DIM;
            wm_fill_rect(w, dx + STEP_DOT_SZ + 2, dy + 3,
                         STEP_DOT_GAP - STEP_DOT_SZ - 4, 2, lc);
        }
    }
}

/* Panel chrome - glass background, dots, separator */
/* Called at the top of every step's draw function. */

static void draw_panel_frame(Window *w, int px, int py, InstallerStep step)
{
    wm_fill_rect_blend(w, px, py, PANEL_W, PANEL_H, C_GLASS, PANEL_GLASS_A);
    wm_draw_rect(w, px, py, PANEL_W, PANEL_H, C_BORDER);
    wm_draw_rect(w, px - 1, py - 1, PANEL_W + 2, PANEL_H + 2, 0xFF222222);

    draw_step_dots(w, px, py, step);

    /* separator below the dots */
    wm_fill_rect(w, px + PANEL_PAD, py + 40, PANEL_W - PANEL_PAD * 2, 1, C_BORDER_DIM);
}

/* Footer bar                                                          */

static void draw_footer(Window *w, InstallerState *s, int cw, int ch)
{
    int fy = ch - FOOTER_H;

    wm_fill_rect_blend(w, 0, fy, cw, FOOTER_H, 0xFF000000, 140);
    wm_fill_rect(w, 0, fy, cw, 1, C_BORDER_DIM);

    int ty = fy + 15;

    /* phase 1: info gathering (done once we hit install) */
    bool phase1_done = (s->step >= STEP_INSTALLING);
    if (phase1_done) {
        draw_check(w, 30, ty + 2, C_ACCENT);
        wm_draw_string(w, 50, ty, "Step 1: Collected information", C_TEXT_MUTE, 0);
    } else {
        wm_draw_string(w, 30, ty, "Step 1: Collecting information", C_TEXT, 0);
    }

    /* phase 2: installation */
    uint32_t p2col = (s->step >= STEP_INSTALLING) ? C_ACCENT : C_TEXT_MUTE;
    wm_draw_string(w, cw / 2, ty, "Step 2: Installing AurionOS", p2col, 0);

    /* percentage readout during install */
    if (s->step == STEP_INSTALLING) {
        char buf[8];
        int n = 0;
        int p = s->progress;

        if (p >= 100)     { buf[n++] = '1'; buf[n++] = '0'; buf[n++] = '0'; }
        else if (p >= 10) { buf[n++] = '0' + p / 10; buf[n++] = '0' + p % 10; }
        else              { buf[n++] = '0' + p; }
        buf[n++] = '%';
        buf[n] = '\0';

        wm_draw_string(w, cw - 60, ty, buf, C_ACCENT, 0);
    }
}

/* STEP 0 - Welcome */

static void draw_welcome(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);
    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    wm_draw_string(w, tx, ty, "AURION OS", C_ACCENT, 0);
    wm_draw_string(w, tx + 80, ty, "Installer", C_TEXT_DIM, 0);

    ty = py + 110;
    wm_draw_string(w, tx, ty, "Your machine. Your rules.", C_TEXT, 0);
    ty += 25;
    wm_draw_string(w, tx, ty, "No telemetry. No phone home. Just an OS.", C_TEXT_DIM, 0);

    ty += 60;
    static const char *features[] = {
        "Boots in seconds, not minutes",
        "Graphics run on bare metal",
        "Files stay on your disk"
    };
    for (int i = 0; i < 3; i++) {
        draw_check(w, tx, ty, C_ACCENT);
        wm_draw_string(w, tx + 24, ty, features[i], C_TEXT_DIM, 0);
        ty += 34;
    }

    /* disk status line */
    ty = py + PANEL_H - 100;
    if (s->disk_available)
        wm_draw_string(w, tx, ty, "Disk: High-speed SSD detected (LBA mapped)", C_ACCENT, 0);
    else
        wm_draw_string(w, tx, ty, "Disk: Local-only storage active.", C_WARN, 0);

    /* next button */
    Rect next = { px + PANEL_W - 150, py + PANEL_H - 60, BTN_W, BTN_H };
    draw_button(w, next, "Next", C_ACCENT, C_TEXT_INV, s->hover_btn == 1);
}

/* STEP 1 - Keyboard layout */

static void draw_keyboard(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);
    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    wm_draw_string(w, tx, ty, "Keyboard Configuration", C_ACCENT, 0);
    ty += 50;

    static const struct { const char *name; int id; } layouts[] = {
        { "English (US)",    0 },
        { "Serbian (Latin)", 1 }
    };

    for (int i = 0; i < 2; i++) {
        bool sel = (keyboard_layout == layouts[i].id);
        bool hov = (s->hover_layout == i);

        Rect r = { tx, ty, LAYOUT_CARD_W, LAYOUT_CARD_H };

        if (sel) {
            wm_draw_rounded_rect(w, r.x, r.y, r.w, r.h, C_ACCENT_DIM, 8);
            wm_draw_rect(w, r.x, r.y, r.w, r.h, C_ACCENT);
        } else if (hov) {
            wm_draw_rounded_rect(w, r.x, r.y, r.w, r.h, C_INPUT_BG, 8);
            wm_draw_rect(w, r.x, r.y, r.w, r.h, C_ACCENT);
        } else {
            wm_draw_rounded_rect(w, r.x, r.y, r.w, r.h, C_INPUT_BG, 8);
            wm_draw_rect(w, r.x, r.y, r.w, r.h, C_INPUT_BORDER);
        }

        wm_draw_string(w, r.x + 12, r.y + 14, layouts[i].name, C_TEXT, 0);
        if (sel)
            draw_check(w, r.x + 270, r.y + 14, C_ACCENT);

        ty += LAYOUT_GAP;
    }

    /* test area */
    ty += 30;
    wm_draw_string(w, tx, ty, "Testing area:", C_TEXT_DIM, 0);
    ty += 28;
    Rect test_r = { tx, ty, LAYOUT_CARD_W, INPUT_H };
    draw_input(w, test_r, s->test_input, true, false, s->cursor_on);

    /* nav buttons */
    int btn_y = py + PANEL_H - 60;
    Rect back = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
    Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
    draw_button(w, back, "Back", C_BTN_SEC, C_BTN_SEC_TXT, s->hover_btn == 0);
    draw_button(w, next, "Next", C_ACCENT, C_TEXT_INV, s->hover_btn == 1);
}

static void installer_accept_account(InstallerState *s, bool replace)
{
    s->account_conflict = false;
    s->replace_existing = replace;
    s->account_prepared = false;
    str_copy(s->approved_username, s->username, sizeof(s->approved_username));
    s->step = STEP_APPS;
    s->step_enter_tick = get_ticks();
}

static void installer_change_name(InstallerState *s)
{
    s->account_conflict = false;
    s->replace_existing = false;
    s->account_prepared = false;
    s->approved_username[0] = 0;
    s->username[0] = 0;
    s->active_field = 0;
}

static void installer_submit_credentials(InstallerState *s)
{
    if (!s->username[0]) { set_error(s, "Please enter a username."); return; }
    for (int i = 0; s->username[i]; i++) {
        if (!valid_user_char(s->username[i])) {
            set_error(s, "Username: only letters, numbers, _ and -");
            return;
        }
    }
    if (str_len(s->password) < 6) { set_error(s, "Password too short (min 6 chars)."); return; }
    if (!streq(s->password, s->confirm)) { set_error(s, "Passwords do not match."); return; }
    s->error[0] = 0;
    s->replace_existing = false;
    s->account_prepared = false;
    if (fs_user_exists(s->username)) {
        s->account_conflict = true;
        s->conflict_choice = 0;
        return;
    }
    installer_accept_account(s, false);
}

/* STEP 2 - Credentials */

static void draw_credentials(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);
    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    if (s->account_conflict) {
        wm_draw_string(w, tx, ty, "Account already exists", C_WARN, 0);
        wm_draw_string(w, tx, ty + 48, s->username, C_TEXT, 0);
        wm_draw_string(w, tx, ty + 92, "Replace updates its password and keeps its files.", C_TEXT_DIM, 0);
        wm_draw_string(w, tx, ty + 116, "Choose a new name to create a separate account.", C_TEXT_DIM, 0);
        Rect replace = { tx, py + 280, 180, BTN_H };
        Rect rename = { tx + 204, py + 280, 180, BTN_H };
        draw_button(w, replace, "Replace account", C_BTN_SEC, C_BTN_SEC_TXT,
                    s->hover_btn == 3 || s->conflict_choice == 1);
        draw_button(w, rename, "Change name", C_ACCENT, C_TEXT_INV,
                    s->hover_btn == 4 || s->conflict_choice == 0);
        wm_draw_string(w, tx, py + 342, "Tab switches choices. Enter selects. Esc changes name.", C_TEXT_MUTE, 0);
        return;
    }
    wm_draw_string(w, tx, ty, "Account Configuration", C_ACCENT, 0);
    ty += 50;
    wm_draw_string(w, tx, ty, "Establish your system credentials:", C_TEXT, 0);

    /* field rows: label + input */
    ty += 40;
    wm_draw_string(w, tx, ty, "Username:", C_TEXT_DIM, 0);
    Rect ur = { tx + LABEL_COL_W, ty - 8, INPUT_W, INPUT_H };
    draw_input(w, ur, s->username, s->active_field == 0, false,
               s->active_field == 0 && s->cursor_on);

    ty += 45;
    wm_draw_string(w, tx, ty, "Password:", C_TEXT_DIM, 0);
    Rect pr = { tx + LABEL_COL_W, ty - 8, INPUT_W, INPUT_H };
    draw_input(w, pr, s->password, s->active_field == 1, true,
               s->active_field == 1 && s->cursor_on);

    ty += 45;
    wm_draw_string(w, tx, ty, "Confirm Password:", C_TEXT_DIM, 0);
    Rect cr = { tx + LABEL_COL_W, ty - 8, INPUT_W, INPUT_H };
    draw_input(w, cr, s->confirm, s->active_field == 2, true,
               s->active_field == 2 && s->cursor_on);

    /* live match indicator */
    if (str_len(s->confirm) > 0) {
        bool match = streq(s->password, s->confirm);
        wm_draw_string(w, tx + LABEL_COL_W, ty + 40,
                       match ? "Passwords match." : "Passwords mismatch.",
                       match ? C_ACCENT : C_DANGER, 0);
    }

    /* transient validation error */
    if (error_active(s))
        wm_draw_string(w, tx, py + PANEL_H - 110, s->error, C_DANGER, 0);

    int btn_y = py + PANEL_H - 60;
    Rect back = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
    Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
    draw_button(w, back, "Back", C_BTN_SEC, C_BTN_SEC_TXT, s->hover_btn == 0);
    draw_button(w, next, "Next", C_ACCENT, C_TEXT_INV, s->hover_btn == 1);
}

/* STEP 3 - App selection */

static void draw_apps(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);
    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    wm_draw_string(w, tx, ty, "Software Selection", C_ACCENT, 0);
    ty += 50;
    wm_draw_string(w, tx, ty, "Choose your initial ecosystem components:", C_TEXT, 0);

    ty += 40;
    for (int i = 0; i < APP_COUNT; i++) {
        bool hov = (s->hover_app == i);
        Rect card = { tx, ty - 5, APP_CARD_W, APP_CARD_H };

        if (hov) {
            wm_draw_rounded_rect(w, card.x, card.y, card.w, card.h, C_INPUT_BG, 6);
            wm_draw_rect(w, card.x, card.y, card.w, card.h, C_ACCENT);
        }

        draw_checkbox(w, tx + 5, ty + 5, s->apps[i], hov);
        wm_draw_string(w, tx + 40,  ty + 10, app_names[i], C_TEXT, 0);
        wm_draw_string(w, tx + 150, ty + 10, app_descs[i], C_TEXT_MUTE, 0);

        ty += APP_ROW_H;
    }

    int btn_y = py + PANEL_H - 60;
    Rect back   = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
    Rect deploy = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
    draw_button(w, back,   "Back",   C_BTN_SEC, C_BTN_SEC_TXT, s->hover_btn == 0);
    draw_button(w, deploy, "Deploy", C_ACCENT,  C_TEXT_INV,    s->hover_btn == 1);
}

/* STEP 4 - Wallpaper: what do you like most? */
/* Four small live previews with names. The highlight auto-cycles     */
/* until the user clicks a tile; the Next button appears only after   */
/* a real choice, so Deploy can never ship the default by accident.   */

static int wp_visible_count(void)
{
    int nc = wm_wallpaper_count();
    if (nc > 4) nc = 4;
    if (nc < 0) nc = 0;
    return nc;
}

static void draw_wallpaper(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);

    /* Live example: the hovered/chosen/cycling tile fills the whole
     * background behind the panel, dimmed so the panel stays readable. */
    {
        int prev = (s->wallpaper >= 0) ? s->wallpaper
                 : (s->hover_wp >= 0) ? s->hover_wp : s->wp_auto;
        if (prev >= 0 && prev < wp_visible_count()) {
            const uint32_t *bg = wm_wallpaper_preview(prev);
            if (bg) {
                wm_draw_icon(w, bg, 144, 81, 0, 0, cw, ch);
                wm_fill_rect_blend(w, 0, 0, cw, ch, 0xFF000000, 150);
            }
        }
    }

    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    wm_draw_string(w, tx, ty, "What do you like most?", C_ACCENT, 0);
    ty += 30;
    wm_draw_string(w, tx, ty, "Pick a wallpaper. The highlight cycles until you choose.", C_TEXT_DIM, 0);
    ty += 36;

    int shown = (s->wallpaper >= 0) ? s->wallpaper : s->wp_auto;
    int nc = wp_visible_count();
    for (int i = 0; i < nc; i++) {
        int cx = tx + (i & 1) * WP_TILE_DX;
        int cy = ty + (i >> 1) * WP_TILE_DY;
        bool hov = (s->hover_wp == i);
        bool hi  = (shown == i);
        bool sel = (s->wallpaper == i);
        const uint32_t *thumb = wm_wallpaper_preview(i);
        if (sel) {
            wm_fill_rect(w, cx - 3, cy - 3, WP_TILE_W + 6, WP_TILE_H + 6, C_ACCENT);
        } else if (hi || hov) {
            wm_draw_rect(w, cx - 1, cy - 1, WP_TILE_W + 2, WP_TILE_H + 2,
                         hi ? C_ACCENT_HI : C_TEXT_MUTE);
        } else {
            wm_draw_rect(w, cx - 1, cy - 1, WP_TILE_W + 2, WP_TILE_H + 2, C_BORDER_DIM);
        }
        if (thumb)
            wm_draw_icon(w, thumb, 144, 81, cx, cy, WP_TILE_W, WP_TILE_H);
        else
            wm_fill_rect(w, cx, cy, WP_TILE_W, WP_TILE_H, C_CHECK_BG);
        wm_draw_string(w, cx, cy + WP_TILE_H + 6, wm_wallpaper_name(i),
                       sel ? C_TEXT : C_TEXT_DIM, 0);
        if (sel)
            draw_check(w, cx + WP_TILE_W - 20, cy + 6, C_ACCENT);
    }

    int btn_y = py + PANEL_H - 60;
    Rect back = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
    draw_button(w, back, "Back", C_BTN_SEC, C_BTN_SEC_TXT, s->hover_btn == 0);
    if (s->wallpaper >= 0) {
        Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
        draw_button(w, next, "Next", C_ACCENT, C_TEXT_INV, s->hover_btn == 1);
    } else {
        wm_draw_string(w, tx, btn_y + 10,
                       "Select one to continue", C_TEXT_MUTE, 0);
    }
}

/* Persist the installer's wallpaper pick for the desktop. */
static void apply_wallpaper_choice(InstallerState *s)
{
    if (s->wallpaper < 0)
        return;
    wm_set_wallpaper(s->wallpaper);
    wm_save_settings();
}

/* STEP 5 - Installing (progress animation) */

static void draw_installing(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);
    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    wm_draw_string(w, tx, ty, "COPYING FILES", C_ACCENT, 0);
    ty += 50;
    wm_draw_string(w, tx, ty, "Writing to disk. Don't unplug anything.", C_TEXT_DIM, 0);
    ty += 50;

    /* sub-task checklist */
    int msg_idx = (s->progress * INSTALL_MSG_COUNT) / 101;
    if (msg_idx >= INSTALL_MSG_COUNT)
        msg_idx = INSTALL_MSG_COUNT - 1;

    for (int i = 0; i < INSTALL_MSG_COUNT; i++) {
        bool done   = (i < msg_idx);
        bool active = (i == msg_idx);

        if (done)
            draw_check(w, tx, ty, C_ACCENT);
        else if (active)
            wm_fill_rect(w, tx, ty + 2, 8, 8, C_ACCENT);

        uint32_t col = (done || active) ? C_TEXT : C_TEXT_MUTE;
        wm_draw_string(w, tx + 24, ty, install_msgs[i], col, 0);
        ty += 34;
    }

    /* progress bar */
    int bar_x = px + PANEL_PAD;
    int bar_w = PANEL_W - PANEL_PAD * 2;
    int bar_y = py + PANEL_H - 80;

    wm_draw_rounded_rect(w, bar_x, bar_y, bar_w, PROGRESS_BAR_H, C_INPUT_BG, 4);
    wm_draw_rect(w, bar_x, bar_y, bar_w, PROGRESS_BAR_H, C_BORDER_DIM);

    int fill_w = (bar_w * s->progress) / 100;
    if (fill_w > 0) {
        wm_draw_rounded_rect(w, bar_x, bar_y, fill_w, PROGRESS_BAR_H, C_ACCENT, 4);
        wm_fill_rect(w, bar_x, bar_y, fill_w, 2, 0xFFFFFFFF);
    }
}

/* STEP 6 - Finished */

static void draw_finished(Window *w, InstallerState *s, int cw, int ch)
{
    int px, py;
    panel_origin(cw, ch, &px, &py);
    draw_panel_frame(w, px, py, s->step);

    int tx = px + PANEL_PAD;
    int ty = py + 52;

    wm_draw_string(w, tx, ty,
                   s->install_ok ? "SYNCHRONIZATION COMPLETE" : "SETUP NEEDS ATTENTION",
                   s->install_ok ? C_ACCENT : C_WARN, 0);
    ty += 60;

    if (s->install_ok) {
        /* big checkmark, centered */
        int cx = px + PANEL_W / 2 - 20;
        int cy = ty + 10;
        wm_fill_rect(w, cx,      cy + 16, 4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 4,  cy + 20, 4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 8,  cy + 24, 4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 12, cy + 20, 4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 16, cy + 16, 4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 20, cy + 12, 4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 24, cy + 8,  4, 4, C_ACCENT);
        wm_fill_rect(w, cx + 28, cy + 4,  4, 4, C_ACCENT);

        ty += 60;
        wm_draw_string(w, tx, ty, "AurionOS has been successfully deployed.", C_TEXT, 0);
        ty += 24;
        wm_draw_string(w, tx, ty, "Your changes are now stored on the persistent disk.", C_TEXT_DIM, 0);
        ty += 18;
        wm_draw_string(w, tx, ty, "Please remove the installation media and reboot.", C_TEXT_DIM, 0);
    } else {
        if (!s->disk_available) {
            wm_draw_string(w, tx, ty, "No writable hard disk was detected.", C_WARN, 0);
            ty += 24;
            wm_draw_string(w, tx, ty, "Attach a hard disk, then retry setup.", C_TEXT_DIM, 0);
        } else {
            wm_draw_string(w, tx, ty, "A hard disk is detected, but setup could not finish.", C_WARN, 0);
            ty += 24;
            static const char *errors[] = {
                "Setup could not be completed.",
                "Writing setup files to disk failed.",
                "The setup completion file could not be read.",
                "The setup completion file failed validation.",
                "Saved setup files could not be verified on disk.",
                "The setup completion file could not be saved.",
                "The user account could not be configured.",
                "The system configuration could not be saved."
            };
            int st = s->fail_stage;
            if (st < 0 || st >= (int)(sizeof(errors) / sizeof(errors[0])))
                st = 0;
            wm_draw_string(w, tx, ty, errors[st], C_WARN, 0);
        }
        ty += 30;
        wm_draw_string(w, tx, ty, "Retry setup before rebooting to save your installation.", C_TEXT_DIM, 0);

    }

    int btn_y = py + PANEL_H - 60;
    Rect reboot   = { px + PANEL_W - 280, btn_y, BTN_W, BTN_H };
    Rect cont_btn = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
    draw_button(w, reboot, s->install_ok ? "Reboot" : "Retry",
                C_ACCENT, C_TEXT_INV, s->hover_btn == 1);
    draw_button(w, cont_btn, "Continue", C_ACCENT, C_TEXT_INV, s->hover_btn == 2);
}

/* Installation logic                                                  */
/*                                                                     */
/* Runs once when progress hits 100. Creates the user, directory       */
/* tree, sample files, config, wallpaper, and the installed.sys        */
/* marker. The marker is written LAST so a crash mid-install           */
/* doesn't leave a system that thinks it's set up.                     */

static bool do_install(InstallerState *s)
{
    s->fail_stage = 0;
    s->disk_available = ata_is_available();
    c_puts("[INSTALLER] Starting system initialization...\n");

    char path[128];
    int n;
    bool approved = streq(s->approved_username, s->username);
    if (!approved || fs_setup_account(s->username, s->password,
                                     s->replace_existing || s->account_prepared) != 0) {
        c_puts("[INSTALLER] Account setup needs a new name or replacement consent.\n");
        s->fail_stage = 6;
        return false;
    }
    /* A retry may update only the same account this setup already prepared. */
    s->account_prepared = true;

    /* directory tree */
    c_puts("[INSTALLER] Creating directories...\n");

    str_copy(path, "/users/", 64);
    str_copy(path + 7, s->username, 32);
    if (cmd_mkdir(path) != 0) {
        s->fail_stage = 7;
        return false;
    }
    for (int i = 0; install_dirs[i]; i++) {
        if (cmd_mkdir(install_dirs[i]) != 0) {
            s->fail_stage = 7;
            return false;
        }
    }

    /* sample files */
    c_puts("[INSTALLER] Writing sample files...\n");

    if (vanta_seed_examples() != 0) {
        s->fail_stage = 7;
        return false;
    }

    /* Root setup uses the same password bytes, including spaces. */
    if (fs_setup_account("root", s->password, true) != 0) {
        s->fail_stage = 6;
        return false;
    }

    /* app visibility markers */
    c_puts("[INSTALLER] Configuring app visibility...\n");
    for (int i = 0; i < APP_COUNT; i++) {
        if (s->apps[i])
            continue;
        str_copy(path, "/Desktop/Applications/.hidden_", 128);
        n = str_len(path);
        str_copy(path + n, app_internal[i], 32);
        if (save_file_content(path, "HIDDEN", 6) != 0) {
            s->fail_stage = 7;
            return false;
        }
    }

    /* system config */
    c_puts("[INSTALLER] Writing system configuration...\n");
    if (save_file_content("/System/current_user", s->username, str_len(s->username)) != 0 ||
        save_file_content("/Keyboard/config.sys",
                          keyboard_layout == 1 ? "KEYBOARD=SERBIAN" : "KEYBOARD=ENGLISH",
                          16) != 0) {
        c_puts("[INSTALLER] System configuration write failed.\n");
        s->fail_stage = 7;
        return false;
    }

    /* wallpapers
     * Desktop (Night Ridge / Misty Conifer / Gate B14 / Sunday) and installer (ins.png)
     * wallpapers ship as PNGs on the ISO and in raw HDD slots
     * (LBAs 10000/20000/40000/50000/30000); the window manager reads them straight
     * from there. Nothing to copy: the 8 KB RAM-FS object cap cannot hold
     * multi-MB PNGs, so a save_file_content() copy would only plant a
     * truncated corrupt file. The install marker below stays the last
     * write, so a mid-install crash still re-runs the installer. */
    /*
     * THE critical file. Written last. If we crash before this,
     * the next boot sees no marker and re-runs the installer.
     */
    c_puts("[INSTALLER] Writing installation marker...\n");
    static const char marker[] = "AURION_INSTALLED";
    if (save_file_content("/installed.sys", marker, sizeof(marker) - 1) != 0) {
        c_puts("[INSTALLER] Completion marker write failed.\n");
        s->fail_stage = 5;
        return false;
    }

    /* disk sync */
    c_puts("[INSTALLER] Syncing filesystem to disk...\n");
    int rc = fs_save_to_disk();
    if (rc != 0) {
        c_puts("[INSTALLER] Disk sync failed. Installation is not verified.\n");
        s->fail_stage = 1;
        return false;
    }

    /* verify */
    c_puts("[INSTALLER] Verifying marker...\n");
    char vbuf[sizeof(marker) - 1];
    if (load_file_content("/installed.sys", vbuf, sizeof(vbuf)) != (int)sizeof(vbuf)) {
        c_puts("[INSTALLER] Completion marker read failed.\n");
        s->fail_stage = 2;
        return false;
    }
    for (unsigned int i = 0; i < sizeof(vbuf); i++) {
        if (vbuf[i] != marker[i]) {
            c_puts("[INSTALLER] Completion marker content mismatch.\n");
            s->fail_stage = 3;
            return false;
        }
    }

    /* on-disk verify: the RAM check above cannot catch a disk that
     * silently dropped the write (no HDD attached, wrong drive). The
     * marker bytes must be readable back from raw disk sectors, or the
     * next boot lands in the installer again
     */
    c_puts("[INSTALLER] Verifying marker on disk...\n");
    if (fs_verify_on_disk("/installed.sys", marker, sizeof(marker) - 1) != 0) {
        c_puts("[INSTALLER] Completion marker failed hard-disk verification.\n");
        s->fail_stage = 4;
        return false;
    }

    c_puts("[INSTALLER] Installation verified.\n");
    s->fail_stage = 0;
    return true;
}

/* Draw dispatch */

static void installer_on_draw(Window *win)
{
    InstallerState *s = (InstallerState *)win->user_data;
    if (!s)
        return;

    int cw = wm_client_w(win);
    int ch = wm_client_h(win);

    wm_draw_desktop_bg();

    /* cursor blink */
    uint32_t now = get_ticks();
    if (now - s->cursor_tick >= CURSOR_BLINK_MS) {
        s->cursor_on = !s->cursor_on;
        s->cursor_tick = now;
    }

    switch (s->step) {
    case STEP_WELCOME:     draw_welcome(win, s, cw, ch);     break;
    case STEP_KEYBOARD:    draw_keyboard(win, s, cw, ch);    break;
    case STEP_CREDENTIALS: draw_credentials(win, s, cw, ch); break;
    case STEP_APPS:        draw_apps(win, s, cw, ch);        break;
    case STEP_WALLPAPER:  draw_wallpaper(win, s, cw, ch);  break;
    case STEP_INSTALLING:  draw_installing(win, s, cw, ch);  break;
    case STEP_FINISHED:    draw_finished(win, s, cw, ch);    break;
    default: break;
    }

    draw_footer(win, s, cw, ch);
}

/* Tick handler - advances install progress                            */
/* Separated from draw because the WM may call draw at variable        */
/* rate (dirty-rect redraws, focus changes, etc). Progress must        */
/* advance exactly once per tick, not once per draw call.              */

static void installer_on_tick(Window *win)
{
    InstallerState *s = (InstallerState *)win->user_data;
    if (!s)
        return;

    /* Wallpaper auto-cycle: runs until the user picks a tile. */
    if (s->step == STEP_WALLPAPER && s->wallpaper < 0) {
        uint32_t now = get_ticks();
        if (now - s->wp_auto_tick >= WP_AUTO_MS) {
            s->wp_auto_tick = now;
            int nc = wp_visible_count();
            if (nc > 0)
                s->wp_auto = (s->wp_auto + 1) % nc;
            win->needs_redraw = true;
        }
        return;
    }

    if (s->step != STEP_INSTALLING)
        return;

    uint32_t now = get_ticks();
    if (now - s->last_tick < 1)
        return;

    s->last_tick = now;
    s->progress++;

    if (s->progress >= 100) {
        s->install_ok = do_install(s);
        s->step = STEP_FINISHED;
        s->step_enter_tick = now;
    }
}

/* Keyboard input */

static void installer_on_key(Window *win, uint16_t key)
{
    InstallerState *s = (InstallerState *)win->user_data;
    if (!s)
        return;

    char c = (char)(key & 0xFF);

    if (s->step == STEP_CREDENTIALS && s->account_conflict) {
        if (key == 0x09) s->conflict_choice = !s->conflict_choice;
        else if (key == 0x1B) installer_change_name(s);
        else if (key == 0x0D) {
            if (s->conflict_choice == 1) installer_accept_account(s, true);
            else installer_change_name(s);
        }
        win->needs_redraw = true;
        return;
    }

    /* keyboard test area (step 1) */
    if (s->step == STEP_KEYBOARD) {
        if (key == 0x08) {
            int len = str_len(s->test_input);
            if (len > 0)
                s->test_input[len - 1] = '\0';
        } else if (c >= 32 && c <= 126) {
            int len = str_len(s->test_input);
            if (len < 63) {
                s->test_input[len]     = keyboard_remap(c);
                s->test_input[len + 1] = '\0';
            }
        }
        return;
    }

    /* Enter = the Next/Continue button on every display step (welcome,
       keyboard test, app pick, wallpaper pick, finished). Click and
       keyboard now advance through the same code, so no step is reachable
       by mouse and dead on Enter. The credential step handles Enter inside
       its own branch below (it has text fields in focus). The wallpaper
       step only advances after a real tile choice.
    */
    if (key == 0x0D) {
        if (s->step == STEP_WELCOME) {
            s->step = STEP_KEYBOARD;
            s->step_enter_tick = get_ticks();
            return;
        }
        if (s->step == STEP_KEYBOARD) {
            s->step = STEP_CREDENTIALS;
            s->step_enter_tick = get_ticks();
            return;
        }
        if (s->step == STEP_APPS) {
            s->step = STEP_WALLPAPER;
            s->wp_auto = 0;
            s->wp_auto_tick = get_ticks();
            s->step_enter_tick = get_ticks();
            return;
        }
        if (s->step == STEP_WALLPAPER) {
            if (s->wallpaper >= 0) {
                apply_wallpaper_choice(s);
                s->step = STEP_INSTALLING;
                s->progress = 0;
                s->last_tick = get_ticks();
                s->step_enter_tick = get_ticks();
            }
            return;
        }
        if (s->step == STEP_FINISHED) {
            c_puts("[INSTALLER] Continuing to desktop...\n");
            fs_save_to_disk();
            wm_destroy_window(win);
            wm_set_installer_mode(false);
            return;
        }
    }

    /* credential fields (step 2) */
    if (s->step != STEP_CREDENTIALS)
        return;

    char *target;
    switch (s->active_field) {
    case 0:  target = s->username; break;
    case 1:  target = s->password; break;
    default: target = s->confirm;  break;
    }

    int len = str_len(target);

    if (key == 0x0D) {
        /* Enter: advance field or validate */
        if (s->active_field < 2) {
            s->active_field++;
        } else {
            installer_submit_credentials(s);
        }
    } else if (key == 0x08) {
        if (len > 0)
            target[len - 1] = '\0';
    } else if (key == 0x09) {
        s->active_field = (s->active_field + 1) % 3;
    } else if (c >= 32 && c <= 126 && len < 31) {
        char mapped = keyboard_remap(c);
        if (s->active_field == 0 && !valid_user_char(mapped)) {
            set_error(s, "Username: only letters, numbers, _ and -");
            return;
        }
        target[len]     = mapped;
        target[len + 1] = '\0';
    }
}

/* Mouse input                                                         */
/* All hit-test rects use the same constants as the draw functions.    */
/* If you move a widget in a draw function, update the matching        */
/* rect here. They share PANEL_PAD, BTN_W, etc. for exactly this       */
/* reason.                                                             */

static void installer_on_mouse(Window *win, int lx, int ly,
                               bool left, bool right)
{
    InstallerState *s = (InstallerState *)win->user_data;
    if (!s)
        return;
    (void)right;

    bool click = left && !s->prev_left;
    s->prev_left = left;

    int cw = wm_client_w(win);
    int ch = wm_client_h(win);
    int px, py;
    panel_origin(cw, ch, &px, &py);

    int tx    = px + PANEL_PAD;
    int btn_y = py + PANEL_H - 60;

    /* reset hover state each frame */
    s->hover_btn    = -1;
    s->hover_app    = -1;
    s->hover_layout = -1;
    s->hover_wp     = -1;

    switch (s->step) {

    case STEP_WELCOME: {
        Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
        if (in_rect(next, lx, ly)) {
            s->hover_btn = 1;
            if (click) {
                s->step = STEP_KEYBOARD;
                s->step_enter_tick = get_ticks();
            }
        }
        break;
    }

    case STEP_KEYBOARD: {
        int ty = py + 40 + 50;
        for (int i = 0; i < 2; i++) {
            Rect r = { tx, ty, LAYOUT_CARD_W, LAYOUT_CARD_H };
            if (in_rect(r, lx, ly)) {
                s->hover_layout = i;
                if (click)
                    keyboard_layout = i;
            }
            ty += LAYOUT_GAP;
        }

        Rect back = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
        Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
        if (in_rect(back, lx, ly)) {
            s->hover_btn = 0;
            if (click) s->step = STEP_WELCOME;
        }
        if (in_rect(next, lx, ly)) {
            s->hover_btn = 1;
            if (click) {
                s->step = STEP_CREDENTIALS;
                s->step_enter_tick = get_ticks();
            }
        }
        break;
    }

    case STEP_CREDENTIALS: {
        if (s->account_conflict) {
            Rect replace = { tx, py + 280, 180, BTN_H };
            Rect rename = { tx + 204, py + 280, 180, BTN_H };
            if (in_rect(replace, lx, ly)) {
                s->hover_btn = 3;
                if (click) installer_accept_account(s, true);
            }
            if (in_rect(rename, lx, ly)) {
                s->hover_btn = 4;
                if (click) installer_change_name(s);
            }
            break;
        }
        int ty = py + 40 + 50 + 40;

        Rect ur = { tx + LABEL_COL_W, ty - 8, INPUT_W, INPUT_H };
        if (in_rect(ur, lx, ly) && click) s->active_field = 0;
        ty += 45;

        Rect pr = { tx + LABEL_COL_W, ty - 8, INPUT_W, INPUT_H };
        if (in_rect(pr, lx, ly) && click) s->active_field = 1;
        ty += 45;

        Rect cr = { tx + LABEL_COL_W, ty - 8, INPUT_W, INPUT_H };
        if (in_rect(cr, lx, ly) && click) s->active_field = 2;

        Rect back = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
        Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
        if (in_rect(back, lx, ly)) {
            s->hover_btn = 0;
            if (click) s->step = STEP_KEYBOARD;
        }
        if (in_rect(next, lx, ly)) {
            s->hover_btn = 1;
            if (click) {
                installer_submit_credentials(s);
            }
        }
        break;
    }

    case STEP_APPS: {
        int ty = py + 40 + 50 + 40;
        for (int i = 0; i < APP_COUNT; i++) {
            Rect row = { tx, ty - 5, APP_CARD_W, APP_CARD_H };
            if (in_rect(row, lx, ly)) {
                s->hover_app = i;
                if (click)
                    s->apps[i] = !s->apps[i];
            }
            ty += APP_ROW_H;
        }

        Rect back   = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
        Rect deploy = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
        if (in_rect(back, lx, ly)) {
            s->hover_btn = 0;
            if (click) s->step = STEP_CREDENTIALS;
        }
        if (in_rect(deploy, lx, ly)) {
            s->hover_btn = 1;
            if (click) {
                s->step = STEP_WALLPAPER;
                s->wp_auto = 0;
                s->wp_auto_tick = get_ticks();
                s->step_enter_tick = get_ticks();
            }
        }
        break;
    }

    case STEP_WALLPAPER: {
        int gy = py + 52 + 30 + 36;
        int nc = wp_visible_count();
        for (int i = 0; i < nc; i++) {
            Rect tile = { tx + (i & 1) * WP_TILE_DX,
                          gy + (i >> 1) * WP_TILE_DY,
                          WP_TILE_W, WP_TILE_H + 24 };
            if (in_rect(tile, lx, ly)) {
                s->hover_wp = i;
                if (click)
                    s->wallpaper = i;
            }
        }

        Rect back = { px + PANEL_W - 275, btn_y, BTN_W, BTN_H };
        if (in_rect(back, lx, ly)) {
            s->hover_btn = 0;
            if (click) s->step = STEP_APPS;
        }
        if (s->wallpaper >= 0) {
            Rect next = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };
            if (in_rect(next, lx, ly)) {
                s->hover_btn = 1;
                if (click) {
                    apply_wallpaper_choice(s);
                    s->step = STEP_INSTALLING;
                    s->progress = 0;
                    s->last_tick = get_ticks();
                    s->step_enter_tick = get_ticks();
                }
            }
        }
        break;
    }

    case STEP_FINISHED: {
        Rect reboot   = { px + PANEL_W - 280, btn_y, BTN_W, BTN_H };
        Rect cont_btn = { px + PANEL_W - 150, btn_y, BTN_W, BTN_H };

        if (in_rect(reboot, lx, ly)) {
            s->hover_btn = 1;
            if (click) {
                if (!s->install_ok) {
                    s->step = STEP_INSTALLING;
                    s->progress = 0;
                    s->last_tick = get_ticks();
                    s->step_enter_tick = s->last_tick;
                    s->fail_stage = 0;
                    win->needs_redraw = true;
                    break;
                }
                c_puts("[INSTALLER] Rebooting...\n");
                fs_save_to_disk();
                /* i8042 reset - pulse CPU reset line */
                __asm__ volatile(
                    "outb %0, %1"
                    :
                    : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64)
                );
            }
        }
        if (in_rect(cont_btn, lx, ly)) {
            s->hover_btn = 2;
            if (click) {
                c_puts("[INSTALLER] Continuing to desktop...\n");
                fs_save_to_disk();
                wm_destroy_window(win);
                wm_set_installer_mode(false);
            }
        }
        break;
    }

    default:
        break;
    }
}

/* Entry point */

void app_installer_create(void)
{
    int sw = wm_get_screen_w();
    int sh = wm_get_screen_h();

    wm_set_installer_mode(true);

    Window *win = wm_create_window("Aurion OS Setup", 0, 0, sw, sh);
    if (!win)
        return;

    win->is_chromeless = true;

    InstallerState *s = (InstallerState *)win->app_data;
    win->user_data = s;

    /* zero the whole struct, then set non-zero defaults */
    s->step            = STEP_WELCOME;
    s->progress        = 0;
    s->last_tick       = 0;
    s->step_enter_tick = get_ticks();
    s->active_field    = 0;
    s->account_conflict = false;
    s->replace_existing = false;
    s->account_prepared = false;
    s->conflict_choice = 0;
    s->approved_username[0] = 0;
    s->prev_left       = false;
    s->cursor_tick     = get_ticks();
    s->cursor_on       = true;
    s->hover_btn       = -1;
    s->hover_app       = -1;
    s->hover_layout    = -1;
    s->hover_wp        = -1;
    s->wallpaper       = -1;
    s->wp_auto         = 0;
    s->wp_auto_tick    = get_ticks();
    s->install_ok      = false;
    s->fail_stage      = 0;
    s->username[0]     = '\0';
    s->password[0]     = '\0';
    s->confirm[0]      = '\0';
    s->test_input[0]   = '\0';
    s->error[0]        = '\0';
    s->error_tick      = 0;

    s->disk_available = ata_is_available();
    c_puts(s->disk_available
        ? "[INSTALLER] Hard disk detected - installation will persist\n"
        : "[INSTALLER] No hard disk - installation will be RAM-only\n");

    for (int i = 0; i < APP_COUNT; i++)
        s->apps[i] = true;

    win->on_draw  = installer_on_draw;
    win->on_mouse = installer_on_mouse;
    win->on_key   = (void *)installer_on_key;
    win->on_tick  = installer_on_tick;

    wm_focus_window(win);
}