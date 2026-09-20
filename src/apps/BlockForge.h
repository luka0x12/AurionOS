/* BlockForge.h - standalone voxel app entry + shared demo helpers.
 *
 * The world, renderer, physics and UI live in BlockForge.c. The five
 * helpers below stay in app_3d_demo.c (other demos use them too) and
 * are exposed here so both translation units share one implementation.
*/

#ifndef AURION_BLOCKFORGE_H
#define AURION_BLOCKFORGE_H

#include <stdint.h>

void BlockForge_create(void);

float bf_hashf(int x, int y, int z);
void demo_int_to_str(int v, char *buf);
void uint_to_hex(uint32_t v, char *buf);

#endif
