/* pcm_start.c: stream start. Validates, loads payload into RAM,
 * hands the first chunk to the SB16. Fail-closed status codes
*/

#include "pcm_engine.h"
#include "pcm_internal.h"
#include "sb16.h"
#include "portio.h"

extern void io_wait(void);
extern void serial_puts(const char *s);
extern int load_file_content(const char *fn, char *buf, int max);
extern int load_file_content_range(const char *fn, char *buf, uint32_t off, int max);
extern int fs_get_stored_content_bytes(const char *fn);
extern bool pcm_locate_wav(const uint8_t *hdr, int n, pcm_stream_t *out);
extern uint32_t pcm_feed_first(void);

static uint32_t start_strlen(const char *s) {
    uint32_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

pcm_status_t pcm_start(const char *filename) {
    uint8_t hdr[PCM_HDR_PROBE];
    int hn;
    int stored;
    uint32_t n;
    uint32_t i;
    uint32_t frame_bytes;
    pcm_stream_t probe;
    if (!filename || !filename[0])
        return PCM_ERR_NO_FILE;
    if (pcm_is_mp3_or_ogg_name(filename))
        return PCM_ERR_UNSUPPORTED;
    if (start_strlen(filename) >= PCM_NAME_MAX)
        return PCM_ERR_NO_FILE;
    stored = fs_get_stored_content_bytes(filename);
    if (stored < 44)
        return PCM_ERR_NO_FILE;
    if ((uint32_t)stored > PCM_HDR_PROBE + PCM_SRC_CAP)
        return PCM_ERR_TOO_LARGE;
    hn = load_file_content(filename, (char *)hdr, PCM_HDR_PROBE);
    if (hn < 44)
        return PCM_ERR_NO_FILE;
    for (i = 0; i < sizeof(probe); i++)
        ((uint8_t *)&probe)[i] = 0;
    if (!pcm_locate_wav(hdr, hn, &probe))
        return PCM_ERR_BAD_WAV;
    frame_bytes = (uint32_t)probe.src_ch * (probe.src_bits == 16 ? 2u : 1u);
    if (frame_bytes == 0 || probe.data_len / frame_bytes == 0)
        return PCM_ERR_BAD_WAV;
    if (probe.data_len > PCM_SRC_CAP)
        return PCM_ERR_TOO_LARGE;
    if (probe.data_off + probe.data_len > (uint32_t)stored)
        return PCM_ERR_BAD_WAV;
    n = (uint32_t)load_file_content_range(filename, (char *)g_pcm_src,
                                          probe.data_off,
                                          (int)probe.data_len);
    if (n != probe.data_len)
        return PCM_ERR_NO_FILE;
    if (!sb16_detect())
        return PCM_ERR_NO_DEVICE;
    pcm_stop();
    for (i = 0; i < sizeof(g_pcm); i++)
        ((uint8_t *)&g_pcm)[i] = 0;
    g_pcm = probe;
    g_pcm.total_frames = probe.data_len / frame_bytes;
    g_pcm.pos_fp = 0;
    g_pcm.done = 0;
    g_pcm.active = true;
    g_pcm.paused = false;
    g_pcm.backend_sb16 = true;
    for (i = 0; filename[i] && i + 1 < PCM_NAME_MAX; i++)
        g_pcm_name[i] = filename[i];
    g_pcm_name[i] = 0;
    sb16_set_volume(sb16_get_volume());
    n = pcm_feed_first();
    if (n == 0) {
        pcm_stop();
        return PCM_ERR_NO_DEVICE;
    }
    serial_puts("[PCM] playing via SB16\r\n");
    return PCM_OK;
}
