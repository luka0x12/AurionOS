/*
    AurionOS 3D demo app. Seven scenes behind a black menu.
    Blockforge: compact 8-byte face cache (positions/UVs re-derived from face
    tables at replay), cached water-surface and flower lists so no frame scans
    the 163k-cell world. Held-key drive, jump-to-climb, world clamp, hotbar +
    E inventory. Culling stays OFF; depth decides. Projection flips clip Y.
*/

#include <stdint.h>
#include <stdbool.h>
#include "utils.h"
#include "AurionGL/auriongl.h"
#include "window_manager.h"

extern uint32_t *gpu_get_backbuffer(void);
extern int gpu_get_width(void);
extern int gpu_get_height(void);
extern void gpu_fill_rect(int x, int y, int w, int h, uint32_t color);
extern void gpu_draw_string(int x, int y, const uint8_t *str, uint32_t fg, uint32_t bg);
extern uint32_t get_ticks(void);
extern void wm_destroy_window(Window *window);
extern void *kmalloc(uint32_t size);
extern void kfree(void *ptr);

static void app_3d_demo_on_close(Window *window);

typedef struct {
    Window *window;
    int demo_mode;
    float time;
    float rotation_x, rotation_y;
    bool auto_rotate;
    bool lighting;
    float aspect;
    bool agl_initialized;
    bool game_active;
    float solar_dist;
    float solar_elev;
    float tunnel_speed;
    bool probe_done;
    bool probe_ok;
    uint32_t probe_got;
    bool ctx_alive;
    uint32_t gl_gen;
    uint32_t now_ms;
} Demo3DState;

static Demo3DState state = {0};
static int menu_hover = -1;

/* shared AurionGL context
   aglInit kfrees the previous g_state, so two apps initialising independently
   destroyed each other's textures and depth buffer. Refcounted acquire keeps
   exactly one context alive while any GL window exists; texture IDs stay
   unique across apps via next_texture_id; generation increments on every real
   aglInit/aglShutdown so apps know when to re-upload. 
*/

static int agl_shared_refs = 0;
static uint32_t agl_shared_gen = 0;

uint32_t agl_shared_acquire(void) {
    if (agl_shared_refs == 0) {
        aglInit(gpu_get_width(), gpu_get_height(), gpu_get_backbuffer());
        agl_shared_gen++;
        aglEnable(AGL_DEPTH_TEST);
        aglDepthFunc(AGL_LESS);
        aglBlendFunc(AGL_SRC_ALPHA, AGL_ONE_MINUS_SRC_ALPHA);
        aglDisable(AGL_CULL_FACE);
    }
    agl_shared_refs++;
    return agl_shared_gen;
}

void agl_shared_release(void) {
    if (agl_shared_refs > 0) agl_shared_refs--;
    if (agl_shared_refs == 0) {
        aglShutdown();
        agl_shared_gen++;
    }
}

uint32_t agl_shared_generation(void) { return agl_shared_gen; }

bool agl_shared_alive(void) {
    int n = -1;
    aglGetIntegerv(AGL_MAX_LIGHTS, &n);
    return n == 4;
}

void demo_int_to_str(int v, char *buf) {
    char tmp[16];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

void uint_to_hex(uint32_t v, char *buf) {
    static const char d[16] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) buf[i] = d[(v >> (28 - i * 4)) & 0xF];
    buf[8] = '\0';
}

static int str_copy(char *d, const char *s) {
    int n = 0;
    while (*s) d[n++] = *s++;
    return n;
}

static void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b) {
    while (h >= 360.0f) h -= 360.0f;
    while (h < 0.0f) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - ((h / 60.0f) - 2.0f * (float)((int)(h / 60.0f) % 2) - 1.0f));
    if (x < 0.0f) x = -x;
    float m = v - c;
    if (h < 60.0f)       { *r = c; *g = x; *b = 0; }
    else if (h < 120.0f) { *r = x; *g = c; *b = 0; }
    else if (h < 180.0f) { *r = 0; *g = c; *b = x; }
    else if (h < 240.0f) { *r = 0; *g = x; *b = c; }
    else if (h < 300.0f) { *r = x; *g = 0; *b = c; }
    else                 { *r = c; *g = 0; *b = x; }
    *r += m; *g += m; *b += m;
}

static uint32_t bf_hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u
               + (uint32_t)z * 2246822519u + 0x9E3779B9u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}

float bf_hashf(int x, int y, int z) {
    return (float)(bf_hash3(x, y, z) & 0xFFFFFFu) / 16777216.0f;
}

/* rasterizer maps ndc +Y to the bottom row; flip clip Y so up is up */
static void demo_projection(float fovy_deg, float znear, float zfar) {
    aglMatrixMode(AGL_PROJECTION);
    aglLoadIdentity();
    aglPerspective(fovy_deg, state.aspect, znear, zfar);
    aglScale(1.0f, -1.0f, 1.0f);
    aglMatrixMode(AGL_MODELVIEW);
    aglLoadIdentity();
}

/* menu */

typedef struct { const char *name; const char *desc; } menu_item_t;

static const menu_item_t MENU_ITEMS[6] = {
    { "ROTATING SHAPES ", "cube, sphere, cylinder " },
    { "SOLAR SYSTEM ",    "eight planets, kepler speeds " },
    { "TUNNEL ",          "infinite flight through rings " },
    { "PARTICLES ",       "golden-angle spiral field " },
    { "TORUS KNOT ",      "(2,3) gold tube " },
    { "CUBE COLLECTOR ",  "arena arcade " },
};

static void menu_rect(int client_w, int i, int *y, int *h) {
    *y = 52 + i * 48;
    *h = 42;
    (void)client_w;
}

static void draw_menu(Window *win, int client_w, int client_h) {
    wm_fill_rect(win, 0, 0, client_w, client_h, 0xFF08080A);
    wm_draw_string(win, 24, 16,  "3D DEMOS", 0xFFF2F2F0, 0);
    wm_fill_rect(win, 24, 34, 56, 2, 0xFFE8A030);
    for (int i = 0; i < 6; i++) {
        int y, h;
        menu_rect(client_w, i, &y, &h);
        bool hov = (menu_hover == i);
        uint32_t bg = hov ? 0xFF1B1B20 : 0xFF121215;
        wm_fill_rect(win, 24, y, client_w - 48, h, bg);
        uint32_t line = hov ? 0xFF3A3A40 : 0xFF26262B;
        wm_fill_rect(win, 24, y, client_w - 48, 1, line);
        wm_fill_rect(win, 24, y + h - 1, client_w - 48, 1, line);
        wm_fill_rect(win, 24, y, 1, h, line);
        wm_fill_rect(win, client_w - 25, y, 1, h, line);
        if (hov) wm_fill_rect(win, 24, y, 3, h, 0xFFE8A030);
        char num[4] = { (char)('1' + i), '.', ' ', 0 };
        wm_draw_string(win, 36, y + 7,  num,
                       hov ? 0xFFE8A030 : 0xFF7A7A80, 0);
        wm_draw_string(win, 62, y + 7,  MENU_ITEMS[i].name,
                       0xFFF2F2F0, 0);
        wm_draw_string(win, 62, y + 23,  MENU_ITEMS[i].desc,
                       0xFF8A8A90, 0);
    }
    wm_draw_string(win, 24, client_h - 24,
                    "1-6 select   esc quit", 0xFF5A5A60, 0);
}

