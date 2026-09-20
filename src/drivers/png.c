/*
 * PNG Decoder Driver for Aurion OS
 * Full PNG decoder: 8/16-bit RGB/RGBA/gray/gray-alpha, 1/2/4/8-bit
 * indexed, all zlib/deflate block types (stored, fixed, dynamic Huffman).
 * Decodes into 32-bit ARGB pixel buffers for icons and wallpapers.
*/

#include <stdint.h>
#include <stdbool.h>
#include "security.h"
#include "png.h"

extern void c_puts(const char *s);
extern void *kmalloc(uint32_t size);
extern void kfree(void *ptr);
extern void gpu_draw_pixel(int x, int y, uint32_t color);
extern void gpu_fill_rect(int x, int y, int w, int h, uint32_t color);

#define PNG_MAX_DIMENSION 4096U
#define PNG_MAX_ALLOCATION (1024U * 1024U * 64U)

#define PNG_OK            0
#define PNG_ERR_BAD_SIG  -1
#define PNG_ERR_TRUNC    -2
#define PNG_ERR_BAD_IHDR -3
#define PNG_ERR_UNSUPP   -4
#define PNG_ERR_NOMEM    -5
#define PNG_ERR_INFLATE  -6
#define PNG_ERR_FILTER   -7
#define PNG_ERR_OVERFLOW -8
#define PNG_ERR_ADLER    -9

