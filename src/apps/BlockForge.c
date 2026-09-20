/*
 * BlockForge - standalone voxel app: mine, build, explore.
 *
 * Own window, own dock entry, own frame state. The world is static
 * between edits: neighbor checks, AO and UV math run once per edit into
 * packed face caches, and each frame only replays them (camera cull,
 * distance cull, fog). Animated water tops, sky, particles and the HUD
 * stay direct. Every mutation funnels through bf_set, which raises
 * bf_cache_dirty for a rebuild on the next frame.
*/

#include <stdint.h>
#include <stdbool.h>
#include "utils.h"
#include "AurionGL/auriongl.h"
#include "window_manager.h"
#include "BlockForge.h"

extern uint32_t *gpu_get_backbuffer(void);
extern int gpu_get_width(void);
extern int gpu_get_height(void);
extern void gpu_fill_rect(int x, int y, int w, int h, uint32_t color);
extern void gpu_draw_string(int x, int y, const uint8_t *str, uint32_t fg, uint32_t bg);
extern uint32_t get_ticks(void);
extern int sys_getkey_rel(void);
extern void wm_destroy_window(Window *window);
extern void *kmalloc(uint32_t size);
extern void kfree(void *ptr);

static inline uint8_t bf_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline void bf_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %b0, %w1" :: "a"(v), "Nd"(port));
}

/* Holding a key only sustains motion if the keyboard repeats it. Some
 * firmware leaves typematic off, turning every hold into one lonely press
 * that the 600 ms net then kills mid-stride. Program 250 ms delay with
 * 30 Hz repeat once: bounded waits, mouse bytes discarded, any failure
 * silently keeps today's behavior.
*/

static void bf_keyboard_typematic(void) {
    static bool done = false;
    int i;
    uint8_t s = 0, d = 0;
    if (done) return;
    done = true;
    for (i = 0; i < 65536; i++) if (!(bf_inb(0x64) & 0x02)) break;
    bf_outb(0x60, 0xF3);
    for (i = 0; i < 65536; i++) {
        s = bf_inb(0x64);
        if (!(s & 0x01)) continue;
        d = bf_inb(0x60);
        if (s & 0x20) continue;
        break;
    }
    if (d != 0xFA) return;
    for (i = 0; i < 65536; i++) if (!(bf_inb(0x64) & 0x02)) break;
    bf_outb(0x60, 0x00);
    for (i = 0; i < 65536; i++) {
        s = bf_inb(0x64);
        if (!(s & 0x01)) continue;
        d = bf_inb(0x60);
        if (s & 0x20) continue;
        break;
    }
}

uint32_t agl_shared_acquire(void);
void agl_shared_release(void);
uint32_t agl_shared_generation(void);
bool agl_shared_alive(void);

static void BlockForge_on_close(Window *window);

static int str_copy(char *d, const char *s) {
    int n = 0;
    while (*s) d[n++] = *s++;
    return n;
}

typedef struct {
    Window *window;
    float aspect;
    bool agl_initialized;
    bool probe_done;
    bool probe_ok;
    uint32_t probe_got;
    bool ctx_alive;
    uint32_t gl_gen;
    uint32_t now_ms;
} BFState;

static BFState bst = {0};

/* rasterizer maps ndc +Y to the bottom row; flip clip Y so up is up */
static void bf_projection(float fovy_deg, float znear, float zfar) {
    aglMatrixMode(AGL_PROJECTION);
    aglLoadIdentity();
    aglPerspective(fovy_deg, bst.aspect, znear, zfar);
    aglScale(1.0f, -1.0f, 1.0f);
    aglMatrixMode(AGL_MODELVIEW);
    aglLoadIdentity();
}

/* BLOCKFORGE */ // TODO: name it better

#define BF_WX 64
#define BF_WY 40
#define BF_WZ 64
#define BF_WATER_LEVEL 13
#define BF_RENDER_DIST 40.0f
#define BF_BATCH_QUADS 1000

enum {
    BF_AIR = 0, BF_GRASS, BF_DIRT, BF_STONE, BF_SAND, BF_WATER, BF_LOG,
    BF_LEAVES, BF_BEDROCK, BF_SNOW, BF_PLANKS, BF_COBBLE, BF_COAL, BF_IRON,
    BF_FLOWER_R, BF_FLOWER_Y, BF_NUM_TYPES
};

enum {
    T_GRASS_TOP = 0, T_GRASS_SIDE, T_DIRT, T_STONE, T_SAND, T_WATER,
    T_LOG_SIDE, T_LOG_TOP, T_LEAVES, T_BEDROCK, T_SNOW, T_SNOW_SIDE,
    T_PLANKS, T_COBBLE, T_COAL, T_IRON, T_FLOWER_R, T_FLOWER_Y, T_NUM_TILES
};

static uint8_t bf_world[BF_WX * BF_WY * BF_WZ];
static uint8_t bf_atlas[128 * 128 * 4];
static float bf_tile_avg[T_NUM_TILES][3];
static uint32_t bf_atlas_tex = 0;
static bool bf_world_ready = false;
static bool bf_atlas_ready = false;

typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float yaw, pitch;
    bool grounded;
    bool flying;
    int selected;
} bf_player_t;

static bf_player_t bf_player;

typedef struct {
    float x, y, z, vx, vy, vz;
    float life, max_life;
    float r, g, b;
} bf_particle_t;

#define BF_MAX_PARTICLES 128
static bf_particle_t bf_particles[BF_MAX_PARTICLES];
static int bf_particle_count = 0;

typedef struct { float x, z, w, d, speed; } bf_cloud_t;
#define BF_MAX_CLOUDS 14
static bf_cloud_t bf_clouds[BF_MAX_CLOUDS];
static bool bf_clouds_ready = false;

static float bf_time = 0.0f;
static int bf_batch_count = 0;
static int bf_fps = 0;
static bool bf_cache_dirty = true;
static bool bf_inv_open = false;

/* held-key drive: break codes clear bits instantly; the timeout is only
 * a safety net for missed breaks, and opposing input cancels the stale
 * axis bit at once so a stuck key cannot keep driving against you
*/
#define BF_KEY_W 0u
#define BF_KEY_S 1u
#define BF_KEY_A 2u
#define BF_KEY_D 3u
#define BF_KEY_SPACE 4u
#define BF_KEY_C 5u
#define BF_HELD_TIMEOUT_MS 600u

/* PS/2 set-1 scancodes arrive in the high byte; bit 7 marks break (release).
    Break codes clear the held bit instantly, so releasing W stops walking.
    The timeout is now only a safety net for missed breaks (focus loss).
*/

#define BF_SC_W 0x11u
#define BF_SC_A 0x1Eu
#define BF_SC_S 0x1Fu
#define BF_SC_D 0x20u
#define BF_SC_C 0x2Eu
#define BF_SC_SPACE 0x39u

static unsigned bf_scancode_bit(unsigned sc) {
    switch (sc) {
    case BF_SC_W: return BF_KEY_W;
    case BF_SC_A: return BF_KEY_A;
    case BF_SC_S: return BF_KEY_S;
    case BF_SC_D: return BF_KEY_D;
    case BF_SC_C: return BF_KEY_C;
    case BF_SC_SPACE: return BF_KEY_SPACE;
    default: return 0xFFu;
    }
}
static uint32_t bf_held = 0;
static uint32_t bf_held_tick[6] = {0};
static uint32_t bf_ground_ms = 0;

static inline int bf_idx(int x, int y, int z) {
    return (y * BF_WZ + z) * BF_WX + x;
}

static inline bool bf_in_bounds(int x, int y, int z) {
    return x >= 0 && x < BF_WX && y >= 0 && y < BF_WY && z >= 0 && z < BF_WZ;
}

static inline uint8_t bf_get(int x, int y, int z) {
    if (y < 0) return BF_BEDROCK;
    if (!bf_in_bounds(x, y, z)) return BF_AIR;
    return bf_world[bf_idx(x, y, z)];
}

/* Incremental rebuild: track dirty block positions so bf_set() triggers
 * a local 27-block re-emit (~100us) instead of full 163K-block rebuild
 * (~2-4 sec). Bit 26 of packed face is free (face=3bits, tile=5bits,
 * y/z/x=6bits each = 21 bits used, bit 26 available).
*/

#define BF_DIRTY_MAX 8
#define BF_FACE_DEAD (1u << 26)
static int bf_dirty_x[BF_DIRTY_MAX];
static int bf_dirty_y[BF_DIRTY_MAX];
static int bf_dirty_z[BF_DIRTY_MAX];
static int bf_dirty_count = 0;
static bool bf_force_full_rebuild = true;

static inline void bf_set(int x, int y, int z, uint8_t block) {
    if (!bf_in_bounds(x, y, z)) return;
    if (block >= BF_NUM_TYPES) return;

    int index = bf_idx(x, y, z);
    if (bf_world[index] == block) return;

    bf_world[index] = block;
    bf_cache_dirty = true;

    /*
     * World generation, cache recovery, or a large edit already needs
     * a complete rebuild. No need to retain individual positions.
    */

    if (bf_force_full_rebuild) return;

    /* Multiple writes to one block require only one neighborhood update. */
    for (int i = 0; i < bf_dirty_count; i++) {
        if (bf_dirty_x[i] == x &&
            bf_dirty_y[i] == y &&
            bf_dirty_z[i] == z) {
            return;
        }
    }

    if (bf_dirty_count >= BF_DIRTY_MAX) {
        bf_force_full_rebuild = true;
        bf_dirty_count = 0;
        return;
    }

    int i = bf_dirty_count++;
    bf_dirty_x[i] = x;
    bf_dirty_y[i] = y;
    bf_dirty_z[i] = z;
}

static inline bool bf_is_solid(uint8_t b) {
    return b != BF_AIR && b != BF_WATER && b != BF_FLOWER_R && b != BF_FLOWER_Y;
}

static inline bool bf_is_opaque(uint8_t b) {
    return bf_is_solid(b);
}

static float bf_noise2(float x, float z) {
    int xi = (int)agl_floorf(x), zi = (int)agl_floorf(z);
    float fx = x - (float)xi, fz = z - (float)zi;
    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fz * fz * (3.0f - 2.0f * fz);
    float a = bf_hashf(xi, 0, zi);
    float b = bf_hashf(xi + 1, 0, zi);
    float c = bf_hashf(xi, 0, zi + 1);
    float d = bf_hashf(xi + 1, 0, zi + 1);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

static float bf_fbm2(float x, float z, int oct) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < oct; i++) {
        sum += bf_noise2(x * freq, z * freq) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

static void bf_plant_tree(int x, int z, int ground_y) {
    int trunk_h = 4 + (int)(bf_hashf(x, 31, z) * 2.9f);
    int top = ground_y + trunk_h;
    if (top + 2 >= BF_WY) return;
    for (int dy = trunk_h - 2; dy <= trunk_h + 1; dy++) {
        int rad = (dy >= trunk_h - 1) ? 1 : 2;
        for (int ox = -rad; ox <= rad; ox++) {
            for (int oz = -rad; oz <= rad; oz++) {
                if (rad == 2 && ox * ox + oz * oz > 5) continue;
                if (dy == trunk_h + 1 && (ox != 0 || oz != 0)) continue;
                int yy = ground_y + dy;
                if (bf_get(x + ox, yy, z + oz) == BF_AIR)
                    bf_set(x + ox, yy, z + oz, BF_LEAVES);
            }
        }
    }
    for (int i = 1; i <= trunk_h; i++) bf_set(x, ground_y + i, z, BF_LOG);
}

static void bf_generate_world(void) {
    for (int x = 0; x < BF_WX; x++) {
        for (int z = 0; z < BF_WZ; z++) {
            float e = bf_fbm2((float)x * 0.045f, (float)z * 0.045f, 4);
            float m = bf_fbm2((float)x * 0.02f + 137.0f, (float)z * 0.02f + 71.0f, 3);
            int h = 9 + (int)(e * 11.0f) + (int)(m * m * 14.0f);
            if (h < 2) h = 2;
            if (h > BF_WY - 8) h = BF_WY - 8;
            bf_set(x, 0, z, BF_BEDROCK);
            for (int y = 1; y < h; y++) {
                if (y < h - 3) {
                    uint8_t b = BF_STONE;
                    float ore = bf_hashf(x, y, z);
                    if (y < 14 && ore < 0.035f) b = BF_COAL;
                    else if (y < 10 && ore > 0.975f) b = BF_IRON;
                    bf_set(x, y, z, b);
                } else {
                    bf_set(x, y, z, BF_DIRT);
                }
            }
            uint8_t top_block;
            if (h <= BF_WATER_LEVEL + 1) top_block = BF_SAND;
            else if (h >= 26) top_block = BF_SNOW;
            else top_block = BF_GRASS;
            bf_set(x, h, z, top_block);
            for (int y = h + 1; y <= BF_WATER_LEVEL; y++) bf_set(x, y, z, BF_WATER);
        }
    }
    for (int x = 3; x < BF_WX - 3; x++) {
        for (int z = 3; z < BF_WZ - 3; z++) {
            for (int y = BF_WY - 10; y > BF_WATER_LEVEL; y--) {
                uint8_t b = bf_get(x, y, z);
                if (b == BF_AIR) continue;
                if (b == BF_GRASS && bf_hashf(x, 777, z) < 0.02f)
                    bf_plant_tree(x, z, y);
                break;
            }
        }
    }
    for (int x = 1; x < BF_WX - 1; x++) {
        for (int z = 1; z < BF_WZ - 1; z++) {
            for (int y = BF_WY - 10; y > BF_WATER_LEVEL; y--) {
                uint8_t b = bf_get(x, y, z);
                if (b == BF_AIR) continue;
                if (b == BF_GRASS && bf_get(x, y + 1, z) == BF_AIR) {
                    float f = bf_hashf(x, 555, z);
                    if (f < 0.012f) bf_set(x, y + 1, z, BF_FLOWER_R);
                    else if (f < 0.024f) bf_set(x, y + 1, z, BF_FLOWER_Y);
                }
                break;
            }
        }
    }

    /* Initial fill, not player edits: frame one must take the full cache
     * build path instead of an incremental patch of empty caches.
    */

    bf_dirty_count = 0;
}

static inline void bf_px(int tile, int x, int y, int r, int g, int b, int a) {
    int col = tile & 7, row = tile >> 3;
    int i = ((row * 16 + y) * 128 + col * 16 + x) * 4;
    bf_atlas[i] = (uint8_t)r;
    bf_atlas[i + 1] = (uint8_t)g;
    bf_atlas[i + 2] = (uint8_t)b;
    bf_atlas[i + 3] = (uint8_t)a;
}

static inline int bf_cl255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void bf_fill_noise_tile(int tile, int br, int bg, int bb, int vary, int alpha) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int v = (int)(bf_hashf(x, y, tile * 131) * 2.0f * vary) - vary;
            bf_px(tile, x, y, bf_cl255(br + v), bf_cl255(bg + v), bf_cl255(bb + v), alpha);
        }
    }
}