/* demo 2: rotating shapes */

static void demo_rotating_shapes(void) {
    aglClearColor(0.09f, 0.09f, 0.11f, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    demo_projection(45.0f, 0.1f, 100.0f);
    aglLookAt(0.0f, 0.6f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    aglDisable(AGL_TEXTURE_2D);
    aglEnable(AGL_DEPTH_TEST);
    if (state.lighting) {
        aglEnable(AGL_LIGHTING);
        /* ambients stay near zero: compute_lighting adds material.ambient
           per light plus a flat 0.1 term, so high ambients blow to white */
        float lp0[4] = { -0.5f, -0.8f, -0.6f, 0.0f };
        float ld0[4] = { 0.48f, 0.46f, 0.43f, 1.0f };
        float la0[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
        aglLightfv(AGL_LIGHT0, AGL_POSITION, lp0);
        aglLightfv(AGL_LIGHT0, AGL_DIFFUSE, ld0);
        aglLightfv(AGL_LIGHT0, AGL_AMBIENT, la0);
        float lspec0[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
        aglLightfv(AGL_LIGHT0, AGL_SPECULAR, lspec0);
        float lp1[4] = { 0.6f, -0.25f, 0.5f, 0.0f };
        float ld1[4] = { 0.20f, 0.23f, 0.29f, 1.0f };
        float la1[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
        aglLightfv(AGL_LIGHT1, AGL_POSITION, lp1);
        aglLightfv(AGL_LIGHT1, AGL_DIFFUSE, ld1);
        aglLightfv(AGL_LIGHT1, AGL_AMBIENT, la1);
        float le[4] = { 0, 0, 0, 1 };
        aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, le);
        float mspec[4] = { 0.05f, 0.05f, 0.05f, 1.0f };
        aglMaterialfv(AGL_FRONT_AND_BACK, AGL_SPECULAR, mspec);
        aglMaterialf(AGL_FRONT_AND_BACK, AGL_SHININESS, 24.0f);
    } else {
        aglDisable(AGL_LIGHTING);
    }
    aglPushMatrix();
    aglTranslate(-3.0f, 0.0f, 0.0f);
    aglRotate(state.rotation_y, 0.0f, 1.0f, 0.0f);
    aglRotate(state.rotation_x, 1.0f, 0.0f, 0.0f);
    float md[4] = { 1.0f, 0.32f, 0.28f, 1.0f };
    float ma0[4] = { 0.10f, 0.03f, 0.03f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, md);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, ma0);
    aglColor3f(1.0f, 0.32f, 0.28f);
    aglDrawCube();
    aglPopMatrix();
    aglPushMatrix();
    aglRotate(state.rotation_x, 1.0f, 0.0f, 0.0f);
    aglRotate(state.rotation_y * 0.7f, 0.0f, 1.0f, 0.0f);
    float mg[4] = { 0.3f, 0.9f, 0.4f, 1.0f };
    float ma1[4] = { 0.03f, 0.09f, 0.04f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, mg);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, ma1);
    aglColor3f(0.3f, 0.9f, 0.4f);
    aglDrawSphere(0.85f, 28, 20);
    aglPopMatrix();
    aglPushMatrix();
    aglTranslate(3.0f, 0.0f, 0.0f);
    aglRotate(state.rotation_y, 0.0f, 1.0f, 0.0f);
    float mb[4] = { 0.85f, 0.6f, 0.3f, 1.0f };
    float ma2[4] = { 0.09f, 0.06f, 0.03f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, mb);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, ma2);
    aglColor3f(0.85f, 0.6f, 0.3f);
    aglDrawCylinder(0.55f, 1.8f, 28);
    aglPopMatrix();
}

/* demo 3: solar system */

static uint32_t solar_earth_tex = 0;
static uint32_t solar_bands_tex = 0;
static bool solar_tex_ready = false;

typedef struct {
    const char *name;
    float radius;
    float dist;
    float period_years;
    float tilt_deg;
    float spin_deg_per_s;
    float r, g, b;
    int tex;
    bool has_ring;
    float phase;
} planet_t;

#define SOLAR_YEAR_S 12.0f

static const planet_t PLANETS[8] = {
    { "mercury ", 0.22f,  2.6f,  0.24f,   0.0f,  20.0f, 0.62f, 0.55f, 0.50f, 0, false, 0.9f },
    { "venus ",   0.42f,  3.6f,  0.62f,   3.0f,  -8.0f, 0.90f, 0.75f, 0.50f, 0, false, 2.1f },
    { "earth ",   0.46f,  4.8f,  1.00f,  23.5f,  90.0f, 1.00f, 1.00f, 1.00f, 1, false, 4.0f },
    { "mars ",    0.30f,  6.0f,  1.88f,  25.0f,  85.0f, 0.80f, 0.42f, 0.22f, 0, false, 1.4f },
    { "jupiter ", 1.30f,  8.4f, 11.86f,   3.0f, 220.0f, 0.85f, 0.75f, 0.60f, 2, false, 5.2f },
    { "saturn ",  1.12f, 10.8f, 29.40f,  26.0f, 200.0f, 0.90f, 0.82f, 0.62f, 2, true,  3.3f },
    { "uranus ",  0.72f, 13.0f, 84.00f,  98.0f, 120.0f, 0.62f, 0.80f, 0.85f, 0, false, 0.4f },
    { "neptune ", 0.70f, 15.0f, 165.0f,  28.0f, 130.0f, 0.32f, 0.42f, 0.85f, 0, false, 2.8f },
};

// TODO: idea, add AurionGL to vanta, so the development can be much easier.
static void solar_make_textures(void) {
    if (solar_tex_ready) return;
    static uint8_t ep[128 * 64 * 3];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) {
            float u = (float)x / 128.0f, v = (float)y / 64.0f;
            float n = 0.5f * (agl_sinf(u * 12.0f + v * 3.0f) * agl_cosf(v * 8.0f - u * 5.0f)
                    + agl_sinf(u * 7.0f - v * 11.0f) * 0.5f
                    + agl_cosf(u * 19.0f + v * 17.0f) * 0.25f);
            int i = (y * 128 + x) * 3;
            if (n > 0.1f) {
                ep[i] = (uint8_t)(58 + n * 44);
                ep[i + 1] = (uint8_t)(112 + n * 55);
                ep[i + 2] = (uint8_t)(84 + n * 30);
            } else {
                ep[i] = 26;
                ep[i + 1] = (uint8_t)(56 + (0.1f - n) * 180);
                int bl = (int)(110 + (0.1f - n) * 300);
                ep[i + 2] = (uint8_t)(bl > 215 ? 215 : bl);
            }
            float pole = v < 0.1f ? (0.1f - v) * 10.0f : (v > 0.9f ? (v - 0.9f) * 10.0f : 0.0f);
            if (pole > 1.0f) pole = 1.0f;
            if (pole > 0.0f) {
                ep[i]     = (uint8_t)(ep[i] * (1.0f - pole) + 238 * pole);
                ep[i + 1] = (uint8_t)(ep[i + 1] * (1.0f - pole) + 238 * pole);
                ep[i + 2] = (uint8_t)(ep[i + 2] * (1.0f - pole) + 248 * pole);
            }
        }
    }
    aglGenTextures(1, &solar_earth_tex);
    aglBindTexture(AGL_TEXTURE_2D, solar_earth_tex);
    aglTexImage2D(AGL_TEXTURE_2D, 0, AGL_RGB, 128, 64, 0, AGL_RGB, AGL_UNSIGNED_BYTE, ep);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_MIN_FILTER, AGL_LINEAR);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_MAG_FILTER, AGL_LINEAR);
    static uint8_t bp[64 * 32 * 3];
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            float v = (float)y / 32.0f;
            float band = 0.5f + 0.5f * agl_sinf(v * 42.0f + agl_sinf(v * 13.0f) * 1.6f);
            float wob = 0.9f + 0.1f * agl_sinf((float)x * 0.4f + v * 30.0f);
            int c = (int)(150 + band * 105 * wob);
            if (c > 255) c = 255;
            int i = (y * 64 + x) * 3;
            bp[i] = (uint8_t)c;
            bp[i + 1] = (uint8_t)c;
            bp[i + 2] = (uint8_t)c;
        }
    }
    aglGenTextures(1, &solar_bands_tex);
    aglBindTexture(AGL_TEXTURE_2D, solar_bands_tex);
    aglTexImage2D(AGL_TEXTURE_2D, 0, AGL_RGB, 64, 32, 0, AGL_RGB, AGL_UNSIGNED_BYTE, bp);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_MIN_FILTER, AGL_LINEAR);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_MAG_FILTER, AGL_LINEAR);
    solar_tex_ready = true;
}

