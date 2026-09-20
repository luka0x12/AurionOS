/* sb16.h: Sound Blaster 16 ISA driver (DSP + DMA1 + mixer).
 * Base 0x220, IRQ5, 8-bit DMA channel 1. Fail-closed: every routine
 * reports absence instead of pretending. No paging here, so a static
 * buffer address is the physical address the ISA DMA engine needs.
*/

#ifndef SB16_H
#define SB16_H
#include <stdint.h>
#include <stdbool.h>
#define SB16_BASE 0x220u
#define SB16_IRQ 5u
#define SB16_DMA8 1u
#define SB16_OUT_RATE 22050u
#define SB16_CHUNK_MAX 4096u

bool sb16_detect(void);
bool sb16_present(void);
uint16_t sb16_version(void);
void sb16_set_volume(float volume);
float sb16_get_volume(void);
uint8_t *sb16_dma_window(void);
bool sb16_start_chunk(uint32_t len);
void sb16_stop(void);
bool sb16_dma_busy(void);
bool sb16_chunk_done(void);
uint32_t sb16_chunk_start_tick(void);
void sb16_irq_entry(void);
int sb16_selftest(void);
#endif