static void bf_precache_uv(void);

static void bf_build_atlas(void) {
    for (int i = 0; i < (int)sizeof(bf_atlas); i++) bf_atlas[i] = 0;
    for (int i = 0; i < T_NUM_TILES; i++)
        for (int c = 0; c < 3; c++) bf_tile_avg[i][c] = 0.0f;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int v = (int)(bf_hashf(x, y, 3) * 36.0f) - 18;
            bf_px(T_GRASS_TOP, x, y, bf_cl255(96 + v), bf_cl255(160 + v), bf_cl255(64 + v), 255);
        }
    bf_fill_noise_tile(T_DIRT, 134, 96, 67, 14, 255);
    
    /* Side faces map face-V 1 to the block top, and atlas V 1 is the last
     * byte row, so the green fringe lives at high row indices.
    */

    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int v = (int)(bf_hashf(x, y, 5) * 24.0f) - 12;
            int fringe = 4 + (int)(bf_hashf(x, 0, 7) * 3.0f);
            if (y >= 16 - fringe)
                bf_px(T_GRASS_SIDE, x, y, bf_cl255(96 + v), bf_cl255(158 + v), bf_cl255(62 + v), 255);
            else
                bf_px(T_GRASS_SIDE, x, y, bf_cl255(134 + v), bf_cl255(96 + v), bf_cl255(67 + v), 255);
        }
    bf_fill_noise_tile(T_STONE, 127, 127, 130, 12, 255);
    for (int i = 0; i < 5; i++) {
        int cx = (int)(bf_hashf(i, 1, 9) * 15), cy = (int)(bf_hashf(i, 2, 9) * 15);
        bf_px(T_STONE, cx, cy, 100, 100, 104, 255);
        bf_px(T_STONE, (cx + 1) & 15, cy, 104, 104, 108, 255);
    }
    bf_fill_noise_tile(T_SAND, 219, 207, 163, 10, 255);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            float w = agl_sinf((float)(x + y) * 0.8f) * 0.5f + 0.5f;
            bf_px(T_WATER, x, y, (int)(48 + w * 22), (int)(92 + w * 26), (int)(190 + w * 30), 168);
        }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int stripe = (x / 4 + (int)bf_hashf(x, y, 11)) & 1;
            int base = stripe ? 104 : 82;
            int v = (int)(bf_hashf(x, y, 13) * 14.0f) - 7;
            bf_px(T_LOG_SIDE, x, y, bf_cl255(base + v), bf_cl255(base * 7 / 10 + v), bf_cl255(base * 4 / 10 + v), 255);
        }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            float dx = (float)x - 7.5f, dy = (float)y - 7.5f;
            float d = agl_sqrtf(dx * dx + dy * dy);
            int ring = ((int)d) & 1;
            int base = ring ? 178 : 148;
            bf_px(T_LOG_TOP, x, y, base, base * 72 / 100, base * 44 / 100, 255);
        }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            float h = bf_hashf(x, y, 17);
            int g = h < 0.18f ? 74 : 106 + (int)(h * 46.0f);
            bf_px(T_LEAVES, x, y, 34 + (int)(h * 22.0f), g, 30 + (int)(h * 18.0f), 255);
        }
    bf_fill_noise_tile(T_BEDROCK, 52, 52, 54, 26, 255);
    bf_fill_noise_tile(T_SNOW, 240, 244, 250, 6, 255);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int v = (int)(bf_hashf(x, y, 19) * 24.0f) - 12;
            if (y >= 12)
                bf_px(T_SNOW_SIDE, x, y, bf_cl255(240 + v / 2), bf_cl255(244 + v / 2), 250, 255);
            else
                bf_px(T_SNOW_SIDE, x, y, bf_cl255(134 + v), bf_cl255(96 + v), bf_cl255(67 + v), 255);
        }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int board = y / 4;
            int seam = (y % 4 == 3) || ((x + board * 5) % 16 == 15);
            int base = seam ? 96 : 162 + (int)(bf_hashf(x, y, 23) * 16.0f) - 8;
            bf_px(T_PLANKS, x, y, base, base * 78 / 100, base * 46 / 100, 255);
        }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int cx = x / 4 + ((y / 8) & 1), cy = y / 4;
            int base = 96 + (int)(bf_hashf(cx, cy, 29) * 54.0f);
            bool edge = (x % 4 == 0) || (y % 4 == 0);
            if (edge) base -= 34;
            bf_px(T_COBBLE, x, y, base, base, base + 3, 255);
        }
    bf_fill_noise_tile(T_COAL, 127, 127, 130, 10, 255);
    bf_fill_noise_tile(T_IRON, 127, 127, 130, 10, 255);
    for (int c = 0; c < 4; c++) {
        int cx = 2 + (int)(bf_hashf(c, 3, 31) * 12), cy = 2 + (int)(bf_hashf(c, 4, 31) * 12);
        bf_px(T_COAL, cx, cy, 28, 28, 30, 255);
        bf_px(T_COAL, cx + 1, cy, 34, 34, 36, 255);
        bf_px(T_COAL, cx, cy + 1, 24, 24, 26, 255);
        bf_px(T_IRON, cx, cy, 216, 168, 128, 255);
        bf_px(T_IRON, cx + 1, cy, 208, 160, 122, 255);
        bf_px(T_IRON, cx, cy + 1, 200, 152, 116, 255);
    }
    for (int t = 0; t < 2; t++) {
        int tile = t ? T_FLOWER_Y : T_FLOWER_R;
        int pr = t ? 226 : 208, pg = t ? 208 : 48, pb = t ? 64 : 44;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) bf_px(tile, x, y, 0, 0, 0, 0);
        for (int y = 8; y < 15; y++) bf_px(tile, 7, y, 48, 128, 40, 255);
        bf_px(tile, 6, 10, 52, 138, 44, 255);
        bf_px(tile, 8, 11, 52, 138, 44, 255);
        for (int dy = 3; dy <= 6; dy++)
            for (int dx = 5; dx <= 9; dx++) {
                float dd = agl_sqrtf((float)((dx - 7) * (dx - 7) + (dy - 4) * (dy - 4)));
                if (dd < 2.3f) bf_px(tile, dx, dy, pr, pg, pb, 255);
            }
        bf_px(tile, 7, 4, 248, 224, 120, 255);
    }
    for (int tile = 0; tile < T_NUM_TILES; tile++) {
        uint32_t sums[3] = {0, 0, 0};
        int tx = (tile & 7) * 16;
        int ty = (tile >> 3) * 16;

        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int i = ((ty + y) * 128 + tx + x) * 4;

                sums[0] += bf_atlas[i];
                sums[1] += bf_atlas[i + 1];
                sums[2] += bf_atlas[i + 2];
            }
        }

        for (int c = 0; c < 3; c++) {
            bf_tile_avg[tile][c] = (float)sums[c] / 256.0f;
        }
    }
    bf_precache_uv();
    aglGenTextures(1, &bf_atlas_tex);
    aglBindTexture(AGL_TEXTURE_2D, bf_atlas_tex);
    aglTexImage2D(AGL_TEXTURE_2D, 0, AGL_RGBA, 128, 128, 0,
                  AGL_RGBA, AGL_UNSIGNED_BYTE, bf_atlas);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_MIN_FILTER, AGL_NEAREST);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_MAG_FILTER, AGL_NEAREST);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_WRAP_S, AGL_CLAMP);
    aglTexParameteri(AGL_TEXTURE_2D, AGL_TEXTURE_WRAP_T, AGL_CLAMP);
}

static const char *BF_NAMES[BF_NUM_TYPES] = {
    "air ", "grass ", "dirt ", "stone ", "sand ", "water ", "log ", "leaves ",
    "bedrock ", "snow ", "planks ", "cobble ", "coal ore ", "iron ore ",
    "red flower ", "yellow flower "
};

static const uint8_t BF_PLACEABLE[9] = {
    BF_GRASS, BF_DIRT, BF_STONE, BF_COBBLE, BF_PLANKS,
    BF_LOG, BF_LEAVES, BF_SAND, BF_SNOW
};

static int bf_block_tile(uint8_t b, int face) {
    switch (b) {
    case BF_GRASS:  return face == 0 ? T_GRASS_TOP : (face == 1 ? T_DIRT : T_GRASS_SIDE);
    case BF_DIRT:   return T_DIRT;
    case BF_STONE:  return T_STONE;
    case BF_SAND:   return T_SAND;
    case BF_WATER:  return T_WATER;
    case BF_LOG:    return face == 2 ? T_LOG_SIDE : T_LOG_TOP;
    case BF_LEAVES: return T_LEAVES;
    case BF_BEDROCK:return T_BEDROCK;
    case BF_SNOW:   return face == 0 ? T_SNOW : (face == 1 ? T_DIRT : T_SNOW_SIDE);
    case BF_PLANKS: return T_PLANKS;
    case BF_COBBLE: return T_COBBLE;
    case BF_COAL:   return T_COAL;
    case BF_IRON:   return T_IRON;
    default:        return T_STONE;
    }
}

static float bf_block_avg_r(uint8_t b) { return bf_tile_avg[bf_block_tile(b, 0)][0] / 255.0f; }
static float bf_block_avg_g(uint8_t b) { return bf_tile_avg[bf_block_tile(b, 0)][1] / 255.0f; }
static float bf_block_avg_b(uint8_t b) { return bf_tile_avg[bf_block_tile(b, 0)][2] / 255.0f; }

