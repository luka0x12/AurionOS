#ifndef AURION_CLIPBOARD_H
#define AURION_CLIPBOARD_H
void clipboard_set(const char *s, int len);
const char *clipboard_get(void);
int  clipboard_len(void);
#endif
