/* kb_poll.c - lets a long-running shell loop notice Ctrl+C while the WM
   key drain is blocked inside it. Drains the buffer: keys typed during a
   busy loop are dropped except Ctrl+C, which is the whole point.
*/

#include <stdint.h>
#include <stdbool.h>
#include "kb_poll.h"

extern uint16_t sys_getkey(void);
extern int      sys_kb_hit(void);

bool kb_poll_abort(void) {
    bool abort = false;
    while (sys_kb_hit()) {
        uint16_t k = sys_getkey();
        if ((k & 0xFF) == 3) abort = true;
    }
    return abort;
}
