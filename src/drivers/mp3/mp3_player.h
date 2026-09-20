/* mp3_player.h: transport over the real SB16 PCM engine.
 * WAV PCM streams through the card. MP3/OGG refuse (no decoder).
 * Speaker keeps beeps/jingles. Volume drives the SB16 mixer. */

#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

#define MP3_MAX_TRACK_NAME 128
#define MP3_MAX_PLAYLIST 32

typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_OGG
} AudioFormat;

typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} AudioState;

typedef struct {
    char filename[256];
    AudioFormat format;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t duration_ms;
    uint32_t file_size;
} AudioTrack;

typedef struct {
    AudioTrack track;
    AudioState state;
    float volume;
    float position_ms;
    uint32_t bytes_played;
    bool loop;
} AudioPlayback;

typedef void (*AudioCallback)(AudioState state, void *user_data);

void mp3_init(void);
void mp3_shutdown(void);

bool mp3_load_file(const char *filename, AudioTrack *track);
bool mp3_play(const char *filename);
void mp3_pause(void);
void mp3_resume(void);
void mp3_stop(void);
void mp3_set_volume(float volume);
float mp3_get_volume(void);
float mp3_get_position(void);
uint32_t mp3_get_duration_ms(void);
void mp3_poll(void);
bool mp3_seek(float position_ms);
bool mp3_is_playing(void);
/* Last mp3_play failure reason, human-readable, never NULL. */
const char *mp3_last_error(void);

void mp3_on_irq(void);

int wav_play(const char *filename);
int wav_stop(void);

void audio_beep(uint32_t freq_hz, uint32_t duration_ms);

void audio_play_boot_sound(void);
void audio_play_shutdown_sound(void);
void audio_play_notification_sound(void);

void audio_boot_jingle_start(void);
void audio_boot_jingle_poll(void);
void audio_boot_jingle_stop(void);

#endif /* MP3_PLAYER_H */