static void solar_draw_stars(void) {
    aglDisable(AGL_LIGHTING);
    aglDisable(AGL_TEXTURE_2D);
    aglBegin(AGL_POINTS);
    for (int i = 0; i < 300; i++) {
        float u = bf_hashf(i, 11, 22) * 2.0f - 1.0f;
        float th = bf_hashf(i, 33, 44) * 6.2831853f;
        float rr = agl_sqrtf(1.0f - u * u);
        float b = 0.25f + 0.75f * bf_hashf(i, 55, 66);
        aglColor3f(b, b, b * 0.96f);
        aglVertex3f(70.0f * rr * agl_cosf(th), 70.0f * u, 70.0f * rr * agl_sinf(th));
    }
    aglEnd();
}

static void solar_draw_orbit(float dist) {
    aglBegin(AGL_LINES);
    for (int i = 0; i < 96; i++) {
        float a0 = (float)i / 96.0f * 6.2831853f;
        float a1 = (float)(i + 1) / 96.0f * 6.2831853f;
        aglVertex3f(dist * agl_cosf(a0), 0.0f, dist * agl_sinf(a0));
        aglVertex3f(dist * agl_cosf(a1), 0.0f, dist * agl_sinf(a1));
    }
    aglEnd();
}

static void solar_draw_ring(float in_r, float out_r) {
    aglEnable(AGL_BLEND);
    aglBegin(AGL_TRIANGLE_STRIP);
    for (int i = 0; i <= 48; i++) {
        float a = (float)i / 48.0f * 6.2831853f;
        float c = agl_cosf(a), s = agl_sinf(a);
        aglColor4f(0.78f, 0.72f, 0.58f, 0.8f);
        aglVertex3f(in_r * c, 0.0f, in_r * s);
        aglColor4f(0.66f, 0.6f, 0.5f, 0.35f);
        aglVertex3f(out_r * c, 0.0f, out_r * s);
    }
    aglEnd();
    aglDisable(AGL_BLEND);
}

