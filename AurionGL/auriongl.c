/*
 * AurionGL 2.1 -- software rasterizer.
 * Public API and all header types are unchanged from 2.0.
 *
 * Correctness fixes over 2.0 (these change rendered output)
 *  1. agl_logf() had its exponent bias off by one (`- 126` on a mantissa
 *     forced into [1,2)), so every log came out +ln2 too large. agl_powf()
 *     therefore returned base^e * 2^e. With shininess 32 a specular term of
 *     0.5 evaluated to ~1.0 instead of ~2e-10: highlights were effectively
 *     pinned on. Fixed to `- 127`, plus an octave fold for faster convergence.
 *  2. pack_color() did not clamp, so the non-blended path could cast a
 *     negative or >1 channel and wrap into the neighbouring byte. All colour
 *     output is now saturated.
 *  3. The two triangle halves both drew the middle scanline when y_mid landed
 *     exactly on an integer. Harmless with depth-LESS, visible as a seam of
 *     double-blended pixels with AGL_BLEND. Now half-open: [ceil(ya), ceil(yb)).
 *  4. aglTexParameteri() silently dropped every parameter when the texture
 *     had been bound but not yet uploaded -- i.e. the ordinary
 *     bind/TexParameter/TexImage2D sequence. Filters/wrap are now stashed on
 *     the slot at bind time.
 *  5. sample_texture() wrote g_state->bound_tex from inside the pixel loop,
 *     which is a write to shared state from every SMP band. The bound texture
 *     is now resolved once per batch into a read-only raster context.
 *  6. Blend factors SRC_COLOR / DST_COLOR used the red channel for green and
 *     blue. They are per-channel now, as in GL.
 *  7. Allocation sizes are overflow-checked; the depth buffer is initialised
 *     at aglInit instead of holding kmalloc garbage until the first aglClear.
 *  8. aglGenTextures() no longer hands out more live ids than MAX_TEXTURES.
 *
 * Performance work
 *  A. Clip/project/cull/setup runs ONCE per batch on the calling core into a
 *     triangle list; SMP bands then only rasterize. 2.0 re-ran the whole
 *     clipper in every band, so setup cost scaled with core count.
 *  B. Plane-equation attribute gradients. d(attr)/dx is a triangle constant,
 *     so the inner loop is one add per attribute and there is no per-scanline
 *     division at all (2.0 did one division per edge per half; the version
 *     before that did 18 per row).
 *  C. Perspective correction by affine subdivision: 1/w is inverted exactly
 *     every 16 pixels and the endpoints are interpolated linearly between.
 *     Removes the per-pixel reciprocal and is *more* accurate than 2.0's
 *     single-Newton agl_rcpf, which carried ~8 bits and visibly swam on
 *     large textured surfaces.
 *  D. Texel addressing is 16.16 fixed point with integer stepping; power-of-
 *     two REPEAT wraps with a mask. Bilinear filtering is integer with 8-bit
 *     weights -- no float->int conversion in the pixel loop.
 *  E. Colour, texture modulate, fog and blend are all fixed point. Floats
 *     appear in the pixel loop only for the depth value.
 *  F. Four specialised span kernels (untextured / nearest / bilinear, all
 *     depth-LESS opaque, plus one generic catch-all). Selected once per
 *     batch through a function pointer; each kernel is the shared body with
 *     its flags constant-folded away.
 *  G. Matrices pass by const pointer. 2.0 copied 64 bytes per matrix per
 *     call, including one copy per vertex in mat4_mul_vec4.
 *  H. The MVP product is lazy (dirty flag) instead of recomputed on every
 *     aglTranslate/Rotate/Scale, and the modelview transform of the position
 *     is skipped entirely when lighting is off -- it only fed the lighting.
 *  I. agl_powf handles positive integer exponents by binary exponentiation,
 *     so specular costs ~8 multiplies instead of an exp+log pair.
 *  J. aglClear issues a single rep stosl for the whole region when the
 *     viewport spans the full framebuffer width.
 *  K. The quadric helpers hoist their ring sin/cos out of the inner loop
 *     (aglDrawTorus was calling sinf/cosf six times per vertex) and
 *     aglDrawCube goes through aglSubmitQuad.
 *
 * Deliberate approximations (say the word and any of them can go)
 *  - Perspective correction is exact every 16 pixels, linear in between.
 *  - EXP/EXP2 fog is evaluated every 16 pixels and interpolated.
 *  - Blending rounds through the /255 identity ((x+128)*257)>>16.
 *  - Normals are still transformed by the modelview upper 3x3 rather than its
 *    inverse transpose, so non-uniform aglScale still skews lighting. Same as
 *    2.0; fixing it needs a 3x3 inverse per matrix change.
*/

#include "auriongl.h"
#include "smp.h"

extern void *kmalloc(uint32_t size);
extern void kfree(void *ptr);

#define AGL_INLINE static inline __attribute__((always_inline))

/* SCALAR HELPERS */

// Ultra manified code lol

static inline int agl_maxi(int a, int b) { return a > b ? a : b; }
static inline int agl_mini(int a, int b) { return a < b ? a : b; }
static inline int agl_clampi(int x, int lo, int hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
static inline int agl_sat8(int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); }

/* (x + 128) * 257 >> 16 == round(x / 255) for x in [0, 65535]. */
AGL_INLINE int agl_div255(int x) { int t = x + 128; return (t + (t >> 8)) >> 8; }
/* 0..255 * 0..255 -> 0..255 */
AGL_INLINE int agl_mul255(int a, int b) { return agl_div255(a * b); }

static inline float agl_fabsf(float x) { return x < 0.0f ? -x : x; }
static inline float agl_clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
static inline float agl_minf(float a, float b) { return a < b ? a : b; }
static inline float agl_maxf(float a, float b) { return a > b ? a : b; }

#define AGL_INT_SAFE 2.0e9f

float agl_floorf(float x) {
    if (x >= AGL_INT_SAFE || x <= -AGL_INT_SAFE) return x;
    int i = (int)x;
    return (float)(i - (x < 0.0f && (float)i != x));
}
static inline float agl_ceilf(float x) {
    if (x >= AGL_INT_SAFE || x <= -AGL_INT_SAFE) return x;
    int i = (int)x;
    return (float)(i + (x > 0.0f && (float)i != x));
}
/* Truncation is toward zero, so ceil == trunc + (x > 0 && it had a fraction). */
AGL_INLINE int agl_ceili(float x) {
    int i = (int)x;
    return i + ((x > 0.0f && (float)i != x) ? 1 : 0);
}
AGL_INLINE int agl_floori(float x) {
    int i = (int)x;
    return i - ((x < 0.0f && (float)i != x) ? 1 : 0);
}

static void *agl_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *agl_memset(void *dst, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

/* One rep stosl per call: EDI=dst, ECX=count, EAX=value. */
static inline void agl_fill_u32(uint32_t *dst, uint32_t v, uint32_t n) {
    if (!n) return;
    __asm__ volatile ("rep stosl" : "+D" (dst), "+c" (n) : "a" (v) : "memory");
}

/* MATH */

/* Quake III fast inverse sqrt + 2 Newton iterations -> full float accuracy. */
static inline float agl_rsqrtf(float x) {
    union { float f; uint32_t u; } u;
    float xhalf = 0.5f * x;
    u.f = x;
    u.u = 0x5f375a86u - (u.u >> 1);
    u.f = u.f * (1.5f - xhalf * u.f * u.f);
    u.f = u.f * (1.5f - xhalf * u.f * u.f);
    return u.f;
}

float agl_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    return x * agl_rsqrtf(x);
}

/* Reciprocal, two Newton steps (~16+ bits). Called once per 16-pixel block
 * now rather than per pixel, so the extra step is free and kills the texture
 * swim the single-step version produced on big polygons. */
AGL_INLINE float agl_rcpf(float x) {
    union { float f; uint32_t u; } u;
    u.f = x;
    u.u = 0x7EF311C2u - u.u;
    u.f = u.f * (2.0f - x * u.f);
    u.f = u.f * (2.0f - x * u.f);
    return u.f;
}

/* Range reduce to [-pi/2, pi/2], then 7-term odd series in Horner form. */
float agl_sinf(float x) {
    const float TWO_PI = 6.283185307179586f;
    const float PI = 3.141592653589793f;
    const float HALF_PI = 1.570796326794897f;

    float n = agl_floorf(x * 0.15915494309189535f + 0.5f);
    x = x - n * TWO_PI;
    if (x > HALF_PI) x = PI - x;
    else if (x < -HALF_PI) x = -PI - x;

    float x2 = x * x;
    // genually i don't know what did i coded here.
    // it was basicly like 1-2am
    
    return x * (1.0f - x2 * (0.16666666666666666f - x2 * (0.008333333333333333f
              - x2 * (0.0001984126984126984f - x2 * (0.000002755731922398589f
              - x2 * (2.755731922398589e-8f - x2 * 2.0876756987868098e-10f))))));
}

float agl_cosf(float x) { return agl_sinf(x + 1.570796326794897f); }

/* Both at once: the callers below always want the pair. */
static inline void agl_sincosf(float a, float *s, float *c) {
    *s = agl_sinf(a);
    *c = agl_sinf(a + 1.570796326794897f);
}

static float agl_tanf(float x) {
    float s, c;
    agl_sincosf(x, &s, &c);
    if (agl_fabsf(c) < 1e-7f) return (s > 0.0f) ? 1e10f : -1e10f;
    return s / c;
}

/* exp(x) = 2^k * exp(r), Cody-Waite split of ln2. */
float agl_expf(float x) {
    if (x > 88.722839f) return 3.402823466e+38f;
    if (x < -87.336545f) return 0.0f;

    const float LN2_INV = 1.4426950408889634f;
    const float LN2_HI = 0.693145751953125f;
    const float LN2_LO = 1.42860682030941723212e-6f;

    float kf = x * LN2_INV;
    int k = (int)(kf + (kf >= 0.0f ? 0.5f : -0.5f));
    float r = x - (float)k * LN2_HI - (float)k * LN2_LO;

    /* 2.0 paired an ln2 range reduction with a 2^r polynomial: the series
     * below is exp(r) - 1, which is what r actually calls for. The mismatch
     * was worth up to ~5% and showed up as wrong EXP/EXP2 fog density. */
    float p = r * (1.0f + r * (0.5f + r * (0.16666666666666666f
              + r * (0.041666666666666664f + r * (0.008333333333333333f
              + r * (0.001388888888888889f + r * 0.0001984126984126984f))))));

    union { float f; uint32_t u; } u;
    u.u = (uint32_t)(k + 127) << 23;
    return u.f * (1.0f + p);
}

/* log(x): split to m * 2^e with m in [1,2), fold to [1/sqrt2, sqrt2) so the
 * atanh series converges in six terms, then sum.
 * NOTE: 2.0 used `- 126` here, which biased every result by +ln2. */
static float agl_logf(float x) {
    if (x <= 0.0f) return -3.402823466e+38f;
    union { float f; uint32_t u; } u;
    u.f = x;
    int e = (int)((u.u >> 23) & 0xFF) - 127;
    u.u = (u.u & 0x007FFFFFu) | 0x3F800000u;
    float m = u.f;
    if (m > 1.4142135623730951f) { m *= 0.5f; e += 1; }

    float f = m - 1.0f;
    float s = f / (2.0f + f);
    float s2 = s * s;
    float log_m = s * (2.0f + s2 * (0.6666666666666666f + s2 * (0.4f
                 + s2 * (0.2857142857142857f + s2 * (0.2222222222222222f
                 + s2 * 0.1818181818181818f)))));

    return (float)e * 0.6931471805599453f + log_m;
}

static float agl_powf(float base, float exp) {
    if (base <= 0.0f) return 0.0f;
    if (exp == 0.0f || base == 1.0f) return 1.0f;
    if (exp == 1.0f) return base;
    if (exp == 0.5f) return agl_sqrtf(base);
    /* Shininess is an integer in every sane material: binary exponentiation
     * beats exp(log()) on both cost and accuracy. */
    int n = (int)exp;
    if ((float)n == exp && n > 0 && n <= 1024) {
        float r = 1.0f, b = base;
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return r;
    }
    return agl_expf(exp * agl_logf(base));
}

/* INTERNAL STATE */

#define MAX_VERTICES        4096
#define MAX_MATRIX_STACK    32
#define MAX_TEXTURES        128
#define MAX_LIGHTS          4

/* Post-clip, post-project triangle. Built once, rasterized by every band. */
typedef struct {
    float x, y;            /* screen space */
    float z;               /* z/w, linear in screen space */
    float ow;              /* 1/w */
    float u, v;            /* u/w, v/w */
    float r, g, b, a;      /* colour/w */
} rvert_t;

typedef struct {
    rvert_t v[3];
    float ymin, ymax;      /* band reject without touching v[] */
} rtri_t;

#define AGL_MAX_BATCH_TRIS   1024
/* Below this a parallel dispatch costs more than the fill it splits. */
#define AGL_PARALLEL_MIN_TRIS 24
#define AGL_PARALLEL_MIN_VERTS 96

