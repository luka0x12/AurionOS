/* audio_glue.c: single definition of transport state + helpers
 * and the RIFF header parse. */
#include "audio_shared.h"
AudioPlayback g_apb;
bool g_ainit = false;
float g_avol = 0.8f;
float g_apaused = 0.0f;
char g_aerr[64] = "idle";
char a_tolower(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}
bool a_ext4(const char *e, const char *w) {
    return a_tolower(e[0]) == a_tolower(w[0]) &&
        a_tolower(e[1]) == a_tolower(w[1]) &&
        a_tolower(e[2]) == a_tolower(w[2]) &&
        a_tolower(e[3]) == a_tolower(w[3]);
}
uint32_t a_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t a_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
uint32_t a_strlen(const char *s) {
    uint32_t n = 0;
    while (s && s[n])
        n++;
    return n;
}
void a_copy(char *d, const char *s, uint32_t cap) {
    uint32_t i = 0;
    if (!cap)
        return;
    while (i + 1 < cap && s && s[i]) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}
void a_err(const char *s) {
    uint32_t i = 0;
    while (s && s[i] && i + 1 < sizeof(g_aerr)) {
        g_aerr[i] = s[i];
        i++;
    }
    g_aerr[i] = 0;
}
bool a_wav_hdr(const uint8_t *h, int n, AudioTrack *t) {
    uint32_t br = 0, ds = 0, rate = 0;
    uint16_t ch = 0, bits = 0;
    int p;
    if (!h || n < 44 || !t)
        return false;
    if (h[0] != 'R' || h[1] != 'I' || h[2] != 'F' || h[3] != 'F')
        return false;
    if (h[8] != 'W' || h[9] != 'A' || h[10] != 'V' || h[11] != 'E')
        return false;
    p = 12;
    while (p + 8 <= n) {
        uint32_t csz = a_rd32(h + p + 4);
        uint32_t nx;
        if (h[p]=='f' && h[p+1]=='m' && h[p+2]=='t' && h[p+3]==' ') {
            if (csz < 16 || p + 8 + 16 > n)
                return false;
            if (a_rd16(h + p + 8) != 1)
                return false;
            ch = a_rd16(h + p + 10);
            rate = a_rd32(h + p + 12);
            br = a_rd32(h + p + 16);
            bits = a_rd16(h + p + 22);
            if (!ch || ch > 8)
                return false;
            if (rate < 4000 || rate > 192000)
                return false;
            if (bits != 8 && bits != 16)
                return false;
            if (!br)
                return false;
        } else if (h[p]=='d' && h[p+1]=='a' && h[p+2]=='t' && h[p+3]=='a') {
            ds = csz;
        }
        nx = (uint32_t)p + 8 + csz + (csz & 1);
        if (nx < (uint32_t)p || nx > (uint32_t)n)
            break;
        p = (int)nx;
        if (ds && br)
            break;
    }
    if (!br || !ds)
        return false;
    t->sample_rate = rate;
    t->channels = ch;
    t->bits_per_sample = bits;
    t->duration_ms = (uint32_t)(((uint64_t)ds * 1000) / br);
    return t->duration_ms > 0;
}