static void demo_solar_system(void) {
    solar_make_textures();
    float t = state.time;
    if (state.solar_dist < 8.0f || state.solar_dist > 60.0f) state.solar_dist = 22.0f;
    if (state.solar_elev < 0.15f || state.solar_elev > 0.9f) state.solar_elev = 0.45f;
    aglClearColor(0.01f, 0.01f, 0.03f, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    demo_projection(50.0f, 0.1f, 300.0f);
    float cam_r = state.solar_dist;
    float ex = cam_r * 0.55f, ey = cam_r * state.solar_elev, ez = cam_r * 0.80f;
    aglLookAt(ex, ey, ez, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    if (state.auto_rotate) aglRotate(t * 0.5f, 0.0f, 1.0f, 0.0f);
    solar_draw_stars();
    float lp[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float ld[4] = { 1.0f, 0.96f, 0.88f, 1.0f };
    float la[4] = { 0.34f, 0.34f, 0.34f, 1.0f };
    aglLightfv(AGL_LIGHT0, AGL_POSITION, lp);
    aglLightfv(AGL_LIGHT0, AGL_DIFFUSE, ld);
    aglLightfv(AGL_LIGHT0, AGL_AMBIENT, la);
    float lspec[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    aglLightfv(AGL_LIGHT0, AGL_SPECULAR, lspec);
    aglDisable(AGL_LIGHTING);
    aglColor3f(0.38f, 0.38f, 0.44f);
    for (int i = 0; i < 8; i++) solar_draw_orbit(PLANETS[i].dist);
    float emiss[4] = { 1.0f, 0.82f, 0.35f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, emiss);
    aglColor3f(1.0f, 0.85f, 0.45f);
    aglDrawSphere(1.6f, 24, 18);
    float no_emiss[4] = { 0, 0, 0, 1 };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, no_emiss);
    {
        aglEnable(AGL_BLEND);
        aglDisable(AGL_DEPTH_TEST);
        aglColor4f(1.0f, 0.75f, 0.30f, 0.14f);
        aglDrawSphere(2.3f, 20, 14);
        aglColor4f(1.0f, 0.70f, 0.28f, 0.06f);
        aglDrawSphere(3.1f, 20, 14);
        aglEnable(AGL_DEPTH_TEST);
        aglDisable(AGL_BLEND);
    }
    aglEnable(AGL_LIGHTING);
    float zero4[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < 8; i++) {
        const planet_t *p = &PLANETS[i];
        float ang = t * 6.2831853f / (p->period_years * SOLAR_YEAR_S) + p->phase;
        float px = p->dist * agl_cosf(ang);
        float pz = p->dist * agl_sinf(ang);
        float diff[4] = { p->r, p->g, p->b, 1.0f };
        float amb[4] = { p->r * 0.22f + 0.07f, p->g * 0.22f + 0.07f, p->b * 0.22f + 0.07f, 1.0f };
        float spec[4] = { 0.05f, 0.05f, 0.05f, 1.0f };
        aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, diff);
        aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, amb);
        aglMaterialfv(AGL_FRONT_AND_BACK, AGL_SPECULAR, spec);
        aglMaterialf(AGL_FRONT_AND_BACK, AGL_SHININESS, 12.0f);
        aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, zero4);
        aglPushMatrix();
        aglTranslate(px, 0.0f, pz);
        aglRotate(p->tilt_deg, 0.0f, 0.0f, 1.0f);
        aglRotate(t * p->spin_deg_per_s, 0.0f, 1.0f, 0.0f);
        if (p->tex == 1) {
            aglEnable(AGL_TEXTURE_2D);
            aglBindTexture(AGL_TEXTURE_2D, solar_earth_tex);
            aglColor3f(1.0f, 1.0f, 1.0f);
        } else if (p->tex == 2) {
            aglEnable(AGL_TEXTURE_2D);
            aglBindTexture(AGL_TEXTURE_2D, solar_bands_tex);
            aglColor3f(p->r, p->g, p->b);
        } else {
            aglDisable(AGL_TEXTURE_2D);
            aglColor3f(p->r, p->g, p->b);
        }
        int seg = p->radius > 0.6f ? 24 : 16;
        aglDrawSphere(p->radius, seg, seg / 2 + 4);
        aglDisable(AGL_TEXTURE_2D);
        aglPopMatrix();
        if (p->has_ring) {
            aglPushMatrix();
            aglTranslate(px, 0.0f, pz);
            aglRotate(p->tilt_deg, 0.0f, 0.0f, 1.0f);
            aglDisable(AGL_LIGHTING);
            solar_draw_ring(p->radius * 1.4f, p->radius * 2.4f);
            aglEnable(AGL_LIGHTING);
            aglPopMatrix();
        }
        if (i == 2) {
            float mang = t * 6.2831853f / (0.0748f * SOLAR_YEAR_S);
            aglPushMatrix();
            aglTranslate(px + 1.0f * agl_cosf(mang), 0.0f, pz + 1.0f * agl_sinf(mang));
            float md[4] = { 0.75f, 0.75f, 0.75f, 1.0f };
            float mamb[4] = { 0.10f, 0.10f, 0.10f, 1.0f };
            aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, md);
            aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, mamb);
            aglColor3f(0.75f, 0.75f, 0.75f);
            aglDrawSphere(0.12f, 12, 8);
            aglPopMatrix();
        }
    }
    {
        char sbuf[80];
        int sn = str_copy(sbuf, "wheel/drag zoom  dist ");
        demo_int_to_str((int)state.solar_dist, sbuf + sn);
        while (sbuf[sn]) sn++;
        sn += str_copy(sbuf + sn, "  R rotate");
        sbuf[sn] = 0;
        wm_draw_string(state.window, 12, 10,  sbuf, 0xFFF2F2F0, 0);
    }
}

/* demo 4: infinite tunnel */

