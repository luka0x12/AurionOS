#ifndef AURION_KB_POLL_H
#define AURION_KB_POLL_H
#include <stdbool.h>
bool kb_poll_abort(void);   /* true if Ctrl+C was pressed since last poll */
#endif