typedef struct {
    uint32_t *framebuffer;
    float *depth_buffer;
    int fb_width, fb_height;
    int vp_x, vp_y, vp_width, vp_height;

    agl_color_t clear_color;
    float clear_depth;

    bool depth_test_enabled;
    bool depth_write_enabled;
    bool cull_face_enabled;
    bool lighting_enabled;
    bool texture_2d_enabled;
    bool blend_enabled;
    bool fog_enabled;

    uint32_t depth_func;
    uint32_t cull_face_mode;
    uint32_t shade_model;
    uint32_t blend_src, blend_dst;

    agl_color_t current_color;
    agl_vec3_t current_normal;
    agl_vec2_t current_texcoord;

    uint32_t matrix_mode;
    agl_mat4_t modelview_stack[MAX_MATRIX_STACK];
    int modelview_stack_top;
    agl_mat4_t projection_stack[MAX_MATRIX_STACK];
    int projection_stack_top;
    agl_mat4_t texture_stack[MAX_MATRIX_STACK];
    int texture_stack_top;

    agl_mat4_t modelview_matrix;
    agl_mat4_t projection_matrix;
    agl_mat4_t texture_matrix;
    agl_mat4_t mvp_matrix;
    bool mvp_dirty;

    agl_light_t lights[MAX_LIGHTS];
    agl_material_t material;
    bool any_light_enabled;

    agl_texture_t textures[MAX_TEXTURES];
    uint32_t current_texture;
    uint32_t next_texture_id;
    int bound_slot;              /* -1 when the id has no slot yet */

    bool in_begin_end;
    uint32_t current_primitive;
    agl_vertex_t vertex_buffer[MAX_VERTICES];
    int vertex_count;

    rtri_t *batch;
    int batch_count;

    uint32_t last_error;

    agl_color_t fog_color;
    float fog_density, fog_start, fog_end;
    uint32_t fog_mode;
} agl_state_t;

static agl_state_t *g_state = NULL;

/* RASTER CONTEXT
 * Filled once per aglEnd on the calling core, then read (never written) by
 * every SMP band. Everything the pixel loop needs lives here so the loop
 * never reloads from g_state.
*/

typedef struct {
    const uint8_t *data;
    int w, h, bpp;
    int32_t wmask, hmask;
    int pow2;                /* both dimensions are powers of two */
    int rep_s, rep_t;
    float wfx, hfx;          /* width * 65536.0f, height * 65536.0f */
    int bilinear;
    int enabled;
} tex_ref_t;

typedef struct {
    int y, xs, xe;           /* xs..xe inclusive, already viewport-clipped */
    float z, dz;
    float ow, dow;
    float u, du, v, dv;      /* u/w, v/w */
    float r, dr, g, dg, b, db, a, da;
} span_t;

struct raster_ctx;
typedef void (*span_fn_t)(const struct raster_ctx *, const span_t *);

typedef struct raster_ctx {
    uint32_t *fb;
    float *zb;
    int fbw;
    int depth_test;
    int depth_write_enabled;
    uint32_t depth_func;
    int blend;
    uint32_t bsrc, bdst;
    int fog;
    uint32_t fog_mode;
    float fog_density, fog_start, fog_end, fog_range_inv;
    int fog_r, fog_g, fog_b;
    tex_ref_t tex;
    span_fn_t span;
    int vp_x0, vp_x1, vp_y0, vp_y1;   /* inclusive pixel bounds */
} raster_ctx_t;

static raster_ctx_t g_rc;

/* VECTOR OPERATIONS */

