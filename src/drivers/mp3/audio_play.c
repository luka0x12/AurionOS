/* audio_play.c: play/stop. Starts the PCM stream on the SB16 and
 * mirrors duration/position into the transport state. Refuses
 * MP3/OGG and non-PCM WAV with a reason, never fake-plays. */
#include "mp3_player.h"
#include "audio_shared.h"
#include "../sb16.h"
#include "../pcm_engine.h"
#include "utils.h"
#include "portio.h"
extern void io_wait(void);
extern uint32_t get_ticks(void);
bool mp3_play(const char *filename) {
    AudioTrack track;
    pcm_status_t st;
    uint32_t i;
    if (!filename) {
        a_err("no file");
        return false;
    }
    if (!g_ainit)
        mp3_init();
    for (i = 0; i < sizeof(track); i++)
        ((uint8_t *)&track)[i] = 0;
    if (!mp3_load_file(filename, &track)) {
        a_err("file not found");
        return false;
    }
    if (track.format == AUDIO_FORMAT_MP3 ||
        track.format == AUDIO_FORMAT_OGG) {
        a_err("needs decode");
        return false;
    }
    if (track.format != AUDIO_FORMAT_WAV || !track.duration_ms) {
        a_err("not PCM WAV");
        return false;
    }
    st = pcm_start(filename);
    if (st != PCM_OK) {
        a_err(pcm_status_text(st));
        return false;
    }
    pcm_set_paused(false);
    g_apb.track = track;
    g_apb.state = AUDIO_STATE_PLAYING;
    g_apb.position_ms = 0.0f;
    g_apb.bytes_played = 0;
    g_apaused = 0.0f;
    a_err("playing");
    return true;
}
void mp3_stop(void) {
    pcm_stop();
    g_apb.state = AUDIO_STATE_STOPPED;
    g_apb.position_ms = 0.0f;
    g_apb.bytes_played = 0;
    g_apaused = 0.0f;
}
int wav_play(const char *filename) {
    return mp3_play(filename) ? 0 : -1;
}
int wav_stop(void) {
    mp3_stop();
    return 0;
}