static const int8_t BF_FACE_N[6][3] = {
    { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
};

static const uint8_t BF_FACE_C[6][4][3] = {
    { {0,1,1}, {1,1,1}, {1,1,0}, {0,1,0} },
    { {0,0,0}, {1,0,0}, {1,0,1}, {0,0,1} },
    { {1,0,0}, {1,1,0}, {1,1,1}, {1,0,1} },
    { {0,0,1}, {0,1,1}, {0,1,0}, {0,0,0} },
    { {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1} },
    { {0,0,0}, {0,1,0}, {1,1,0}, {1,0,0} },
};

static const float BF_FACE_UV[6][4][2] = {
    { {0,1}, {1,1}, {1,0}, {0,0} },
    { {0,0}, {1,0}, {1,1}, {0,1} },
    { {0,0}, {0,1}, {1,1}, {1,0} },
    { {1,0}, {1,1}, {0,1}, {0,0} },
    { {0,0}, {1,0}, {1,1}, {0,1} },
    { {0,0}, {0,1}, {1,1}, {1,0} },
};

static const float BF_FACE_SHADE[6] = { 1.0f, 0.55f, 0.72f, 0.72f, 0.82f, 0.82f };
static const float BF_AO_TAB[4] = { 0.42f, 0.62f, 0.82f, 1.0f };

static void bf_uv_rect(int tile, float *u0, float *v0, float *u1, float *v1) {
    int col = tile & 7, row = tile >> 3;
    *u0 = ((float)col * 16.0f + 0.5f) / 128.0f;
    *v0 = ((float)row * 16.0f + 0.5f) / 128.0f;
    *u1 = ((float)col * 16.0f + 15.5f) / 128.0f;
    *v1 = ((float)row * 16.0f + 15.5f) / 128.0f;
}

/* Precomputed UV rects per tile: eliminates 4 float ops per face at replay. */
static float bf_uv_cache[T_NUM_TILES][4];

static void bf_precache_uv(void) {
    for (int t = 0; t < T_NUM_TILES; t++) {
        int col = t & 7, row = t >> 3;
        bf_uv_cache[t][0] = ((float)col * 16.0f + 0.5f) / 128.0f;
        bf_uv_cache[t][1] = ((float)row * 16.0f + 0.5f) / 128.0f;
        bf_uv_cache[t][2] = ((float)col * 16.0f + 15.5f) / 128.0f;
        bf_uv_cache[t][3] = ((float)row * 16.0f + 15.5f) / 128.0f;
    }
}

static void bf_batch_flush(void) {
    aglEnd();
    bf_batch_count = 0;
}

static void bf_batch_check(void) {
    if (bf_batch_count >= BF_BATCH_QUADS) {
        aglEnd();
        bf_batch_count = 0;
        aglBegin(AGL_QUADS);
    }
}

static float bf_cam_x, bf_cam_y, bf_cam_z;
static float bf_fog_r, bf_fog_g, bf_fog_b, bf_fog_start, bf_fog_end;
static float bf_fog_range_inv;

static float bf_frustum[6][4];

static void bf_extract_frustum(const float *m) {
    bf_frustum[0][0] = m[3]  + m[0];
    bf_frustum[0][1] = m[7]  + m[4];
    bf_frustum[0][2] = m[11] + m[8];
    bf_frustum[0][3] = m[15] + m[12];
    bf_frustum[1][0] = m[3]  - m[0];
    bf_frustum[1][1] = m[7]  - m[4];
    bf_frustum[1][2] = m[11] - m[8];
    bf_frustum[1][3] = m[15] - m[12];
    bf_frustum[2][0] = m[3]  + m[1];
    bf_frustum[2][1] = m[7]  + m[5];
    bf_frustum[2][2] = m[11] + m[9];
    bf_frustum[2][3] = m[15] + m[13];
    bf_frustum[3][0] = m[3]  - m[1];
    bf_frustum[3][1] = m[7]  - m[5];
    bf_frustum[3][2] = m[11] - m[9];
    bf_frustum[3][3] = m[15] - m[13];
    bf_frustum[4][0] = m[3]  + m[2];
    bf_frustum[4][1] = m[7]  + m[6];
    bf_frustum[4][2] = m[11] + m[10];
    bf_frustum[4][3] = m[15] + m[14];
    bf_frustum[5][0] = m[3]  - m[2];
    bf_frustum[5][1] = m[7]  - m[6];
    bf_frustum[5][2] = m[11] - m[10];
    bf_frustum[5][3] = m[15] - m[14];
    for (int i = 0; i < 6; i++) {
        float len = agl_sqrtf(bf_frustum[i][0] * bf_frustum[i][0]
                            + bf_frustum[i][1] * bf_frustum[i][1]
                            + bf_frustum[i][2] * bf_frustum[i][2]);
        if (len > 1e-8f) {
            float inv = 1.0f / len;
            bf_frustum[i][0] *= inv;
            bf_frustum[i][1] *= inv;
            bf_frustum[i][2] *= inv;
            bf_frustum[i][3] *= inv;
        }
    }
}

static inline bool bf_in_frustum(float x, float y, float z, float r) {
    for (int i = 0; i < 6; i++) {
        if (bf_frustum[i][0] * x + bf_frustum[i][1] * y + bf_frustum[i][2] * z + bf_frustum[i][3] < -r)
            return false;
    }
    return true;
}

/* face cache
    * 8 bytes per face: packed coords/face/tile plus four corner brightness
    * bytes. Positions and UVs are re-derived from the face tables at replay,
    * so the whole-world cache fits in ~600 KB of kernel heap. Water surfaces
    * and flowers keep their own packed lists: their geometry is static, only
    * the wave offset and the fog move.
*/

typedef struct {
    uint32_t p;             /* x | z<<6 | y<<12 | face<<18 | tile<<21 */
    uint8_t b[4];           /* corner brightness, 0..255 */
} bf_cface_t;

#define BF_OPAQUE_CAP 65536
#define BF_TRANS_CAP  16384
#define BF_WTOP_CAP   16384
#define BF_FLOWER_CAP 1024

static bf_cface_t *bf_opaque_cache = 0;
static int bf_opaque_n = 0;
static bf_cface_t *bf_trans_cache = 0;
static int bf_trans_n = 0;
static uint32_t *bf_wtop_list = 0;
static int bf_wtop_n = 0;
static uint32_t *bf_flower_list = 0;
static int bf_flower_n = 0;
static int bf_building = 0;
static bool bf_cache_overflow = false;

#define BF_SORT_BUCKETS 32
static int bf_sort_head[BF_SORT_BUCKETS];
static int *bf_sort_next = 0;

static bool bf_cache_alloc(void) {
    if (!bf_opaque_cache) {
        bf_opaque_cache = (bf_cface_t *)kmalloc((uint32_t)(sizeof(bf_cface_t) * BF_OPAQUE_CAP));
        if (!bf_opaque_cache) return false;
    }
    if (!bf_trans_cache) {
        bf_trans_cache = (bf_cface_t *)kmalloc((uint32_t)(sizeof(bf_cface_t) * BF_TRANS_CAP));
        if (!bf_trans_cache) return false;
    }
    if (!bf_wtop_list) {
        bf_wtop_list = (uint32_t *)kmalloc((uint32_t)(sizeof(uint32_t) * BF_WTOP_CAP));
        if (!bf_wtop_list) return false;
    }
    if (!bf_flower_list) {
        bf_flower_list = (uint32_t *)kmalloc((uint32_t)(sizeof(uint32_t) * BF_FLOWER_CAP));
        if (!bf_flower_list) return false;
    }
    if (!bf_sort_next) {
        bf_sort_next = (int *)kmalloc((uint32_t)(sizeof(int) * BF_OPAQUE_CAP));
        /* best-effort: null falls back to unsorted replay, cache stays valid */
    }
    return true;
}

static void bf_cache_push(int water, uint32_t p, const uint8_t *b) {
    bf_cface_t *dst = water ? bf_trans_cache : bf_opaque_cache;
    int *np = water ? &bf_trans_n : &bf_opaque_n;
    int cap = water ? BF_TRANS_CAP : BF_OPAQUE_CAP;
    if (dst && *np < cap) {
        dst[*np].p = p;
        dst[*np].b[0] = b[0];
        dst[*np].b[1] = b[1];
        dst[*np].b[2] = b[2];
        dst[*np].b[3] = b[3];
        (*np)++;
    } else {
        bf_cache_overflow = true;
    }
}

/* Single quad submit: replaces 13 immediate-mode calls per face. */
static void bf_submit_cached_face(bf_cface_t *c, int i, float dist) {
    uint32_t p = c[i].p;
    int x = (int)(p & 63u);
    int z = (int)((p >> 6) & 63u);
    int y = (int)((p >> 12) & 63u);
    int f = (int)((p >> 18) & 7u);
    int tile = (int)((p >> 21) & 31u);
    int nx = BF_FACE_N[f][0], ny = BF_FACE_N[f][1], nz = BF_FACE_N[f][2];
    float fog = (bf_fog_end - dist) * bf_fog_range_inv;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;
    const float *uv = bf_uv_cache[tile];
    float u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
    float fr = bf_fog_r * (1.0f - fog), fg = bf_fog_g * (1.0f - fog), fb = bf_fog_b * (1.0f - fog);
    agl_qv_t q[4];
    for (int k = 0; k < 4; k++) {
        float br = (float)c[i].b[k] * (1.0f / 255.0f) * fog;
        q[k].r = fr + br; q[k].g = fg + br; q[k].b = fb + br; q[k].a = 1.0f;
        q[k].u = BF_FACE_UV[f][k][0] > 0.5f ? u1 : u0;
        q[k].v = BF_FACE_UV[f][k][1] > 0.5f ? v1 : v0;
        q[k].x = (float)x + (float)BF_FACE_C[f][k][0];
        q[k].y = (float)y + (float)BF_FACE_C[f][k][1];
        q[k].z = (float)z + (float)BF_FACE_C[f][k][2];
    }
    aglSubmitQuad((float)nx, (float)ny, (float)nz, q);
}

static void bf_replay_cache_unsorted(bf_cface_t *c, int n) {
    if (!c || n <= 0) return;
    float rd2 = BF_RENDER_DIST * BF_RENDER_DIST;
    int count = 0;
    aglBegin(AGL_QUADS);
    for (int i = 0; i < n; i++) {
        uint32_t p = c[i].p;
        if (p & BF_FACE_DEAD) continue;
        int x = (int)(p & 63u);
        int z = (int)((p >> 6) & 63u);
        int y = (int)((p >> 12) & 63u);
        int f = (int)((p >> 18) & 7u);
        float rx = (float)x + 0.5f - bf_cam_x;
        float ry = (float)y + 0.5f - bf_cam_y;
        float rz = (float)z + 0.5f - bf_cam_z;
        float d2 = rx * rx + ry * ry + rz * rz;
        if (d2 > rd2) continue;
        if (!bf_in_frustum((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f, 0.87f))
            continue;
        int nx = BF_FACE_N[f][0], ny = BF_FACE_N[f][1], nz = BF_FACE_N[f][2];
        if (rx * (float)nx + ry * (float)ny + rz * (float)nz >= 0.0f) continue;
        bf_submit_cached_face(c, i, agl_sqrtf(d2));
        if (++count >= BF_BATCH_QUADS) {
            aglEnd();
            count = 0;
            aglBegin(AGL_QUADS);
        }
    }
    aglEnd();
}

/* Two-pass bucket sort by distance squared (no sqrt in pass 1).
 * Opaque sorts front-to-back (early-z kills overdraw); transparent sorts
 * back-to-front (correct alpha blend with depth writes on).
*/

static void bf_replay_cache(bf_cface_t *c, int n, bool front_to_back) {
    if (!c || n <= 0) return;
    if (!bf_sort_next) {
        bf_replay_cache_unsorted(c, n);
        return;
    }
    if (n > BF_OPAQUE_CAP) n = BF_OPAQUE_CAP;
    float rd2 = BF_RENDER_DIST * BF_RENDER_DIST;
    for (int b = 0; b < BF_SORT_BUCKETS; b++) bf_sort_head[b] = -1;
    float bucket_scale = (float)BF_SORT_BUCKETS / rd2;
    for (int i = 0; i < n; i++) {
        uint32_t p = c[i].p;
        if (p & BF_FACE_DEAD) continue;
        int x = (int)(p & 63u);
        int z = (int)((p >> 6) & 63u);
        int y = (int)((p >> 12) & 63u);
        int f = (int)((p >> 18) & 7u);
        float rx = (float)x + 0.5f - bf_cam_x;
        float ry = (float)y + 0.5f - bf_cam_y;
        float rz = (float)z + 0.5f - bf_cam_z;
        float d2 = rx * rx + ry * ry + rz * rz;
        if (d2 > rd2) continue;
        int nx = BF_FACE_N[f][0], ny = BF_FACE_N[f][1], nz = BF_FACE_N[f][2];
        if (rx * (float)nx + ry * (float)ny + rz * (float)nz >= 0.0f) continue;
        if (!bf_in_frustum((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f, 0.87f))
            continue;
        int bucket = (int)(d2 * bucket_scale);
        if (bucket < 0) bucket = 0;
        else if (bucket >= BF_SORT_BUCKETS) bucket = BF_SORT_BUCKETS - 1;
        bf_sort_next[i] = bf_sort_head[bucket];
        bf_sort_head[bucket] = i;
    }
    int count = 0;
    aglBegin(AGL_QUADS);
    if (front_to_back) {
        for (int b = 0; b < BF_SORT_BUCKETS; b++) {
            for (int i = bf_sort_head[b]; i >= 0; i = bf_sort_next[i]) {
                uint32_t p = c[i].p;
                if (p & BF_FACE_DEAD) continue;
                int x = (int)(p & 63u);
                int z = (int)((p >> 6) & 63u);
                int y = (int)((p >> 12) & 63u);
                float rx = (float)x + 0.5f - bf_cam_x;
                float ry = (float)y + 0.5f - bf_cam_y;
                float rz = (float)z + 0.5f - bf_cam_z;
                bf_submit_cached_face(c, i, agl_sqrtf(rx * rx + ry * ry + rz * rz));
                if (++count >= BF_BATCH_QUADS) {
                    aglEnd();
                    count = 0;
                    aglBegin(AGL_QUADS);
                }
            }
        }
    } else {
        for (int b = BF_SORT_BUCKETS - 1; b >= 0; b--) {
            for (int i = bf_sort_head[b]; i >= 0; i = bf_sort_next[i]) {
                uint32_t p = c[i].p;
                if (p & BF_FACE_DEAD) continue;
                int x = (int)(p & 63u);
                int z = (int)((p >> 6) & 63u);
                int y = (int)((p >> 12) & 63u);
                float rx = (float)x + 0.5f - bf_cam_x;
                float ry = (float)y + 0.5f - bf_cam_y;
                float rz = (float)z + 0.5f - bf_cam_z;
                bf_submit_cached_face(c, i, agl_sqrtf(rx * rx + ry * ry + rz * rz));
                if (++count >= BF_BATCH_QUADS) {
                    aglEnd();
                    count = 0;
                    aglBegin(AGL_QUADS);
                }
            }
        }
    }
    aglEnd();
}

static void bf_emit_face(int x, int y, int z, int f, uint8_t block, float extra_shade) {
    int nx = BF_FACE_N[f][0], ny = BF_FACE_N[f][1], nz = BF_FACE_N[f][2];
    float cx = (float)x + 0.5f, cy = (float)y + 0.5f, cz = (float)z + 0.5f;
    if (!bf_building) {
        float to_cam_x = bf_cam_x - cx, to_cam_y = bf_cam_y - cy, to_cam_z = bf_cam_z - cz;
        if (to_cam_x * (float)nx + to_cam_y * (float)ny + to_cam_z * (float)nz <= 0.0f) return;
    }
    float ao[4];
    for (int c = 0; c < 4; c++) {
        int ox = BF_FACE_C[f][c][0], oy = BF_FACE_C[f][c][1], oz = BF_FACE_C[f][c][2];
        int sx = ox * 2 - 1, sy = oy * 2 - 1, sz = oz * 2 - 1;
        int bx = x + nx, by = y + ny, bz = z + nz;
        int s1x = 0, s1y = 0, s1z = 0, s2x = 0, s2y = 0, s2z = 0;
        if (nx != 0)      { s1y = sy; s2z = sz; }
        else if (ny != 0) { s1x = sx; s2z = sz; }
        else              { s1x = sx; s2y = sy; }
        bool o1 = bf_is_opaque(bf_get(bx + s1x, by + s1y, bz + s1z));
        bool o2 = bf_is_opaque(bf_get(bx + s2x, by + s2y, bz + s2z));
        bool oc = bf_is_opaque(bf_get(bx + s1x + s2x, by + s1y + s2y, bz + s1z + s2z));
        int level = (o1 && o2) ? 0 : 3 - ((int)o1 + (int)o2 + (int)oc);
        ao[c] = BF_AO_TAB[level];
    }
    int tile = bf_block_tile(block, ny != 0 ? (ny > 0 ? 0 : 1) : 2);
    float shade = BF_FACE_SHADE[f] * extra_shade;
    if (bf_building) {
        uint8_t bb[4];
        for (int c = 0; c < 4; c++) {
            float v = shade * ao[c] * 255.0f + 0.5f;
            bb[c] = (uint8_t)(v > 255.0f ? 255.0f : v);
        }
        uint32_t p = (uint32_t)x | ((uint32_t)z << 6) | ((uint32_t)y << 12)
                   | ((uint32_t)f << 18) | ((uint32_t)tile << 21);
        bf_cache_push(block == BF_WATER, p, bb);
        return;
    }
    float to_cam_x = bf_cam_x - cx, to_cam_y = bf_cam_y - cy, to_cam_z = bf_cam_z - cz;
    float dist = agl_sqrtf(to_cam_x * to_cam_x + to_cam_y * to_cam_y + to_cam_z * to_cam_z);
    float fog = (bf_fog_end - dist) * bf_fog_range_inv;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;
    float u0, v0, u1, v1;
    bf_uv_rect(tile, &u0, &v0, &u1, &v1);
    float efr = bf_fog_r * (1.0f - fog), efg = bf_fog_g * (1.0f - fog), efb = bf_fog_b * (1.0f - fog);
    agl_qv_t q[4];
    for (int c = 0; c < 4; c++) {
        float bright = shade * ao[c] * fog;
        q[c].r = efr + bright; q[c].g = efg + bright; q[c].b = efb + bright; q[c].a = 1.0f;
        q[c].u = BF_FACE_UV[f][c][0] > 0.5f ? u1 : u0;
        q[c].v = BF_FACE_UV[f][c][1] > 0.5f ? v1 : v0;
        q[c].x = (float)x + (float)BF_FACE_C[f][c][0];
        q[c].y = (float)y + (float)BF_FACE_C[f][c][1];
        q[c].z = (float)z + (float)BF_FACE_C[f][c][2];
    }
    aglSubmitQuad((float)nx, (float)ny, (float)nz, q);
    bf_batch_count++;
    bf_batch_check();
}

static void bf_render_opaque(void) {
    float rd2 = BF_RENDER_DIST * BF_RENDER_DIST;
    if (!bf_building) {
        bf_batch_count = 0;
        aglBegin(AGL_QUADS);
    }
    extern void sched_heartbeat(void);
    for (int y = 0; y < BF_WY; y++) {
        sched_heartbeat(); /* full-world rebuilds take seconds: alive, not hung */
        for (int z = 0; z < BF_WZ; z++) {
            for (int x = 0; x < BF_WX; x++) {
                uint8_t b = bf_world[bf_idx(x, y, z)];
                if (b == BF_AIR || b == BF_WATER || b == BF_FLOWER_R || b == BF_FLOWER_Y)
                    continue;
                if (!bf_building) {
                    float dx = (float)x + 0.5f - bf_cam_x;
                    float dy = (float)y + 0.5f - bf_cam_y;
                    float dz = (float)z + 0.5f - bf_cam_z;
                    if (dx * dx + dy * dy + dz * dz > rd2) continue;
                }
                float tint = 1.0f;
                if (b == BF_GRASS)
                    tint = 0.96f + 0.08f * bf_hashf(x, 41, z);
                for (int f = 0; f < 6; f++) {
                    uint8_t nb = bf_get(x + BF_FACE_N[f][0],
                                        y + BF_FACE_N[f][1],
                                        z + BF_FACE_N[f][2]);
                    if (!bf_is_opaque(nb))
                        bf_emit_face(x, y, z, f, b, tint);
                }
            }
        }
    }
    if (!bf_building) bf_batch_flush();
}

/* flowers: direct draw, double-sided (culling is off) */
static void bf_emit_cross(int x, int y, int z, uint8_t block) {
    float dx = (float)x + 0.5f - bf_cam_x;
    float dy = (float)y + 0.5f - bf_cam_y;
    float dz = (float)z + 0.5f - bf_cam_z;
    float dist = agl_sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist > BF_RENDER_DIST) return;
    if (!bf_in_frustum((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f, 0.87f))
        return;
    float fog = (bf_fog_end - dist) * bf_fog_range_inv;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;
    int tile = block == BF_FLOWER_R ? T_FLOWER_R : T_FLOWER_Y;
    float u0, v0, u1, v1;
    bf_uv_rect(tile, &u0, &v0, &u1, &v1);
    float fx = (float)x, fy = (float)y, fz = (float)z;
    static const float DIAG[2][4][2] = {
        { {0.15f, 0.15f}, {0.85f, 0.85f}, {0.85f, 0.85f}, {0.15f, 0.15f} },
        { {0.85f, 0.15f}, {0.15f, 0.85f}, {0.15f, 0.85f}, {0.85f, 0.15f} },
    };
    float cr = bf_fog_r * (1.0f - fog) + 0.9f * fog;
    float cg = bf_fog_g * (1.0f - fog) + 0.9f * fog;
    float cb = bf_fog_b * (1.0f - fog) + 0.9f * fog;
    for (int d = 0; d < 2; d++) {
        agl_qv_t q[4];
        for (int c = 0; c < 4; c++) {
            q[c].r = cr; q[c].g = cg; q[c].b = cb; q[c].a = 1.0f;
            q[c].u = (c == 1 || c == 2) ? u1 : u0;
            q[c].v = (c >= 2) ? v0 : v1;
            q[c].x = fx + DIAG[d][c][0];
            q[c].y = fy + (c >= 2 ? 1.0f : 0.0f);
            q[c].z = fz + DIAG[d][c][1];
        }
        aglSubmitQuad(0.0f, 0.0f, 1.0f, q);
        bf_batch_count++;
        bf_batch_check();
    }
}

/* corner-evaluated wave: shared corners get identical heights from both
 * cells, so the surface is watertight. Per-cell flat tops opened wedge
 * cracks that showed sand through the seams.
*/

static float bf_wave_h(int x, int z) {
    return 0.04f * agl_sinf(bf_time * 1.6f + (float)(x + z) * 0.7f);
}

static void bf_emit_water_top(int x, int y, int z) {
    float dx = (float)x + 0.5f - bf_cam_x;
    float dy = (float)y + 0.5f - bf_cam_y;
    float dz = (float)z + 0.5f - bf_cam_z;
    if (dx * dx + dy * dy + dz * dz > BF_RENDER_DIST * BF_RENDER_DIST) return;
    if (!bf_in_frustum((float)x + 0.5f, (float)y + 0.88f, (float)z + 0.5f, 0.87f))
        return;
    float base = (float)y + 0.88f;
    float h00 = base + bf_wave_h(x, z + 1);
    float h10 = base + bf_wave_h(x + 1, z + 1);
    float h11 = base + bf_wave_h(x + 1, z);
    float h01 = base + bf_wave_h(x, z);
    float dist2 = agl_sqrtf(dx * dx + dy * dy + dz * dz);
    float fog = (bf_fog_end - dist2) * bf_fog_range_inv;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;
    float u0, v0, u1, v1;
    bf_uv_rect(T_WATER, &u0, &v0, &u1, &v1);
    float wr = bf_fog_r * (1.0f - fog) + 0.95f * fog;
    float wg = bf_fog_g * (1.0f - fog) + 0.95f * fog;
    float wb = bf_fog_b * (1.0f - fog) + 0.95f * fog;
    agl_qv_t q[4];
    q[0].r = wr; q[0].g = wg; q[0].b = wb; q[0].a = 1.0f;
    q[0].u = u0; q[0].v = v1; q[0].x = (float)x; q[0].y = h00; q[0].z = (float)z + 1.0f;
    q[1].r = wr; q[1].g = wg; q[1].b = wb; q[1].a = 1.0f;
    q[1].u = u1; q[1].v = v1; q[1].x = (float)x + 1.0f; q[1].y = h10; q[1].z = (float)z + 1.0f;
    q[2].r = wr; q[2].g = wg; q[2].b = wb; q[2].a = 1.0f;
    q[2].u = u1; q[2].v = v0; q[2].x = (float)x + 1.0f; q[2].y = h11; q[2].z = (float)z;
    q[3].r = wr; q[3].g = wg; q[3].b = wb; q[3].a = 1.0f;
    q[3].u = u0; q[3].v = v0; q[3].x = (float)x; q[3].y = h01; q[3].z = (float)z;
    aglSubmitQuad(0.0f, 1.0f, 0.0f, q);
    bf_batch_count++;
    bf_batch_check();
}

static void bf_render_transparent(void) {
    float rd2 = BF_RENDER_DIST * BF_RENDER_DIST;
    if (!bf_building) {
        bf_batch_count = 0;
        aglBegin(AGL_QUADS);
    }
    extern void sched_heartbeat(void);
    for (int y = 0; y < BF_WY; y++) {
        sched_heartbeat(); /* full-world rebuilds take seconds: alive, not hung */
        for (int z = 0; z < BF_WZ; z++) {
            for (int x = 0; x < BF_WX; x++) {
                uint8_t b = bf_world[bf_idx(x, y, z)];
                if (b == BF_FLOWER_R || b == BF_FLOWER_Y) {
                    if (bf_building) {
                        if (bf_flower_list && bf_flower_n < BF_FLOWER_CAP)
                            bf_flower_list[bf_flower_n++] =
                                (uint32_t)x |
                                ((uint32_t)z << 6) |
                                ((uint32_t)y << 12) |
                                ((uint32_t)b << 18);
                        else bf_cache_overflow = true;
                    } else {
                        bf_emit_cross(x, y, z, b);
                    }
                    continue;
                }
                if (b != BF_WATER) continue;
                if (!bf_building) {
                    float dx = (float)x + 0.5f - bf_cam_x;
                    float dy = (float)y + 0.5f - bf_cam_y;
                    float dz = (float)z + 0.5f - bf_cam_z;
                    if (dx * dx + dy * dy + dz * dz > rd2) continue;
                }
                for (int f = 0; f < 6; f++) {
                    uint8_t nb = bf_get(x + BF_FACE_N[f][0],
                                        y + BF_FACE_N[f][1],
                                        z + BF_FACE_N[f][2]);
                    if (nb != BF_AIR) continue;
                    if (f == 0) {
                        if (bf_building) {
                            if (bf_wtop_list && bf_wtop_n < BF_WTOP_CAP)
                                bf_wtop_list[bf_wtop_n++] =
                                    (uint32_t)x | ((uint32_t)z << 6) | ((uint32_t)y << 12);
                            else bf_cache_overflow = true;
                        } else {
                            bf_emit_water_top(x, y, z);
                        }
                    } else if (f != 1) {
                        bf_emit_face(x, y, z, f, BF_WATER, 1.0f);
                    }
                }
            }
        }
    }
    if (!bf_building) bf_batch_flush();
}

static void bf_render_water_top(void) {
    if (!bf_wtop_list || bf_wtop_n <= 0) return;
    bf_batch_count = 0;
    aglBegin(AGL_QUADS);
    for (int i = 0; i < bf_wtop_n; i++) {
        uint32_t p = bf_wtop_list[i];
        bf_emit_water_top((int)(p & 63u), (int)((p >> 12) & 63u), (int)((p >> 6) & 63u));
    }
    bf_batch_flush();
}

static void bf_render_flowers(void) {
    if (!bf_flower_list || bf_flower_n <= 0) return;

    bf_batch_count = 0;
    aglBegin(AGL_QUADS);

    for (int i = 0; i < bf_flower_n; i++) {
        uint32_t p = bf_flower_list[i];

        int x = (int)(p & 63u);
        int z = (int)((p >> 6) & 63u);
        int y = (int)((p >> 12) & 63u);
        uint8_t block = (uint8_t)((p >> 18) & 255u);

        bf_emit_cross(x, y, z, block);
    }

    bf_batch_flush();
}

#define BF_POS_MASK 0x3FFFFu
#define BF_REBUILD_BYTES ((BF_WX * BF_WY * BF_WZ + 7) / 8)

static uint8_t bf_rebuild_bits[BF_REBUILD_BYTES];

static uint32_t bf_pack_position(int x, int y, int z) {
    return (uint32_t)x |
           ((uint32_t)z << 6) |
           ((uint32_t)y << 12);
}

static void bf_mark_rebuild(int x, int y, int z) {
    if (!bf_in_bounds(x, y, z)) return;

    int i = bf_idx(x, y, z);
    bf_rebuild_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static bool bf_position_needs_rebuild(uint32_t p) {
    int x = (int)(p & 63u);
    int z = (int)((p >> 6) & 63u);
    int y = (int)((p >> 12) & 63u);

    if (!bf_in_bounds(x, y, z)) return false;

    int i = bf_idx(x, y, z);
    return (bf_rebuild_bits[i >> 3] &
            (uint8_t)(1u << (i & 7))) != 0;
}

static void bf_clear_rebuild(int x, int y, int z) {
    int i = bf_idx(x, y, z);
    bf_rebuild_bits[i >> 3] &=
        (uint8_t)~(1u << (i & 7));
}

/*
 * Remove affected records while preserving the order of survivors.
 * This also cleans up any legacy tombstones.
*/

static int bf_compact_faces(bf_cface_t *cache, int count) {
    int out = 0;

    for (int i = 0; i < count; i++) {
        uint32_t p = cache[i].p;

        if (p & BF_FACE_DEAD) continue;
        if (bf_position_needs_rebuild(p)) continue;

        if (out != i) cache[out] = cache[i];
        out++;
    }

    return out;
}

static int bf_compact_positions(uint32_t *list, int count) {
    int out = 0;

    for (int i = 0; i < count; i++) {
        if (bf_position_needs_rebuild(list[i])) continue;
        list[out++] = list[i];
    }

    return out;
}

/*
 * Emit one block into the appropriate cache.
 * Requires allocated caches and bf_building != 0.
 *
 * Flower layout:
 *   bits  0..17: position
 *   bits 18..25: block type
*/

static void bf_cache_emit_block(int x, int y, int z) {
    uint8_t block = bf_get(x, y, z);
    if (block == BF_AIR) return;

    uint32_t position = bf_pack_position(x, y, z);

    if (block == BF_FLOWER_R || block == BF_FLOWER_Y) {
        if (bf_flower_n >= BF_FLOWER_CAP) {
            bf_cache_overflow = true;
            return;
        }

        bf_flower_list[bf_flower_n++] =
            position | ((uint32_t)block << 18);
        return;
    }

    if (block == BF_WATER) {
        for (int face = 0; face < 6; face++) {
            if (face == 1) continue;

            uint8_t neighbor = bf_get(
                x + BF_FACE_N[face][0],
                y + BF_FACE_N[face][1],
                z + BF_FACE_N[face][2]
            );

            if (neighbor != BF_AIR) continue;

            if (face == 0) {
                if (bf_wtop_n < BF_WTOP_CAP) {
                    bf_wtop_list[bf_wtop_n++] = position;
                } else {
                    bf_cache_overflow = true;
                }
            } else {
                bf_emit_face(x, y, z, face, block, 1.0f);
            }
        }

        return;
    }

    float tint = 1.0f;
    if (block == BF_GRASS) {
        tint = 0.96f + 0.08f * bf_hashf(x, 41, z);
    }

    for (int face = 0; face < 6; face++) {
        uint8_t neighbor = bf_get(
            x + BF_FACE_N[face][0],
            y + BF_FACE_N[face][1],
            z + BF_FACE_N[face][2]
        );

        if (!bf_is_opaque(neighbor)) {
            bf_emit_face(x, y, z, face, block, tint);
        }
    }
}

static void bf_build_caches(void) {
    bf_cache_dirty = false;
    bf_dirty_count = 0;
    bf_force_full_rebuild = false;
    bf_cache_overflow = false;
    bf_building = 0;

    bf_opaque_n = 0;
    bf_trans_n = 0;
    bf_wtop_n = 0;
    bf_flower_n = 0;

    if (!bf_cache_alloc()) {

        /*
         * Existing frame logic uses direct rendering on failure.
         * Retry allocation on a later mutation rather than each frame.
        */

        bf_cache_overflow = true;
        bf_force_full_rebuild = true;
        return;
    }

    extern void sched_heartbeat(void);

    bf_building = 1;

    /* One world traversal populates all four caches. */
    for (int y = 0; y < BF_WY; y++) {
        sched_heartbeat();

        for (int z = 0; z < BF_WZ; z++) {
            for (int x = 0; x < BF_WX; x++) {
                bf_cache_emit_block(x, y, z);
            }
        }
    }

    bf_building = 0;

    if (bf_cache_overflow) {
        bf_force_full_rebuild = true;
    }
}

static void bf_rebuild_incremental(void) {
    if (bf_force_full_rebuild ||
        bf_cache_overflow ||
        !bf_opaque_cache ||
        !bf_trans_cache ||
        !bf_wtop_list ||
        !bf_flower_list) {
        bf_build_caches();
        return;
    }

    if (bf_dirty_count == 0) {
        bf_cache_dirty = false;
        return;
    }

    for (int i = 0; i < BF_REBUILD_BYTES; i++) {
        bf_rebuild_bits[i] = 0;
    }

    /*
     * Visibility uses direct neighbors; AO also uses diagonal neighbors.
     * Mark the UNION of every affected 3x3x3 neighborhood.
    */
    for (int d = 0; d < bf_dirty_count; d++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    bf_mark_rebuild(
                        bf_dirty_x[d] + dx,
                        bf_dirty_y[d] + dy,
                        bf_dirty_z[d] + dz
                    );
                }
            }
        }
    }

    /*
     * Remove every old representation before inserting replacements.
     * Water and flowers participate in the same invalidation process.
    */
    
    bf_opaque_n = bf_compact_faces(bf_opaque_cache, bf_opaque_n);
    bf_trans_n = bf_compact_faces(bf_trans_cache, bf_trans_n);
    bf_wtop_n = bf_compact_positions(bf_wtop_list, bf_wtop_n);
    bf_flower_n = bf_compact_positions(bf_flower_list, bf_flower_n);

    bf_cache_overflow = false;
    bf_building = 1;

    for (int d = 0; d < bf_dirty_count; d++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int x = bf_dirty_x[d] + dx;
                    int y = bf_dirty_y[d] + dy;
                    int z = bf_dirty_z[d] + dz;

                    if (!bf_in_bounds(x, y, z)) continue;

                    uint32_t p = bf_pack_position(x, y, z);
                    if (!bf_position_needs_rebuild(p)) continue;

                    /* Overlapping neighborhoods emit this block once. */
                    bf_clear_rebuild(x, y, z);
                    bf_cache_emit_block(x, y, z);
                }
            }
        }
    }

    bf_building = 0;
    bf_dirty_count = 0;
    bf_cache_dirty = false;

    if (bf_cache_overflow) {
        /*
         * Ensure failure is handled consistently. A full rebuild may
         * still overflow; the existing direct-render fallback handles it.
        */

        bf_build_caches();
    }
}