agl_vec3_t agl_vec3_add(agl_vec3_t a, agl_vec3_t b) {
    agl_vec3_t r; r.x = a.x + b.x; r.y = a.y + b.y; r.z = a.z + b.z; return r;
}
agl_vec3_t agl_vec3_sub(agl_vec3_t a, agl_vec3_t b) {
    agl_vec3_t r; r.x = a.x - b.x; r.y = a.y - b.y; r.z = a.z - b.z; return r;
}
agl_vec3_t agl_vec3_scale(agl_vec3_t v, float s) {
    agl_vec3_t r; r.x = v.x * s; r.y = v.y * s; r.z = v.z * s; return r;
}
float agl_vec3_dot(agl_vec3_t a, agl_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
agl_vec3_t agl_vec3_cross(agl_vec3_t a, agl_vec3_t b) {
    agl_vec3_t r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}
float agl_vec3_length(agl_vec3_t v) { return agl_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
agl_vec3_t agl_vec3_normalize(agl_vec3_t v) {
    float len_sq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len_sq < 1e-10f) { agl_vec3_t z = {0, 0, 0}; return z; }
    float inv_len = agl_rsqrtf(len_sq);
    agl_vec3_t r; r.x = v.x * inv_len; r.y = v.y * inv_len; r.z = v.z * inv_len; return r;
}

/* MATRIX OPERATIONS
 * Internally everything goes through pointers; the two by-value entry points
 * stay for API compatibility. */

static void mat4_identity_p(agl_mat4_t *m) {
    agl_memset(m, 0, sizeof(*m));
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

agl_mat4_t agl_mat4_identity(void) {
    agl_mat4_t m;
    mat4_identity_p(&m);
    return m;
}

/* out = a * b. out may alias neither a nor b. */
static void mat4_mul_p(agl_mat4_t *out, const agl_mat4_t *a, const agl_mat4_t *b) {
    const float *A = a->m, *B = b->m;
    for (int j = 0; j < 4; j++) {
        float b0 = B[j * 4], b1 = B[j * 4 + 1], b2 = B[j * 4 + 2], b3 = B[j * 4 + 3];
        out->m[j * 4 + 0] = A[0] * b0 + A[4] * b1 + A[8]  * b2 + A[12] * b3;
        out->m[j * 4 + 1] = A[1] * b0 + A[5] * b1 + A[9]  * b2 + A[13] * b3;
        out->m[j * 4 + 2] = A[2] * b0 + A[6] * b1 + A[10] * b2 + A[14] * b3;
        out->m[j * 4 + 3] = A[3] * b0 + A[7] * b1 + A[11] * b2 + A[15] * b3;
    }
}

agl_mat4_t agl_mat4_multiply(agl_mat4_t a, agl_mat4_t b) {
    agl_mat4_t r;
    mat4_mul_p(&r, &a, &b);
    return r;
}

/* dst = dst * t, in place. */
static void mat4_mul_into(agl_mat4_t *dst, const agl_mat4_t *t) {
    agl_mat4_t tmp;
    mat4_mul_p(&tmp, dst, t);
    *dst = tmp;
}

AGL_INLINE void mat4_mul_vec4(const agl_mat4_t *mm, float x, float y, float z, float w,
                              agl_vec4_t *out) {
    const float *m = mm->m;
    out->x = m[0]*x + m[4]*y + m[8] *z + m[12]*w;
    out->y = m[1]*x + m[5]*y + m[9] *z + m[13]*w;
    out->z = m[2]*x + m[6]*y + m[10]*z + m[14]*w;
    out->w = m[3]*x + m[7]*y + m[11]*z + m[15]*w;
}

AGL_INLINE void mat3_mul_vec3(const agl_mat4_t *mm, float x, float y, float z,
                              agl_vec3_t *out) {
    const float *m = mm->m;
    out->x = m[0]*x + m[4]*y + m[8] *z;
    out->y = m[1]*x + m[5]*y + m[9] *z;
    out->z = m[2]*x + m[6]*y + m[10]*z;
}

static agl_mat4_t mat4_translation(float x, float y, float z) {
    agl_mat4_t m = agl_mat4_identity();
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

static agl_mat4_t mat4_rotation(float angle_rad, float x, float y, float z) {
    float len_sq = x*x + y*y + z*z;
    if (len_sq < 1e-12f) return agl_mat4_identity();
    float inv = agl_rsqrtf(len_sq);
    x *= inv; y *= inv; z *= inv;
    float s, c;
    agl_sincosf(angle_rad, &s, &c);
    float t = 1.0f - c;
    agl_mat4_t m = agl_mat4_identity();
    m.m[0]  = t*x*x + c;    m.m[4] = t*x*y - s*z; m.m[8]  = t*x*z + s*y;
    m.m[1]  = t*x*y + s*z;  m.m[5] = t*y*y + c;   m.m[9]  = t*y*z - s*x;
    m.m[2]  = t*x*z - s*y;  m.m[6] = t*y*z + s*x; m.m[10] = t*z*z + c;
    return m;
}

static agl_mat4_t mat4_scale(float x, float y, float z) {
    agl_mat4_t m = agl_mat4_identity();
    m.m[0] = x; m.m[5] = y; m.m[10] = z;
    return m;
}

static agl_mat4_t mat4_perspective(float fovy_rad, float aspect, float near, float far) {
    float t = agl_tanf(fovy_rad * 0.5f);
    agl_mat4_t m;
    agl_memset(&m, 0, sizeof(m));
    if (agl_fabsf(t) < 1e-9f || agl_fabsf(aspect) < 1e-9f ||
        agl_fabsf(near - far) < 1e-9f) {
        mat4_identity_p(&m);
        return m;
    }
    float f = 1.0f / t;
    m.m[0]  = f / aspect;
    m.m[5]  = f;
    m.m[10] = (far + near) / (near - far);
    m.m[11] = -1.0f;
    m.m[14] = (2.0f * far * near) / (near - far);
    return m;
}

static agl_mat4_t mat4_ortho(float l, float r, float b, float t, float n, float f) {
    agl_mat4_t m;
    agl_memset(&m, 0, sizeof(m));
    if (agl_fabsf(r - l) < 1e-9f || agl_fabsf(t - b) < 1e-9f || agl_fabsf(f - n) < 1e-9f) {
        mat4_identity_p(&m);
        return m;
    }
    m.m[0]  = 2.0f / (r - l);
    m.m[5]  = 2.0f / (t - b);
    m.m[10] = -2.0f / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    m.m[15] = 1.0f;
    return m;
}

static agl_mat4_t mat4_lookat(float ex, float ey, float ez,
                              float cx, float cy, float cz,
                              float ux, float uy, float uz) {
    agl_vec3_t eye = {ex, ey, ez}, center = {cx, cy, cz}, up = {ux, uy, uz};
    agl_vec3_t f = agl_vec3_normalize(agl_vec3_sub(center, eye));
    agl_vec3_t s = agl_vec3_normalize(agl_vec3_cross(f, up));
    agl_vec3_t u = agl_vec3_cross(s, f);

    agl_mat4_t m = agl_mat4_identity();
    m.m[0] = s.x;  m.m[4] = s.y;  m.m[8]  = s.z;  m.m[12] = -agl_vec3_dot(s, eye);
    m.m[1] = u.x;  m.m[5] = u.y;  m.m[9]  = u.z;  m.m[13] = -agl_vec3_dot(u, eye);
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z; m.m[14] = agl_vec3_dot(f, eye);
    return m;
}

/* The MVP is only ever consumed at aglEnd, so mark and fold instead of
 * recomputing a 4x4 product on every transform call. */
static inline void mark_mvp_dirty(void) { g_state->mvp_dirty = true; }

static const agl_mat4_t *resolve_mvp(void) {
    if (g_state->mvp_dirty) {
        mat4_mul_p(&g_state->mvp_matrix, &g_state->projection_matrix,
                   &g_state->modelview_matrix);
        g_state->mvp_dirty = false;
    }
    return &g_state->mvp_matrix;
}

/* STATE MANAGEMENT */

static void refresh_bound_slot(void) {
    g_state->bound_slot = -1;
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (g_state->textures[i].id == g_state->current_texture) {
            g_state->bound_slot = i;
            return;
        }
    }
}

static void tex_slot_defaults(agl_texture_t *t) {
    t->mag_filter = AGL_LINEAR;
    t->min_filter = AGL_LINEAR;
    t->wrap_s = AGL_REPEAT;
    t->wrap_t = AGL_REPEAT;
}

void aglInit(int width, int height, uint32_t *framebuffer) {
    if (g_state) aglShutdown();
    if (width <= 0 || height <= 0) return;
    /* (w * h * 4) must not wrap a uint32. */
    if ((uint32_t)width > 0x3FFFFFFFu / (uint32_t)height) return;

    g_state = (agl_state_t *)kmalloc(sizeof(agl_state_t));
    if (!g_state) return;
    agl_memset(g_state, 0, sizeof(*g_state));

    g_state->framebuffer = framebuffer;
    g_state->fb_width = width;
    g_state->fb_height = height;

    uint32_t px = (uint32_t)width * (uint32_t)height;
    g_state->depth_buffer = (float *)kmalloc(px * (uint32_t)sizeof(float));
    if (!g_state->depth_buffer) { kfree(g_state); g_state = NULL; return; }

    g_state->batch = (rtri_t *)kmalloc(AGL_MAX_BATCH_TRIS * (uint32_t)sizeof(rtri_t));
    if (!g_state->batch) {
        kfree(g_state->depth_buffer);
        kfree(g_state);
        g_state = NULL;
        return;
    }

    g_state->vp_width = width; g_state->vp_height = height;
    g_state->clear_color = (agl_color_t){0, 0, 0, 1};
    g_state->clear_depth = 1.0f;
    g_state->depth_func = AGL_LESS;
    g_state->depth_write_enabled = true;
    g_state->cull_face_mode = AGL_BACK;
    g_state->shade_model = AGL_SMOOTH;
    g_state->blend_src = AGL_SRC_ALPHA;
    g_state->blend_dst = AGL_ONE_MINUS_SRC_ALPHA;
    g_state->current_color = (agl_color_t){1, 1, 1, 1};
    g_state->current_normal = (agl_vec3_t){0, 0, 1};
    g_state->matrix_mode = AGL_MODELVIEW;
    g_state->modelview_matrix = agl_mat4_identity();
    g_state->projection_matrix = agl_mat4_identity();
    g_state->texture_matrix = agl_mat4_identity();
    g_state->mvp_matrix = agl_mat4_identity();
    g_state->mvp_dirty = false;
    g_state->bound_slot = -1;

    /* kmalloc does not zero: a draw before the first aglClear would otherwise
     * depth-test against garbage. */
    {
        uint32_t d;
        agl_memcpy(&d, &g_state->clear_depth, sizeof(d));
        agl_fill_u32((uint32_t *)g_state->depth_buffer, d, px);
    }

    for (int i = 0; i < MAX_LIGHTS; i++) {
        g_state->lights[i].position = (agl_vec4_t){0, 0, 1, 0};
        g_state->lights[i].ambient  = (agl_color_t){0, 0, 0, 1};
        g_state->lights[i].diffuse  = (agl_color_t){0, 0, 0, 1};
        g_state->lights[i].specular = (agl_color_t){0, 0, 0, 1};
    }
    g_state->material.ambient  = (agl_color_t){0.2f, 0.2f, 0.2f, 1};
    g_state->material.diffuse  = (agl_color_t){0.8f, 0.8f, 0.8f, 1};
    g_state->material.specular = (agl_color_t){0, 0, 0, 1};
    g_state->material.emission = (agl_color_t){0, 0, 0, 1};

    for (int i = 0; i < MAX_TEXTURES; i++) tex_slot_defaults(&g_state->textures[i]);
    g_state->next_texture_id = 1;
    g_state->fog_density = 1.0f;
    g_state->fog_end = 1.0f;
    g_state->fog_mode = AGL_EXP;
}

void aglSetFramebuffer(uint32_t *fb) { if (g_state) g_state->framebuffer = fb; }

void aglGetViewport(int *x, int *y, int *w, int *h) {
    if (!g_state) return;
    if (x) *x = g_state->vp_x;
    if (y) *y = g_state->vp_y;
    if (w) *w = g_state->vp_width;
    if (h) *h = g_state->vp_height;
}

void aglViewport(int x, int y, int w, int h) {
    if (!g_state) return;
    g_state->vp_x = x; g_state->vp_y = y;
    g_state->vp_width = w; g_state->vp_height = h;
}

void aglShutdown(void) {
    if (!g_state) return;
    if (g_state->depth_buffer) kfree(g_state->depth_buffer);
    if (g_state->batch) kfree(g_state->batch);
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (g_state->textures[i].data) kfree(g_state->textures[i].data);
    kfree(g_state);
    g_state = NULL;
}

void aglEnable(uint32_t cap) {
    if (!g_state) return;
    switch (cap) {
        case AGL_DEPTH_TEST:  g_state->depth_test_enabled = true; break;
        case AGL_CULL_FACE:   g_state->cull_face_enabled = true; break;
        case AGL_LIGHTING:    g_state->lighting_enabled = true; break;
        case AGL_TEXTURE_2D:  g_state->texture_2d_enabled = true; break;
        case AGL_BLEND:       g_state->blend_enabled = true; break;
        case AGL_FOG:         g_state->fog_enabled = true; break;
        default: g_state->last_error = AGL_INVALID_ENUM;
    }
}

void aglDisable(uint32_t cap) {
    if (!g_state) return;
    switch (cap) {
        case AGL_DEPTH_TEST:  g_state->depth_test_enabled = false; break;
        case AGL_CULL_FACE:   g_state->cull_face_enabled = false; break;
        case AGL_LIGHTING:    g_state->lighting_enabled = false; break;
        case AGL_TEXTURE_2D:  g_state->texture_2d_enabled = false; break;
        case AGL_BLEND:       g_state->blend_enabled = false; break;
        case AGL_FOG:         g_state->fog_enabled = false; break;
        default: g_state->last_error = AGL_INVALID_ENUM;
    }
}

bool aglIsEnabled(uint32_t cap) {
    if (!g_state) return false;
    switch (cap) {
        case AGL_DEPTH_TEST:  return g_state->depth_test_enabled;
        case AGL_CULL_FACE:   return g_state->cull_face_enabled;
        case AGL_LIGHTING:    return g_state->lighting_enabled;
        case AGL_TEXTURE_2D:  return g_state->texture_2d_enabled;
        case AGL_BLEND:       return g_state->blend_enabled;
        case AGL_FOG:         return g_state->fog_enabled;
        default: g_state->last_error = AGL_INVALID_ENUM; return false;
    }
}

void aglClearColor(float r, float g, float b, float a) {
    if (!g_state) return;
    g_state->clear_color = (agl_color_t){agl_clampf(r,0,1), agl_clampf(g,0,1),
                                         agl_clampf(b,0,1), agl_clampf(a,0,1)};
}

void aglClearDepth(float d) {
    if (g_state) g_state->clear_depth = d;
}

typedef struct { int y0, y1, x0, ncols; uint32_t mask, color, depth; } agl_clear_ctx_t;

/* Rows [s,e) of one band. When the region is the full framebuffer width the
 * rows are contiguous, so the whole band goes out as a single rep stosl. */
static void clear_rows(const agl_clear_ctx_t *c, int s, int e) {
    if (e <= s) return;
    int full = (c->x0 == 0 && c->ncols == g_state->fb_width);
    uint32_t stride = (uint32_t)g_state->fb_width;

    if ((c->mask & AGL_COLOR_BUFFER_BIT) && g_state->framebuffer) {
        uint32_t *p = g_state->framebuffer + (uint32_t)s * stride + (uint32_t)c->x0;
        if (full) {
            agl_fill_u32(p, c->color, (uint32_t)(e - s) * stride);
        } else {
            for (int y = s; y < e; y++, p += stride)
                agl_fill_u32(p, c->color, (uint32_t)c->ncols);
        }
    }
    if ((c->mask & AGL_DEPTH_BUFFER_BIT) && g_state->depth_buffer) {
        uint32_t *p = (uint32_t *)g_state->depth_buffer + (uint32_t)s * stride + (uint32_t)c->x0;
        if (full) {
            agl_fill_u32(p, c->depth, (uint32_t)(e - s) * stride);
        } else {
            for (int y = s; y < e; y++, p += stride)
                agl_fill_u32(p, c->depth, (uint32_t)c->ncols);
        }
    }
}

static void agl_clear_band(int band, int nbands, void *ctx) {
    agl_clear_ctx_t *c = (agl_clear_ctx_t *)ctx;
    int rows = c->y1 - c->y0;
    clear_rows(c, c->y0 + (rows * band) / nbands,
                  c->y0 + (rows * (band + 1)) / nbands);
}

void aglClear(uint32_t mask) {
    if (!g_state) return;
    agl_clear_ctx_t c;
    c.x0 = agl_maxi(g_state->vp_x, 0);
    c.y0 = agl_maxi(g_state->vp_y, 0);
    int x1 = agl_mini(g_state->vp_x + g_state->vp_width, g_state->fb_width);
    c.y1 = agl_mini(g_state->vp_y + g_state->vp_height, g_state->fb_height);
    c.ncols = (x1 > c.x0) ? (x1 - c.x0) : 0;
    if (c.ncols == 0 || c.y1 <= c.y0) return;
    c.mask = mask;
    c.color =
        ((uint32_t)(agl_clampf(g_state->clear_color.a, 0, 1) * 255.0f + 0.5f) << 24) |
        ((uint32_t)(agl_clampf(g_state->clear_color.r, 0, 1) * 255.0f + 0.5f) << 16) |
        ((uint32_t)(agl_clampf(g_state->clear_color.g, 0, 1) * 255.0f + 0.5f) << 8)  |
        ((uint32_t)(agl_clampf(g_state->clear_color.b, 0, 1) * 255.0f + 0.5f));
    agl_memcpy(&c.depth, &g_state->clear_depth, sizeof(c.depth));

    int nb = smp_cpu_count();
    if (nb > 1 && (c.y1 - c.y0) >= 32) smp_parallel_bands(agl_clear_band, &c, nb);
    else clear_rows(&c, c.y0, c.y1);
}

void aglDepthFunc(uint32_t func) {
    if (!g_state) return;
    if (func >= AGL_NEVER && func <= AGL_ALWAYS) g_state->depth_func = func;
    else g_state->last_error = AGL_INVALID_ENUM;
}

void aglDepthMask(bool enabled) {
    if (g_state) {
        g_state->depth_write_enabled = enabled;
    }
}

void aglCullFace(uint32_t mode) {
    if (!g_state) return;
    if (mode == AGL_FRONT || mode == AGL_BACK || mode == AGL_FRONT_AND_BACK)
        g_state->cull_face_mode = mode;
    else g_state->last_error = AGL_INVALID_ENUM;
}

void aglShadeModel(uint32_t mode) {
    if (!g_state) return;
    if (mode == AGL_FLAT || mode == AGL_SMOOTH) g_state->shade_model = mode;
    else g_state->last_error = AGL_INVALID_ENUM;
}

void aglBlendFunc(uint32_t sfactor, uint32_t dfactor) {
    if (!g_state) return;
    if (sfactor >= AGL_ZERO && sfactor <= AGL_ONE_MINUS_DST_COLOR &&
        dfactor >= AGL_ZERO && dfactor <= AGL_ONE_MINUS_DST_COLOR) {
        g_state->blend_src = sfactor;
        g_state->blend_dst = dfactor;
    } else g_state->last_error = AGL_INVALID_ENUM;
}

uint32_t aglGetError(void) {
    if (!g_state) return AGL_INVALID_OPERATION;
    uint32_t e = g_state->last_error;
    g_state->last_error = AGL_NO_ERROR;
    return e;
}

/* MATRIX STACK */

static agl_mat4_t *current_matrix(void) {
    switch (g_state->matrix_mode) {
        case AGL_PROJECTION: return &g_state->projection_matrix;
        case AGL_TEXTURE:    return &g_state->texture_matrix;
        default:             return &g_state->modelview_matrix;
    }
}

void aglMatrixMode(uint32_t mode) {
    if (!g_state) return;
    if (mode == AGL_MODELVIEW || mode == AGL_PROJECTION || mode == AGL_TEXTURE)
        g_state->matrix_mode = mode;
    else g_state->last_error = AGL_INVALID_ENUM;
}

void aglLoadIdentity(void) {
    if (!g_state) return;
    mat4_identity_p(current_matrix());
    mark_mvp_dirty();
}

static void push_stack(agl_mat4_t *stack, int *top, const agl_mat4_t *current) {
    if (*top < MAX_MATRIX_STACK - 1) stack[(*top)++] = *current;
    else g_state->last_error = AGL_STACK_OVERFLOW;
}

static void pop_stack(agl_mat4_t *stack, int *top, agl_mat4_t *dst) {
    if (*top > 0) { *dst = stack[--(*top)]; return; }
    g_state->last_error = AGL_STACK_UNDERFLOW;
    mat4_identity_p(dst);
}

void aglPushMatrix(void) {
    if (!g_state) return;
    switch (g_state->matrix_mode) {
        case AGL_MODELVIEW:  push_stack(g_state->modelview_stack,  &g_state->modelview_stack_top,  &g_state->modelview_matrix);  break;
        case AGL_PROJECTION: push_stack(g_state->projection_stack, &g_state->projection_stack_top, &g_state->projection_matrix); break;
        case AGL_TEXTURE:    push_stack(g_state->texture_stack,    &g_state->texture_stack_top,    &g_state->texture_matrix);    break;
    }
}

void aglPopMatrix(void) {
    if (!g_state) return;
    switch (g_state->matrix_mode) {
        case AGL_MODELVIEW:  pop_stack(g_state->modelview_stack,  &g_state->modelview_stack_top,  &g_state->modelview_matrix);  break;
        case AGL_PROJECTION: pop_stack(g_state->projection_stack, &g_state->projection_stack_top, &g_state->projection_matrix); break;
        case AGL_TEXTURE:    pop_stack(g_state->texture_stack,    &g_state->texture_stack_top,    &g_state->texture_matrix);    break;
    }
    mark_mvp_dirty();
}

void aglLoadMatrix(const float *m) {
    if (!g_state || !m) return;
    agl_memcpy(current_matrix()->m, m, 16 * sizeof(float));
    mark_mvp_dirty();
}

void aglMultMatrix(const float *m) {
    if (!g_state || !m) return;
    agl_mat4_t mat;
    agl_memcpy(mat.m, m, 16 * sizeof(float));
    mat4_mul_into(current_matrix(), &mat);
    mark_mvp_dirty();
}

static void apply_transform(const agl_mat4_t *t) {
    mat4_mul_into(current_matrix(), t);
    mark_mvp_dirty();
}

void aglTranslate(float x, float y, float z) {
    if (!g_state) return;
    agl_mat4_t t = mat4_translation(x, y, z);
    apply_transform(&t);
}
void aglRotate(float angle_deg, float x, float y, float z) {
    if (!g_state) return;
    agl_mat4_t t = mat4_rotation(angle_deg * 0.017453292519943295f, x, y, z);
    apply_transform(&t);
}
void aglScale(float x, float y, float z) {
    if (!g_state) return;
    agl_mat4_t t = mat4_scale(x, y, z);
    apply_transform(&t);
}

void aglPerspective(float fovy_deg, float aspect, float near, float far) {
    if (!g_state) return;
    agl_mat4_t p = mat4_perspective(fovy_deg * 0.017453292519943295f, aspect, near, far);
    if (g_state->matrix_mode == AGL_PROJECTION) g_state->projection_matrix = p;
    else mat4_mul_into(&g_state->projection_matrix, &p);
    mark_mvp_dirty();
}

void aglOrtho(float l, float r, float b, float t, float n, float f) {
    if (!g_state) return;
    agl_mat4_t o = mat4_ortho(l, r, b, t, n, f);
    if (g_state->matrix_mode == AGL_PROJECTION) g_state->projection_matrix = o;
    else mat4_mul_into(&g_state->projection_matrix, &o);
    mark_mvp_dirty();
}

void aglLookAt(float ex, float ey, float ez, float cx, float cy, float cz,
               float ux, float uy, float uz) {
    if (!g_state) return;
    agl_mat4_t l = mat4_lookat(ex, ey, ez, cx, cy, cz, ux, uy, uz);
    if (g_state->matrix_mode == AGL_MODELVIEW) g_state->modelview_matrix = l;
    else mat4_mul_into(&g_state->modelview_matrix, &l);
    mark_mvp_dirty();
}

void aglFrustum(float l, float r, float b, float t, float n, float f) {
    if (!g_state) return;
    agl_mat4_t m;
    agl_memset(&m, 0, sizeof(m));
    if (agl_fabsf(r - l) < 1e-9f || agl_fabsf(t - b) < 1e-9f || agl_fabsf(f - n) < 1e-9f) {
        g_state->last_error = AGL_INVALID_OPERATION;
        return;
    }
    m.m[0]  = (2.0f * n) / (r - l);
    m.m[5]  = (2.0f * n) / (t - b);
    m.m[8]  = (r + l) / (r - l);
    m.m[9]  = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1.0f;
    m.m[14] = -(2.0f * f * n) / (f - n);
    if (g_state->matrix_mode == AGL_PROJECTION) g_state->projection_matrix = m;
    else mat4_mul_into(&g_state->projection_matrix, &m);
    mark_mvp_dirty();
}

/* IMMEDIATE MODE */

void aglBegin(uint32_t mode) {
    if (!g_state) return;
    if (g_state->in_begin_end) { g_state->last_error = AGL_INVALID_OPERATION; return; }
    if (mode > AGL_QUADS) { g_state->last_error = AGL_INVALID_ENUM; return; }
    g_state->current_primitive = mode;
    g_state->in_begin_end = true;
    g_state->vertex_count = 0;
}

void aglColor3f(float r, float g, float b) {
    if (!g_state) return;
    g_state->current_color = (agl_color_t){agl_clampf(r,0,1), agl_clampf(g,0,1),
                                          agl_clampf(b,0,1), 1.0f};
}
void aglColor4f(float r, float g, float b, float a) {
    if (!g_state) return;
    g_state->current_color = (agl_color_t){agl_clampf(r,0,1), agl_clampf(g,0,1),
                                          agl_clampf(b,0,1), agl_clampf(a,0,1)};
}
void aglColor3ub(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_state) return;
    g_state->current_color = (agl_color_t){r * 0.003921568627451f, g * 0.003921568627451f,
                                          b * 0.003921568627451f, 1.0f};
}
void aglColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!g_state) return;
    g_state->current_color = (agl_color_t){r * 0.003921568627451f, g * 0.003921568627451f,
                                          b * 0.003921568627451f, a * 0.003921568627451f};
}