static void png_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void png_memset(void *dst, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Deflate bit reader: bits arrive LSB-first, buffered low-bit-first. */
typedef struct {
    const uint8_t *next;
    uint32_t left;
    uint32_t bitbuf;
    int bitcnt;
} InfState;

static void inf_refill(InfState *s) {
    while (s->bitcnt <= 24 && s->left) {
        s->bitbuf |= (uint32_t)(*s->next++) << s->bitcnt;
        s->left--;
        s->bitcnt += 8;
    }
}

static bool inf_need(InfState *s, int n) {
    inf_refill(s);
    return s->bitcnt >= n;
}

static uint32_t inf_take(InfState *s, int n) {
    uint32_t v;
    if (n == 0) return 0;
    v = s->bitbuf & (n >= 32 ? 0xFFFFFFFFU : (((uint32_t)1 << n) - 1U));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

/* Drop the bit buffer and return whole buffered bytes to the input, so the
   next read starts at the first byte after the last consumed bit. The
   refill-ahead reader may hold up to 4 unconsumed bytes in bitbuf. */
static void inf_align(InfState *s) {
    uint32_t unread = (uint32_t)(s->bitcnt >> 3);
    s->next -= unread;
    s->left += unread;
    s->bitbuf = 0;
    s->bitcnt = 0;
}

/* Canonical Huffman table: counts per length + symbols in code order. */
#define INF_MAXBITS  15
#define INF_MAXLCODES 288
#define INF_MAXDCODES 32

typedef struct {
    uint16_t count[INF_MAXBITS + 1];
    uint16_t symbol[INF_MAXLCODES];
} InfHuff;

static int inf_construct(InfHuff *h, const uint8_t *lengths, int n) {
    int sym, len, left;
    uint16_t offs[INF_MAXBITS + 1];
    for (len = 0; len <= INF_MAXBITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) {
        if (lengths[sym] > INF_MAXBITS) return -1;
        h->count[lengths[sym]]++;
    }
    if (h->count[0] == (uint16_t)n) return 0;
    left = 1;
    for (len = 1; len <= INF_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (len = 1; len < INF_MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (uint16_t)sym;
    return left;
}

static int inf_decode(InfState *s, const InfHuff *h) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= INF_MAXBITS; len++) {
        if (!inf_need(s, 1)) return -1;
        code |= (int)inf_take(s, 1);
        {
            int count = h->count[len];
            if (code - first < count)
                return h->symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }
    return -1;
}

static const uint16_t inf_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t inf_len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t inf_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t inf_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* Decode length/distance pairs with the given tables into out. */
static int inf_codes(InfState *s, const InfHuff *litlen, const InfHuff *dist,
                     uint8_t *out, uint32_t out_len, uint32_t *out_pos,
                     uint32_t *adler_s1, uint32_t *adler_s2) {
    uint32_t pos = *out_pos;
    uint32_t s1 = *adler_s1, s2 = *adler_s2;
    uint32_t hb_mark = pos;
    extern void sched_heartbeat(void);
    for (;;) {
        int sym = inf_decode(s, litlen);
        if (sym < 0) return PNG_ERR_INFLATE;
        /* A 1080p wallpaper inflates ~6MB a byte at a time; without a
         * heartbeat the 5s scheduler watchdog mistakes it for a hang. */
        if (pos - hb_mark >= 16384u) {
            hb_mark = pos;
            sched_heartbeat();
        }
        if (sym < 256) {
            uint8_t b = (uint8_t)sym;
            if (pos >= out_len) return PNG_ERR_INFLATE;
            out[pos++] = b;
            s1 += b; if (s1 >= 65521U) s1 -= 65521U;
            s2 += s1; if (s2 >= 65521U) s2 -= 65521U;
        } else if (sym == 256) {
            break;
        } else {
            int li = sym - 257;
            uint32_t length, distance;
            int di;
            if (li < 0 || li >= 29) return PNG_ERR_INFLATE;
            if (!inf_need(s, inf_len_extra[li])) return PNG_ERR_INFLATE;
            length = inf_len_base[li] + inf_take(s, inf_len_extra[li]);
            di = inf_decode(s, dist);
            if (di < 0 || di >= 30) return PNG_ERR_INFLATE;
            if (!inf_need(s, inf_dist_extra[di])) return PNG_ERR_INFLATE;
            distance = inf_dist_base[di] + inf_take(s, inf_dist_extra[di]);
            if (distance == 0 || distance > pos) return PNG_ERR_INFLATE;
            if (pos + length > out_len) return PNG_ERR_INFLATE;
            for (uint32_t k = 0; k < length; k++) {
                uint8_t b = out[pos - distance];
                out[pos++] = b;
                s1 += b; if (s1 >= 65521U) s1 -= 65521U;
                s2 += s1; if (s2 >= 65521U) s2 -= 65521U;
            }
        }
    }
    *out_pos = pos;
    *adler_s1 = s1;
    *adler_s2 = s2;
    return PNG_OK;
}

static int inf_fixed(InfState *s, uint8_t *out, uint32_t out_len,
                     uint32_t *out_pos, uint32_t *s1, uint32_t *s2) {
    uint8_t lens[INF_MAXLCODES];
    InfHuff litlen, dist;
    int i;
    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    if (inf_construct(&litlen, lens, 288) < 0) return PNG_ERR_INFLATE;
    for (i = 0; i < 30; i++) lens[i] = 5;
    if (inf_construct(&dist, lens, 30) < 0) return PNG_ERR_INFLATE;
    return inf_codes(s, &litlen, &dist, out, out_len, out_pos, s1, s2);
}

static const uint8_t inf_cl_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int inf_dynamic(InfState *s, uint8_t *out, uint32_t out_len,
                       uint32_t *out_pos, uint32_t *s1, uint32_t *s2) {
    uint8_t lens[INF_MAXLCODES + INF_MAXDCODES];
    InfHuff litlen, dist, clhuff;
    uint8_t cl_lens[19];
    int hlit, hdist, hclen, i, nlen, r;
    static InfHuff zero_huff;
    if (!inf_need(s, 14)) return PNG_ERR_INFLATE;
    hlit = (int)inf_take(s, 5) + 257;
    hdist = (int)inf_take(s, 5) + 1;
    hclen = (int)inf_take(s, 4) + 4;
    if (hlit > 286 || hdist > 30) return PNG_ERR_INFLATE;
    for (i = 0; i < 19; i++) cl_lens[i] = 0;
    for (i = 0; i < hclen; i++) {
        if (!inf_need(s, 3)) return PNG_ERR_INFLATE;
        cl_lens[inf_cl_order[i]] = (uint8_t)inf_take(s, 3);
    }
    if (inf_construct(&clhuff, cl_lens, 19) < 0) return PNG_ERR_INFLATE;
    nlen = hlit + hdist;
    i = 0;
    while (i < nlen) {
        int sym = inf_decode(s, &clhuff);
        if (sym < 0) return PNG_ERR_INFLATE;
        if (sym <= 15) {
            lens[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            int rep, prev;
            if (i == 0) return PNG_ERR_INFLATE;
            if (!inf_need(s, 2)) return PNG_ERR_INFLATE;
            rep = (int)inf_take(s, 2) + 3;
            prev = lens[i - 1];
            if (i + rep > nlen) return PNG_ERR_INFLATE;
            while (rep--) lens[i++] = (uint8_t)prev;
        } else if (sym == 17) {
            int rep;
            if (!inf_need(s, 3)) return PNG_ERR_INFLATE;
            rep = (int)inf_take(s, 3) + 3;
            if (i + rep > nlen) return PNG_ERR_INFLATE;
            while (rep--) lens[i++] = 0;
        } else {
            int rep;
            if (!inf_need(s, 7)) return PNG_ERR_INFLATE;
            rep = (int)inf_take(s, 7) + 11;
            if (i + rep > nlen) return PNG_ERR_INFLATE;
            while (rep--) lens[i++] = 0;
        }
    }
    if (i != nlen) return PNG_ERR_INFLATE;
    r = inf_construct(&litlen, lens, hlit);
    if (r < 0) return PNG_ERR_INFLATE;
    if (r > 0 && hlit != 1) {
        bool any = false;
        for (i = 0; i < hlit; i++) if (lens[i]) { any = true; break; }
        if (!any) return PNG_ERR_INFLATE;
    }
    r = inf_construct(&dist, lens + hlit, hdist);
    if (r < 0) return PNG_ERR_INFLATE;
    if (r > 0) {
        bool any = false;
        for (i = 0; i < hdist; i++) if (lens[hlit + i]) { any = true; break; }
        if (!any) {
            png_memset(&zero_huff, 0, sizeof(zero_huff));
            return inf_codes(s, &litlen, &zero_huff, out, out_len,
                             out_pos, s1, s2);
        }
    }
    return inf_codes(s, &litlen, &dist, out, out_len, out_pos, s1, s2);
}

/* Inflate a zlib stream into out. Returns bytes written or negative error. */
static int inflate_data(const uint8_t *compressed, uint32_t comp_len,
                        uint8_t *output, uint32_t out_max) {
    InfState s;
    uint32_t out_pos = 0;
    uint32_t adler_s1 = 1, adler_s2 = 0;
    int bfinal, rc;
    if (comp_len < 2) return PNG_ERR_INFLATE;
    if ((compressed[0] & 0x0F) != 8) return PNG_ERR_INFLATE;
    if ((((uint32_t)compressed[0] << 8) | compressed[1]) % 31U != 0)
        return PNG_ERR_INFLATE;
    s.next = compressed + 2;
    s.left = comp_len - 2;
    s.bitbuf = 0;
    s.bitcnt = 0;
    do {
        int btype;
        if (!inf_need(&s, 3)) return PNG_ERR_INFLATE;
        bfinal = (int)inf_take(&s, 1);
        btype = (int)inf_take(&s, 2);
        if (btype == 0) {
            uint16_t len, nlen;
            inf_align(&s);
            if (s.left < 4) return PNG_ERR_INFLATE;
            len = (uint16_t)(s.next[0] | ((uint16_t)s.next[1] << 8));
            nlen = (uint16_t)(s.next[2] | ((uint16_t)s.next[3] << 8));
            s.next += 4;
            s.left -= 4;
            if ((uint16_t)(len ^ nlen) != 0xFFFFU) return PNG_ERR_INFLATE;
            if (len > s.left) return PNG_ERR_INFLATE;
            if (out_pos + len > out_max) return PNG_ERR_INFLATE;
            for (uint16_t k = 0; k < len; k++) {
                uint8_t b = *s.next++;
                s.left--;
                output[out_pos++] = b;
                adler_s1 += b; if (adler_s1 >= 65521U) adler_s1 -= 65521U;
                adler_s2 += adler_s1; if (adler_s2 >= 65521U) adler_s2 -= 65521U;
            }
        } else if (btype == 1) {
            rc = inf_fixed(&s, output, out_max, &out_pos, &adler_s1, &adler_s2);
            if (rc) return rc;
        } else if (btype == 2) {
            rc = inf_dynamic(&s, output, out_max, &out_pos, &adler_s1, &adler_s2);
            if (rc) return rc;
        } else {
            return PNG_ERR_INFLATE;
        }
    } while (!bfinal);
    inf_align(&s);
    if (s.left < 4) return PNG_ERR_INFLATE;
    {
        uint32_t want = ((uint32_t)s.next[0] << 24) | ((uint32_t)s.next[1] << 16) |
                        ((uint32_t)s.next[2] << 8) | (uint32_t)s.next[3];
        uint32_t got = (adler_s2 << 16) | adler_s1;
        if (want != got) return PNG_ERR_ADLER;
    }
    return (int)out_pos;
}

/* PNG filter reconstruction, in place from filtered scanlines. */
static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int png_unfilter(uint8_t *raw, uint32_t height,
                        uint32_t stride, uint32_t bpp) {
    uint32_t raw_stride;
    if (!safe_add_u32(stride, 1, &raw_stride)) return PNG_ERR_OVERFLOW;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t filter = raw[y * raw_stride];
        uint8_t *scanline = raw + y * raw_stride + 1;
        uint8_t *prev = (y > 0) ? raw + (y - 1) * raw_stride + 1 : 0;
        if (filter > 4) return PNG_ERR_FILTER;
        for (uint32_t x = 0; x < stride; x++) {
            uint16_t v = scanline[x];
            uint8_t a = (x >= bpp) ? scanline[x - bpp] : 0;
            uint8_t b = prev ? prev[x] : 0;
            uint8_t c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (uint8_t)(((uint16_t)a + (uint16_t)b) >> 1); break;
                case 4: v += paeth_predictor(a, b, c); break;
                default: break;
            }
            scanline[x] = (uint8_t)(v & 0xFF);
        }
    }
    return PNG_OK;
}

/* PNG file signature. */
static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/* Validate IHDR payload, fill dims on success. */
static int png_parse_ihdr(const uint8_t *payload, uint32_t len,
                          uint32_t *w, uint32_t *h,
                          uint8_t *bit_depth, uint8_t *color_type) {
    uint32_t width, height;
    uint8_t bd, ct;
    if (len < 13) return PNG_ERR_BAD_IHDR;
    width = read_be32(payload);
    height = read_be32(payload + 4);
    bd = payload[8];
    ct = payload[9];
    if (payload[10] != 0 || payload[11] != 0) return PNG_ERR_UNSUPP;
    if (payload[12] != 0) return PNG_ERR_UNSUPP;
    if (width == 0 || height == 0) return PNG_ERR_BAD_IHDR;
    if (width > PNG_MAX_DIMENSION || height > PNG_MAX_DIMENSION)
        return PNG_ERR_BAD_IHDR;
    switch (ct) {
        case 0:
            if (bd != 1 && bd != 2 && bd != 4 && bd != 8 && bd != 16)
                return PNG_ERR_UNSUPP;
            break;
        case 2:
        case 4:
        case 6:
            if (bd != 8 && bd != 16) return PNG_ERR_UNSUPP;
            break;
        case 3:
            if (bd != 1 && bd != 2 && bd != 4 && bd != 8)
                return PNG_ERR_UNSUPP;
            break;
        default:
            return PNG_ERR_UNSUPP;
    }
    *w = width;
    *h = height;
    *bit_depth = bd;
    *color_type = ct;
    return PNG_OK;
}

/* Chunk cursor: reads header at pos, validates bounds. */
static bool png_chunk_at(const uint8_t *data, uint32_t data_len, uint32_t pos,
                         uint32_t *len, const uint8_t **type,
                         const uint8_t **payload) {
    uint64_t end;
    uint32_t chunk_len;
    if (pos + 12 > data_len) return false;
    chunk_len = read_be32(data + pos);
    if (chunk_len > data_len) return false;
    end = (uint64_t)pos + 12 + chunk_len;
    if (end > data_len) return false;
    *len = chunk_len;
    *type = data + pos + 4;
    *payload = data + pos + 8;
    return true;
}

static bool png_is(const uint8_t *t, char a, char b, char c, char d) {
    return t[0] == (uint8_t)a && t[1] == (uint8_t)b &&
           t[2] == (uint8_t)c && t[3] == (uint8_t)d;
}

int png_decode(const uint8_t *data, uint32_t data_len, PngImage *img) {
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0;
    uint8_t *idat_buf = 0;
    uint32_t idat_len = 0, idat_capacity = 0;
    uint8_t palette[256 * 3];
    uint8_t trns_alpha[256];
    uint16_t trns_gray = 0, trns_red = 0, trns_green = 0, trns_blue = 0;
    bool ihdr_found = false, plte_found = false;
    bool has_trns = false;
    uint32_t pos;
    uint32_t i;
    if (!data || !img) return PNG_ERR_BAD_SIG;
    img->width = 0;
    img->height = 0;
    img->pixels = 0;
    if (data_len < 8) return PNG_ERR_BAD_SIG;
    for (i = 0; i < 8; i++) {
        if (data[i] != png_sig[i]) return PNG_ERR_BAD_SIG;
    }
    png_memset(palette, 0, sizeof(palette));
    for (i = 0; i < 256; i++) trns_alpha[i] = 255;

    pos = 8;
    /* Pass 1: IHDR, palette, transparency, and the exact IDAT total so the
       concat buffer is allocated at file size, not worst-case size. */
    while (pos + 12 <= data_len) {
        uint32_t chunk_len;
        const uint8_t *chunk_type, *chunk_data;
        uint64_t chunk_end;
        if (!png_chunk_at(data, data_len, pos, &chunk_len, &chunk_type, &chunk_data))
            return PNG_ERR_TRUNC;
        chunk_end = (uint64_t)pos + 12 + chunk_len;
        if (png_is(chunk_type, 'I', 'H', 'D', 'R')) {
            int rc;
            if (ihdr_found) return PNG_ERR_BAD_IHDR;
            rc = png_parse_ihdr(chunk_data, chunk_len, &width, &height,
                                &bit_depth, &color_type);
            if (rc) return rc;
            ihdr_found = true;
        } else if (png_is(chunk_type, 'P', 'L', 'T', 'E')) {
            uint32_t n = chunk_len / 3;
            if (!ihdr_found) return PNG_ERR_BAD_IHDR;
            if (n > 256) n = 256;
            png_memcpy(palette, chunk_data, n * 3);
            plte_found = true;
        } else if (png_is(chunk_type, 't', 'R', 'N', 'S')) {
            if (!ihdr_found) return PNG_ERR_BAD_IHDR;
            has_trns = true;
            if (color_type == 3) {
                uint32_t n = chunk_len;
                if (n > 256) n = 256;
                for (i = 0; i < n; i++) trns_alpha[i] = chunk_data[i];
            } else if (color_type == 0) {
                if (chunk_len < 2) return PNG_ERR_TRUNC;
                trns_gray = read_be16(chunk_data);
            } else if (color_type == 2) {
                if (chunk_len < 6) return PNG_ERR_TRUNC;
                trns_red = read_be16(chunk_data);
                trns_green = read_be16(chunk_data + 2);
                trns_blue = read_be16(chunk_data + 4);
            }
        } else if (png_is(chunk_type, 'I', 'D', 'A', 'T')) {
            uint64_t total;
            if (!ihdr_found) return PNG_ERR_BAD_IHDR;
            total = (uint64_t)idat_len + chunk_len;
            if (total > PNG_MAX_ALLOCATION) return PNG_ERR_NOMEM;
            idat_len = (uint32_t)total;
        } else if (png_is(chunk_type, 'I', 'E', 'N', 'D')) {
            break;
        }
        pos = (uint32_t)chunk_end;
    }

    if (!ihdr_found || idat_len == 0) return PNG_ERR_TRUNC;
    if (color_type == 3 && !plte_found) return PNG_ERR_BAD_IHDR;

    idat_buf = (uint8_t *)kmalloc(idat_len);
    if (!idat_buf) return PNG_ERR_NOMEM;

    /* Pass 2: gather the IDAT payloads into the exact-size buffer. */
    pos = 8;
    idat_capacity = 0;
    while (pos + 12 <= data_len) {
        uint32_t chunk_len;
        const uint8_t *chunk_type, *chunk_data;
        uint64_t chunk_end;
        if (!png_chunk_at(data, data_len, pos, &chunk_len, &chunk_type, &chunk_data)) {
            kfree(idat_buf);
            return PNG_ERR_TRUNC;
        }
        chunk_end = (uint64_t)pos + 12 + chunk_len;
        if (png_is(chunk_type, 'I', 'D', 'A', 'T')) {
            if (idat_capacity + chunk_len < idat_capacity ||
                idat_capacity + chunk_len > idat_len) {
                kfree(idat_buf);
                return PNG_ERR_TRUNC;
            }
            png_memcpy(idat_buf + idat_capacity, chunk_data, chunk_len);
            idat_capacity += chunk_len;
        } else if (png_is(chunk_type, 'I', 'E', 'N', 'D')) {
            break;
        }
        pos = (uint32_t)chunk_end;
    }
    if (idat_capacity != idat_len) {
        kfree(idat_buf);
        return PNG_ERR_TRUNC;
    }

    {
        uint32_t channels, stride, raw_size, raw_stride;
        uint32_t pixel_count, argb_size;
        uint8_t *raw;
        int decompressed, rc;
        uint32_t *pixels;
        switch (color_type) {
            case 0: case 3: channels = 1; break;
            case 4: channels = 2; break;
            case 2: channels = 3; break;
            default: channels = 4; break;
        }
        {
            uint64_t bits = (uint64_t)width * channels * bit_depth;
            uint64_t rb = (bits + 7) / 8;
            if (rb == 0 || rb > 0xFFFFFFFU) { kfree(idat_buf); return PNG_ERR_OVERFLOW; }
            stride = (uint32_t)rb;
        }
        if (!safe_mul_u32(width, height, &pixel_count)) {
            kfree(idat_buf);
            return PNG_ERR_OVERFLOW;
        }
        if (!safe_mul_u32(pixel_count, 4, &argb_size)) {
            kfree(idat_buf);
            return PNG_ERR_OVERFLOW;
        }
        if (!safe_add_u32(stride, 1, &raw_stride) ||
            !safe_mul_u32(raw_stride, height, &raw_size)) {
            kfree(idat_buf);
            return PNG_ERR_OVERFLOW;
        }
        raw = (uint8_t *)kmalloc(raw_size);
        if (!raw) {
            kfree(idat_buf);
            return PNG_ERR_NOMEM;
        }
        decompressed = inflate_data(idat_buf, idat_len, raw, raw_size);
        kfree(idat_buf);
        idat_buf = 0;
        if (decompressed < 0) {
            kfree(raw);
            return decompressed;
        }
        if ((uint32_t)decompressed != raw_size) {
            kfree(raw);
            return PNG_ERR_INFLATE;
        }
        {
            /* Filter reconstruction works on packed bytes; the bpp unit is
               the filter byte distance: ceil(bit_depth*channels/8). */
            uint32_t fbytes = (uint32_t)(((uint64_t)bit_depth * channels + 7) / 8);
            if (fbytes == 0) fbytes = 1;
            rc = png_unfilter(raw, height, stride, fbytes);
            if (rc) {
                kfree(raw);
                return rc;
            }
        }
        pixels = (uint32_t *)kmalloc(argb_size);
        if (!pixels) {
            kfree(raw);
            return PNG_ERR_NOMEM;
        }
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t *row = raw + y * (stride + 1) + 1;
            uint32_t *dst = pixels + y * width;
            if (color_type == 3) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t idx;
                    if (bit_depth == 8) {
                        idx = row[x];
                    } else {
                        uint32_t bits = 8 / bit_depth;
                        uint32_t mask = (1U << bit_depth) - 1U;
                        uint32_t shift = (bits - 1 - (x % bits)) * bit_depth;
                        idx = (row[x / bits] >> shift) & mask;
                    }
                    {
                        uint8_t r = palette[idx * 3];
                        uint8_t g = palette[idx * 3 + 1];
                        uint8_t b = palette[idx * 3 + 2];
                        uint8_t a = has_trns ? trns_alpha[idx] : 255;
                        dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                 ((uint32_t)g << 8) | b;
                    }
                }
            } else if (color_type == 0) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t v;
                    if (bit_depth == 16) {
                        v = row[x * 2];
                        if (has_trns && read_be16(row + x * 2) == trns_gray)
                            dst[x] = (uint32_t)(v << 16) | (v << 8) | v;
                        else
                            dst[x] = 0xFF000000U | (v << 16) | (v << 8) | v;
                    } else if (bit_depth == 8) {
                        v = row[x];
                        if (has_trns && v == trns_gray)
                            dst[x] = (v << 16) | (v << 8) | v;
                        else
                            dst[x] = 0xFF000000U | (v << 16) | (v << 8) | v;
                    } else {
                        uint32_t bits = 8 / bit_depth;
                        uint32_t mask = (1U << bit_depth) - 1U;
                        uint32_t shift = (bits - 1 - (x % bits)) * bit_depth;
                        uint32_t s = (row[x / bits] >> shift) & mask;
                        v = (s * 255U) / mask;
                        if (has_trns && s == trns_gray)
                            dst[x] = (v << 16) | (v << 8) | v;
                        else
                            dst[x] = 0xFF000000U | (v << 16) | (v << 8) | v;
                    }
                }
            } else if (color_type == 2) {
                if (bit_depth == 8) {
                    for (uint32_t x = 0; x < width; x++) {
                        uint8_t r = row[x * 3], g = row[x * 3 + 1], b = row[x * 3 + 2];
                        uint32_t px = 0xFF000000U | ((uint32_t)r << 16) |
                                      ((uint32_t)g << 8) | b;
                        if (has_trns && r == trns_red && g == trns_green && b == trns_blue)
                            px &= 0x00FFFFFFU;
                        dst[x] = px;
                    }
                } else {
                    for (uint32_t x = 0; x < width; x++) {
                        uint8_t r = row[x * 6], g = row[x * 6 + 2], b = row[x * 6 + 4];
                        uint32_t px = 0xFF000000U | ((uint32_t)r << 16) |
                                      ((uint32_t)g << 8) | b;
                        if (has_trns &&
                            read_be16(row + x * 6) == trns_red &&
                            read_be16(row + x * 6 + 2) == trns_green &&
                            read_be16(row + x * 6 + 4) == trns_blue)
                            px &= 0x00FFFFFFU;
                        dst[x] = px;
                    }
                }
            } else if (color_type == 4) {
                if (bit_depth == 8) {
                    for (uint32_t x = 0; x < width; x++) {
                        uint8_t v = row[x * 2], a = row[x * 2 + 1];
                        dst[x] = ((uint32_t)a << 24) | ((uint32_t)v << 16) |
                                 ((uint32_t)v << 8) | v;
                    }
                } else {
                    for (uint32_t x = 0; x < width; x++) {
                        uint8_t v = row[x * 4], a = row[x * 4 + 2];
                        dst[x] = ((uint32_t)a << 24) | ((uint32_t)v << 16) |
                                 ((uint32_t)v << 8) | v;
                    }
                }
            } else {
                if (bit_depth == 8) {
                    for (uint32_t x = 0; x < width; x++) {
                        uint8_t r = row[x * 4], g = row[x * 4 + 1];
                        uint8_t b = row[x * 4 + 2], a = row[x * 4 + 3];
                        dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                 ((uint32_t)g << 8) | b;
                    }
                } else {
                    for (uint32_t x = 0; x < width; x++) {
                        uint8_t r = row[x * 8], g = row[x * 8 + 2];
                        uint8_t b = row[x * 8 + 4], a = row[x * 8 + 6];
                        dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                 ((uint32_t)g << 8) | b;
                    }
                }
            }
        }
        kfree(raw);
        img->width = width;
        img->height = height;
        img->pixels = pixels;
        return PNG_OK;
    }
}