static void bf_render_sky(void) {
    float sd[3] = { 0.45f, 0.72f, -0.53f };
    float ex = bf_cam_x + sd[0] * 180.0f;
    float ey = bf_cam_y + sd[1] * 180.0f;
    float ez = bf_cam_z + sd[2] * 180.0f;
    aglDisable(AGL_TEXTURE_2D);
    aglDisable(AGL_LIGHTING);
    aglEnable(AGL_BLEND);
    aglDisable(AGL_DEPTH_TEST);
    /* Skip sun sphere when looking down: it's above, never visible */
    if (bf_player.pitch > -30.0f) {
        aglColor4f(1.0f, 0.96f, 0.82f, 0.95f);
        aglPushMatrix();
        aglTranslate(ex, ey, ez);
        aglDrawSphere(8.0f, 12, 8);
        aglPopMatrix();
    }
    aglEnable(AGL_DEPTH_TEST);
    if (!bf_clouds_ready) {
        for (int i = 0; i < BF_MAX_CLOUDS; i++) {
            bf_clouds[i].x = bf_hashf(i, 1, 51) * (BF_WX + 80.0f) - 40.0f;
            bf_clouds[i].z = bf_hashf(i, 2, 51) * BF_WZ;
            bf_clouds[i].w = 8.0f + bf_hashf(i, 3, 51) * 14.0f;
            bf_clouds[i].d = 5.0f + bf_hashf(i, 4, 51) * 8.0f;
            bf_clouds[i].speed = 0.5f + bf_hashf(i, 5, 51) * 0.7f;
        }
        bf_clouds_ready = true;
    }
    aglBegin(AGL_QUADS);
    for (int i = 0; i < BF_MAX_CLOUDS; i++) {
        float cx = bf_clouds[i].x + bf_time * bf_clouds[i].speed;
        float span = BF_WX + 80.0f;
        while (cx > BF_WX + 40.0f) cx -= span;
        float cz = bf_clouds[i].z;
        if (!bf_in_frustum(cx, 35.0f, cz, bf_clouds[i].w))
            continue;
        float w = bf_clouds[i].w, d = bf_clouds[i].d;
        float cy = 35.0f;
        aglColor4f(1.0f, 1.0f, 1.0f, 0.72f);
        aglVertex3f(cx - w * 0.5f, cy, cz - d * 0.5f);
        aglVertex3f(cx + w * 0.5f, cy, cz - d * 0.5f);
        aglVertex3f(cx + w * 0.5f, cy, cz + d * 0.5f);
        aglVertex3f(cx - w * 0.5f, cy, cz + d * 0.5f);
    }
    aglEnd();
    aglDisable(AGL_BLEND);
    aglEnable(AGL_TEXTURE_2D);
    aglBindTexture(AGL_TEXTURE_2D, bf_atlas_tex);
}