void aglNormal3f(float x, float y, float z) {
    if (!g_state) return;
    g_state->current_normal = (agl_vec3_t){x, y, z};
}
void aglTexCoord2f(float u, float v) {
    if (!g_state) return;
    g_state->current_texcoord = (agl_vec2_t){u, v};
}

static void submit_vertex(float x, float y, float z, float w) {
    if (!g_state || !g_state->in_begin_end) return;
    if (g_state->vertex_count >= MAX_VERTICES) { g_state->last_error = AGL_OUT_OF_MEMORY; return; }
    agl_vertex_t *v = &g_state->vertex_buffer[g_state->vertex_count++];
    v->px = x; v->py = y; v->pz = z; v->pw = w;
    v->nx = g_state->current_normal.x;
    v->ny = g_state->current_normal.y;
    v->nz = g_state->current_normal.z;
    v->r = g_state->current_color.r; v->g = g_state->current_color.g;
    v->b = g_state->current_color.b; v->a = g_state->current_color.a;
    v->u = g_state->current_texcoord.u; v->v = g_state->current_texcoord.v;
}

void aglVertex2f(float x, float y) { submit_vertex(x, y, 0.0f, 1.0f); }
void aglVertex3f(float x, float y, float z) { submit_vertex(x, y, z, 1.0f); }
void aglVertex4f(float x, float y, float z, float w) { submit_vertex(x, y, z, w); }

/* One call per quad instead of 13 immediate-mode calls. */
void aglSubmitQuad(float nx, float ny, float nz, const agl_qv_t v[4]) {
    if (!g_state || !g_state->in_begin_end || !v) return;
    if (g_state->vertex_count + 4 > MAX_VERTICES) {
        g_state->last_error = AGL_OUT_OF_MEMORY;
        return;
    }
    agl_vertex_t *vb = &g_state->vertex_buffer[g_state->vertex_count];
    g_state->vertex_count += 4;
    for (int k = 0; k < 4; k++) {
        vb[k].px = v[k].x; vb[k].py = v[k].y; vb[k].pz = v[k].z; vb[k].pw = 1.0f;
        vb[k].nx = nx; vb[k].ny = ny; vb[k].nz = nz;
        vb[k].r = v[k].r; vb[k].g = v[k].g; vb[k].b = v[k].b; vb[k].a = v[k].a;
        vb[k].u = v[k].u; vb[k].v = v[k].v;
    }
}

/* LIGHTING (Gouraud: per-vertex, colours interpolated) */

static void compute_lighting(agl_vertex_t *v) {
    const agl_material_t *mat = &g_state->material;

    float rr = mat->emission.r + mat->ambient.r * 0.1f;
    float gg = mat->emission.g + mat->ambient.g * 0.1f;
    float bb = mat->emission.b + mat->ambient.b * 0.1f;

    agl_vec3_t normal = agl_vec3_normalize((agl_vec3_t){v->nx, v->ny, v->nz});
    agl_vec3_t view_pos = (agl_vec3_t){v->vx, v->vy, v->vz};
    agl_vec3_t view_dir = agl_vec3_normalize(agl_vec3_scale(view_pos, -1.0f));
    float shininess = mat->shininess;

    for (int i = 0; i < MAX_LIGHTS; i++) {
        const agl_light_t *light = &g_state->lights[i];
        if (!light->enabled) continue;

        agl_vec3_t light_dir;
        float atten = 1.0f;

        if (light->position.w == 0.0f) {
            light_dir = agl_vec3_normalize(agl_vec3_scale(
                (agl_vec3_t){light->position.x, light->position.y, light->position.z}, -1.0f));
        } else {
            agl_vec3_t lp = {light->position.x, light->position.y, light->position.z};
            light_dir = agl_vec3_sub(lp, view_pos);
            float d2 = light_dir.x*light_dir.x + light_dir.y*light_dir.y + light_dir.z*light_dir.z;
            if (d2 < 1e-12f) continue;
            float inv_d = agl_rsqrtf(d2);
            light_dir = agl_vec3_scale(light_dir, inv_d);
            atten = 1.0f / (1.0f + 0.01f * d2);   /* dist*dist without the sqrt */
        }

        rr += mat->ambient.r * light->ambient.r;
        gg += mat->ambient.g * light->ambient.g;
        bb += mat->ambient.b * light->ambient.b;

        float diff = agl_vec3_dot(normal, light_dir);
        if (diff <= 0.0f) continue;

        float k = diff * atten;
        rr += mat->diffuse.r * light->diffuse.r * k;
        gg += mat->diffuse.g * light->diffuse.g * k;
        bb += mat->diffuse.b * light->diffuse.b * k;

        if (shininess > 0.0f) {
            agl_vec3_t half_vec = agl_vec3_normalize(agl_vec3_add(light_dir, view_dir));
            float spec = agl_vec3_dot(normal, half_vec);
            if (spec > 0.0f) {
                spec = agl_powf(spec, shininess) * atten;
                rr += mat->specular.r * light->specular.r * spec;
                gg += mat->specular.g * light->specular.g * spec;
                bb += mat->specular.b * light->specular.b * spec;
            }
        }
    }

    v->r = agl_clampf(rr, 0, 1);
    v->g = agl_clampf(gg, 0, 1);
    v->b = agl_clampf(bb, 0, 1);
    /* Alpha comes from the material's diffuse alpha, not from emission. */
    v->a = agl_clampf(mat->diffuse.a, 0, 1);
}

static void transform_vertices(void) {
    const agl_mat4_t *mvp = resolve_mvp();
    const agl_mat4_t *mv = &g_state->modelview_matrix;
    int lit = g_state->lighting_enabled && g_state->any_light_enabled;
    int n = g_state->vertex_count;

    for (int i = 0; i < n; i++) {
        agl_vertex_t *v = &g_state->vertex_buffer[i];
        float px = v->px, py = v->py, pz = v->pz, pw = v->pw;

        if (lit) {
            /* The view-space position only ever fed lighting; 2.0 paid for it
             * on every vertex regardless. */
            agl_vec4_t view4;
            mat4_mul_vec4(mv, px, py, pz, pw, &view4);
            v->vx = view4.x; v->vy = view4.y; v->vz = view4.z;

            agl_vec3_t nrm;
            mat3_mul_vec3(mv, v->nx, v->ny, v->nz, &nrm);
            v->nx = nrm.x; v->ny = nrm.y; v->nz = nrm.z;
        }

        agl_vec4_t clip4;
        mat4_mul_vec4(mvp, px, py, pz, pw, &clip4);
        v->px = clip4.x; v->py = clip4.y; v->pz = clip4.z; v->pw = clip4.w;

        if (lit) compute_lighting(v);
    }
}

/* NEAR-PLANE CLIPPING (Sutherland-Hodgman in clip space, z + w >= 0) */

static void lerp_vertex(agl_vertex_t *r, const agl_vertex_t *a, const agl_vertex_t *b, float t) {
    r->px = a->px + t * (b->px - a->px);
    r->py = a->py + t * (b->py - a->py);
    r->pz = a->pz + t * (b->pz - a->pz);
    r->pw = a->pw + t * (b->pw - a->pw);
    r->r  = a->r  + t * (b->r  - a->r);
    r->g  = a->g  + t * (b->g  - a->g);
    r->b  = a->b  + t * (b->b  - a->b);
    r->a  = a->a  + t * (b->a  - a->a);
    r->u  = a->u  + t * (b->u  - a->u);
    r->v  = a->v  + t * (b->v  - a->v);
    /* vx/vy/vz and the normal are consumed before clipping (lighting is
     * per-vertex and already resolved), so they are not interpolated here. */
    r->vx = r->vy = r->vz = 0.0f;
    r->nx = r->ny = r->nz = 0.0f;
}

static int clip_polygon_near(const agl_vertex_t *in, int n_in, agl_vertex_t *out) {
    int n_out = 0;
    float d_prev = in[n_in - 1].pz + in[n_in - 1].pw;
    for (int i = 0; i < n_in; i++) {
        float d_cur = in[i].pz + in[i].pw;
        bool in_cur = d_cur >= 0.0f, in_prev = d_prev >= 0.0f;
        if (in_cur != in_prev) {
            const agl_vertex_t *p = &in[(i + n_in - 1) % n_in];
            lerp_vertex(&out[n_out++], p, &in[i], d_prev / (d_prev - d_cur));
        }
        if (in_cur) out[n_out++] = in[i];
        d_prev = d_cur;
    }
    return n_out;
}

/* TEXTURE SAMPLING (16.16 texel coordinates, integer filtering) */

static inline int is_pow2(int x) { return x > 0 && (x & (x - 1)) == 0; }

/* Wrap/clamp an integer texel index. */
AGL_INLINE int tex_wrap(int x, int n, int32_t mask, int pow2, int repeat) {
    if (repeat) {
        if (pow2) return x & mask;
        x %= n;
        return x < 0 ? x + n : x;
    }
    return agl_clampi(x, 0, n - 1);
}

AGL_INLINE void tex_nearest(const tex_ref_t *T, int32_t u, int32_t v, int *o) {
    int x = tex_wrap(u >> 16, T->w, T->wmask, T->pow2, T->rep_s);
    int y = tex_wrap(v >> 16, T->h, T->hmask, T->pow2, T->rep_t);
    const uint8_t *p = T->data + (uint32_t)(y * T->w + x) * (uint32_t)T->bpp;
    o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
    o[3] = (T->bpp == 4) ? p[3] : 255;
}

