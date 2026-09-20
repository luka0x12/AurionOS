/* sb16.c part 1: probe, volume, DSP primitives. ISA DMA + DSP bytes
 * are the documented SB16/PC-AT values. DMA window is static and
 * 64K-aligned at link time, never kmalloc.
*/

#include "sb16.h"
#include "portio.h"

extern void io_wait(void);
extern uint32_t get_ticks(void);
extern void serial_puts(const char *s);

#define DSP_RESET  0x226u
#define DSP_READ   0x22Au
#define DSP_WRITE  0x22Cu
#define DSP_WSTAT  0x22Cu
#define DSP_RSTAT  0x22Eu
#define DSP_RACK8  0x22Eu
#define DSP_MIXER  0x224u
#define DSP_MDATA  0x225u

#define CMD_SPK_ON  0xD1u
#define CMD_SPK_OFF 0xD3u
#define CMD_HALT8   0xD0u
#define CMD_EXIT16  0xD9u

#define MIX_MASTER_L 0x30u
#define MIX_MASTER_R 0x31u
#define MIX_VOICE_L  0x32u
#define MIX_VOICE_R  0x33u
#define MIX_OUT_ST   0x3Eu
#define MIX_IRQSEL   0x80u

#define DMA1_MASK 0x0Au

#define IRQ_POLL_LOOPS 100000u
#define SPK_ON_RETRY   3u

static bool g_found = false;
static bool g_probed = false;
static float g_volume = 0.8f;

volatile uint8_t g_sb16_dma_busy = 0;
volatile uint8_t g_sb16_chunk_done = 0;
volatile uint32_t g_sb16_chunk_start = 0;

static void dsp_mixer_write(uint8_t reg, uint8_t val) {
    outb(DSP_MIXER, reg);
    io_wait();
    outb(DSP_MDATA, val);
    io_wait();
}

static uint8_t dsp_mixer_read(uint8_t reg) {
    uint8_t v;
    outb(DSP_MIXER, reg);
    io_wait();
    v = inb(DSP_MDATA);
    io_wait();
    return v;
}

static bool dsp_write_ready(void) {
    uint32_t i;
    for (i = 0; i < IRQ_POLL_LOOPS; i++) {
        if (!(inb(DSP_WSTAT) & 0x80u))
            return true;
        io_wait();
    }
    return false;
}

static bool dsp_read_ready(void) {
    uint32_t i;
    for (i = 0; i < IRQ_POLL_LOOPS; i++) {
        if (inb(DSP_RSTAT) & 0x80u)
            return true;
        io_wait();
    }
    return false;
}

bool sb16_dsp_write(uint8_t v) {
    if (!dsp_write_ready())
        return false;
    outb(DSP_WRITE, v);
    io_wait();
    return true;
}

int sb16_dsp_read(void) {
    if (!dsp_read_ready())
        return -1;
    return (int)inb(DSP_READ);
}

static bool dsp_reset(void) {
    int got;
    uint32_t i;
    outb(DSP_RESET, 1u);
    for (i = 0; i < 64u; i++)
        io_wait();
    outb(DSP_RESET, 0u);
    io_wait();
    got = sb16_dsp_read();
    return got == 0xAA;
}

static uint16_t dsp_version(void) {
    int major;
    int minor;
    if (!sb16_dsp_write(0xE1u))
        return 0u;
    major = sb16_dsp_read();
    minor = sb16_dsp_read();
    if (major < 0 || minor < 0)
        return 0u;
    return (uint16_t)(((uint16_t)major << 8) | (uint16_t)minor);
}

bool sb16_detect(void) {
    uint16_t ver;
    uint8_t irqsel;
    if (g_probed)
        return g_found;
    g_probed = true;
    g_found = false;
    g_sb16_dma_busy = 0;
    g_sb16_chunk_done = 0;
    if (!dsp_reset())
        return false;
    ver = dsp_version();
    if ((ver >> 8) < 4u)
        return false;
    sb16_dsp_write(CMD_EXIT16);
    sb16_dsp_write(CMD_HALT8);
    irqsel = dsp_mixer_read(MIX_IRQSEL);
    if (irqsel == 0xFFu || !(irqsel & 0x04u))
        return false;
    sb16_dsp_write(CMD_SPK_OFF);
    outb(DMA1_MASK, (uint8_t)(0x04u | (SB16_DMA8 & 3u)));
    io_wait();
    sb16_set_volume(g_volume);
    g_found = true;
    return true;
}

bool sb16_present(void) {
    return sb16_detect();
}

uint16_t sb16_version(void) {
    if (!sb16_detect())
        return 0u;
    return dsp_version();
}

void sb16_set_volume(float volume) {
    uint8_t level;
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;
    g_volume = volume;
    level = (uint8_t)(volume * 8.0f + 0.5f);
    if (level > 8u)
        level = 8u;
    if (!g_probed || !g_found)
        return;
    dsp_mixer_write(MIX_MASTER_L, (uint8_t)((level << 4) | level));
    dsp_mixer_write(MIX_MASTER_R, (uint8_t)((level << 4) | level));
    dsp_mixer_write(MIX_VOICE_L, (uint8_t)((level << 4) | level));
    dsp_mixer_write(MIX_VOICE_R, (uint8_t)((level << 4) | level));
    dsp_mixer_write(MIX_OUT_ST, 0x07u);
}

float sb16_get_volume(void) {
    return g_volume;
}

bool sb16_dma_busy(void) {
    return g_sb16_dma_busy != 0;
}

uint32_t sb16_chunk_start_tick(void) {
    return g_sb16_chunk_start;
}

bool sb16_chunk_done(void) {
    if (!g_sb16_chunk_done)
        return false;
    g_sb16_chunk_done = 0;
    return true;
}