static void demo_tunnel(void) {
    float t = state.time;
    if (state.tunnel_speed < 1.0f || state.tunnel_speed > 20.0f) state.tunnel_speed = 6.5f;
    aglClearColor(0.02f, 0.02f, 0.05f, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    demo_projection(62.0f, 0.1f, 120.0f);
    aglTranslate(-agl_sinf(t * 0.6f) * 0.5f, -agl_cosf(t * 0.47f) * 0.4f, 0.0f);
    aglDisable(AGL_LIGHTING);
    aglDisable(AGL_TEXTURE_2D);
    aglEnable(AGL_DEPTH_TEST);
    aglBegin(AGL_POINTS);
    for (int i = 0; i < 160; i++) {
        float sx = (bf_hashf(i, 71, 3) - 0.5f) * 24.0f;
        float sy = (bf_hashf(i, 72, 3) - 0.5f) * 18.0f;
        float sz = -((bf_hashf(i, 73, 3) * 110.0f + t * state.tunnel_speed * 2.0f));
        while (sz < -110.0f) sz += 110.0f;
        while (sz > -0.5f) sz -= 110.0f;
        float b = 0.35f + 0.65f * bf_hashf(i, 74, 3);
        aglColor3f(b, b, b);
        aglVertex3f(sx, sy, sz);
    }
    aglEnd();
    const float SPACING = 3.0f;
    const int RINGS = 18;
    const float FAR_D = SPACING * (float)RINGS;
    float spd = state.tunnel_speed;
    float off = t * spd - SPACING * (float)((int)(t * spd / SPACING));
    for (int k = 0; k < RINGS; k++) {
        float z = -(k * SPACING) - (SPACING - off);
        if (z > -1.2f) continue;
        float d = -z;
        float fade = (FAR_D - 4.0f - d) / (FAR_D - 14.0f);
        if (fade < 0.0f) fade = 0.0f;
        if (fade > 1.0f) fade = 1.0f;
        fade = 0.25f + 0.75f * fade;
        float hue = t * 40.0f + (float)k * 22.0f;
        float r, g, b;
        hsv_to_rgb(hue, 0.85f, 1.0f, &r, &g, &b);
        float wob = 2.3f + 0.35f * agl_sinf(t * 1.3f + (float)k * 0.7f);
        aglPushMatrix();
        aglTranslate(agl_sinf(t * 0.8f + (float)k * 0.55f) * 0.35f,
                     agl_cosf(t * 0.7f + (float)k * 0.5f) * 0.3f, z);
        aglRotate(t * 24.0f + (float)k * 14.0f, 0.0f, 0.0f, 1.0f);
        aglColor3f(r * fade + 0.08f * fade, g * fade + 0.08f * fade, b * fade + 0.08f * fade);
        aglDrawTorus(0.32f, wob, 10, 26);
        aglColor3f(1.0f * fade + 0.15f, 0.95f * fade + 0.15f, 0.9f * fade + 0.15f);
        aglDrawTorus(0.10f, wob, 6, 26);
        aglPopMatrix();
    }
    {
        char tbuf[64];
        int tn = str_copy(tbuf, "W/S speed ");
        demo_int_to_str((int)state.tunnel_speed, tbuf + tn);
        while (tbuf[tn]) tn++;
        tbuf[tn] = 0;
        wm_draw_string(state.window, 12, 10,  tbuf, 0xFFF2F2F0, 0);
    }
}

/* demo 5: particles */

static void demo_particles(void) {
    float t = state.time;
    aglClearColor(0.04f, 0.04f, 0.07f, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    demo_projection(45.0f, 0.1f, 100.0f);
    aglLookAt(0.0f, 2.2f, 11.0f, 0.0f, 0.3f, 0.0f, 0.0f, 1.0f, 0.0f);
    if (state.auto_rotate) aglRotate(t * 8.0f, 0.0f, 1.0f, 0.0f);
    aglDisable(AGL_LIGHTING);
    aglDisable(AGL_TEXTURE_2D);
    aglEnable(AGL_DEPTH_TEST);
    aglEnable(AGL_BLEND);
    aglBlendFunc(AGL_SRC_ALPHA, AGL_ONE_MINUS_SRC_ALPHA);
    {
        float pulse = 0.9f + 0.25f * agl_sinf(t * 2.2f);
        aglDisable(AGL_DEPTH_TEST);
        aglColor4f(1.0f, 0.8f, 0.35f, 0.14f);
        aglPushMatrix();
        aglDrawSphere(1.1f * pulse, 14, 10);
        aglPopMatrix();
        aglEnable(AGL_DEPTH_TEST);
    }
    const int N = 140;
    for (int i = 0; i < N; i++) {
        float fi = (float)i;
        float wob = 1.0f + 0.25f * agl_sinf(t * 1.7f + fi * 0.6f);
        float radius = (1.2f + (float)(i % 12) * 0.28f) * wob;
        float speed = 0.7f + 0.9f * (float)(i % 5) / 5.0f;
        float ang = fi * 2.39996f + t * speed;
        float y = agl_sinf(t * 1.4f + fi * 0.35f) * 2.4f + (float)(i % 7 - 3) * 0.18f;
        float x = radius * agl_cosf(ang);
        float z = radius * agl_sinf(ang);
        float r, g, b;
        hsv_to_rgb(fi * 8.0f + t * 55.0f, 0.75f, 1.0f, &r, &g, &b);
        float tw = 0.65f + 0.35f * agl_sinf(t * 4.0f + fi * 1.3f);
        float sz = 0.10f + 0.09f * (0.5f + 0.5f * agl_sinf(t * 3.0f + fi * 2.1f));
        aglColor4f(r, g, b, tw);
        aglPushMatrix();
        aglTranslate(x, y, z);
        aglDrawSphere(sz, 7, 5);
        aglPopMatrix();
    }
    for (int i = 0; i < 45; i++) {
        float fi = (float)i;
        float cyc = 2.8f;
        float ph = bf_hashf(i, 81, 5);
        float lt = t * (0.8f + ph) + ph * cyc;
        float k = lt - cyc * (float)((int)(lt / cyc));
        float f = k / cyc;
        float ang = fi * 2.4f + ph * 6.28f;
        float rr = 0.3f + f * (1.6f + ph * 1.4f);
        float x = rr * agl_cosf(ang + t * 0.8f);
        float z = rr * agl_sinf(ang + t * 0.8f);
        float y = -2.4f + f * 6.4f;
        float r, g, b;
        hsv_to_rgb(35.0f + f * 25.0f + t * 20.0f, 0.9f, 1.0f, &r, &g, &b);
        aglColor4f(r, g, b, 1.0f - f * 0.85f);
        aglPushMatrix();
        aglTranslate(x, y, z);
        aglDrawSphere(0.13f * (1.0f - f * 0.6f), 6, 5);
        aglPopMatrix();
    }
    aglBegin(AGL_POINTS);
    for (int i = 0; i < 90; i++) {
        float u = bf_hashf(i, 91, 15) * 2.0f - 1.0f;
        float th = bf_hashf(i, 92, 15) * 6.2831853f;
        float rr = agl_sqrtf(1.0f - u * u);
        float rad = 7.0f + bf_hashf(i, 93, 15) * 5.0f;
        float tw = 0.35f + 0.65f * (0.5f + 0.5f * agl_sinf(t * 5.0f + (float)i * 1.7f));
        aglColor4f(0.75f * tw + 0.25f, 0.8f * tw + 0.2f, tw, tw);
        aglVertex3f(rad * rr * agl_cosf(th), rad * u * 0.6f, rad * rr * agl_sinf(th));
    }
    aglEnd();
    aglDisable(AGL_BLEND);
}

/* demo 6: torus knot */

static void knot_point(float t, float scale, agl_vec3_t *out) {
    float p = 2.0f, q = 3.0f;
    float r = 2.0f + 0.6f * agl_cosf(q * t);
    out->x = r * agl_cosf(p * t) * scale;
    out->y = r * agl_sinf(p * t) * scale;
    out->z = 0.6f * agl_sinf(q * t) * scale;
}

static void demo_torus_knot(void) {
    aglClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    demo_projection(45.0f, 0.1f, 100.0f);
    aglTranslate(0.0f, 0.0f, -9.0f);
    if (state.auto_rotate) {
        aglRotate(state.time * 14.0f, 0.0f, 1.0f, 0.0f);
        aglRotate(18.0f, 1.0f, 0.0f, 0.0f);
    } else {
        aglRotate(state.rotation_x, 1.0f, 0.5f, 0.2f);
        aglRotate(state.rotation_y, 0.2f, 1.0f, 0.5f);
    }
    if (state.lighting) {
        aglEnable(AGL_LIGHTING);
        float l0p[4] = { -0.5f, -0.75f, -0.6f, 0.0f };
        float l0d[4] = { 0.50f, 0.47f, 0.42f, 1.0f };
        float l0a[4] = { 0.10f, 0.10f, 0.10f, 1.0f };
        float l0s[4] = { 0.10f, 0.10f, 0.10f, 1.0f };
        aglLightfv(AGL_LIGHT0, AGL_POSITION, l0p);
        aglLightfv(AGL_LIGHT0, AGL_DIFFUSE, l0d);
        aglLightfv(AGL_LIGHT0, AGL_AMBIENT, l0a);
        aglLightfv(AGL_LIGHT0, AGL_SPECULAR, l0s);
        float l1p[4] = { 0.6f, -0.2f, 0.5f, 0.0f };
        float l1d[4] = { 0.24f, 0.28f, 0.36f, 1.0f };
        float l1a[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
        aglLightfv(AGL_LIGHT1, AGL_POSITION, l1p);
        aglLightfv(AGL_LIGHT1, AGL_DIFFUSE, l1d);
        aglLightfv(AGL_LIGHT1, AGL_AMBIENT, l1a);
    } else {
        aglDisable(AGL_LIGHTING);
    }
    aglDisable(AGL_TEXTURE_2D);
    float matd[4] = { 1.0f, 0.78f, 0.25f, 1.0f };
    float mats[4] = { 0.10f, 0.09f, 0.07f, 1.0f };
    float mata[4] = { 0.12f, 0.09f, 0.03f, 1.0f };
    float mate[4] = { 0.03f, 0.02f, 0.005f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, matd);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_SPECULAR, mats);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, mata);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, mate);
    aglMaterialf(AGL_FRONT_AND_BACK, AGL_SHININESS, 32.0f);
    aglColor3f(1.0f, 0.8f, 0.3f);
    const int SEG = 150;
    const int RING = 10;
    const float TUBE = 0.22f;
    const float SCALE = 1.05f;
    int quad_count = 0;
    aglBegin(AGL_QUADS);
    for (int i = 0; i < SEG; i++) {
        float t0 = (float)i / SEG * 6.2831853f;
        float t1 = (float)(i + 1) / SEG * 6.2831853f;
        agl_vec3_t c0, c1, ahead, behind;
        knot_point(t0, SCALE, &c0);
        knot_point(t1, SCALE, &c1);
        knot_point(t0 + 0.02f, SCALE, &ahead);
        knot_point(t0 - 0.02f, SCALE, &behind);
        agl_vec3_t T = agl_vec3_normalize(agl_vec3_sub(ahead, behind));
        agl_vec3_t core = { 2.0f * SCALE * agl_cosf(2.0f * t0),
                            2.0f * SCALE * agl_sinf(2.0f * t0), 0.0f };
        agl_vec3_t N = agl_vec3_normalize(agl_vec3_sub(c0, core));
        agl_vec3_t B = agl_vec3_cross(T, N);
        N = agl_vec3_cross(B, T);
        agl_vec3_t ahead2, behind2;
        knot_point(t1 + 0.02f, SCALE, &ahead2);
        knot_point(t1 - 0.02f, SCALE, &behind2);
        agl_vec3_t T1 = agl_vec3_normalize(agl_vec3_sub(ahead2, behind2));
        agl_vec3_t core1 = { 2.0f * SCALE * agl_cosf(2.0f * t1),
                             2.0f * SCALE * agl_sinf(2.0f * t1), 0.0f };
        agl_vec3_t N1 = agl_vec3_normalize(agl_vec3_sub(c1, core1));
        agl_vec3_t B1 = agl_vec3_cross(T1, N1);
        N1 = agl_vec3_cross(B1, T1);
        for (int j = 0; j < RING; j++) {
            float a0 = (float)j / RING * 6.2831853f;
            float a1 = (float)(j + 1) / RING * 6.2831853f;
            float ca0 = agl_cosf(a0), sa0 = agl_sinf(a0);
            float ca1 = agl_cosf(a1), sa1 = agl_sinf(a1);
            agl_vec3_t n00 = agl_vec3_add(agl_vec3_scale(N, ca0), agl_vec3_scale(B, sa0));
            agl_vec3_t n01 = agl_vec3_add(agl_vec3_scale(N, ca1), agl_vec3_scale(B, sa1));
            agl_vec3_t n10 = agl_vec3_add(agl_vec3_scale(N1, ca0), agl_vec3_scale(B1, sa0));
            agl_vec3_t n11 = agl_vec3_add(agl_vec3_scale(N1, ca1), agl_vec3_scale(B1, sa1));
            agl_vec3_t p00 = agl_vec3_add(c0, agl_vec3_scale(n00, TUBE));
            agl_vec3_t p01 = agl_vec3_add(c0, agl_vec3_scale(n01, TUBE));
            agl_vec3_t p11 = agl_vec3_add(c1, agl_vec3_scale(n11, TUBE));
            agl_vec3_t p10 = agl_vec3_add(c1, agl_vec3_scale(n10, TUBE));
            aglNormal3f(n00.x, n00.y, n00.z); aglVertex3f(p00.x, p00.y, p00.z);
            aglNormal3f(n01.x, n01.y, n01.z); aglVertex3f(p01.x, p01.y, p01.z);
            aglNormal3f(n11.x, n11.y, n11.z); aglVertex3f(p11.x, p11.y, p11.z);
            aglNormal3f(n10.x, n10.y, n10.z); aglVertex3f(p10.x, p10.y, p10.z);
            if (++quad_count >= 900) {
                aglEnd();
                quad_count = 0;
                aglBegin(AGL_QUADS);
            }
        }
    }
    aglEnd();
}