AGL_INLINE void tex_bilinear(const tex_ref_t *T, int32_t u, int32_t v, int *o) {
    u -= 0x8000; v -= 0x8000;               /* sample at texel centres */
    int fx = (u >> 8) & 0xFF, fy = (v >> 8) & 0xFF;
    int x0 = tex_wrap(u >> 16, T->w, T->wmask, T->pow2, T->rep_s);
    int y0 = tex_wrap(v >> 16, T->h, T->hmask, T->pow2, T->rep_t);
    int x1 = tex_wrap((u >> 16) + 1, T->w, T->wmask, T->pow2, T->rep_s);
    int y1 = tex_wrap((v >> 16) + 1, T->h, T->hmask, T->pow2, T->rep_t);

    int bpp = T->bpp, w = T->w;
    const uint8_t *d = T->data;
    const uint8_t *p00 = d + (uint32_t)(y0 * w + x0) * (uint32_t)bpp;
    const uint8_t *p10 = d + (uint32_t)(y0 * w + x1) * (uint32_t)bpp;
    const uint8_t *p01 = d + (uint32_t)(y1 * w + x0) * (uint32_t)bpp;
    const uint8_t *p11 = d + (uint32_t)(y1 * w + x1) * (uint32_t)bpp;

    /* Weights are exact and sum to 65536, so one shift finishes the job. */
    int ifx = 256 - fx, ify = 256 - fy;
    int w00 = ifx * ify, w10 = fx * ify, w01 = ifx * fy, w11 = fx * fy;

    o[0] = (p00[0]*w00 + p10[0]*w10 + p01[0]*w01 + p11[0]*w11) >> 16;
    o[1] = (p00[1]*w00 + p10[1]*w10 + p01[1]*w01 + p11[1]*w11) >> 16;
    o[2] = (p00[2]*w00 + p10[2]*w10 + p01[2]*w01 + p11[2]*w11) >> 16;
    o[3] = (bpp == 4)
         ? ((p00[3]*w00 + p10[3]*w10 + p01[3]*w01 + p11[3]*w11) >> 16)
         : 255;
}

/* BLEND / FOG */

AGL_INLINE int agl_bfac(uint32_t f, int s, int d, int sa, int da) {
    switch (f) {
        case AGL_ZERO:                  return 0;
        case AGL_ONE:                   return 255;
        case AGL_SRC_COLOR:             return s;
        case AGL_ONE_MINUS_SRC_COLOR:   return 255 - s;
        case AGL_SRC_ALPHA:             return sa;
        case AGL_ONE_MINUS_SRC_ALPHA:   return 255 - sa;
        case AGL_DST_ALPHA:             return da;
        case AGL_ONE_MINUS_DST_ALPHA:   return 255 - da;
        case AGL_DST_COLOR:             return d;
        case AGL_ONE_MINUS_DST_COLOR:   return 255 - d;
        default:                        return sa;
    }
}

