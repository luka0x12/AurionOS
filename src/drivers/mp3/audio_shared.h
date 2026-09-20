/* One transport state shared by audio_*.c. Defined in audio_glue.c. */
#ifndef AUDIO_SHARED_H
#define AUDIO_SHARED_H
#include "mp3_player.h"
extern AudioPlayback g_apb;
extern bool g_ainit;
extern float g_avol;
extern float g_apaused;
extern char g_aerr[64];
char a_tolower(char c);
bool a_ext4(const char *e, const char *w);
uint32_t a_rd32(const uint8_t *p);
uint16_t a_rd16(const uint8_t *p);
uint32_t a_strlen(const char *s);
void a_copy(char *d, const char *s, uint32_t cap);
void a_err(const char *s);
bool a_wav_hdr(const uint8_t *h, int n, AudioTrack *t);
#endif

