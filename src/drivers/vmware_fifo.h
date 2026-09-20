/* Legacy SVGA FIFO producer. STOP is owned by the device, not the guest. */
#ifndef AURION_VMWARE_FIFO_H
#define AURION_VMWARE_FIFO_H

#include <stdbool.h>
#include <stdint.h>

/* Nonblocking: false leaves the queue unchanged, including on malformed state.
 * The caller owns serialization and must SYNC the device after publishing.
 * fifo_size is the mapped device aperture size in bytes.
*/

static inline bool vmware_fifo_write_update(volatile uint32_t *fifo,
                                            uint32_t fifo_size,
                                            uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height)
{
    if (!fifo || fifo_size < 16U || !width || !height) {
        return false;
    }
    uint32_t minimum = fifo[0];
    uint32_t maximum = fifo[1];
    uint32_t next = fifo[2];
    uint32_t stop = fifo[3];
    if (((minimum | maximum | next | stop) & 3U) != 0 ||
        minimum < 16U || minimum >= maximum || maximum > fifo_size ||
        next < minimum || next >= maximum || stop < minimum || stop >= maximum) {
        return false;
    }
    uint32_t available = next >= stop
        ? (maximum - next) + (stop - minimum) : stop - next;
    /* Leave one dword empty so NEXT == STOP can only mean an empty queue. */
    if (available <= 20U) {
        return false;
    }
    const uint32_t command[5] = {1U, x, y, width, height};
    for (uint32_t i = 0; i < 5U; ++i) {
        fifo[next / 4U] = command[i];
        next += 4U;
        if (next == maximum) {
            next = minimum;
        }
    }
    /* x86 orders stores; prevent compiler reordering before publication. */
    __asm__ volatile("" ::: "memory");
    fifo[2] = next;
    return true;
}

#endif
