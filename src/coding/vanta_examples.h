#ifndef VANTA_EXAMPLES_H
#define VANTA_EXAMPLES_H
#include <stdint.h>
typedef struct { const char *path; const char *source; uint32_t size; } VantaExample;
extern const VantaExample vanta_examples[];
extern const uint32_t vanta_example_count;
int vanta_seed_examples(void);
#endif
