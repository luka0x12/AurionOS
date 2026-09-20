/* sb16_stream.c part 2: DMA program, DSP start, IRQ entry, selftest.
 * 8-bit unsigned mono at 22050 Hz, single-cycle mode, DMA channel 1.
*/

#include "sb16.h"
#include "portio.h"

extern void io_wait(void);
extern uint32_t get_ticks(void);
extern void serial_puts(const char *s);
extern volatile uint8_t g_sb16_dma_busy;
extern volatile uint8_t g_sb16_chunk_done;
extern volatile uint32_t g_sb16_chunk_start;
extern bool sb16_dsp_write(uint8_t v);

#define DSP_RACK8  0x22Eu
#define CMD_RATE_OUT 0x41u
#define CMD_SPK_ON   0xD1u
#define CMD_HALT8    0xD0u
#define CMD_OUT8     0xC0u

#define DMA1_MASK  0x0Au
#define DMA1_MODE  0x0Bu
#define DMA1_CLEAR 0x0Cu
#define DMA1_ADDR  0x02u
#define DMA1_COUNT 0x03u
#define DMA1_PAGE  0x83u
#define DMA_MODE_W1 0x49u

#define SPK_ON_RETRY 3u

static uint8_t g_dma_window[SB16_CHUNK_MAX * 2] __attribute__((aligned(65536)));

uint8_t *sb16_dma_window(void) {
    return g_dma_window;
}

static void dma_program(uint32_t phys, uint32_t len) {
    uint8_t page = (uint8_t)((phys >> 16) & 0xFFu);
    uint16_t off = (uint16_t)(phys & 0xFFFFu);
    uint16_t cnt = (uint16_t)((len - 1u) & 0xFFFFu);
    outb(DMA1_MASK, (uint8_t)(0x04u | (SB16_DMA8 & 3u)));
    io_wait();
    outb(DMA1_CLEAR, 0u);
    io_wait();
    outb(DMA1_MODE, DMA_MODE_W1);
    io_wait();
    outb(DMA1_ADDR, (uint8_t)(off & 0xFFu));
    io_wait();
    outb(DMA1_ADDR, (uint8_t)(off >> 8));
    io_wait();
    outb(DMA1_PAGE, page);
    io_wait();
    outb(DMA1_CLEAR, 0u);
    io_wait();
    outb(DMA1_COUNT, (uint8_t)(cnt & 0xFFu));
    io_wait();
    outb(DMA1_COUNT, (uint8_t)(cnt >> 8));
    io_wait();
    outb(DMA1_MASK, (uint8_t)(SB16_DMA8 & 3u));
    io_wait();
}

static bool dsp_set_rate(uint16_t hz) {
    if (!sb16_dsp_write(CMD_RATE_OUT))
        return false;
    if (!sb16_dsp_write((uint8_t)(hz >> 8)))
        return false;
    return sb16_dsp_write((uint8_t)(hz & 0xFFu));
}

static bool dsp_start_8bit(uint32_t len) {
    uint16_t cnt = (uint16_t)((len - 1u) & 0xFFFFu);
    uint8_t i;
    for (i = 0; i < SPK_ON_RETRY; i++) {
        if (sb16_dsp_write(CMD_SPK_ON))
            break;
    }
    if (i >= SPK_ON_RETRY)
        return false;
    if (!dsp_set_rate(SB16_OUT_RATE))
        return false;
    if (!sb16_dsp_write(CMD_OUT8))
        return false;
    if (!sb16_dsp_write(0x00u))
        return false;
    if (!sb16_dsp_write((uint8_t)(cnt & 0xFFu)))
        return false;
    return sb16_dsp_write((uint8_t)(cnt >> 8));
}

bool sb16_start_chunk(uint32_t len) {
    uint32_t phys;
    uint32_t end;
    if (len == 0u || len > SB16_CHUNK_MAX)
        return false;
    if (!sb16_detect())
        return false;
    if (g_sb16_dma_busy)
        return false;
    phys = (uint32_t)(uintptr_t)g_dma_window;
    end = phys + len - 1u;
    if (end < phys)
        return false;
    if ((phys & 0xFFFF0000u) != (end & 0xFFFF0000u))
        return false;
    if (phys + len > 0x1000000u)
        return false;
    sb16_dsp_write(CMD_HALT8);
    dma_program(phys, len);
    g_sb16_chunk_done = 0;
    g_sb16_chunk_start = get_ticks();
    g_sb16_dma_busy = 1;
    if (!dsp_start_8bit(len)) {
        g_sb16_dma_busy = 0;
        outb(DMA1_MASK, (uint8_t)(0x04u | (SB16_DMA8 & 3u)));
        io_wait();
        return false;
    }
    return true;
}

void sb16_stop(void) {
    sb16_dsp_write(CMD_HALT8);
    outb(DMA1_MASK, (uint8_t)(0x04u | (SB16_DMA8 & 3u)));
    io_wait();
    g_sb16_dma_busy = 0;
    g_sb16_chunk_done = 0;
}

void sb16_irq_entry(void) {
    (void)inb(DSP_RACK8);
    io_wait();
    g_sb16_dma_busy = 0;
    g_sb16_chunk_done = 1;
}

static uint8_t sine_step(uint32_t *phase) {
    static const uint8_t QTAB[64] = {
        0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
        49, 51, 54, 57, 60, 62, 65, 68, 70, 73, 75, 78, 80, 83, 85, 88,
        90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 114, 115, 117,
        118, 119, 120, 121, 122, 123, 124, 125, 125, 126, 126, 127, 127, 127, 127, 127
    };
    uint32_t step = *phase >> 8;
    uint8_t q = (uint8_t)((step >> 6) & 3u);
    uint8_t mag = QTAB[step & 63u];
    uint8_t s;
    if (q == 0u)
        s = (uint8_t)(128u + mag);
    else if (q == 1u)
        s = (uint8_t)(128u + (127u - mag));
    else if (q == 2u)
        s = (uint8_t)(128u - mag);
    else
        s = (uint8_t)(128u - (127u - mag));
    *phase += 1308u;
    return s;
}

int sb16_selftest(void) {
    uint32_t i;
    uint32_t start;
    uint32_t phase = 0u;
    if (!sb16_detect())
        return -3;
    for (i = 0; i < SB16_CHUNK_MAX; i++)
        g_dma_window[i] = sine_step(&phase);
    if (!sb16_start_chunk(SB16_CHUNK_MAX))
        return -2;
    start = get_ticks();
    while (!sb16_chunk_done()) {
        if (get_ticks() - start > 60u) {
            sb16_stop();
            serial_puts("[SB16] selftest: IRQ timeout\r\n");
            return -1;
        }
    }
    sb16_stop();
    return (int)SB16_CHUNK_MAX;
}
