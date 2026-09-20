/* audio_init.c: init/shutdown/load/last-error. WAV header parsed
 * from the real file. MP3/OGG tagged, refused at play time. */
#include "mp3_player.h"
#include "audio_shared.h"
#include "../sb16.h"
#include "../pcm_engine.h"
#include "utils.h"
#include "portio.h"
extern void io_wait(void);
extern uint32_t get_ticks(void);
extern int load_file_content(const char *f, char *b, int m);
extern int fs_get_stored_content_bytes(const char *f);
#define HDR_MAX 512
void mp3_init(void) {
    uint32_t i;
    if (g_ainit)
        return;
    for (i = 0; i < sizeof(g_apb); i++)
        ((uint8_t *)&g_apb)[i] = 0;
    g_apb.volume = g_avol;
    g_apb.state = AUDIO_STATE_STOPPED;
    g_apaused = 0.0f;
    g_ainit = true;
    a_err("idle");
}
void mp3_shutdown(void) {
    mp3_stop();
    g_ainit = false;
}
const char *mp3_last_error(void) {
    return g_aerr;
}
bool mp3_load_file(const char *filename, AudioTrack *track) {
    uint32_t len;
    int stored;
    uint8_t hdr[HDR_MAX];
    if (!filename || !track)
        return false;
    for (len = 0; len < sizeof(AudioTrack); len++)
        ((uint8_t *)track)[len] = 0;
    len = a_strlen(filename);
    if (!len || len > sizeof(track->filename) - 1)
        return false;
    a_copy(track->filename, filename, sizeof(track->filename));
    stored = fs_get_stored_content_bytes(filename);
    if (stored < 0)
        return false;
    track->file_size = (uint32_t)stored;
    if (len > 4) {
        const char *ext = filename + len - 4;
        if (a_ext4(ext, ".wav")) {
            int n;
            track->format = AUDIO_FORMAT_WAV;
            n = load_file_content(filename, (char *)hdr, HDR_MAX);
            if (n > 0)
                a_wav_hdr(hdr, n, track);
        } else if (a_ext4(ext, ".mp3")) {
            track->format = AUDIO_FORMAT_MP3;
        } else if (a_ext4(ext, ".ogg")) {
            track->format = AUDIO_FORMAT_OGG;
        } else {
            track->format = AUDIO_FORMAT_UNKNOWN;
        }
    }
    return true;
}