/* demo 7: cube collector */

typedef struct {
    float px, pz;
    float tx, tz;
    int score;
    float time_left;
    bool over;
    bool ready;
} collector_t;

static collector_t col = {0};

static void collector_place_target(int seed) {
    for (int tries = 0; tries < 12; tries++) {
        uint32_t h = bf_hash3(seed + tries, 91, 7);
        float nx = (float)((int)(h % 9) - 4);
        float nz = (float)((int)((h >> 8) % 9) - 4);
        float dx = nx - col.px, dz = nz - col.pz;
        if (dx * dx + dz * dz > 4.0f) {
            col.tx = nx;
            col.tz = nz;
            return;
        }
    }
    col.tx = 4.0f;
    col.tz = 4.0f;
}

static void collector_reset(void) {
    col.px = 0.0f; col.pz = 0.0f;
    col.score = 0;
    col.time_left = 60.0f;
    col.over = false;
    col.ready = true;
    collector_place_target(1);
}

static void demo_cube_collector(float dt) {
    if (!col.ready) collector_reset();
    if (!col.over) {
        col.time_left -= dt;
        if (col.time_left <= 0.0f) {
            col.time_left = 0.0f;
            col.over = true;
        }
        float dx = col.tx - col.px, dz = col.tz - col.pz;
        if (dx * dx + dz * dz < 1.3f * 1.3f) {
            col.score++;
            collector_place_target(col.score);
        }
    }
    aglClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    demo_projection(45.0f, 0.1f, 100.0f);
    aglLookAt(0.0f, 3.6f, 14.8f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    float lp[4] = { -0.35f, -0.85f, -0.45f, 0.0f };
    float ld[4] = { 0.55f, 0.53f, 0.50f, 1.0f };
    float la[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
    aglLightfv(AGL_LIGHT0, AGL_POSITION, lp);
    aglLightfv(AGL_LIGHT0, AGL_DIFFUSE, ld);
    aglLightfv(AGL_LIGHT0, AGL_AMBIENT, la);
    float lspec[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
    aglLightfv(AGL_LIGHT0, AGL_SPECULAR, lspec);
    float lp1[4] = { 0.5f, -0.3f, 0.6f, 0.0f };
    float ld1[4] = { 0.22f, 0.26f, 0.34f, 1.0f };
    float la1[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
    aglLightfv(AGL_LIGHT1, AGL_POSITION, lp1);
    aglLightfv(AGL_LIGHT1, AGL_DIFFUSE, ld1);
    aglLightfv(AGL_LIGHT1, AGL_AMBIENT, la1);
    aglDisable(AGL_TEXTURE_2D);
    aglEnable(AGL_DEPTH_TEST);
    aglDisable(AGL_LIGHTING);
    aglBegin(AGL_QUADS);

    for (int gx = -5; gx <= 5; gx++) {
        for (int gz = -5; gz <= 5; gz++) {
            bool c = ((gx ^ gz) & 1) == 0;
            float v = c ? 0.46f : 0.36f;
            aglColor3f(v, v, v * 1.08f);
            aglNormal3f(0.0f, 1.0f, 0.0f);
            aglVertex3f((float)gx - 0.5f, 0.0f, (float)gz - 0.5f);
            aglVertex3f((float)gx + 0.5f, 0.0f, (float)gz - 0.5f);
            aglVertex3f((float)gx + 0.5f, 0.0f, (float)gz + 0.5f);
            aglVertex3f((float)gx - 0.5f, 0.0f, (float)gz + 0.5f);
        }
    }

    aglEnd();
    aglEnable(AGL_LIGHTING);
    /* previous demos leave emission/specular behind; walls want neither */

    // slopy-manified code final boss
    float zeroe[4] = { 0, 0, 0, 1 };
    float zeros[4] = { 0, 0, 0, 1 };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, zeroe);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_SPECULAR, zeros);
    float wd[4] = { 0.62f, 0.58f, 0.52f, 1.0f };
    float wa[4] = { 0.10f, 0.09f, 0.08f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, wd);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, wa);
    aglColor3f(0.62f, 0.58f, 0.52f);
    aglPushMatrix(); aglTranslate(0.0f, 0.25f, -5.75f); aglScale(12.5f, 0.5f, 0.5f); aglDrawCube(); aglPopMatrix();
    aglPushMatrix(); aglTranslate(0.0f, 0.25f,  5.75f); aglScale(12.5f, 0.5f, 0.5f); aglDrawCube(); aglPopMatrix();
    aglPushMatrix(); aglTranslate(-5.75f, 0.25f, 0.0f); aglScale(0.5f, 0.5f, 12.5f); aglDrawCube(); aglPopMatrix();
    aglPushMatrix(); aglTranslate( 5.75f, 0.25f, 0.0f); aglScale(0.5f, 0.5f, 12.5f); aglDrawCube(); aglPopMatrix();
    float pd[4] = { 0.95f, 0.30f, 0.24f, 1.0f };
    float pa[4] = { 0.12f, 0.04f, 0.03f, 1.0f };
    float pe[4] = { 0.05f, 0.015f, 0.01f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, pd);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, pa);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, pe);
    aglColor3f(0.95f, 0.30f, 0.24f);
    aglPushMatrix();
    aglTranslate(col.px, 0.5f, col.pz);
    aglDrawCube();
    aglPopMatrix();
    float pulse = 0.8f + 0.1f * agl_sinf(state.time * 5.0f);
    float gd[4] = { 0.30f, 0.95f, 0.35f, 1.0f };
    float ga[4] = { 0.04f, 0.12f, 0.04f, 1.0f };
    float ge[4] = { 0.05f, 0.14f, 0.05f, 1.0f };
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_DIFFUSE, gd);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_AMBIENT, ga);
    aglMaterialfv(AGL_FRONT_AND_BACK, AGL_EMISSION, ge);
    aglColor3f(0.30f, 0.95f, 0.35f);
    aglPushMatrix();
    aglTranslate(col.tx, 0.7f + 0.15f * agl_sinf(state.time * 3.0f), col.tz);
    aglRotate(state.time * 90.0f, 0.0f, 1.0f, 0.0f);
    aglScale(pulse, pulse, pulse);
    aglDrawCube();
    aglPopMatrix();
    char buf[64];
    int n = str_copy(buf, "SCORE ");
    demo_int_to_str(col.score, buf + n);
    while (buf[n]) n++;
    n += str_copy(buf + n, "   TIME ");
    demo_int_to_str((int)col.time_left, buf + n);
    while (buf[n]) n++;
    buf[n] = 0;
    wm_draw_string(state.window, 12, 10,  buf, 0xFFF2F2F0, 0);
    if (col.over) {
        wm_draw_string(state.window, 12, 30,
                        "TIME UP - press R to restart, ESC for menu",
                       0xFFE8A030, 0);
    } else {
        wm_draw_string(state.window, 12, 30,
                        "WASD move - catch the green cube", 0xFF8A8A90, 0);
    }
}