static void bf_spawn_particles(float x, float y, float z, uint8_t block) {
    float r = bf_block_avg_r(block), g = bf_block_avg_g(block), b = bf_block_avg_b(block);
    for (int i = 0; i < 22; i++) {
        if (bf_particle_count >= BF_MAX_PARTICLES) break;
        bf_particle_t *p = &bf_particles[bf_particle_count++];
        p->x = x + bf_hashf(i, 61, bf_particle_count) * 0.8f + 0.1f;
        p->y = y + bf_hashf(i, 62, bf_particle_count) * 0.8f + 0.1f;
        p->z = z + bf_hashf(i, 63, bf_particle_count) * 0.8f + 0.1f;
        p->vx = (bf_hashf(i, 64, bf_particle_count) - 0.5f) * 4.0f;
        p->vy = bf_hashf(i, 65, bf_particle_count) * 4.5f + 1.0f;
        p->vz = (bf_hashf(i, 66, bf_particle_count) - 0.5f) * 4.0f;
        p->max_life = 0.5f + bf_hashf(i, 67, bf_particle_count) * 0.35f;
        p->life = p->max_life;
        float vary = 0.85f + bf_hashf(i, 68, bf_particle_count) * 0.3f;
        p->r = r * vary;
        p->g = g * vary;
        p->b = b * vary;
    }
}

