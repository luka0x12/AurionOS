/* audio_poll.c: pause/resume/seek/volume/position/poll/irq.
 * Pump on every query so progress tracks bytes delivered. Pause
 * halts DMA and freezes position; resume restarts the stream at
 * the saved output offset. */
#include "mp3_player.h"
#include "audio_shared.h"
#include "../sb16.h"
#include "../pcm_engine.h"
#include "../pcm_skip.h"
#include "utils.h"
#include "portio.h"
extern void io_wait(void);
extern uint32_t get_ticks(void);
void mp3_poll(void) {
    uint32_t total;
    if (!g_ainit)
        return;
    if (g_apb.state != AUDIO_STATE_PLAYING)
        return;
    pcm_pump();
    if (!pcm_active()) {
        if (g_apb.loop) {
            if (mp3_play(g_apb.track.filename))
                return;
        }
        g_apb.state = AUDIO_STATE_STOPPED;
        g_apb.position_ms = g_apb.track.duration_ms ?
            (float)g_apb.track.duration_ms : 0.0f;
        g_apb.bytes_played = g_apb.track.file_size;
        (void)total;
        return;
    }
    g_apb.bytes_played = pcm_bytes_done();
    if (g_apb.track.duration_ms) {
        uint32_t total_out = pcm_bytes_total();
        if (total_out)
            g_apb.position_ms = (float)(((uint64_t)pcm_bytes_done() *
                g_apb.track.duration_ms) / total_out);
    }
}
void mp3_pause(void) {
    if (g_apb.state == AUDIO_STATE_PLAYING) {
        mp3_poll();
        g_apaused = g_apb.position_ms;
        g_apb.position_ms = g_apaused;
        g_apb.state = AUDIO_STATE_PAUSED;
        pcm_set_paused(true);
    }
}
void mp3_resume(void) {
    pcm_status_t st;
    uint32_t total_out;
    uint64_t skip;
    if (g_apb.state != AUDIO_STATE_PAUSED)
        return;
    st = pcm_start(g_apb.track.filename);
    if (st != PCM_OK) {
        a_err(pcm_status_text(st));
        return;
    }
    total_out = pcm_bytes_total();
    skip = 0;
    if (total_out && g_apb.track.duration_ms)
        skip = ((uint64_t)(uint32_t)g_apaused * total_out) /
            g_apb.track.duration_ms;
    if (skip > 0xFFFFFFFFu)
        skip = 0xFFFFFFFFu;
    pcm_skip_output((uint32_t)skip);
    pcm_set_paused(false);
    g_apb.state = AUDIO_STATE_PLAYING;
    g_apb.position_ms = g_apaused;
}
void mp3_set_volume(float volume) {
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;
    g_avol = volume;
    g_apb.volume = volume;
    sb16_set_volume(volume);
}
float mp3_get_volume(void) {
    return g_avol;
}
uint32_t mp3_get_duration_ms(void) {
    return g_apb.track.duration_ms;
}
float mp3_get_position(void) {
    float pos;
    if (!g_ainit)
        return 0.0f;
    mp3_poll();
    if (g_apb.state == AUDIO_STATE_PLAYING) {
        pos = g_apb.position_ms;
        if (g_apb.track.duration_ms && pos > (float)g_apb.track.duration_ms)
            pos = (float)g_apb.track.duration_ms;
        return pos;
    }
    return g_apb.position_ms;
}
bool mp3_seek(float position_ms) {
    uint32_t dur;
    uint32_t want;
    uint32_t total_out;
    uint64_t skip;
    pcm_status_t st;
    if (position_ms < 0)
        position_ms = 0;
    dur = g_apb.track.duration_ms;
    if (dur && position_ms > (float)dur)
        position_ms = (float)dur;
    if (g_apb.state != AUDIO_STATE_PLAYING &&
        g_apb.state != AUDIO_STATE_PAUSED)
        return false;
    want = (uint32_t)position_ms;
    st = pcm_start(g_apb.track.filename);
    if (st != PCM_OK) {
        a_err(pcm_status_text(st));
        return false;
    }
    total_out = pcm_bytes_total();
    skip = 0;
    if (total_out && dur)
        skip = ((uint64_t)want * total_out) / dur;
    if (skip > 0xFFFFFFFFu)
        skip = 0xFFFFFFFFu;
    pcm_skip_output((uint32_t)skip);
    pcm_set_paused(g_apb.state == AUDIO_STATE_PAUSED);
    g_apb.position_ms = position_ms;
    g_apaused = position_ms;
    return true;
}
bool mp3_is_playing(void) {
    if (!g_ainit)
        return false;
    mp3_poll();
    return g_apb.state == AUDIO_STATE_PLAYING;
}
void mp3_on_irq(void) {
    mp3_poll();
}