AGL_INLINE uint32_t agl_blend_px(const raster_ctx_t *C, uint32_t dst,
                                 int sr, int sg, int sb, int sa) {
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF;
    int db = dst & 0xFF, da = (dst >> 24) & 0xFF;

    /* The overwhelmingly common pair; skip the factor switches entirely. */
    if (C->bsrc == AGL_SRC_ALPHA && C->bdst == AGL_ONE_MINUS_SRC_ALPHA) {
        int ia = 255 - sa;
        int r = agl_div255(sr * sa + dr * ia);
        int g = agl_div255(sg * sa + dg * ia);
        int b = agl_div255(sb * sa + db * ia);
        int a = agl_div255(sa * sa + da * ia);
        return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    int r = agl_sat8(agl_div255(sr * agl_bfac(C->bsrc, sr, dr, sa, da)) +
                     agl_div255(dr * agl_bfac(C->bdst, sr, dr, sa, da)));
    int g = agl_sat8(agl_div255(sg * agl_bfac(C->bsrc, sg, dg, sa, da)) +
                     agl_div255(dg * agl_bfac(C->bdst, sg, dg, sa, da)));
    int b = agl_sat8(agl_div255(sb * agl_bfac(C->bsrc, sb, db, sa, da)) +
                     agl_div255(db * agl_bfac(C->bdst, sb, db, sa, da)));
    int a = agl_sat8(agl_div255(sa * agl_bfac(C->bsrc, sa, da, sa, da)) +
                     agl_div255(da * agl_bfac(C->bdst, sa, da, sa, da)));
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Fog blend factor as 0..256 (256 == no fog). */
static int agl_fog_factor(const raster_ctx_t *C, float z) {
    float ff;
    if (C->fog_mode == AGL_EXP) {
        ff = agl_expf(-C->fog_density * z);
    } else if (C->fog_mode == AGL_EXP2) {
        float fd = C->fog_density * z;
        ff = agl_expf(-fd * fd);
    } else {
        ff = (C->fog_end - z) * C->fog_range_inv;
    }
    int i = (int)(agl_clampf(ff, 0.0f, 1.0f) * 256.0f);
    return i > 256 ? 256 : (i < 0 ? 0 : i);
}

/* SPAN KERNELS
 *
 * One body; the flags are compile-time constants at every call site so each
 * wrapper collapses to exactly the code it needs. Perspective correction and
 * fog are exact at 16-pixel block boundaries and linear inside a block.
*/

#define AGL_SUBDIV 16

static const float agl_inv_tab[AGL_SUBDIV + 1] = {
    0.0f,
    1.0f/1.0f,  1.0f/2.0f,  1.0f/3.0f,  1.0f/4.0f,
    1.0f/5.0f,  1.0f/6.0f,  1.0f/7.0f,  1.0f/8.0f,
    1.0f/9.0f,  1.0f/10.0f, 1.0f/11.0f, 1.0f/12.0f,
    1.0f/13.0f, 1.0f/14.0f, 1.0f/15.0f, 1.0f/16.0f
};

/* Colour channel -> 8.16 fixed point, saturated. */
AGL_INLINE int32_t agl_c816(float c) {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255 << 16;
    return (int32_t)(c * 16711680.0f);      /* 255 * 65536 */
}

/* Texel coordinate -> 16.16, kept inside int32 so stepping cannot wrap. */
AGL_INLINE int32_t agl_tfx(float t) {
    if (t >= 30000.0f) return 30000 << 16;
    if (t <= -30000.0f) return -(30000 << 16);
    return (int32_t)(t * 65536.0f);
}

AGL_INLINE void agl_span_body(const raster_ctx_t *C, const span_t *S,
                              const int TEX, const int BILIN, const int BLEND,
                              const int FOG, const int DEPTH_GENERIC) {
    int x = S->xs;
    const int xe = S->xe;
    if (x > xe) return;

    uint32_t row = (uint32_t)S->y * (uint32_t)C->fbw;
    uint32_t *fb = C->fb + row;
    float *zb = C->zb ? C->zb + row : NULL;
    const int use_depth = C->depth_test && zb;
    const uint32_t dfunc = C->depth_func;
    const tex_ref_t *T = &C->tex;

    float z = S->z;
    const float dz = S->dz;
    float ow = S->ow;
    const float dow = S->dow;
    float uc = S->u, vc = S->v;
    const float du = S->du, dv = S->dv;
    float rc = S->r, gc = S->g, bc = S->b, ac = S->a;
    const float drc = S->dr, dgc = S->dg, dbc = S->db, dac = S->da;

    while (x <= xe) {
        int n = xe - x + 1;
        if (n > AGL_SUBDIV) n = AGL_SUBDIV;
        const float fn = (float)n;
        const float inv_n = agl_inv_tab[n];

        /* Exact endpoints of this block. */
        float w0 = agl_rcpf(ow);
        float ow1 = ow + dow * fn;
        float w1 = agl_rcpf(ow1);

        int32_t cr = agl_c816(rc * w0);
        int32_t cg = agl_c816(gc * w0);
        int32_t cb = agl_c816(bc * w0);
        int32_t ca = agl_c816(ac * w0);
        float rc1 = rc + drc * fn, gc1 = gc + dgc * fn;
        float bc1 = bc + dbc * fn, ac1 = ac + dac * fn;
        int32_t dcr = (int32_t)((float)(agl_c816(rc1 * w1) - cr) * inv_n);
        int32_t dcg = (int32_t)((float)(agl_c816(gc1 * w1) - cg) * inv_n);
        int32_t dcb = (int32_t)((float)(agl_c816(bc1 * w1) - cb) * inv_n);
        int32_t dca = (int32_t)((float)(agl_c816(ac1 * w1) - ca) * inv_n);

        int32_t tu = 0, tv = 0, dtu = 0, dtv = 0;
        float uc1 = 0.0f, vc1 = 0.0f;
        if (TEX) {
            uc1 = uc + du * fn;
            vc1 = vc + dv * fn;
            tu = agl_tfx(uc * w0 * T->wfx);
            tv = agl_tfx(vc * w0 * T->hfx);
            dtu = (int32_t)((float)(agl_tfx(uc1 * w1 * T->wfx) - tu) * inv_n);
            dtv = (int32_t)((float)(agl_tfx(vc1 * w1 * T->hfx) - tv) * inv_n);
        }

        int fogf = 256, dfog = 0;
        if (FOG && C->fog) {
            fogf = agl_fog_factor(C, z);
            dfog = (int)((float)(agl_fog_factor(C, z + dz * fn) - fogf) * inv_n);
        }

        for (int i = 0; i < n; i++, x++) {
            int pass = 1;
            if (use_depth) {
                float cur = zb[x];
                if (!DEPTH_GENERIC) {
                    pass = (z < cur);
                } else {
                    switch (dfunc) {
                        case AGL_NEVER:    pass = 0; break;
                        case AGL_LESS:     pass = z <  cur; break;
                        case AGL_EQUAL:    pass = z == cur; break;
                        case AGL_LEQUAL:   pass = z <= cur; break;
                        case AGL_GREATER:  pass = z >  cur; break;
                        case AGL_NOTEQUAL: pass = z != cur; break;
                        case AGL_GEQUAL:   pass = z >= cur; break;
                        default:           pass = 1; break;
                    }
                }
            }

            if (pass) {
                /* +0.5 LSB: truncation here costs a code value and turns a
                 * solid 1.0 channel into 254. */
                int sr = agl_sat8((cr + 0x8000) >> 16);
                int sg = agl_sat8((cg + 0x8000) >> 16);
                int sb = agl_sat8((cb + 0x8000) >> 16);
                int sa = agl_sat8((ca + 0x8000) >> 16);

                if (TEX) {
                    int t[4];
                    if (BILIN) tex_bilinear(T, tu, tv, t);
                    else       tex_nearest(T, tu, tv, t);
                    sr = agl_mul255(sr, t[0]);
                    sg = agl_mul255(sg, t[1]);
                    sb = agl_mul255(sb, t[2]);
                    sa = agl_mul255(sa, t[3]);
                }

                /*
                 * With standard source-alpha blending, a fully transparent texel
                 * contributes nothing and must not occlude later geometry.
                 */
                bool invisible = BLEND && C->blend &&
                                 C->bsrc == AGL_SRC_ALPHA &&
                                 C->bdst == AGL_ONE_MINUS_SRC_ALPHA &&
                                 sa == 0;

                if (use_depth && C->depth_write_enabled && !invisible) {
                    zb[x] = z;
                }

                if (FOG && C->fog) {
                    int inv = 256 - fogf;
                    sr = (sr * fogf + C->fog_r * inv) >> 8;
                    sg = (sg * fogf + C->fog_g * inv) >> 8;
                    sb = (sb * fogf + C->fog_b * inv) >> 8;
                }

                if (BLEND && C->blend) {
                    fb[x] = agl_blend_px(C, fb[x], sr, sg, sb, sa);
                } else {
                    fb[x] = ((uint32_t)sa << 24) |
                            ((uint32_t)sr << 16) |
                            ((uint32_t)sg << 8) |
                            (uint32_t)sb;
                }
            }

            z += dz;
            cr += dcr; cg += dcg; cb += dcb; ca += dca;
            if (TEX) { tu += dtu; tv += dtv; }
            if (FOG && C->fog) fogf += dfog;
        }

        ow = ow1;
        rc = rc1; gc = gc1; bc = bc1; ac = ac1;
        if (TEX) { uc = uc1; vc = vc1; }
    }
}

/* Opaque, depth-LESS, untextured. */
static void span_flat(const raster_ctx_t *C, const span_t *S) {
    agl_span_body(C, S, 0, 0, 0, 0, 0);
}
/* Opaque, depth-LESS, nearest-filtered texture. */
static void span_texn(const raster_ctx_t *C, const span_t *S) {
    agl_span_body(C, S, 1, 0, 0, 0, 0);
}
/* Opaque, depth-LESS, bilinear texture. The BlockForge default. */
static void span_texb(const raster_ctx_t *C, const span_t *S) {
    agl_span_body(C, S, 1, 1, 0, 0, 0);
}
/* Everything else: blending, fog, exotic depth functions. */
static void span_generic(const raster_ctx_t *C, const span_t *S) {
    if (C->tex.enabled) {
        if (C->tex.bilinear) agl_span_body(C, S, 1, 1, 1, 1, 1);
        else                 agl_span_body(C, S, 1, 0, 1, 1, 1);
    } else {
        agl_span_body(C, S, 0, 0, 1, 1, 1);
    }
}

/* RASTER CONTEXT BUILD */

static void build_raster_ctx(void) {
    raster_ctx_t *C = &g_rc;
    agl_memset(C, 0, sizeof(*C));

    C->fb = g_state->framebuffer;
    C->zb = g_state->depth_buffer;
    C->fbw = g_state->fb_width;
    C->depth_test = g_state->depth_test_enabled ? 1 : 0;
    C->depth_write_enabled = g_state->depth_write_enabled ? 1 : 0;
    C->depth_func = g_state->depth_func;
    C->blend = g_state->blend_enabled ? 1 : 0;
    C->bsrc = g_state->blend_src;
    C->bdst = g_state->blend_dst;
    C->fog = g_state->fog_enabled ? 1 : 0;
    C->fog_mode = g_state->fog_mode;
    C->fog_density = g_state->fog_density;
    C->fog_start = g_state->fog_start;
    C->fog_end = g_state->fog_end;
    {
        float d = g_state->fog_end - g_state->fog_start;
        C->fog_range_inv = (agl_fabsf(d) < 1e-9f) ? 0.0f : 1.0f / d;
    }
    C->fog_r = agl_sat8((int)(agl_clampf(g_state->fog_color.r, 0, 1) * 255.0f + 0.5f));
    C->fog_g = agl_sat8((int)(agl_clampf(g_state->fog_color.g, 0, 1) * 255.0f + 0.5f));
    C->fog_b = agl_sat8((int)(agl_clampf(g_state->fog_color.b, 0, 1) * 255.0f + 0.5f));

    C->vp_x0 = agl_maxi(g_state->vp_x, 0);
    C->vp_y0 = agl_maxi(g_state->vp_y, 0);
    C->vp_x1 = agl_mini(g_state->vp_x + g_state->vp_width, g_state->fb_width) - 1;
    C->vp_y1 = agl_mini(g_state->vp_y + g_state->vp_height, g_state->fb_height) - 1;

    /* Resolve the bound texture exactly once, here, on one core. */
    if (g_state->texture_2d_enabled) {
        if (g_state->bound_slot < 0) refresh_bound_slot();
        if (g_state->bound_slot >= 0) {
            const agl_texture_t *t = &g_state->textures[g_state->bound_slot];
            if (t->data && t->width > 0 && t->height > 0) {
                C->tex.data = t->data;
                C->tex.w = t->width;
                C->tex.h = t->height;
                C->tex.bpp = (t->format == AGL_RGB) ? 3 : 4;
                C->tex.pow2 = is_pow2(t->width) && is_pow2(t->height);
                C->tex.wmask = t->width - 1;
                C->tex.hmask = t->height - 1;
                C->tex.rep_s = (t->wrap_s == AGL_REPEAT);
                C->tex.rep_t = (t->wrap_t == AGL_REPEAT);
                C->tex.wfx = (float)t->width;
                C->tex.hfx = (float)t->height;
                C->tex.bilinear = !(t->mag_filter == AGL_NEAREST &&
                                    t->min_filter == AGL_NEAREST);
                C->tex.enabled = 1;
            }
        }
    }

    int simple = !C->blend && !C->fog && C->depth_test &&
                 C->depth_func == AGL_LESS && C->zb;
    if (simple) {
        if (!C->tex.enabled) C->span = span_flat;
        else C->span = C->tex.bilinear ? span_texb : span_texn;
    } else {
        C->span = span_generic;
    }
}

/* TRIANGLE SETUP AND SCAN CONVERSION */

typedef struct { float base, gx, gy; } grad_t;

/* A(x,y) = base + gx*(x - x0) + gy*(y - y0) */
AGL_INLINE grad_t make_grad(float a0, float a1, float a2,
                            float dx1, float dy1, float dx2, float dy2, float ia) {
    grad_t g;
    float p = a1 - a0, q = a2 - a0;
    g.base = a0;
    g.gx = (p * dy2 - q * dy1) * ia;
    g.gy = (q * dx1 - p * dx2) * ia;
    return g;
}

AGL_INLINE float grad_at(const grad_t *g, float fx, float fy) {
    return g->base + g->gx * fx + g->gy * fy;
}

static void raster_tri(const rtri_t *T, int y_lo, int y_hi) {
    const raster_ctx_t *C = &g_rc;
    if (!C->fb) return;

    /* Band and viewport reject before any setup work at all. */
    int ytop = agl_maxi(agl_maxi(y_lo, C->vp_y0), agl_ceili(T->ymin));
    int ybot = agl_mini(agl_mini(y_hi, C->vp_y1), agl_ceili(T->ymax) - 1);
    if (ytop > ybot) return;

    const rvert_t *v0 = &T->v[0], *v1 = &T->v[1], *v2 = &T->v[2];
    float x0 = v0->x, y0 = v0->y;
    float dx1 = v1->x - x0, dy1 = v1->y - y0;
    float dx2 = v2->x - x0, dy2 = v2->y - y0;
    float area = dx1 * dy2 - dx2 * dy1;
    if (agl_fabsf(area) < 1e-6f) return;
    float ia = 1.0f / area;

    grad_t gz  = make_grad(v0->z,  v1->z,  v2->z,  dx1, dy1, dx2, dy2, ia);
    grad_t gow = make_grad(v0->ow, v1->ow, v2->ow, dx1, dy1, dx2, dy2, ia);
    grad_t gu  = make_grad(v0->u,  v1->u,  v2->u,  dx1, dy1, dx2, dy2, ia);
    grad_t gv  = make_grad(v0->v,  v1->v,  v2->v,  dx1, dy1, dx2, dy2, ia);
    grad_t gr  = make_grad(v0->r,  v1->r,  v2->r,  dx1, dy1, dx2, dy2, ia);
    grad_t gg  = make_grad(v0->g,  v1->g,  v2->g,  dx1, dy1, dx2, dy2, ia);
    grad_t gb  = make_grad(v0->b,  v1->b,  v2->b,  dx1, dy1, dx2, dy2, ia);
    grad_t ga  = make_grad(v0->a,  v1->a,  v2->a,  dx1, dy1, dx2, dy2, ia);

    /* Sort by y. */
    int i0 = 0, i1 = 1, i2 = 2, t;
    if (T->v[i0].y > T->v[i1].y) { t = i0; i0 = i1; i1 = t; }
    if (T->v[i1].y > T->v[i2].y) { t = i1; i1 = i2; i2 = t; }
    if (T->v[i0].y > T->v[i1].y) { t = i0; i0 = i1; i1 = t; }

    float yA = T->v[i0].y, yB = T->v[i1].y, yC = T->v[i2].y;
    float xA = T->v[i0].x, xB = T->v[i1].x, xC = T->v[i2].x;

    float hAC = yC - yA, hAB = yB - yA, hBC = yC - yB;
    float mAC = (agl_fabsf(hAC) < 1e-6f) ? 0.0f : (xC - xA) / hAC;
    float mAB = (agl_fabsf(hAB) < 1e-6f) ? 0.0f : (xB - xA) / hAB;
    float mBC = (agl_fabsf(hBC) < 1e-6f) ? 0.0f : (xC - xB) / hBC;

    /* Half-open in y: the middle scanline belongs to the lower half only. */
    int split = agl_ceili(yB);
    const float xlim_lo = (float)C->vp_x0;
    const float xlim_hi = (float)C->vp_x1 + 1.0f;

    span_t S;
    S.dz  = gz.gx;
    S.dow = gow.gx;
    S.du  = gu.gx;  S.dv = gv.gx;
    S.dr  = gr.gx;  S.dg = gg.gx;
    S.db  = gb.gx;  S.da = ga.gx;

    for (int y = ytop; y <= ybot; y++) {
        float fy = (float)y;
        float ea = xA + (fy - yA) * mAC;                  /* long edge */
        float eb = (y < split) ? xA + (fy - yA) * mAB     /* short upper */
                               : xB + (fy - yB) * mBC;    /* short lower */
        float xl = ea < eb ? ea : eb;
        float xr = ea < eb ? eb : ea;

        /* Clamp in float first: the int conversion below would otherwise be
         * undefined for a vertex that sits just in front of the near plane. */
        if (xl < xlim_lo) xl = xlim_lo;
        if (xr > xlim_hi) xr = xlim_hi;
        if (xr <= xl) continue;

        int xs = agl_ceili(xl);
        int xe = agl_ceili(xr) - 1;
        if (xs < C->vp_x0) xs = C->vp_x0;
        if (xe > C->vp_x1) xe = C->vp_x1;
        if (xs > xe) continue;

        float fx = (float)xs - x0;
        float fyr = fy - y0;

        S.y = y; S.xs = xs; S.xe = xe;
        S.z  = grad_at(&gz,  fx, fyr);
        S.ow = grad_at(&gow, fx, fyr);
        S.u  = grad_at(&gu,  fx, fyr);
        S.v  = grad_at(&gv,  fx, fyr);
        S.r  = grad_at(&gr,  fx, fyr);
        S.g  = grad_at(&gg,  fx, fyr);
        S.b  = grad_at(&gb,  fx, fyr);
        S.a  = grad_at(&ga,  fx, fyr);

        C->span(C, &S);
    }
}

/* BATCH: project + cull + store, then rasterize in bands */

static void agl_tri_band(int band, int nbands, void *ctx);

static void flush_tris(void) {
    int n = g_state->batch_count;
    if (!n) return;
    int nb = smp_cpu_count();
    if (nb > 1 && n >= AGL_PARALLEL_MIN_TRIS) {
        smp_parallel_bands(agl_tri_band, NULL, nb);
    } else {
        int lo = g_rc.vp_y0, hi = g_rc.vp_y1;
        for (int i = 0; i < n; i++) raster_tri(&g_state->batch[i], lo, hi);
    }
    g_state->batch_count = 0;
}

static void agl_band_rows(int band, int nbands, int *lo, int *hi) {
    int y0 = g_state->vp_y, h = g_state->vp_height;
    *lo = y0 + (h * band) / nbands;
    *hi = y0 + (h * (band + 1)) / nbands - 1;
}

static void agl_tri_band(int band, int nbands, void *ctx) {
    (void)ctx;
    int lo, hi;
    agl_band_rows(band, nbands, &lo, &hi);
    int n = g_state->batch_count;
    const rtri_t *b = g_state->batch;
    for (int i = 0; i < n; i++) raster_tri(&b[i], lo, hi);
}

/* Project one clipped triangle, cull it, and append it to the batch. */
static void emit_tri(const agl_vertex_t *a, const agl_vertex_t *b, const agl_vertex_t *c) {
    if (a->pw <= 1e-7f || b->pw <= 1e-7f || c->pw <= 1e-7f) return;

    const agl_vertex_t *src[3] = { a, b, c };
    float half_w = 0.5f * (float)g_state->vp_width;
    float half_h = 0.5f * (float)g_state->vp_height;
    float ox = (float)g_state->vp_x, oy = (float)g_state->vp_y;

    rtri_t tri;
    float ymin = 3.4e38f, ymax = -3.4e38f;

    for (int i = 0; i < 3; i++) {
        const agl_vertex_t *v = src[i];
        float ow = 1.0f / v->pw;
        rvert_t *o = &tri.v[i];
        o->x = (v->px * ow + 1.0f) * half_w + ox;
        o->y = (v->py * ow + 1.0f) * half_h + oy;
        o->z = v->pz * ow;
        o->ow = ow;
        o->u = v->u * ow;  o->v = v->v * ow;
        o->r = v->r * ow;  o->g = v->g * ow;
        o->b = v->b * ow;  o->a = v->a * ow;
        if (o->y < ymin) ymin = o->y;
        if (o->y > ymax) ymax = o->y;
    }

    if (g_state->cull_face_enabled) {
        if (g_state->cull_face_mode == AGL_FRONT_AND_BACK) return;
        float cross = (tri.v[1].x - tri.v[0].x) * (tri.v[2].y - tri.v[0].y) -
                      (tri.v[1].y - tri.v[0].y) * (tri.v[2].x - tri.v[0].x);
        if (g_state->cull_face_mode == AGL_BACK  && cross > 0.0f) return;
        if (g_state->cull_face_mode == AGL_FRONT && cross < 0.0f) return;
    }

    /* Whole-triangle viewport reject. */
    if (ymax < (float)g_rc.vp_y0 || ymin > (float)g_rc.vp_y1 + 1.0f) return;
    {
        float xmn = tri.v[0].x, xmx = tri.v[0].x;
        for (int i = 1; i < 3; i++) {
            if (tri.v[i].x < xmn) xmn = tri.v[i].x;
            if (tri.v[i].x > xmx) xmx = tri.v[i].x;
        }
        if (xmx < (float)g_rc.vp_x0 || xmn > (float)g_rc.vp_x1 + 1.0f) return;
    }

    tri.ymin = ymin;
    tri.ymax = ymax;

    if (g_state->batch_count >= AGL_MAX_BATCH_TRIS) flush_tris();
    g_state->batch[g_state->batch_count++] = tri;
}

/* Clip a triangle against the near plane and fan whatever survives. */
static void emit_clipped(const agl_vertex_t *in) {
    /* Fast path: entirely in front, no clipping work at all. */
    if (in[0].pz + in[0].pw >= 0.0f &&
        in[1].pz + in[1].pw >= 0.0f &&
        in[2].pz + in[2].pw >= 0.0f) {
        emit_tri(&in[0], &in[1], &in[2]);
        return;
    }
    if (in[0].pz + in[0].pw < 0.0f &&
        in[1].pz + in[1].pw < 0.0f &&
        in[2].pz + in[2].pw < 0.0f) return;

    agl_vertex_t clipped[8];
    int n = clip_polygon_near(in, 3, clipped);
    for (int j = 1; j + 1 < n; j++)
        emit_tri(&clipped[0], &clipped[j], &clipped[j + 1]);
}

/* POINTS AND LINES
 * These stay on the straightforward per-pixel path: they are a rounding
 * error of total pixel count and not worth a specialised kernel.
*/

static void write_pixel(int x, int y, float depth,
                        int sr, int sg, int sb, int sa, int y_lo, int y_hi) {
    const raster_ctx_t *C = &g_rc;
    if (x < C->vp_x0 || x > C->vp_x1) return;
    if (y < C->vp_y0 || y > C->vp_y1) return;
    if (y < y_lo || y > y_hi) return;

    uint32_t idx = (uint32_t)y * (uint32_t)C->fbw + (uint32_t)x;

    if (C->depth_test && C->zb) {
        float cur = C->zb[idx];
        int pass;
        switch (C->depth_func) {
            case AGL_NEVER:    pass = 0; break;
            case AGL_LESS:     pass = depth <  cur; break;
            case AGL_EQUAL:    pass = depth == cur; break;
            case AGL_LEQUAL:   pass = depth <= cur; break;
            case AGL_GREATER:  pass = depth >  cur; break;
            case AGL_NOTEQUAL: pass = depth != cur; break;
            case AGL_GEQUAL:   pass = depth >= cur; break;
            default:           pass = 1; break;
        }
        if (!pass) return;
        bool invisible = C->blend &&
                         C->bsrc == AGL_SRC_ALPHA &&
                         C->bdst == AGL_ONE_MINUS_SRC_ALPHA &&
                         sa == 0;
        if (C->depth_write_enabled && !invisible) {
            C->zb[idx] = depth;
        }
    }

    if (C->fog) {
        int f = agl_fog_factor(C, depth), inv = 256 - f;
        sr = (sr * f + C->fog_r * inv) >> 8;
        sg = (sg * f + C->fog_g * inv) >> 8;
        sb = (sb * f + C->fog_b * inv) >> 8;
    }

    if (C->blend) C->fb[idx] = agl_blend_px(C, C->fb[idx], sr, sg, sb, sa);
    else C->fb[idx] = ((uint32_t)sa << 24) | ((uint32_t)sr << 16) |
                      ((uint32_t)sg << 8)  | (uint32_t)sb;
}

AGL_INLINE void shade_fragment(float uf, float vf, float r, float g, float b, float a,
                               int *o) {
    int sr = agl_sat8((int)(agl_clampf(r, 0, 1) * 255.0f + 0.5f));
    int sg = agl_sat8((int)(agl_clampf(g, 0, 1) * 255.0f + 0.5f));
    int sb = agl_sat8((int)(agl_clampf(b, 0, 1) * 255.0f + 0.5f));
    int sa = agl_sat8((int)(agl_clampf(a, 0, 1) * 255.0f + 0.5f));
    if (g_rc.tex.enabled) {
        int t[4];
        int32_t tu = agl_tfx(uf * g_rc.tex.wfx);
        int32_t tv = agl_tfx(vf * g_rc.tex.hfx);
        if (g_rc.tex.bilinear) tex_bilinear(&g_rc.tex, tu, tv, t);
        else                   tex_nearest(&g_rc.tex, tu, tv, t);
        sr = agl_mul255(sr, t[0]); sg = agl_mul255(sg, t[1]);
        sb = agl_mul255(sb, t[2]); sa = agl_mul255(sa, t[3]);
    }
    o[0] = sr; o[1] = sg; o[2] = sb; o[3] = sa;
}

static void rasterize_line(const agl_vertex_t *v0, const agl_vertex_t *v1,
                           int y_lo, int y_hi) {
    if (v0->pw <= 1e-7f || v1->pw <= 1e-7f) return;
    float oow0 = 1.0f / v0->pw, oow1 = 1.0f / v1->pw;
    float half_w = 0.5f * (float)g_state->vp_width;
    float half_h = 0.5f * (float)g_state->vp_height;

    float x0 = (v0->px * oow0 + 1.0f) * half_w + (float)g_state->vp_x;
    float y0 = (v0->py * oow0 + 1.0f) * half_h + (float)g_state->vp_y;
    float x1 = (v1->px * oow1 + 1.0f) * half_w + (float)g_state->vp_x;
    float y1 = (v1->py * oow1 + 1.0f) * half_h + (float)g_state->vp_y;

    if (agl_fabsf(x0) > 1e6f || agl_fabsf(y0) > 1e6f ||
        agl_fabsf(x1) > 1e6f || agl_fabsf(y1) > 1e6f) return;

    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    int dx = ix1 > ix0 ? ix1 - ix0 : ix0 - ix1;
    int dy = iy1 > iy0 ? iy1 - iy0 : iy0 - iy1;
    int sx = ix0 < ix1 ? 1 : -1, sy = iy0 < iy1 ? 1 : -1;
    int err = dx - dy;
    int steps = dx > dy ? dx : dy;
    if (steps == 0) steps = 1;

    float inv_steps = 1.0f / (float)steps;
    float z = v0->pz * oow0, dz = (v1->pz * oow1 - z) * inv_steps;
    float cow = oow0, dow = (oow1 - oow0) * inv_steps;
    float uc = v0->u * oow0, duc = (v1->u * oow1 - uc) * inv_steps;
    float vc = v0->v * oow0, dvc = (v1->v * oow1 - vc) * inv_steps;
    float rc = v0->r * oow0, drc = (v1->r * oow1 - rc) * inv_steps;
    float gc = v0->g * oow0, dgc = (v1->g * oow1 - gc) * inv_steps;
    float bc = v0->b * oow0, dbc = (v1->b * oow1 - bc) * inv_steps;
    float ac = v0->a * oow0, dac = (v1->a * oow1 - ac) * inv_steps;

    int ix = ix0, iy = iy0;
    for (int i = 0; i <= steps; i++) {
        float w = agl_rcpf(cow);
        int px[4];
        shade_fragment(uc * w, vc * w, rc * w, gc * w, bc * w, ac * w, px);
        write_pixel(ix, iy, z, px[0], px[1], px[2], px[3], y_lo, y_hi);

        z += dz; cow += dow; uc += duc; vc += dvc;
        rc += drc; gc += dgc; bc += dbc; ac += dac;

        if (ix == ix1 && iy == iy1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; ix += sx; }
        if (e2 <  dx) { err += dx; iy += sy; }
    }
}

static void rasterize_point(const agl_vertex_t *v, int y_lo, int y_hi) {
    if (v->pw <= 1e-7f) return;
    float oow = 1.0f / v->pw;
    float half_w = 0.5f * (float)g_state->vp_width;
    float half_h = 0.5f * (float)g_state->vp_height;
    float x = (v->px * oow + 1.0f) * half_w + (float)g_state->vp_x;
    float y = (v->py * oow + 1.0f) * half_h + (float)g_state->vp_y;
    if (agl_fabsf(x) > 1e6f || agl_fabsf(y) > 1e6f) return;

    int px[4];
    shade_fragment(v->u, v->v, v->r, v->g, v->b, v->a, px);
    write_pixel((int)x, (int)y, v->pz * oow, px[0], px[1], px[2], px[3], y_lo, y_hi);
}

static void rasterize_thin(int y_lo, int y_hi) {
    int n = g_state->vertex_count;
    const agl_vertex_t *vb = g_state->vertex_buffer;
    switch (g_state->current_primitive) {
        case AGL_POINTS:
            for (int i = 0; i < n; i++) rasterize_point(&vb[i], y_lo, y_hi);
            break;
        case AGL_LINES:
            for (int i = 0; i + 1 < n; i += 2) rasterize_line(&vb[i], &vb[i+1], y_lo, y_hi);
            break;
        case AGL_LINE_STRIP:
            for (int i = 0; i + 1 < n; i++) rasterize_line(&vb[i], &vb[i+1], y_lo, y_hi);
            break;
        default: break;
    }
}

static void agl_thin_band(int band, int nbands, void *ctx) {
    (void)ctx;
    int lo, hi;
    agl_band_rows(band, nbands, &lo, &hi);
    rasterize_thin(lo, hi);
}

/* aglEnd */

void aglEnd(void) {
    if (!g_state || !g_state->in_begin_end) return;
    g_state->in_begin_end = false;

    int n = g_state->vertex_count;
    g_state->vertex_count = 0;
    if (n == 0 || !g_state->framebuffer) return;
    if (g_state->vp_width <= 0 || g_state->vp_height <= 0) return;

    g_state->vertex_count = n;
    transform_vertices();

    /* Whole-batch near-plane reject. */
    {
        bool any = false;
        for (int i = 0; i < n; i++) {
            if (g_state->vertex_buffer[i].pz + g_state->vertex_buffer[i].pw >= 0.0f) {
                any = true;
                break;
            }
        }
        if (!any) { g_state->vertex_count = 0; return; }
    }

    build_raster_ctx();
    if (g_rc.vp_x1 < g_rc.vp_x0 || g_rc.vp_y1 < g_rc.vp_y0) {
        g_state->vertex_count = 0;
        return;
    }

    const agl_vertex_t *vb = g_state->vertex_buffer;
    agl_vertex_t in[3];

    switch (g_state->current_primitive) {
        case AGL_POINTS:
        case AGL_LINES:
        case AGL_LINE_STRIP: {
            int nb = smp_cpu_count();
            if (nb > 1 && n >= AGL_PARALLEL_MIN_VERTS)
                smp_parallel_bands(agl_thin_band, NULL, nb);
            else
                rasterize_thin(g_rc.vp_y0, g_rc.vp_y1);
            g_state->vertex_count = 0;
            return;
        }
        case AGL_TRIANGLES:
            for (int i = 0; i + 2 < n; i += 3) {
                in[0] = vb[i]; in[1] = vb[i+1]; in[2] = vb[i+2];
                emit_clipped(in);
            }
            break;
        case AGL_TRIANGLE_STRIP:
            for (int i = 0; i + 2 < n; i++) {
                in[0] = vb[i];
                if (i & 1) { in[1] = vb[i+2]; in[2] = vb[i+1]; }
                else       { in[1] = vb[i+1]; in[2] = vb[i+2]; }
                emit_clipped(in);
            }
            break;
        case AGL_TRIANGLE_FAN:
            for (int i = 1; i + 1 < n; i++) {
                in[0] = vb[0]; in[1] = vb[i]; in[2] = vb[i+1];
                emit_clipped(in);
            }
            break;
        case AGL_QUADS:
            for (int i = 0; i + 3 < n; i += 4) {
                in[0] = vb[i]; in[1] = vb[i+1]; in[2] = vb[i+2];
                emit_clipped(in);
                in[0] = vb[i]; in[1] = vb[i+2]; in[2] = vb[i+3];
                emit_clipped(in);
            }
            break;
        default: break;
    }

    flush_tris();
    g_state->vertex_count = 0;
}

/* LIGHTING / MATERIAL / TEXTURE API */

static void refresh_light_summary(void) {
    g_state->any_light_enabled = false;
    for (int i = 0; i < MAX_LIGHTS; i++)
        if (g_state->lights[i].enabled) { g_state->any_light_enabled = true; return; }
}

void aglLightfv(uint32_t light, uint32_t pname, const float *p) {
    if (!g_state || !p) return;
    int idx;
    switch (light) {
        case AGL_LIGHT0: idx = 0; break; case AGL_LIGHT1: idx = 1; break;
        case AGL_LIGHT2: idx = 2; break; case AGL_LIGHT3: idx = 3; break;
        default: g_state->last_error = AGL_INVALID_ENUM; return;
    }
    agl_light_t *l = &g_state->lights[idx];
    switch (pname) {
        case AGL_AMBIENT:  l->ambient  = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_DIFFUSE:  l->diffuse  = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_SPECULAR: l->specular = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_POSITION:
            l->position = (agl_vec4_t){p[0],p[1],p[2],p[3]};
            l->enabled = true;
            refresh_light_summary();
            break;
        default: g_state->last_error = AGL_INVALID_ENUM;
    }
}

void aglMaterialfv(uint32_t face, uint32_t pname, const float *p) {
    if (!g_state || !p) return;
    (void)face;
    agl_material_t *m = &g_state->material;
    switch (pname) {
        case AGL_AMBIENT:  m->ambient  = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_DIFFUSE:  m->diffuse  = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_SPECULAR: m->specular = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_EMISSION: m->emission = (agl_color_t){p[0],p[1],p[2],p[3]}; break;
        case AGL_AMBIENT_AND_DIFFUSE:
            m->ambient = (agl_color_t){p[0],p[1],p[2],p[3]};
            m->diffuse = (agl_color_t){p[0],p[1],p[2],p[3]};
            break;
        default: g_state->last_error = AGL_INVALID_ENUM;
    }
}

void aglMaterialf(uint32_t face, uint32_t pname, float param) {
    if (!g_state) return;
    (void)face;
    if (pname == AGL_SHININESS) g_state->material.shininess = param;
    else g_state->last_error = AGL_INVALID_ENUM;
}

void aglGenTextures(int n, uint32_t *textures) {
    if (!g_state || !textures || n <= 0) return;
    for (int i = 0; i < n; i++) {
        /* Only hand out an id if a slot could actually hold it. */
        int free_slots = 0;
        for (int j = 0; j < MAX_TEXTURES; j++)
            if (g_state->textures[j].id == 0) { free_slots = 1; break; }
        if (!free_slots) {
            g_state->last_error = AGL_OUT_OF_MEMORY;
            textures[i] = 0;
            continue;
        }
        textures[i] = g_state->next_texture_id++;
        /* Reserve the slot immediately so TexParameter before TexImage2D
         * has somewhere to land. */
        for (int j = 0; j < MAX_TEXTURES; j++) {
            if (g_state->textures[j].id == 0) {
                tex_slot_defaults(&g_state->textures[j]);
                g_state->textures[j].id = textures[i];
                g_state->textures[j].data = NULL;
                g_state->textures[j].width = 0;
                g_state->textures[j].height = 0;
                break;
            }
        }
    }
}

void aglDeleteTextures(int n, const uint32_t *textures) {
    if (!g_state || !textures) return;
    for (int i = 0; i < n; i++) {
        if (textures[i] == 0) continue;
        for (int j = 0; j < MAX_TEXTURES; j++) {
            if (g_state->textures[j].id == textures[i]) {
                if (g_state->textures[j].data) kfree(g_state->textures[j].data);
                agl_memset(&g_state->textures[j], 0, sizeof(agl_texture_t));
                tex_slot_defaults(&g_state->textures[j]);
                if (g_state->bound_slot == j) g_state->bound_slot = -1;
                break;
            }
        }
    }
}

void aglBindTexture(uint32_t target, uint32_t texture) {
    if (!g_state) return;
    if (target != AGL_TEXTURE_2D) { g_state->last_error = AGL_INVALID_ENUM; return; }
    g_state->current_texture = texture;
    refresh_bound_slot();
    if (g_state->bound_slot < 0 && texture != 0) {
        /* Claim an empty slot now so filter/wrap set before the upload stick. */
        for (int i = 0; i < MAX_TEXTURES; i++) {
            if (g_state->textures[i].id == 0) {
                tex_slot_defaults(&g_state->textures[i]);
                g_state->textures[i].id = texture;
                g_state->bound_slot = i;
                break;
            }
        }
    }
}

void aglTexImage2D(uint32_t target, int level, int internalformat,
                   int width, int height, int border,
                   uint32_t format, uint32_t type, const void *pixels) {
    if (!g_state) return;
    (void)level; (void)internalformat; (void)border; (void)type;
    if (target != AGL_TEXTURE_2D) { g_state->last_error = AGL_INVALID_ENUM; return; }
    if (width <= 0 || height <= 0) { g_state->last_error = AGL_INVALID_ENUM; return; }
    if ((uint32_t)width > 0x0FFFFFFFu / (uint32_t)height) {
        g_state->last_error = AGL_OUT_OF_MEMORY;
        return;
    }

    int slot = g_state->bound_slot;
    if (slot < 0) {
        refresh_bound_slot();
        slot = g_state->bound_slot;
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_TEXTURES; i++)
            if (g_state->textures[i].id == 0) { slot = i; break; }
        if (slot >= 0) {
            tex_slot_defaults(&g_state->textures[slot]);
            g_state->textures[slot].id = g_state->current_texture;
            g_state->bound_slot = slot;
        }
    }
    if (slot < 0) { g_state->last_error = AGL_OUT_OF_MEMORY; return; }

    agl_texture_t *t = &g_state->textures[slot];
    int bpp = (format == AGL_RGB) ? 3 : 4;
    uint32_t bytes = (uint32_t)width * (uint32_t)height * (uint32_t)bpp;

    uint8_t *mem = (uint8_t *)kmalloc(bytes);
    if (!mem) { g_state->last_error = AGL_OUT_OF_MEMORY; return; }
    if (pixels) agl_memcpy(mem, pixels, bytes);
    else agl_memset(mem, 0, bytes);

    /* Only drop the old surface once the new one is secured. */
    if (t->data) kfree(t->data);
    t->data = mem;
    t->id = g_state->current_texture;
    t->width = width;
    t->height = height;
    t->format = format;
}

void aglTexParameteri(uint32_t target, uint32_t pname, int param) {
    if (!g_state) return;
    if (target != AGL_TEXTURE_2D) { g_state->last_error = AGL_INVALID_ENUM; return; }
    int slot = g_state->bound_slot;
    if (slot < 0) { refresh_bound_slot(); slot = g_state->bound_slot; }
    if (slot < 0) { g_state->last_error = AGL_INVALID_OPERATION; return; }
    agl_texture_t *t = &g_state->textures[slot];
    switch (pname) {
        case AGL_TEXTURE_MAG_FILTER: t->mag_filter = param; break;
        case AGL_TEXTURE_MIN_FILTER: t->min_filter = param; break;
        case AGL_TEXTURE_WRAP_S:     t->wrap_s = param; break;
        case AGL_TEXTURE_WRAP_T:     t->wrap_t = param; break;
        default: g_state->last_error = AGL_INVALID_ENUM;
    }
}

void aglFogf(uint32_t pname, float param) {
    if (!g_state) return;
    switch (pname) {
        case AGL_FOG_DENSITY: g_state->fog_density = param; break;
        case AGL_FOG_START:   g_state->fog_start = param; break;
        case AGL_FOG_END:     g_state->fog_end = param; break;
        case AGL_FOG_MODE:    g_state->fog_mode = (uint32_t)param; break;
        default: g_state->last_error = AGL_INVALID_ENUM;
    }
}

void aglFogfv(uint32_t pname, const float *p) {
    if (!g_state || !p) return;
    if (pname == AGL_FOG_COLOR) g_state->fog_color = (agl_color_t){p[0],p[1],p[2],p[3]};
    else g_state->last_error = AGL_INVALID_ENUM;
}

void aglFlush(void) {}
const char *aglGetString(uint32_t name) { (void)name; return "AurionGL 2.1"; }

void aglGetIntegerv(uint32_t pname, int *params) {
    if (!g_state || !params) return;
    switch (pname) {
        case AGL_MAX_TEXTURE_SIZE: *params = 1024; break;
        case AGL_MAX_LIGHTS: *params = MAX_LIGHTS; break;
        case AGL_MAX_MODELVIEW_STACK_DEPTH: *params = MAX_MATRIX_STACK; break;
        case AGL_MAX_PROJECTION_STACK_DEPTH: *params = MAX_MATRIX_STACK; break;
        case AGL_MAX_TEXTURE_STACK_DEPTH: *params = MAX_MATRIX_STACK; break;
        default: *params = 0;
    }
}

void aglGetFloatv(uint32_t pname, float *params) {
    if (!g_state || !params) return;
    switch (pname) {
        case AGL_MODELVIEW_MATRIX:  agl_memcpy(params, g_state->modelview_matrix.m,  16*sizeof(float)); break;
        case AGL_PROJECTION_MATRIX: agl_memcpy(params, g_state->projection_matrix.m, 16*sizeof(float)); break;
        case AGL_TEXTURE_MATRIX:    agl_memcpy(params, g_state->texture_matrix.m,    16*sizeof(float)); break;
        default: agl_memset(params, 0, 16*sizeof(float));
    }
}

/* UTILITY DRAWING */

void aglDrawCube(void) {
    static const float N[6][3] = {
        { 0, 0, 1}, { 0, 0,-1}, { 0, 1, 0}, { 0,-1, 0}, { 1, 0, 0}, {-1, 0, 0}
    };
    /* x, y, z, u, v per corner, six faces. */
    static const float P[6][4][5] = {
        // holy moly
        {{-0.5f,-0.5f, 0.5f, 0,0}, { 0.5f,-0.5f, 0.5f, 1,0},
         { 0.5f, 0.5f, 0.5f, 1,1}, {-0.5f, 0.5f, 0.5f, 0,1}},
        {{-0.5f,-0.5f,-0.5f, 1,0}, {-0.5f, 0.5f,-0.5f, 1,1},
         { 0.5f, 0.5f,-0.5f, 0,1}, { 0.5f,-0.5f,-0.5f, 0,0}},
        {{-0.5f, 0.5f,-0.5f, 0,1}, {-0.5f, 0.5f, 0.5f, 0,0},
         { 0.5f, 0.5f, 0.5f, 1,0}, { 0.5f, 0.5f,-0.5f, 1,1}},
        {{-0.5f,-0.5f,-0.5f, 1,1}, { 0.5f,-0.5f,-0.5f, 0,1},
         { 0.5f,-0.5f, 0.5f, 0,0}, {-0.5f,-0.5f, 0.5f, 1,0}},
        {{ 0.5f,-0.5f,-0.5f, 1,0}, { 0.5f, 0.5f,-0.5f, 1,1},
         { 0.5f, 0.5f, 0.5f, 0,1}, { 0.5f,-0.5f, 0.5f, 0,0}},
        {{-0.5f,-0.5f,-0.5f, 0,0}, {-0.5f,-0.5f, 0.5f, 1,0},
         {-0.5f, 0.5f, 0.5f, 1,1}, {-0.5f, 0.5f,-0.5f, 0,1}}
    };
    if (!g_state) return;
    agl_color_t c = g_state->current_color;
    agl_qv_t q[4];
    aglBegin(AGL_QUADS);
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            q[k].x = P[f][k][0]; q[k].y = P[f][k][1]; q[k].z = P[f][k][2];
            q[k].u = P[f][k][3]; q[k].v = P[f][k][4];
            q[k].r = c.r; q[k].g = c.g; q[k].b = c.b; q[k].a = c.a;
        }
        aglSubmitQuad(N[f][0], N[f][1], N[f][2], q);
    }
    aglEnd();
}