static void bf_update_particles(float dt) {
    int i = 0;
    while (i < bf_particle_count) {
        bf_particle_t *p = &bf_particles[i];
        p->life -= dt;
        if (p->life <= 0.0f) {
            *p = bf_particles[bf_particle_count - 1];
            bf_particle_count--;
            continue;
        }
        p->vy -= 16.0f * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->z += p->vz * dt;
        if (bf_is_solid(bf_get((int)agl_floorf(p->x), (int)agl_floorf(p->y), (int)agl_floorf(p->z)))) {
            p->y = agl_floorf(p->y) + 1.01f;
            p->vy = 0.0f;
            p->vx *= 0.6f;
            p->vz *= 0.6f;
        }
        i++;
    }
}

static void bf_render_particles(void) {
    if (bf_particle_count == 0) return;
    float yaw_r = bf_player.yaw * 0.01745329f;
    float pitch_r = bf_player.pitch * 0.01745329f;
    float fx = -agl_sinf(yaw_r) * agl_cosf(pitch_r);
    float fy = agl_sinf(pitch_r);
    float fz = -agl_cosf(yaw_r) * agl_cosf(pitch_r);
    agl_vec3_t fwd = { fx, fy, fz };
    agl_vec3_t up_w = { 0, 1, 0 };
    agl_vec3_t right = agl_vec3_normalize(agl_vec3_cross(fwd, up_w));
    agl_vec3_t up = agl_vec3_cross(right, fwd);
    if (right.x == 0.0f && right.y == 0.0f && right.z == 0.0f) return;
    aglDisable(AGL_TEXTURE_2D);
    aglEnable(AGL_BLEND);
    aglBegin(AGL_QUADS);
    for (int i = 0; i < bf_particle_count; i++) {
        bf_particle_t *p = &bf_particles[i];
        float a = p->life / p->max_life;
        float s = 0.07f + 0.05f * a;
        aglColor4f(p->r, p->g, p->b, a);
        aglVertex3f(p->x - right.x * s - up.x * s, p->y - right.y * s - up.y * s, p->z - right.z * s - up.z * s);
        aglVertex3f(p->x + right.x * s - up.x * s, p->y + right.y * s - up.y * s, p->z + right.z * s - up.z * s);
        aglVertex3f(p->x + right.x * s + up.x * s, p->y + right.y * s + up.y * s, p->z + right.z * s + up.z * s);
        aglVertex3f(p->x - right.x * s + up.x * s, p->y - right.y * s + up.y * s, p->z - right.z * s + up.z * s);
    }
    aglEnd();
    aglDisable(AGL_BLEND);
}

static bool bf_box_collides(float x, float y, float z) {
    int x0 = (int)agl_floorf(x - 0.3f), x1 = (int)agl_floorf(x + 0.3f);
    int y0 = (int)agl_floorf(y), y1 = (int)agl_floorf(y + 1.8f);
    int z0 = (int)agl_floorf(z - 0.3f), z1 = (int)agl_floorf(z + 0.3f);
    for (int by = y0; by <= y1; by++)
        for (int bz = z0; bz <= z1; bz++)
            for (int bx = x0; bx <= x1; bx++)
                if (bf_is_solid(bf_get(bx, by, bz))) return true;
    return false;
}

static int bf_surface_height(int x, int z) {
    for (int y = BF_WY - 1; y >= 0; y--) {
        uint8_t b = bf_get(x, y, z);
        if (b != BF_AIR && b != BF_WATER && b != BF_FLOWER_R && b != BF_FLOWER_Y)
            return y;
    }
    return 1;
}

static void bf_spawn_player(void) {
    int sx = BF_WX / 2, sz = BF_WZ / 2;
    int best = -1000000;
    for (int x = 8; x < BF_WX - 8; x += 2) {
        for (int z = 8; z < BF_WZ - 8; z += 2) {
            int h = bf_surface_height(x, z);
            uint8_t top = bf_get(x, h, z);
            if (top == BF_LOG || top == BF_LEAVES) continue;
            if (top != BF_GRASS) continue;
            int mn = 99, mx = -99;
            for (int ox = -3; ox <= 3; ox++)
                for (int oz = -3; oz <= 3; oz++) {
                    int hh = bf_surface_height(x + ox, z + oz);
                    if (hh < mn) mn = hh;
                    if (hh > mx) mx = hh;
                }
            int dxc = x > BF_WX / 2 ? x - BF_WX / 2 : BF_WX / 2 - x;
            int dzc = z > BF_WZ / 2 ? z - BF_WZ / 2 : BF_WZ / 2 - z;
            int score = -(mx - mn) * 10 - (dxc + dzc) / 4;
            if (h < BF_WATER_LEVEL + 2) score -= 50;
            if (score > best) { best = score; sx = x; sz = z; }
        }
    }
    bf_player.x = (float)sx + 0.5f;
    bf_player.z = (float)sz + 0.5f;
    bf_player.y = (float)bf_surface_height(sx, sz) + 1.1f;
    bf_player.vx = bf_player.vy = bf_player.vz = 0.0f;
    bf_player.yaw = -30.0f;
    bf_player.pitch = -12.0f;
    bf_player.grounded = false;
    bf_player.flying = false;
    bf_player.selected = 0;
    bf_held = 0;
    bf_ground_ms = 0;
}

/* walk then jump to climb: no auto-step, blocked means blocked.
 * Landing uses an exact contact search so grounded stays true even at
 * huge dt, plus a short snap-down so jumps never get eaten. A blocked
 * axis stops dead and never touches the other axis.
*/

static void bf_update_player(float dt) {
    bf_player_t *p = &bf_player;
    for (unsigned b = 0; b < 6; b++) {
        if ((bf_held >> b) & 1u) {
            if (bst.now_ms - bf_held_tick[b] > BF_HELD_TIMEOUT_MS) bf_held &= ~(1u << b);
        }
    }
    float fwd_in =
        ((bf_held & (1u << BF_KEY_W)) ? 1.0f : 0.0f) -
        ((bf_held & (1u << BF_KEY_S)) ? 1.0f : 0.0f);

    float str_in =
        ((bf_held & (1u << BF_KEY_D)) ? 1.0f : 0.0f) -
        ((bf_held & (1u << BF_KEY_A)) ? 1.0f : 0.0f);
    float up_in = 0.0f;
    if (p->flying) {
        up_in = ((bf_held >> BF_KEY_SPACE) & 1u) ? 1.0f : 0.0f;
        up_in -= ((bf_held >> BF_KEY_C) & 1u) ? 1.0f : 0.0f;
    }
    float yaw_r = p->yaw * 0.01745329f;
    float fx = -agl_sinf(yaw_r), fz = -agl_cosf(yaw_r);
    float rx = agl_cosf(yaw_r), rz = -agl_sinf(yaw_r);
    if (!p->flying) {
        p->vy -= 24.0f * dt;
        if (p->vy < -38.0f) p->vy = -38.0f;
    } else {
        p->vy += up_in * 30.0f * dt;
        p->vy *= agl_expf(-3.0f * dt);
        if (p->vy > 9.0f) p->vy = 9.0f;
        if (p->vy < -9.0f) p->vy = -9.0f;
    }
    float input_len2 = fwd_in * fwd_in + str_in * str_in;

    if (input_len2 > 1.0f) {
        float inv_len = 1.0f / agl_sqrtf(input_len2);
        fwd_in *= inv_len;
        str_in *= inv_len;
    }

    float speed = p->flying ? 9.0f : 5.6f;
    float target_vx = (fx * fwd_in + rx * str_in) * speed;
    float target_vz = (fz * fwd_in + rz * str_in) * speed;

    float response;
    if (input_len2 > 0.0f) {
        response = p->flying ? 8.0f : 18.0f;
    } else {
        response = p->flying ? 6.0f : 24.0f;
    }

    float blend = 1.0f - agl_expf(-response * dt);

    p->vx += (target_vx - p->vx) * blend;
    p->vz += (target_vz - p->vz) * blend;

    if (p->vx > -0.001f && p->vx < 0.001f) p->vx = 0.0f;
    if (p->vz > -0.001f && p->vz < 0.001f) p->vz = 0.0f;
    float old_x = p->x;
    p->x += p->vx * dt;
    if (bf_box_collides(p->x, p->y, p->z)) {
        p->x = old_x;
        p->vx = 0.0f;
    }
    float old_z = p->z;
    p->z += p->vz * dt;
    if (bf_box_collides(p->x, p->y, p->z)) {
        p->z = old_z;
        p->vz = 0.0f;
    }
    float old_y = p->y;
    float new_y = p->y + p->vy * dt;
    p->y = new_y;
    p->grounded = false;
    if (bf_box_collides(p->x, p->y, p->z)) {
        if (p->vy <= 0.0f) {
            float lo = new_y, hi = old_y;
            for (int it = 0; it < 10; it++) {
                float mid = (lo + hi) * 0.5f;
                if (bf_box_collides(p->x, mid, p->z)) lo = mid;
                else hi = mid;
            }
            p->y = hi;
            p->grounded = true;
            bf_ground_ms = bst.now_ms;
        } else {
            p->y = old_y;
        }
        p->vy = 0.0f;
    } else if (p->vy <= 0.0f) {
        for (float s = 0.05f; s <= 0.34f; s += 0.05f) {
            if (bf_box_collides(p->x, p->y - s, p->z)) {
                p->y -= (s - 0.05f);
                p->grounded = true;
                p->vy = 0.0f;
                bf_ground_ms = bst.now_ms;
                break;
            }
        }
    }

    /* world clamp: out of bounds there is no terrain, only the y<0
       bedrock floor, which is how the player used to leave the map
    */

    if (p->x < 0.8f) { p->x = 0.8f; p->vx = 0.0f; }
    if (p->x > (float)BF_WX - 0.8f) { p->x = (float)BF_WX - 0.8f; p->vx = 0.0f; }
    if (p->z < 0.8f) { p->z = 0.8f; p->vz = 0.0f; }
    if (p->z > (float)BF_WZ - 0.8f) { p->z = (float)BF_WZ - 0.8f; p->vz = 0.0f; }
    if (p->y < -8.0f) bf_spawn_player();
    uint8_t feet = bf_get((int)agl_floorf(p->x), (int)agl_floorf(p->y + 0.4f), (int)agl_floorf(p->z));
    if (feet == BF_WATER && !p->flying) {
        if (p->vy < -3.0f) p->vy = -3.0f;
    }
}

typedef struct {
    bool hit;
    int x, y, z;
    int nx, ny, nz;
    uint8_t block;
} bf_ray_t;

static void bf_forward_vec(float *fx, float *fy, float *fz) {
    float yaw_r = bf_player.yaw * 0.01745329f;
    float pitch_r = bf_player.pitch * 0.01745329f;
    *fx = -agl_sinf(yaw_r) * agl_cosf(pitch_r);
    *fy = agl_sinf(pitch_r);
    *fz = -agl_cosf(yaw_r) * agl_cosf(pitch_r);
}

