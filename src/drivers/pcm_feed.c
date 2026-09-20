/* pcm_feed.c: chunk feed + pump. 16.16 fixed-point resample from
 * g_pcm_src to 22050 Hz unsigned 8-bit mono. Converts straight from
 * the RAM payload. IRQ5 completion flag drives chunk advance; a
 * tick timeout stops a wedged transfer instead of hanging.
*/

#include "pcm_engine.h"
#include "pcm_internal.h"
#include "sb16.h"
#include "portio.h"
extern void io_wait(void);
extern uint32_t get_ticks(void);
extern void serial_puts(const char *s);
#define IRQ_TIMEOUT_TICKS 60u
static int16_t feed_decode(uint32_t rel) {
    const uint8_t *f = g_pcm_src + rel;
    uint8_t ch = g_pcm.src_ch;
    int32_t acc = 0;
    uint8_t c;
    if (g_pcm.src_bits == 8) {
        for (c = 0; c < ch; c++)
            acc += (int32_t)f[c] - 128;
        acc /= (int32_t)ch;
        return (int16_t)(acc * 256);
    }
    for (c = 0; c < ch; c++) {
        uint32_t o = (uint32_t)c * 2u;
        int16_t s = (int16_t)((uint16_t)f[o] | ((uint16_t)f[o+1] << 8));
        acc += (int32_t)s;
    }
    acc /= (int32_t)ch;
    return (int16_t)acc;
}
static uint32_t feed_convert(void) {
    uint8_t *win = sb16_dma_window();
    uint32_t out = 0;
    uint32_t frame_bytes;
    uint32_t step;
    if (!g_pcm.src_rate)
        return 0;
    frame_bytes = (uint32_t)g_pcm.src_ch * (g_pcm.src_bits == 16 ? 2u : 1u);
    if (!frame_bytes)
        return 0;
    step = (uint32_t)(((uint64_t)g_pcm.src_rate << 16) / PCM_RATE);
    if (!step)
        step = 1u;
    while (out < SB16_CHUNK_MAX) {
        uint32_t frame = g_pcm.pos_fp >> 16;
        uint64_t rel64;
        int16_t s;
        if (frame >= g_pcm.total_frames)
            break;
        rel64 = (uint64_t)frame * frame_bytes;
        if (rel64 + frame_bytes > g_pcm.data_len)
            break;
        s = feed_decode((uint32_t)rel64);
        win[out++] = (uint8_t)(((int32_t)s >> 8) + 128);
        g_pcm.pos_fp += step;
        if (g_pcm.pos_fp < step)
            break;
    }
    return out;
}
uint32_t pcm_feed_first(void) {
    uint32_t n = feed_convert();
    if (!n)
        return 0;
    if (!sb16_start_chunk(n))
        return 0;
    g_pcm.done += n;
    return n;
}
static uint32_t feed_next(void) {
    uint32_t n = feed_convert();
    if (!n)
        return 0;
    if (!sb16_start_chunk(n))
        return 0;
    g_pcm.done += n;
    return n;
}
void pcm_pump(void) {
    if (!g_pcm.active || g_pcm.paused || !g_pcm.backend_sb16)
        return;
    if (sb16_dma_busy()) {
        if (get_ticks() - sb16_chunk_start_tick() > IRQ_TIMEOUT_TICKS) {
            sb16_stop();
            g_pcm.active = false;
            serial_puts("[PCM] IRQ timeout, stopped\r\n");
        }
        return;
    }
    if (sb16_chunk_done()) {
    }
    if ((g_pcm.pos_fp >> 16) >= g_pcm.total_frames) {
        g_pcm.active = false;
        sb16_stop();
        return;
    }
    feed_next();
}
void pcm_stop(void) {
    sb16_stop();
    g_pcm.active = false;
    g_pcm.paused = false;
    g_pcm.backend_sb16 = false;
}
bool pcm_active(void) {
    return g_pcm.active && !g_pcm.paused;
}
bool pcm_paused_flag(void) {
    return g_pcm.paused;
}
void pcm_set_paused(bool paused) {
    g_pcm.paused = paused;
    if (paused)
        sb16_stop();
}
uint32_t pcm_bytes_done(void) {
    return g_pcm.done;
}
uint32_t pcm_bytes_total(void) {
    uint64_t total;
    if (!g_pcm.src_rate || !g_pcm.total_frames)
        return 0;
    total = ((uint64_t)g_pcm.total_frames * PCM_RATE) / g_pcm.src_rate;
    if (total > 0xFFFFFFFFu)
        return 0xFFFFFFFFu;
    return (uint32_t)total;
}
bool pcm_backend_is_sb16(void) {
    return g_pcm.backend_sb16;
}