void png_free(PngImage *img) {
    if (img && img->pixels) {
        kfree(img->pixels);
        img->pixels = 0;
    }
}

const char *png_error_string(int code) {
    switch (code) {
        case PNG_OK: return "ok";
        case PNG_ERR_BAD_SIG: return "bad signature";
        case PNG_ERR_TRUNC: return "truncated";
        case PNG_ERR_BAD_IHDR: return "bad IHDR";
        case PNG_ERR_UNSUPP: return "unsupported format";
        case PNG_ERR_NOMEM: return "out of memory";
        case PNG_ERR_INFLATE: return "inflate failed";
        case PNG_ERR_FILTER: return "bad filter";
        case PNG_ERR_OVERFLOW: return "size overflow";
        case PNG_ERR_ADLER: return "adler mismatch";
        default: return "unknown";
    }
}

void gpu_draw_icon(const uint32_t *pixels, int img_w, int img_h,
                   int dest_x, int dest_y, int dest_w, int dest_h) {
    int dx, dy;
    if (!pixels || img_w <= 0 || img_h <= 0 || dest_w <= 0 || dest_h <= 0)
        return;
    for (dy = 0; dy < dest_h; dy++) {
        int src_y = (dy * img_h) / dest_h;
        uint32_t sr, sg, sb, sa;
        if (src_y >= img_h) src_y = img_h - 1;
        for (dx = 0; dx < dest_w; dx++) {
            int src_x = (dx * img_w) / dest_w;
            uint32_t pixel;
            if (src_x >= img_w) src_x = img_w - 1;
            pixel = pixels[src_y * img_w + src_x];
            sa = (pixel >> 24) & 0xFF;
            if (sa < 128) continue;
            sr = (pixel >> 16) & 0xFF;
            sg = (pixel >> 8) & 0xFF;
            sb = pixel & 0xFF;
            /* Blend semi-transparent texels over the backdrop instead of
               snapping them opaque, so rounded icon edges anti-alias. */
            if (sa < 255) {
                uint32_t inv = 255 - sa;
                extern uint32_t *gpu_get_backbuffer(void);
                extern int gpu_get_width(void);
                extern int gpu_get_height(void);
                uint32_t *bb = gpu_get_backbuffer();
                int sw = gpu_get_width();
                int sh = gpu_get_height();
                int px = dest_x + dx, py = dest_y + dy;
                if (bb && px >= 0 && py >= 0 && px < sw && py < sh) {
                    uint32_t bg = bb[py * sw + px];
                    uint32_t br = (bg >> 16) & 0xFF;
                    uint32_t bgc = (bg >> 8) & 0xFF;
                    uint32_t bbc = bg & 0xFF;
                    sr = (sr * sa + br * inv) / 255;
                    sg = (sg * sa + bgc * inv) / 255;
                    sb = (sb * sa + bbc * inv) / 255;
                }
            }
            gpu_draw_pixel(dest_x + dx, dest_y + dy,
                           0xFF000000U | (sr << 16) | (sg << 8) | sb);
        }
    }
}

