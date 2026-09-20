/* smp.h - symmetric multiprocessing: detect cores, boot APs, run band jobs.
 *
 * Model: BSP keeps all interrupts and drivers. APs boot with interrupts
 * disabled and spin on a generation-counted band queue (no IPIs after
 * boot, no locks in the job path except lock-prefixed xadd). The only
 * parallel client is AurionGL: fullscreen clear and triangle fill split
 * by screen-Y bands, so workers never share a pixel or depth cell.
 * Single CPU falls back to direct serial execution with zero overhead.
*/

#ifndef AURION_SMP_H
#define AURION_SMP_H

#include <stdint.h>
#include <stdbool.h>

#define SMP_MAX_CPUS 4
#define SMP_TRAMP_ADDR 0x8000u
#define SMP_TRAMP_SIZE 512u
#define SMP_MBOX_GDTR 0x7000u
#define SMP_MBOX_ENTRY 0x7006u
#define SMP_MBOX_STACK 0x700Au

typedef void (*smp_band_fn_t)(int band, int nbands, void *ctx);

void smp_init(void);
int smp_cpu_count(void);
int smp_ap_count(void);
int smp_cpu_id(void);
bool smp_available(void);

void smp_parallel_bands(smp_band_fn_t fn, void *ctx, int nbands);

static inline uint32_t smp_atomic_xadd(volatile uint32_t *p, uint32_t v) {
    __asm__ volatile("lock xaddl %0, %1" : "+r"(v), "+m"(*p) :: "memory", "cc");
    return v;
}

static inline void smp_cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

#endif