/* BlockForge lives in src/apps/BlockForge.c now. Shared helpers
   (bf_hashf, str_copy, demo_int_to_str, uint_to_hex) stay global here. */

/* context lifecycle                                                   */

static void agl_drop_resources(void) {
    solar_tex_ready = false;
    solar_earth_tex = 0;
    solar_bands_tex = 0;
    state.probe_done = false;
}

static void agl_teardown(void) {
    if (state.agl_initialized) {
        agl_shared_release();
        state.agl_initialized = false;
        agl_drop_resources();
    }
}

/* input                                                               */

void app_3d_demo_mouse(Window *window, int local_x, int local_y, bool left, bool right) {
    static bool prev_l = false, prev_r = false;
    static int last_x = -1, last_y = -1;
    if (state.demo_mode == 0) {
        int cw = wm_client_w(window);
        menu_hover = -1;
        for (int i = 0; i < 6; i++) {
            int y, h;
            menu_rect(cw, i, &y, &h);
            if (local_x >= 24 && local_x < cw - 24 && local_y >= y && local_y < y + h) {
                menu_hover = i;
                break;
            }
        }
        bool click = left && !prev_l;
        if (click && menu_hover >= 0) {
            state.demo_mode = menu_hover + 2;
            state.time = 0.0f;
            state.rotation_x = 0.0f;
            state.rotation_y = 0.0f;
            state.auto_rotate = true;
            state.lighting = true;
            window->needs_redraw = true;
        }
        prev_l = left;
        prev_r = right;
        return;
    }
    if (state.demo_mode == 3) {
        bool any_down = left || right;
        bool was_down = prev_l || prev_r;
        if (any_down && was_down && last_x >= 0) {
            int dy = local_y - last_y;
            if (right) {
                state.solar_dist += (float)dy * 0.15f;
                if (state.solar_dist < 8.0f) state.solar_dist = 8.0f;
                if (state.solar_dist > 60.0f) state.solar_dist = 60.0f;
            } else if (left) {
                state.solar_elev -= (float)dy * 0.004f;
                if (state.solar_elev < 0.15f) state.solar_elev = 0.15f;
                if (state.solar_elev > 0.9f) state.solar_elev = 0.9f;
            }
        }
        prev_l = left;
        prev_r = right;
        last_x = local_x;
        last_y = local_y;
        return;
    }
    prev_l = left;
    prev_r = right;
    last_x = local_x;
    last_y = local_y;
}

