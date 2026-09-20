/* audio_beep.c: PC speaker tones. PIT channel 2 square wave,
 * 100 Hz tick math. WAV PCM never touches the speaker. */

#include "mp3_player.h"
#include "utils.h"
#include "portio.h"

extern void io_wait(void);
extern uint32_t get_ticks(void);

#define PIT_CHANNEL2 0x42
#define PIT_CONTROL 0x43
#define PIT_FREQ_CMD 0xB6
#define SPEAKER_PORT 0x61
#define MS_PER_TICK 10

static uint32_t beep_ms_to_ticks(uint32_t ms) {
    return (ms + MS_PER_TICK - 1) / MS_PER_TICK;
}

static void beep_wait(uint32_t n) {
    uint32_t start = get_ticks();
    while (get_ticks() - start < n) {
    }
}

static void beep_freq(uint32_t freq) {
    uint32_t div;
    if (freq < 20 || freq > 20000)
        return;
    div = 1193180 / freq;
    outb(PIT_CONTROL, PIT_FREQ_CMD);
    outb(PIT_CHANNEL2, (uint8_t)(div & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((div >> 8) & 0xFF));
}

static void beep_on(void) {
    uint8_t v = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, v | 0x03);
}

static void beep_off(void) {
    uint8_t v = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, v & 0xFC);
}

void audio_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz < 20 || freq_hz > 20000)
        return;
    if (duration_ms == 0)
        return;
    if (duration_ms > 5000)
        duration_ms = 5000;
    beep_freq(freq_hz);
    beep_on();
    beep_wait(beep_ms_to_ticks(duration_ms));
    beep_off();
}

typedef struct {
    uint32_t freq_hz;
    uint32_t tone_ms;
    uint32_t gap_ms;
} BeepStep;

static const BeepStep BOOT_JINGLE[] = {
    { 523, 110, 30 },
    { 659, 110, 30 },
    { 784, 110, 30 },
    { 1047, 240, 0 },
};
#define BOOT_STEPS 4

static const BeepStep DOWN_JINGLE[] = {
    { 784, 110, 30 },
    { 659, 110, 30 },
    { 523, 240, 0 },
};
#define DOWN_STEPS 3

static void beep_jingle(const BeepStep *s, int n) {
    int i;
    if (!s || n <= 0)
        return;
    for (i = 0; i < n; i++) {
        audio_beep(s[i].freq_hz, s[i].tone_ms);
        if (s[i].gap_ms)
            beep_wait(beep_ms_to_ticks(s[i].gap_ms));
    }
}

void audio_play_boot_sound(void) {
    beep_jingle(BOOT_JINGLE, BOOT_STEPS);
}

void audio_play_shutdown_sound(void) {
    beep_jingle(DOWN_JINGLE, DOWN_STEPS);
}

void audio_play_notification_sound(void) {
    audio_beep(784, 70);
    beep_wait(beep_ms_to_ticks(30));
    audio_beep(1047, 90);
}

static int g_jstep = -1;
static uint32_t g_jedge = 0;
static bool g_jgap = false;

void audio_boot_jingle_start(void) {
    g_jstep = 0;
    g_jgap = false;
    beep_freq(BOOT_JINGLE[0].freq_hz);
    beep_on();
    g_jedge = get_ticks() + beep_ms_to_ticks(BOOT_JINGLE[0].tone_ms);
}

void audio_boot_jingle_poll(void) {
    uint32_t now;
    if (g_jstep < 0 || g_jstep >= BOOT_STEPS)
        return;
    now = get_ticks();
    if ((int32_t)(now - g_jedge) < 0)
        return;
    if (!g_jgap) {
        uint32_t gap = BOOT_JINGLE[g_jstep].gap_ms;
        beep_off();
        g_jstep++;
        if (g_jstep >= BOOT_STEPS) {
            g_jstep = -1;
            return;
        }
        if (gap == 0) {
            beep_freq(BOOT_JINGLE[g_jstep].freq_hz);
            beep_on();
            g_jedge = now + beep_ms_to_ticks(BOOT_JINGLE[g_jstep].tone_ms);
        } else {
            g_jgap = true;
            g_jedge = now + beep_ms_to_ticks(gap);
        }
    } else {
        g_jgap = false;
        beep_freq(BOOT_JINGLE[g_jstep].freq_hz);
        beep_on();
        g_jedge = now + beep_ms_to_ticks(BOOT_JINGLE[g_jstep].tone_ms);
    }
}

void audio_boot_jingle_stop(void) {
    g_jstep = -1;
    g_jgap = false;
    beep_off();
}