static bf_ray_t bf_raycast(void) {
    bf_ray_t out = {0};
    float fx, fy, fz;
    bf_forward_vec(&fx, &fy, &fz);
    float ox = bf_player.x, oy = bf_player.y + 1.62f, oz = bf_player.z;
    int x = (int)agl_floorf(ox), y = (int)agl_floorf(oy), z = (int)agl_floorf(oz);
    int step_x = fx >= 0.0f ? 1 : -1;
    int step_y = fy >= 0.0f ? 1 : -1;
    int step_z = fz >= 0.0f ? 1 : -1;
    float adx = fx == 0.0f ? 1e30f : (fx < 0.0f ? -fx : fx);
    float ady = fy == 0.0f ? 1e30f : (fy < 0.0f ? -fy : fy);
    float adz = fz == 0.0f ? 1e30f : (fz < 0.0f ? -fz : fz);
    float t_delta_x = 1.0f / adx, t_delta_y = 1.0f / ady, t_delta_z = 1.0f / adz;
    float t_max_x = fx == 0.0f ? 1e30f : ((step_x > 0 ? (float)(x + 1) - ox : ox - (float)x) / adx);
    float t_max_y = fy == 0.0f ? 1e30f : ((step_y > 0 ? (float)(y + 1) - oy : oy - (float)y) / ady);
    float t_max_z = fz == 0.0f ? 1e30f : ((step_z > 0 ? (float)(z + 1) - oz : oz - (float)z) / adz);
    int face_x = 0, face_y = 0, face_z = 0;
    for (int i = 0; i < 80; i++) {
        float t = t_max_x;
        if (t_max_y < t) t = t_max_y;
        if (t_max_z < t) t = t_max_z;
        if (t > 7.0f) break;
        if (t_max_x <= t_max_y && t_max_x <= t_max_z) {
            x += step_x; t_max_x += t_delta_x;
            face_x = -step_x; face_y = 0; face_z = 0;
        } else if (t_max_y <= t_max_z) {
            y += step_y; t_max_y += t_delta_y;
            face_x = 0; face_y = -step_y; face_z = 0;
        } else {
            z += step_z; t_max_z += t_delta_z;
            face_x = 0; face_y = 0; face_z = -step_z;
        }
        uint8_t b = bf_get(x, y, z);
        if (b != BF_AIR && b != BF_WATER) {
            out.hit = true;
            out.x = x; out.y = y; out.z = z;
            out.nx = face_x; out.ny = face_y; out.nz = face_z;
            out.block = b;
            return out;
        }
    }
    return out;
}

static bool bf_intersects_player(int bx, int by, int bz) {
    float px = bf_player.x, py = bf_player.y, pz = bf_player.z;
    return (float)bx + 1.0f > px - 0.3f && (float)bx < px + 0.3f
        && (float)by + 1.0f > py         && (float)by < py + 1.8f
        && (float)bz + 1.0f > pz - 0.3f && (float)bz < pz + 0.3f;
}

static void bf_break_block(void) {
    bf_ray_t r = bf_raycast();
    if (!r.hit || r.block == BF_BEDROCK) return;
    bf_spawn_particles((float)r.x, (float)r.y, (float)r.z, r.block);
    bf_set(r.x, r.y, r.z, BF_AIR);
}

static void bf_place_block(void) {
    bf_ray_t r = bf_raycast();
    if (!r.hit) return;
    int px = r.x + r.nx, py = r.y + r.ny, pz = r.z + r.nz;
    if (!bf_in_bounds(px, py, pz)) return;
    uint8_t cur = bf_get(px, py, pz);
    if (cur != BF_AIR && cur != BF_WATER) return;
    if (bf_intersects_player(px, py, pz)) return;
    bf_set(px, py, pz, BF_PLACEABLE[bf_player.selected]);
}

/* inventory UI: hotbar always on, E opens the grid
 * Bottom bar owns the lowest 44 px: solid strip, slots, then the
 * selected name and the help line stacked above it. Nothing overlaps:
 * the old layout drew slot numbers and help text on the same rows.
*/