extern int wm_client_x(void *win);
extern int wm_client_y(void *win);
extern int wm_client_w(void *win);
extern int wm_client_h(void *win);
void wm_draw_icon(void *win, const uint32_t *pixels, int img_w, int img_h,
                  int x, int y, int w, int h) {
    int cx = wm_client_x(win), cy = wm_client_y(win);
    int cw = wm_client_w(win), ch = wm_client_h(win);
    int dx0 = cx + x, dy0 = cy + y;
    int dx1 = dx0 + w, dy1 = dy0 + h;
    if (dx0 < cx) dx0 = cx;
    if (dy0 < cy) dy0 = cy;
    if (dx1 > cx + cw) dx1 = cx + cw;
    if (dy1 > cy + ch) dy1 = cy + ch;
    int vis_w = dx1 - dx0, vis_h = dy1 - dy0;
    if (vis_w <= 0 || vis_h <= 0) return;
    for (int py = dy0; py < dy1; py++) {
        int src_y = ((py - dy0) * img_h) / h;
        if (src_y >= img_h) src_y = img_h - 1;
        for (int px = dx0; px < dx1; px++) {
            int src_x = ((px - dx0) * img_w) / w;
            if (src_x >= img_w) src_x = img_w - 1;
            uint32_t pixel = pixels[src_y * img_w + src_x];
            uint32_t sa = (pixel >> 24) & 0xFF;
            if (sa < 128) continue;
            uint32_t sr = (pixel >> 16) & 0xFF;
            uint32_t sg = (pixel >> 8) & 0xFF;
            uint32_t sb = pixel & 0xFF;
            if (sa < 255) {
                uint32_t inv = 255 - sa;
                extern uint32_t *gpu_get_backbuffer(void);
                extern int gpu_get_width(void);
                extern int gpu_get_height(void);
                uint32_t *bb = gpu_get_backbuffer();
                int sw = gpu_get_width(), sh = gpu_get_height();
                if (bb && px >= 0 && py >= 0 && px < sw && py < sh) {
                    uint32_t bg = bb[py * sw + px];
                    sr = (sr * sa + ((bg >> 16) & 0xFF) * inv) / 255;
                    sg = (sg * sa + ((bg >>  8) & 0xFF) * inv) / 255;
                    sb = (sb * sa + ((bg)       & 0xFF) * inv) / 255;
                }
            }
            gpu_draw_pixel(px, py,
                           0xFF000000U | (sr << 16) | (sg << 8) | sb);
        }
    }
}
