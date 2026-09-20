/* pcm_skip.c: output-offset skip for resume/seek. Linear map:
 * skip_frames = skip_out * src_rate / PCM_RATE, clamped.
*/

#include "pcm_engine.h"
#include "pcm_internal.h"

void pcm_skip_output(uint32_t out_bytes) {
    uint64_t frames;
    if (!g_pcm.active || !g_pcm.src_rate)
        return;
    frames = ((uint64_t)out_bytes * g_pcm.src_rate) / PCM_RATE;
    if (frames > g_pcm.total_frames)
        frames = g_pcm.total_frames;
    g_pcm.pos_fp = (uint32_t)(frames << 16);
    g_pcm.done = out_bytes;
    if (g_pcm.done > pcm_bytes_total())
        g_pcm.done = pcm_bytes_total();
}