void aglDrawSphere(float radius, int slices, int stacks) {
    if (slices < 3 || stacks < 2 || radius <= 0.0f) return;
    const float PI = 3.141592653589793f;
    float inv_r = 1.0f / radius;
    float inv_sl = 1.0f / (float)slices, inv_st = 1.0f / (float)stacks;

    for (int i = 0; i < stacks; i++) {
        float s0, c0, s1, c1;
        agl_sincosf(PI * (-0.5f + (float)i * inv_st), &s0, &c0);
        agl_sincosf(PI * (-0.5f + (float)(i + 1) * inv_st), &s1, &c1);
        float z0 = s0 * radius, r0 = c0 * radius;
        float z1 = s1 * radius, r1 = c1 * radius;
        float t0 = (float)i * inv_st, t1 = (float)(i + 1) * inv_st;

        aglBegin(AGL_TRIANGLE_STRIP);
        for (int j = 0; j <= slices; j++) {
            float sj, cj;
            agl_sincosf(2.0f * PI * (float)j * inv_sl, &sj, &cj);
            float u = (float)j * inv_sl;
            aglNormal3f(cj * r0 * inv_r, sj * r0 * inv_r, z0 * inv_r);
            aglTexCoord2f(u, t0);
            aglVertex3f(cj * r0, sj * r0, z0);
            aglNormal3f(cj * r1 * inv_r, sj * r1 * inv_r, z1 * inv_r);
            aglTexCoord2f(u, t1);
            aglVertex3f(cj * r1, sj * r1, z1);
        }
        aglEnd();
    }
}