static uint32_t bf_pack_rgb(float r, float g, float b) {
    int ri = (int)(r * 255.0f), gi = (int)(g * 255.0f), bi = (int)(b * 255.0f);
    if (ri > 255) ri = 255;
    if (gi > 255) gi = 255;
    if (bi > 255) bi = 255;
    return 0xFF000000u | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

/* three-rect iso cube tinted from the real atlas averages */
static void bf_draw_block_icon(Window *win, int x, int y, int s, uint8_t block) {
    float r = bf_block_avg_r(block), g = bf_block_avg_g(block), b = bf_block_avg_b(block);
    int th = s * 34 / 100;
    int fh = s - th;
    int hw = (s - 4) / 2;
    wm_fill_rect(win, x + 2, y, s - 4, th, bf_pack_rgb(r * 1.05f, g * 1.05f, b * 1.05f));
    wm_fill_rect(win, x + 2, y + th, hw, fh, bf_pack_rgb(r * 0.72f, g * 0.72f, b * 0.72f));
    wm_fill_rect(win, x + 2 + hw, y + th, s - 4 - hw, fh, bf_pack_rgb(r * 0.50f, g * 0.50f, b * 0.50f));
}

/* hotbar sits above the label; grey help owns the bottom row */
static void bf_hotbar_rect(int cw, int ch, int *x0, int *y0) {
    *x0 = (cw - (9 * 26 + 8 * 2)) / 2;
    *y0 = ch - 58;
}

static int bf_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void bf_draw_inventory(Window *win, int cw, int ch) {
    int hx, hy;
    bf_hotbar_rect(cw, ch, &hx, &hy);
    wm_fill_rect(win, 0, ch - 62, cw, 34, 0xE8141418);
    wm_fill_rect(win, 0, ch - 62, cw, 1, 0xFF3A3A40);
    wm_fill_rect(win, 0, ch - 29, cw, 1, 0xFF3A3A40);
    for (int i = 0; i < 9; i++) {
        int sx = hx + i * 28;
        bool sel = (i == bf_player.selected);
        wm_fill_rect(win, sx, hy, 26, 26, sel ? 0xFF2A2A30 : 0xFF17171B);
        uint32_t edge = sel ? 0xFFE8A030 : 0xFF3A3A40;
        wm_fill_rect(win, sx, hy, 26, 1, edge);
        wm_fill_rect(win, sx, hy + 25, 26, 1, edge);
        wm_fill_rect(win, sx, hy, 1, 26, edge);
        wm_fill_rect(win, sx + 25, hy, 1, 26, edge);
        bf_draw_block_icon(win, sx + 4, hy + 4, 18, BF_PLACEABLE[i]);
        char num[2] = { (char)('1' + i), 0 };
        wm_draw_string(win, sx + 3, hy + 2, num, 0xFF9A9AA2, 0);
    }
    const char *sel_name = BF_NAMES[BF_PLACEABLE[bf_player.selected]];
    wm_draw_string(win, cw / 2 - bf_strlen(sel_name) * 4, ch - 88,
                   sel_name, 0xFFF2F2F0, 0);
    if (!bf_inv_open) return;
    int pw = 3 * 46 + 2 * 6 + 24;
    int ph = 3 * 46 + 2 * 6 + 40;
    int px0 = (cw - pw) / 2;
    int py0 = (ch - ph) / 2 - 20;
    if (py0 + ph > ch - 68) py0 = ch - 68 - ph;
    if (py0 < 4) py0 = 4;
    wm_fill_rect(win, px0, py0, pw, ph, 0xE6101014);
    wm_fill_rect(win, px0, py0, pw, 1, 0xFF3A3A40);
    wm_fill_rect(win, px0, py0 + ph - 1, pw, 1, 0xFF3A3A40);
    wm_fill_rect(win, px0, py0, 1, ph, 0xFF3A3A40);
    wm_fill_rect(win, px0 + pw - 1, py0, 1, ph, 0xFF3A3A40);
    wm_draw_string(win, px0 + 12, py0 + 8, "BLOCKS", 0xFFE8A030, 0);
    wm_draw_string(win, px0 + 12, py0 + ph - 14,
                   "click or 1-9 select   E close", 0xFF9A9AA2, 0);
    for (int i = 0; i < 9; i++) {
        int cx = px0 + 12 + (i % 3) * 52;
        int cy = py0 + 24 + (i / 3) * 52;
        bool sel = (i == bf_player.selected);
        wm_fill_rect(win, cx, cy, 46, 46, sel ? 0xFF2A2A30 : 0xFF17171B);
        uint32_t edge = sel ? 0xFFE8A030 : 0xFF3A3A40;
        wm_fill_rect(win, cx, cy, 46, 1, edge);
        wm_fill_rect(win, cx, cy + 45, 46, 1, edge);
        wm_fill_rect(win, cx, cy, 1, 46, edge);
        wm_fill_rect(win, cx + 45, cy, 1, 46, edge);
        bf_draw_block_icon(win, cx + 8, cy + 6, 30, BF_PLACEABLE[i]);
        wm_draw_string(win, cx + 4, cy + 38,
                        BF_NAMES[BF_PLACEABLE[i]], 0xFF9A9AA0, 0);
    }
}

static int bf_inventory_click(int local_x, int local_y, int cw, int ch) {
    if (bf_inv_open) {
        int pw = 3 * 46 + 2 * 6 + 24;
        int ph = 3 * 46 + 2 * 6 + 40;
        int px0 = (cw - pw) / 2, py0 = (ch - ph) / 2 - 20;
        if (py0 + ph > ch - 68) py0 = ch - 68 - ph;
        if (py0 < 4) py0 = 4;
        for (int i = 0; i < 9; i++) {
            int cx = px0 + 12 + (i % 3) * 52;
            int cy = py0 + 24 + (i / 3) * 52;
            if (local_x >= cx && local_x < cx + 46 && local_y >= cy && local_y < cy + 46)
                return i;
        }
    }
    int hx, hy;
    bf_hotbar_rect(cw, ch, &hx, &hy);
    for (int i = 0; i < 9; i++) {
        int sx = hx + i * 28;
        if (local_x >= sx && local_x < sx + 26 && local_y >= hy && local_y < hy + 26)
            return i;
    }
    return -1;
}

static void bf_draw_hud(Window *win, int cw, int ch) {
    char buf[96];
    int n;
    wm_draw_string(win, cw / 2 - 4, ch / 2 - 4, "+", 0xFF101010, 0);
    n = str_copy(buf, "BLOCKFORGE  fps ");
    demo_int_to_str(bf_fps, buf + n);
    while (buf[n]) n++;
    n += str_copy(buf + n, "   xyz ");
    demo_int_to_str((int)bf_player.x, buf + n);
    while (buf[n]) n++;
    buf[n++] = ' ';
    demo_int_to_str((int)bf_player.y, buf + n);
    while (buf[n]) n++;
    buf[n++] = ' ';
    demo_int_to_str((int)bf_player.z, buf + n);
    while (buf[n]) n++;
    n += str_copy(buf + n, bf_player.grounded ? " g1" : " g0");
    {
        char km[6];
        km[0] = (bf_held & (1u << BF_KEY_W)) ? 'W' : '-';
        km[1] = (bf_held & (1u << BF_KEY_A)) ? 'A' : '-';
        km[2] = (bf_held & (1u << BF_KEY_S)) ? 'S' : '-';
        km[3] = (bf_held & (1u << BF_KEY_D)) ? 'D' : '-';
        km[4] = (bf_held & (1u << BF_KEY_SPACE)) ? '_' : '.';
        km[5] = 0;
        n += str_copy(buf + n, " k");
        n += str_copy(buf + n, km);
    }
    buf[n] = 0;
    wm_draw_string(win, 10, 8, buf, 0xFFF2F2F0, 0);
    n = str_copy(buf, "block: ");
    n += str_copy(buf + n, BF_NAMES[BF_PLACEABLE[bf_player.selected]]);
    if (bf_player.flying) n += str_copy(buf + n, "   [flying]");
    buf[n] = 0;
    wm_draw_string(win, 10, 24, buf, 0xFFE8A030, 0);
    if (!bst.ctx_alive) {
        wm_draw_string(win, 10, 40,
                        "agl context DEAD - retrying init (kmalloc depth buffer?)",
                        0xFFFF4040, 0);
    } else if (bst.probe_done && !bst.probe_ok) {
        char hx[9];
        uint_to_hex(bst.probe_got, hx);
        n = str_copy(buf, "fb probe FAIL got 0x");
        str_copy(buf + n, hx);
        wm_draw_string(win, 10, 40, buf, 0xFFFF4040, 0);
    }
    bf_draw_inventory(win, cw, ch);
    wm_draw_string(win, 10, ch - 16,
        "drag look  LMB break  RMB place  WASD move  SPACE jump  F fly  E blocks  ESC quit",
        0xFF5A5A62, 0);
}

static void bf_frame(Window *win, float dt, int cw, int ch) {
    if (!bf_world_ready) {
        bf_generate_world();
        bf_world_ready = true;
        bf_spawn_player();
    }
    if (!bf_atlas_ready) {
        bf_build_atlas();
        bf_atlas_ready = true;
    }
    bf_time += dt;

    /*
     * Adaptive substeps, not a fixed-timestep accumulator.
     * BlockForge_render() already caps dt at 0.1 seconds.
    */

    int physics_steps = (int)(dt * 120.0f);
    if ((float)physics_steps < dt * 120.0f) physics_steps++;

    if (physics_steps < 1) physics_steps = 1;
    if (physics_steps > 12) physics_steps = 12;

    float physics_dt = dt / (float)physics_steps;

    for (int i = 0; i < physics_steps; i++) {
        bf_update_player(physics_dt);
        bf_update_particles(physics_dt);
    }
    float eye_x = bf_player.x, eye_y = bf_player.y + 1.62f, eye_z = bf_player.z;
    bf_cam_x = eye_x; bf_cam_y = eye_y; bf_cam_z = eye_z;
    bool underwater = bf_get((int)agl_floorf(eye_x), (int)agl_floorf(eye_y), (int)agl_floorf(eye_z)) == BF_WATER;
    if (underwater) {
        bf_fog_r = 0.05f; bf_fog_g = 0.16f; bf_fog_b = 0.30f;
        bf_fog_start = 2.0f; bf_fog_end = 14.0f;
    } else {
        bf_fog_r = 0.58f; bf_fog_g = 0.73f; bf_fog_b = 0.90f;
        bf_fog_start = BF_RENDER_DIST * 0.5f; bf_fog_end = BF_RENDER_DIST;
    }
    bf_fog_range_inv = 1.0f / (bf_fog_end - bf_fog_start);
    aglClearColor(bf_fog_r, bf_fog_g, bf_fog_b, 1.0f);
    aglClear(AGL_COLOR_BUFFER_BIT | AGL_DEPTH_BUFFER_BIT);
    bf_projection(70.0f, 0.1f, 400.0f);
    float fx, fy, fz;
    bf_forward_vec(&fx, &fy, &fz);
    aglLookAt(eye_x, eye_y, eye_z,
              eye_x + fx, eye_y + fy, eye_z + fz,
              0.0f, 1.0f, 0.0f);
    {
        float proj[16], view[16], mvp[16];
        aglGetFloatv(AGL_PROJECTION_MATRIX, proj);
        aglGetFloatv(AGL_MODELVIEW_MATRIX, view);
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                mvp[c * 4 + r] = proj[r]     * view[c * 4]
                               + proj[4 + r]  * view[c * 4 + 1]
                               + proj[8 + r]  * view[c * 4 + 2]
                               + proj[12 + r] * view[c * 4 + 3];
        bf_extract_frustum(mvp);
    }
    aglDisable(AGL_LIGHTING);
    aglDisable(AGL_FOG);        /* BlockForge currently computes its own fog. */
    aglDisable(AGL_CULL_FACE);  /* Flowers require double-sided rendering. */
    aglDisable(AGL_BLEND);

    aglEnable(AGL_DEPTH_TEST);
    aglDepthFunc(AGL_LESS);
    aglDepthMask(true);
    aglShadeModel(AGL_SMOOTH);

    aglMatrixMode(AGL_TEXTURE);
    aglLoadIdentity();
    aglMatrixMode(AGL_MODELVIEW);

    aglEnable(AGL_TEXTURE_2D);
    aglBindTexture(AGL_TEXTURE_2D, bf_atlas_tex);

    aglBlendFunc(AGL_SRC_ALPHA, AGL_ONE_MINUS_SRC_ALPHA);

    aglDepthMask(false);
    bf_render_sky();
    aglDepthMask(true);
    if (bf_cache_dirty) {
        if (bf_force_full_rebuild || bf_dirty_count == 0) {
            bf_build_caches();
        } else {
            bf_rebuild_incremental();
        }
    }
    if (bf_cache_overflow) {
        bf_render_opaque();
    } else {
        bf_replay_cache(bf_opaque_cache, bf_opaque_n, true);
    }
    aglEnable(AGL_BLEND);
    aglBlendFunc(AGL_SRC_ALPHA, AGL_ONE_MINUS_SRC_ALPHA);

    if (bf_cache_overflow) {
        /*
         * Correctness fallback. This legacy direct path is not globally
         * transparency-sorted, but must not occlude through alpha-zero texels.
         */
        aglDepthMask(false);
        bf_render_transparent();
    } else {
        /*
         * Flower textures are binary cutouts. Visible texels write depth;
         * alpha-zero texels do not, thanks to the rasterizer fix.
         */
        aglDepthMask(true);
        bf_render_flowers();

        aglDepthMask(false);
        bf_replay_cache(bf_trans_cache, bf_trans_n, false);
        bf_render_water_top();
    }

    aglDepthMask(false);
    bf_render_particles();

    aglDepthMask(true);
    aglDisable(AGL_BLEND);
    bf_draw_hud(win, cw, ch);
}

/* context lifecycle */

static void bf_agl_drop(void) {
    bf_atlas_ready = false;
    bf_atlas_tex = 0;
    bst.probe_done = false;
}

static void bf_agl_teardown(void) {
    if (bst.agl_initialized) {
        agl_shared_release();
        bst.agl_initialized = false;
        bf_agl_drop();
    }
}

/* input */

static void bf_key_hold(unsigned bit) {
    bf_held |= (1u << bit);
    bf_held_tick[bit] = bst.now_ms;
    /* opposing input cancels the stale axis bit immediately */
    switch (bit) {
    case BF_KEY_W: bf_held &= ~(1u << BF_KEY_S); break;
    case BF_KEY_S: bf_held &= ~(1u << BF_KEY_W); break;
    case BF_KEY_A: bf_held &= ~(1u << BF_KEY_D); break;
    case BF_KEY_D: bf_held &= ~(1u << BF_KEY_A); break;
    default: break;
    }
}

void BlockForge_mouse(Window *window, int local_x, int local_y, bool left, bool right) {
    static bool prev_l = false, prev_r = false;
    static int last_x = -1, last_y = -1;
    static int drag_dist = 0;
    int cw = wm_client_w(window), ch = wm_client_h(window);
    bool click_l = left && !prev_l;
    bool click_r = right && !prev_r;
    if (click_l || click_r) {
        int slot = bf_inventory_click(local_x, local_y, cw, ch);
        if (slot >= 0) {
            bf_player.selected = slot;
            prev_l = left; prev_r = right;
            last_x = local_x; last_y = local_y;
            return;
        }
    }
    if (bf_inv_open) {
        prev_l = left; prev_r = right;
        last_x = local_x; last_y = local_y;
        return;
    }
    bool any_down = left || right;
    bool was_down = prev_l || prev_r;
    if (any_down && !was_down) drag_dist = 0;
    if (any_down && was_down && last_x >= 0) {
        int dx = local_x - last_x;
        int dy = local_y - last_y;
        drag_dist += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        /* mouse right must turn right: forward is (-sin yaw, -cos yaw),
           so increasing yaw turns left; subtract
        */
        bf_player.yaw -= (float)dx * 0.28f;
        bf_player.pitch -= (float)dy * 0.28f;
        if (bf_player.pitch > 89.0f) bf_player.pitch = 89.0f;
        if (bf_player.pitch < -89.0f) bf_player.pitch = -89.0f;
    }
    if (!any_down && was_down) {
        if (drag_dist < 5) {
            if (prev_l) bf_break_block();
            else bf_place_block();
        }
    }
    prev_l = left;
    prev_r = right;
    last_x = local_x;
    last_y = local_y;
}

void BlockForge_keypress(Window *window, uint16_t key_code) {
    (void)window;
    char key = (char)(key_code & 0xFF);
    uint8_t sc_hi = (uint8_t)((key_code >> 8) & 0xFF);
    if (sc_hi & 0x80u) {                 /* break: release, no action */
        unsigned rb = bf_scancode_bit(sc_hi & 0x7Fu);
        if (rb != 0xFFu) bf_held &= ~(1u << rb);
        return;
    }
    {
        unsigned mb = bf_scancode_bit(sc_hi & 0x7Fu);
        if (mb != 0xFFu) {
            bf_key_hold(mb);
            if (mb == BF_KEY_SPACE && !bf_player.flying) {
                bool ground_near =
                    bf_box_collides(bf_player.x, bf_player.y - 0.35f, bf_player.z);
                if (ground_near || bst.now_ms - bf_ground_ms < 150u) {
                    bf_player.vy = 8.6f;
                    bf_ground_ms = bst.now_ms - 1000u;
                }
            }
            return;
        }
    }
    if (key == 27) {
        bf_agl_teardown();
        wm_destroy_window(window);
        return;
    }
    switch (key) {
    case 'w': case 'W': bf_key_hold(BF_KEY_W); break;
    case 's': case 'S': bf_key_hold(BF_KEY_S); break;
    case 'a': case 'A': bf_key_hold(BF_KEY_A); break;
    case 'd': case 'D': bf_key_hold(BF_KEY_D); break;
    case 'c': case 'C': bf_key_hold(BF_KEY_C); break;
    case ' ':
        bf_key_hold(BF_KEY_SPACE);
        if (!bf_player.flying) {
            /* Belt and suspenders: jump when the coyote window is fresh
             * OR when ground is physically within a third of a block.
             * The probe makes jump independent of every flag.
            */
            bool ground_near =
                bf_box_collides(bf_player.x, bf_player.y - 0.35f, bf_player.z);
            if (ground_near || bst.now_ms - bf_ground_ms < 150u) {
                bf_player.vy = 8.6f;
                bf_ground_ms = bst.now_ms - 1000u;
            }
        }
        break;
    case 'f': case 'F':
        bf_player.flying = !bf_player.flying;
        bf_player.vy = 0.0f;
        break;
    case 'e': case 'E':
        bf_inv_open = !bf_inv_open;
        break;
    default:
        if (key >= '1' && key <= '9') bf_player.selected = key - '1';
        break;
    }
}

/* render + lifecycle */

static void BlockForge_on_close(Window *window) {
    (void)window;
    bf_agl_teardown();
}

void BlockForge_render(Window *window) {
    bst.window = window;
    static uint32_t last_tick = 0;
    static uint32_t fps_tick = 0;
    static int fps_frames = 0;
    uint32_t now = get_ticks();
    if (last_tick == 0) { last_tick = now; fps_tick = now; }
    uint32_t dtick = now - last_tick;
    last_tick = now;
    bst.now_ms = now * 10u;
    /* Drain physical releases first: a released key stops THIS frame,
     * no timeout, no typematic dependence. The 600 ms net below stays
     * only for missed releases (focus loss, full ring). */
    int rel;
    while ((rel = sys_getkey_rel()) >= 0) {
        unsigned rb = bf_scancode_bit((unsigned)rel & 0x7Fu);
        if (rb != 0xFFu) bf_held &= ~(1u << rb);
    }
    float dt = (float)dtick * 0.01f;
    if (dt > 0.1f) dt = 0.1f;
    if (dt <= 0.0f) dt = 0.016f;
    int client_x = wm_client_x(window);
    int client_y = wm_client_y(window);
    int client_w = wm_client_w(window);
    int client_h = wm_client_h(window);
    bst.aspect = (client_h > 0) ? ((float)client_w / (float)client_h) : (4.0f / 3.0f);
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
    if (!bst.agl_initialized) {
        bst.gl_gen = agl_shared_acquire();
        bst.agl_initialized = true;
        bf_agl_drop();
    }
    if (agl_shared_generation() != bst.gl_gen) {
        bst.gl_gen = agl_shared_generation();
        bf_agl_drop();
    }
    bst.ctx_alive = agl_shared_alive();
    if (!bst.ctx_alive) {
        gpu_fill_rect(client_x, client_y, client_w, client_h, 0xFF000000);
        gpu_draw_string(client_x + 10, client_y + 10,
                        (const uint8_t *)"agl context dead - retrying (kernel heap?)",
                        0xFFFF4040, 0);
        bf_agl_teardown();
        return;
    }
    aglSetFramebuffer(fb);
    aglViewport(client_x, client_y, client_w, client_h);
    if (!bst.probe_done) {
        aglClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        aglClear(AGL_COLOR_BUFFER_BIT);
        bst.probe_got = fb[client_y * screen_w + client_x];
        bst.probe_ok = (bst.probe_got == 0xFFFF00FFu);
        bst.probe_done = true;
    }
    bf_frame(window, dt, client_w, client_h);
    fps_frames++;
    uint32_t fel = now - fps_tick;
    if (fel >= 100) {
        bf_fps = (int)((uint32_t)fps_frames * 100 / fel);
        fps_frames = 0;
        fps_tick = now;
    }
}

void BlockForge_init(Window *window) {
    bst.window = window;
    bf_keyboard_typematic();
    bst.aspect = 4.0f / 3.0f;
    bst.agl_initialized = false;
    bst.probe_done = false;
    bst.probe_ok = false;
    bst.ctx_alive = false;
    bst.gl_gen = 0;
    bst.now_ms = 0;
    bf_inv_open = false;
    bf_held = 0;
    bf_ground_ms = 0;
}

void BlockForge_create(void) {
    Window *win = wm_create_window("BlockForge", 80, 10, 660, 500);
    if (win) {
        BlockForge_init(win);
        win->on_draw = BlockForge_render;
        win->on_key = BlockForge_keypress;
        win->on_mouse = BlockForge_mouse;
        win->on_close = BlockForge_on_close;
    }
}

// FINALLY finished writing this file. I thought i will never finsih it.