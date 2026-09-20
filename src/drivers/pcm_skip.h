/* Output-offset skip for resume/seek. Linear map, clamped. */
#ifndef PCM_SKIP_H
#define PCM_SKIP_H
#include <stdint.h>
void pcm_skip_output(uint32_t out_bytes);
#endif