void aglDrawCylinder(float radius, float height, int slices) {
    if (slices < 3) return;
    const float PI = 3.141592653589793f;
    float hh = height * 0.5f;
    float inv_sl = 1.0f / (float)slices;

    aglBegin(AGL_TRIANGLE_STRIP);
    for (int i = 0; i <= slices; i++) {
        float s, c;
        agl_sincosf(2.0f * PI * (float)i * inv_sl, &s, &c);
        float u = (float)i * inv_sl;
        aglNormal3f(c, s, 0);
        aglTexCoord2f(u, 0); aglVertex3f(c * radius, s * radius, -hh);
        aglTexCoord2f(u, 1); aglVertex3f(c * radius, s * radius,  hh);
    }
    aglEnd();

    aglBegin(AGL_TRIANGLE_FAN);
    aglNormal3f(0, 0, 1);
    aglTexCoord2f(0.5f, 0.5f); aglVertex3f(0, 0, hh);
    for (int i = 0; i <= slices; i++) {
        float s, c;
        agl_sincosf(2.0f * PI * (float)i * inv_sl, &s, &c);
        aglTexCoord2f(0.5f + 0.5f * c, 0.5f + 0.5f * s);
        aglVertex3f(c * radius, s * radius, hh);
    }
    aglEnd();

    aglBegin(AGL_TRIANGLE_FAN);
    aglNormal3f(0, 0, -1);
    aglTexCoord2f(0.5f, 0.5f); aglVertex3f(0, 0, -hh);
    for (int i = slices; i >= 0; i--) {
        float s, c;
        agl_sincosf(2.0f * PI * (float)i * inv_sl, &s, &c);
        aglTexCoord2f(0.5f + 0.5f * c, 0.5f + 0.5f * s);
        aglVertex3f(c * radius, s * radius, -hh);
    }
    aglEnd();
}

void aglDrawTorus(float innerRadius, float outerRadius, int sides, int rings) {
    if (sides < 3 || rings < 3) return;
    const float PI = 3.141592653589793f;
    float inv_rings = 1.0f / (float)rings, inv_sides = 1.0f / (float)sides;

    for (int i = 0; i < rings; i++) {
        /* 2.0 evaluated these six times per vertex inside the inner loop. */
        float s1, c1, s2, c2;
        agl_sincosf(2.0f * PI * (float)i * inv_rings, &s1, &c1);
        agl_sincosf(2.0f * PI * (float)(i + 1) * inv_rings, &s2, &c2);
        float t1 = (float)i * inv_rings, t2 = (float)(i + 1) * inv_rings;

        aglBegin(AGL_TRIANGLE_STRIP);
        for (int j = 0; j <= sides; j++) {
            float sp, cp;
            agl_sincosf(2.0f * PI * (float)j * inv_sides, &sp, &cp);
            float r = outerRadius + innerRadius * cp;
            float z = innerRadius * sp;
            float vtex = (float)j * inv_sides;
            aglNormal3f(c1 * cp, s1 * cp, sp);
            aglTexCoord2f(t1, vtex);
            aglVertex3f(r * c1, r * s1, z);
            aglNormal3f(c2 * cp, s2 * cp, sp);
            aglTexCoord2f(t2, vtex);
            aglVertex3f(r * c2, r * s2, z);
        }
        aglEnd();
    }
}