void app_3d_demo_keypress(Window *window, uint16_t key_code) {
    char key = (char)(key_code & 0xFF);
    uint8_t key_hi = (uint8_t)((key_code >> 8) & 0xFF);
    if (key == 27) {
        if (state.demo_mode == 0) {
            agl_teardown();
            wm_destroy_window(window);
        } else {
            state.demo_mode = 0;
            state.game_active = false;
            menu_hover = -1;
        }
        return;
    }
    if (state.demo_mode == 0) {
        if (key >= '1' && key <= '6') {
            state.demo_mode = key - '0' + 1;
            state.time = 0.0f;
            state.rotation_x = 0.0f;
            state.rotation_y = 0.0f;
            state.auto_rotate = true;
            state.lighting = true;
        }
        return;
    }
    if (key_hi == 0x51 || key_hi == 0x49) {
        bool zin = (key_hi == 0x51);
        if (state.demo_mode == 3) {
            if (zin) state.solar_dist -= 2.5f;
            else state.solar_dist += 2.5f;
            if (state.solar_dist < 8.0f) state.solar_dist = 8.0f;
            if (state.solar_dist > 60.0f) state.solar_dist = 60.0f;
        } else if (state.demo_mode == 4) {
            if (zin) state.tunnel_speed += 1.0f;
            else state.tunnel_speed -= 1.0f;
            if (state.tunnel_speed < 1.0f) state.tunnel_speed = 1.0f;
            if (state.tunnel_speed > 20.0f) state.tunnel_speed = 20.0f;
        }
        return;
    }
    if (key == 'l' || key == 'L') state.lighting = !state.lighting;
    if ((key == 'r' || key == 'R') && state.demo_mode != 7) state.auto_rotate = !state.auto_rotate;
    if (state.demo_mode == 3) {
        if (key == 'w' || key == 'W' || key == '+' || key == '=') {
            state.solar_dist -= 2.5f;
            if (state.solar_dist < 8.0f) state.solar_dist = 8.0f;
        }
        if (key == 's' || key == 'S' || key == '-' || key == '_') {
            state.solar_dist += 2.5f;
            if (state.solar_dist > 60.0f) state.solar_dist = 60.0f;
        }
        if (key == 'a' || key == 'A') {
            state.solar_elev += 0.06f;
            if (state.solar_elev > 0.9f) state.solar_elev = 0.9f;
        }
        if (key == 'd' || key == 'D') {
            state.solar_elev -= 0.06f;
            if (state.solar_elev < 0.15f) state.solar_elev = 0.15f;
        }
        return;
    }
    if (state.demo_mode == 4) {
        if (key == 'w' || key == 'W' || key == '+' || key == '=') {
            state.tunnel_speed += 1.0f;
            if (state.tunnel_speed > 20.0f) state.tunnel_speed = 20.0f;
        }
        if (key == 's' || key == 'S' || key == '-' || key == '_') {
            state.tunnel_speed -= 1.0f;
            if (state.tunnel_speed < 1.0f) state.tunnel_speed = 1.0f;
        }
        return;
    }
    if (state.demo_mode == 7) {
        if (col.over) {
            if (key == 'r' || key == 'R') collector_reset();
            return;
        }
        float step = 0.5f;
        if (key == 'w' || key == 'W') col.pz -= step;
        if (key == 's' || key == 'S') col.pz += step;
        if (key == 'a' || key == 'A') col.px -= step;
        if (key == 'd' || key == 'D') col.px += step;
        if (col.px < -4.5f) col.px = -4.5f;
        if (col.px > 4.5f) col.px = 4.5f;
        if (col.pz < -4.5f) col.pz = -4.5f;
        if (col.pz > 4.5f) col.pz = 4.5f;
    }
}

/* render + lifecycle */

static void app_3d_demo_on_close(Window *window) {
    (void)window;
    agl_teardown();
}

void app_3d_demo_render(Window *window) {
    state.window = window;
    static uint32_t last_tick = 0;
    uint32_t now = get_ticks();
    if (last_tick == 0 || state.time == 0.0f) last_tick = now;
    uint32_t dtick = now - last_tick;
    last_tick = now;
    state.now_ms = now * 10u;
    float dt = (float)dtick * 0.01f;
    if (dt > 0.1f) dt = 0.1f;
    if (dt <= 0.0f) dt = 0.016f;
    state.time += dt;
    if (state.auto_rotate) {
        state.rotation_x += 40.0f * dt;
        state.rotation_y += 55.0f * dt;
    }
    int client_x = wm_client_x(window);
    int client_y = wm_client_y(window);
    int client_w = wm_client_w(window);
    int client_h = wm_client_h(window);
    state.aspect = (client_h > 0) ? ((float)client_w / (float)client_h) : (4.0f / 3.0f);
    if (state.demo_mode == 0) {
        draw_menu(window, client_w, client_h);
        return;
    }
    uint32_t *fb = gpu_get_backbuffer();
    if (!fb) {
        gpu_fill_rect(client_x, client_y, client_w, client_h, 0xFF000000);
        gpu_draw_string(client_x + 10, client_y + 10,
                        (const uint8_t *)"Error: no framebuffer", 0xFFFF0000, 0);
        return;
    }
    if (client_w <= 0 || client_h <= 0) return;
    int screen_w = gpu_get_width();
    int screen_h = gpu_get_height();
    if (screen_w <= 0 || screen_h <= 0) return;
    if (!state.agl_initialized) {
        state.gl_gen = agl_shared_acquire();
        state.agl_initialized = true;
        agl_drop_resources();
    }
    if (agl_shared_generation() != state.gl_gen) {
        state.gl_gen = agl_shared_generation();
        agl_drop_resources();
    }
    state.ctx_alive = agl_shared_alive();
    if (!state.ctx_alive) {
        gpu_fill_rect(client_x, client_y, client_w, client_h, 0xFF000000);
        gpu_draw_string(client_x + 10, client_y + 10,
                        (const uint8_t *)"agl context dead - retrying (kernel heap?)",
                        0xFFFF4040, 0);
        agl_teardown();
        return;
    }
    aglSetFramebuffer(fb);
    aglViewport(client_x, client_y, client_w, client_h);
    if (!state.probe_done) {
        aglClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        aglClear(AGL_COLOR_BUFFER_BIT);
        state.probe_got = fb[client_y * screen_w + client_x];
        state.probe_ok = (state.probe_got == 0xFFFF00FFu);
        state.probe_done = true;
    }
    switch (state.demo_mode) {
    case 2: demo_rotating_shapes(); break;
    case 3: demo_solar_system(); break;
    case 4: demo_tunnel(); break;
    case 5: demo_particles(); break;
    case 6: demo_torus_knot(); break;
    case 7: demo_cube_collector(dt); break;
    default: state.demo_mode = 0; break;
    }
}

void app_3d_demo_init(Window *window) {
    state.window = window;
    state.demo_mode = 0;
    state.time = 0.0f;
    state.rotation_x = 0.0f;
    state.rotation_y = 0.0f;
    state.auto_rotate = true;
    state.lighting = true;
    state.game_active = false;
    state.aspect = 4.0f / 3.0f;
    state.agl_initialized = false;
    state.probe_done = false;
    state.probe_ok = false;
    state.ctx_alive = false;
    state.solar_dist = 22.0f;
    state.solar_elev = 0.45f;
    state.tunnel_speed = 6.5f;
    state.now_ms = 0;
    state.gl_gen = 0;
    menu_hover = -1;
}

void app_3d_demo_create(void) {
    Window *win = wm_create_window("3D Demos", 80, 10, 660, 500);
    if (win) {
        app_3d_demo_init(win);
        win->on_draw = app_3d_demo_render;
        win->on_key = app_3d_demo_keypress;
        win->on_mouse = app_3d_demo_mouse;
        win->on_close = app_3d_demo_on_close;
    }
}