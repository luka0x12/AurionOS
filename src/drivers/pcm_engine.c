/* pcm_engine.c: WAV locate + stream start. Payload capped at 8192
 * bytes (FS store cap): data loads once into g_pcm_src, chunks
 * convert from RAM. SB16-only, refuses MP3/OGG and non-PCM WAV.
*/

#include "pcm_engine.h"
#include "pcm_internal.h"
#include "sb16.h"
#include "portio.h"

extern void io_wait(void);
extern uint32_t get_ticks(void);
extern void serial_puts(const char *s);
extern int load_file_content(const char *fn, char *buf, int max);
extern int load_file_content_range(const char *fn, char *buf, uint32_t off, int max);
extern int fs_get_stored_content_bytes(const char *fn);

pcm_stream_t g_pcm;
uint8_t g_pcm_src[PCM_SRC_CAP];
uint8_t g_pcm_convert_tmp[PCM_CONVERT_TMP];
char g_pcm_name[PCM_NAME_MAX];

extern uint32_t pcm_feed_first(void);

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static char pcm_tolower(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static uint32_t pcm_strlen(const char *s) {
    uint32_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

bool pcm_is_mp3_or_ogg_name(const char *filename) {
    const char *e;
    uint32_t n = pcm_strlen(filename);
    if (!filename)
        return false;
    if (n < 5 || filename[n - 4] != '.')
        return false;
    e = filename + n - 3;
    if (pcm_tolower(e[0]) == 'm' && pcm_tolower(e[1]) == 'p' &&
        pcm_tolower(e[2]) == '3')
        return true;
    if (pcm_tolower(e[0]) == 'o' && pcm_tolower(e[1]) == 'g' &&
        pcm_tolower(e[2]) == 'g')
        return true;
    return false;
}

const char *pcm_status_text(pcm_status_t st) {
    switch (st) {
    case PCM_OK: return "playing";
    case PCM_ERR_NO_FILE: return "file not found";
    case PCM_ERR_BAD_WAV: return "not PCM WAV";
    case PCM_ERR_NO_DEVICE: return "no SB16";
    case PCM_ERR_TOO_LARGE: return "file too large";
    case PCM_ERR_BUSY: return "busy";
    case PCM_ERR_UNSUPPORTED: return "needs decode";
    default: return "error";
    }
}
