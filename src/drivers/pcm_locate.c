/* pcm_locate.c: RIFF walk. Fills rate/ch/bits + data range.
 * Bounds-checked, PCM-only (format tag 1), 8/16-bit. */

#include "pcm_engine.h"
#include "pcm_internal.h"

static uint32_t loc_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t loc_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool pcm_locate_wav(const uint8_t *hdr, int n, pcm_stream_t *out) {
    int p = 12;
    uint32_t byte_rate = 0;
    if (!hdr || n < 44 || !out)
        return false;
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F')
        return false;
    if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E')
        return false;
    out->data_off = 0;
    out->data_len = 0;
    while (p + 8 <= n) {
        uint32_t csz = loc_rd32(hdr + p + 4);
        uint32_t next;
        if (hdr[p] == 'f' && hdr[p + 1] == 'm' && hdr[p + 2] == 't' &&
            hdr[p + 3] == ' ') {
            uint16_t fmt;
            if (csz < 16 || p + 8 + 16 > n)
                return false;
            fmt = loc_rd16(hdr + p + 8);
            if (fmt != 1)
                return false;
            out->src_ch = (uint8_t)loc_rd16(hdr + p + 10);
            out->src_rate = loc_rd32(hdr + p + 12);
            byte_rate = loc_rd32(hdr + p + 16);
            out->src_bits = (uint8_t)loc_rd16(hdr + p + 22);
            if (out->src_ch == 0 || out->src_ch > 8)
                return false;
            if (out->src_rate < 4000u || out->src_rate > 192000u)
                return false;
            if (out->src_bits != 8 && out->src_bits != 16)
                return false;
            if (byte_rate == 0)
                return false;
        } else if (hdr[p] == 'd' && hdr[p + 1] == 'a' &&
                   hdr[p + 2] == 't' && hdr[p + 3] == 'a') {
            uint32_t at = (uint32_t)p + 8u;
            if (at < (uint32_t)p)
                return false;
            out->data_off = at;
            out->data_len = csz;
        }
        next = (uint32_t)p + 8u + csz + (csz & 1u);
        if (next < (uint32_t)p || next > (uint32_t)n)
            break;
        p = (int)next;
        if (out->data_len && byte_rate)
            break;
    }
    if (!out->data_len || !byte_rate)
        return false;
    return true;
}
