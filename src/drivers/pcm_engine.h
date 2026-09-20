/* pcm_engine.h: WAV PCM through the SB16 in 4K chunks. WAV plays
 * sound. MP3/OGG refuse with a reason, never fake-played.
*/

#ifndef PCM_ENGINE_H
#define PCM_ENGINE_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PCM_OK = 0,
    PCM_ERR_NO_FILE = 1,
    PCM_ERR_BAD_WAV = 2,
    PCM_ERR_NO_DEVICE = 3,
    PCM_ERR_TOO_LARGE = 4,
    PCM_ERR_BUSY = 5,
    PCM_ERR_UNSUPPORTED = 6
} pcm_status_t;

bool pcm_is_mp3_or_ogg_name(const char *filename);
pcm_status_t pcm_start(const char *filename);
void pcm_pump(void);
void pcm_stop(void);
bool pcm_active(void);
bool pcm_paused_flag(void);
void pcm_set_paused(bool paused);
uint32_t pcm_bytes_done(void);
uint32_t pcm_bytes_total(void);
bool pcm_backend_is_sb16(void);
const char *pcm_status_text(pcm_status_t st);
#endif

