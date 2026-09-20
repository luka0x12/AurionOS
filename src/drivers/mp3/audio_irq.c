/* IRQ5 entry + timer pump. Stub owns the single DSP ack per IRQ. */
#include "mp3_player.h"
#include "audio_shared.h"
#include "../sb16.h"
#include "../pcm_engine.h"
#include "utils.h"
#include "portio.h"
extern void io_wait(void);
extern uint32_t get_ticks(void);
void sb16_irq_entry(void);
void sb16_irq_handler_c(void) {
    if (!g_ainit)
        return;
    if (g_apb.state != AUDIO_STATE_PLAYING)
        return;
    pcm_pump();
}
void sb16_irq_stub_entry(void) {
    sb16_irq_entry();
    sb16_irq_handler_c();
}

