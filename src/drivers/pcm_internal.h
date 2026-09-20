/* pcm_internal.h: state shared by pcm_* units. One stream,
 * bounded buffers, no kmalloc. Payload capped at 8192 (FS cap).
*/

#ifndef PCM_INTERNAL_H
#define PCM_INTERNAL_H
#include <stdint.h>
#include <stdbool.h>
#define PCM_RATE 22050u
#define PCM_HDR_PROBE 512u
#define PCM_SRC_CAP 8192u
#define PCM_CONVERT_TMP 512u
#define PCM_NAME_MAX 256u
typedef struct {
    bool active;
    bool paused;
    bool backend_sb16;
    uint32_t data_off;
    uint32_t data_len;
    uint32_t src_rate;
    uint8_t src_ch;
    uint8_t src_bits;
    uint32_t total_frames;
    uint32_t pos_fp;
    uint32_t done;
} pcm_stream_t;
extern pcm_stream_t g_pcm;
extern uint8_t g_pcm_convert_tmp[PCM_CONVERT_TMP];
extern uint8_t g_pcm_src[PCM_SRC_CAP];
extern char g_pcm_name[PCM_NAME_MAX];
#endif

