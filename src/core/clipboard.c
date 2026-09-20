/* clipboard.c - one flat buffer, whole OS. set replaces, get never NULL. */
#include "clipboard.h"

static char clip_buf[4096];
static int  clip_len = 0;

void clipboard_set(const char *s, int len) {
    if (!s || len < 0) { clip_len = 0; clip_buf[0] = 0; return; }
    if (len > (int)sizeof(clip_buf) - 1) len = (int)sizeof(clip_buf) - 1;
    for (int i = 0; i < len; i++) clip_buf[i] = s[i];
    clip_buf[len] = 0;
    clip_len = len;
}

const char *clipboard_get(void) { return clip_buf; }
int  clipboard_len(void) { return clip_len; }
