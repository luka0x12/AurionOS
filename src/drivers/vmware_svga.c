/* src/drivers/vmware_svga.c */
/*
 * VMware SVGA II driver for AurionOS.
 * Framebuffer updates use the device-owned FIFO aperture reported by
 * MEM_START/MEM_SIZE. Those registers are read-only, not DMA configuration.
*/

#include <stdint.h>
#include <stdbool.h>
#include "portio.h"
#include "vmware_svga.h"
#include "vmware_fifo.h"
#include "pci.h"

extern void c_puts(const char *s);
extern void c_putc(char c);

static void dbg_u32(uint32_t v)
{
    char buf[11];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 9; i >= 2; i--) {
        uint32_t d = v & 0xFu;
        buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    buf[10] = '\0';
    c_puts(buf);
}

#define VMWARE_VENDOR_ID        0x15AD
#define VMWARE_SVGA2_ID         0x0405
#define VBOX_VENDOR_ID          0x80EE
#define VBOX_VMSVGA_ID          0xCAFE

/* BAR0 I/O register offsets. */
#define SVGA_INDEX_PORT         0x0
#define SVGA_VALUE_PORT         0x1
#define SVGA_BIOS_PORT          0x2
#define SVGA_IRQ_PORT           0x8

/* SVGA registers. */
#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_DEPTH          6
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_FB_SIZE        16
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_START      18
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22

/* FIFO layout. Offsets are in dwords inside the FIFO memory. */
#define SVGA_FIFO_MIN           0
#define SVGA_FIFO_MAX           1
#define SVGA_FIFO_NEXT_CMD      2
#define SVGA_FIFO_STOP          3

/* Reserve the first page for FIFO registers, including device-owned ones. */
#define SVGA_FIFO_CMD_START     0x1000UL

/* SVGA IDs encode the magic in bits 31:8 and the version in bits 7:0. */
#define SVGA_MAGIC              0x900000UL
#define SVGA_ID_1               ((SVGA_MAGIC << 8) | 1UL)
#define SVGA_ID_2               ((SVGA_MAGIC << 8) | 2UL)

#define SVGA_BOOT_BLOCK         0x9000UL

#define SVGA_MIN_WIDTH          640
#define SVGA_MIN_HEIGHT         480
#define SVGA_FALLBACK_WIDTH     1024
#define SVGA_FALLBACK_HEIGHT    768
#define SVGA_HARD_MAX_WIDTH     4096
#define SVGA_HARD_MAX_HEIGHT    4096

static bool     vmware_svga_present = false;
static uint16_t svga_iobase = 0;
static uint32_t svga_fb_addr = 0;
static uint32_t svga_fb_size = 0;
static uint32_t svga_max_width = 0;
static uint32_t svga_max_height = 0;
static uint32_t svga_version = 0;

static uint32_t svga_current_width = 0;
static uint32_t svga_current_height = 0;
static uint32_t svga_current_bpp = 0;
static uint32_t svga_pitch = 0;

static bool svga_fifo_ready = false;
static volatile uint32_t *svga_fifo = 0;
static uint32_t svga_fifo_size = 0;

static inline void svga_outl(uint16_t port, uint32_t value)
{
    io_outl(port, value);
}

static inline uint32_t svga_inl(uint16_t port)
{
    return io_inl(port);
}

static void svga_delay(uint32_t spins)
{
    volatile uint32_t i;
    for (i = 0; i < spins; i++) {
        /* spin */
    }
}

static void svga_write_reg(uint32_t index, uint32_t value)
{
    svga_outl((uint16_t)(svga_iobase + SVGA_INDEX_PORT), index);
    svga_outl((uint16_t)(svga_iobase + SVGA_VALUE_PORT), value);
}

static uint32_t svga_read_reg(uint32_t index)
{
    svga_outl((uint16_t)(svga_iobase + SVGA_INDEX_PORT), index);
    return svga_inl((uint16_t)(svga_iobase + SVGA_VALUE_PORT));
}

static bool svga_negotiate_version(void)
{
    svga_version = 0;
    /* Use only the specified value port. BIOS/IRQ ports can return small
     * integers that look plausible but are not SVGA register contents. */
    svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
    if (svga_read_reg(SVGA_REG_ID) == SVGA_ID_2) {
        svga_version = 2U;
        return true;
    }
    svga_write_reg(SVGA_REG_ID, SVGA_ID_1);
    if (svga_read_reg(SVGA_REG_ID) == SVGA_ID_1) {
        svga_version = 1U;
        return true;
    }
    c_puts("[VMSVGA] protocol negotiation failed\n");
    return false;
}

static uint32_t svga_bytes_per_pixel(uint32_t bpp)
{
    switch (bpp) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 24:
        return 3;
    case 32:
        return 4;
    default:
        return 0;
    }
}

static bool svga_mode_sane(uint32_t width, uint32_t height, uint32_t bpp)
{
    if (width < SVGA_MIN_WIDTH || height < SVGA_MIN_HEIGHT) {
        return false;
    }
    if (width > SVGA_HARD_MAX_WIDTH || height > SVGA_HARD_MAX_HEIGHT) {
        return false;
    }
    if (svga_bytes_per_pixel(bpp) == 0) {
        return false;
    }
    return true;
}

static void svga_clamp_mode(uint32_t *width, uint32_t *height, uint32_t *bpp)
{
    uint32_t w;
    uint32_t h;
    uint32_t b;
    uint32_t max_w;
    uint32_t max_h;

    if (!width || !height || !bpp) {
        return;
    }

    w = *width;
    h = *height;
    b = *bpp;

    if (w == 0) {
        w = SVGA_FALLBACK_WIDTH;
    }
    if (h == 0) {
        h = SVGA_FALLBACK_HEIGHT;
    }
    if (b == 0) {
        b = 32;
    }

    max_w = svga_max_width;
    max_h = svga_max_height;

    if (max_w < SVGA_MIN_WIDTH || max_w > SVGA_HARD_MAX_WIDTH) {
        max_w = SVGA_FALLBACK_WIDTH;
    }
    if (max_h < SVGA_MIN_HEIGHT || max_h > SVGA_HARD_MAX_HEIGHT) {
        max_h = SVGA_FALLBACK_HEIGHT;
    }

    if (w < SVGA_MIN_WIDTH) {
        w = SVGA_MIN_WIDTH;
    }
    if (h < SVGA_MIN_HEIGHT) {
        h = SVGA_MIN_HEIGHT;
    }
    if (w > max_w) {
        w = max_w;
    }
    if (h > max_h) {
        h = max_h;
    }
    if (w > SVGA_HARD_MAX_WIDTH) {
        w = SVGA_HARD_MAX_WIDTH;
    }
    if (h > SVGA_HARD_MAX_HEIGHT) {
        h = SVGA_HARD_MAX_HEIGHT;
    }

    if (b != 32 && b != 24 && b != 16 && b != 8) {
        b = 32;
    }

    *width = w;
    *height = h;
    *bpp = b;
}

static void svga_publish_boot_block(uint32_t width,
                                    uint32_t height,
                                    uint32_t bpp,
                                    uint32_t pitch)
{
    volatile uint32_t *fb_ptr =
        (volatile uint32_t *)(uintptr_t)SVGA_BOOT_BLOCK;
    volatile uint16_t *width_ptr =
        (volatile uint16_t *)(uintptr_t)(SVGA_BOOT_BLOCK + 4UL);
    volatile uint16_t *height_ptr =
        (volatile uint16_t *)(uintptr_t)(SVGA_BOOT_BLOCK + 6UL);
    volatile uint8_t *bpp_ptr =
        (volatile uint8_t *)(uintptr_t)(SVGA_BOOT_BLOCK + 8UL);
    volatile uint16_t *pitch_ptr =
        (volatile uint16_t *)(uintptr_t)(SVGA_BOOT_BLOCK + 10UL);
    volatile uint16_t *lin_pitch_ptr =
        (volatile uint16_t *)(uintptr_t)(SVGA_BOOT_BLOCK + 12UL);

    *fb_ptr = svga_fb_addr;
    *width_ptr = (uint16_t)width;
    *height_ptr = (uint16_t)height;
    *bpp_ptr = (uint8_t)bpp;
    *pitch_ptr = (uint16_t)pitch;
    *lin_pitch_ptr = (uint16_t)pitch;
}

static bool svga_fifo_init(void)
{
    svga_fifo_ready = false;
    svga_fifo = 0;
    svga_fifo_size = 0;
    svga_write_reg(SVGA_REG_CONFIG_DONE, 0U);

    /* MEM_START/MEM_SIZE describe device memory, not a guest DMA buffer.
     * With paging disabled the physical aperture is directly accessible. */
    uint32_t address = svga_read_reg(SVGA_REG_MEM_START);
    uint32_t size = svga_read_reg(SVGA_REG_MEM_SIZE);
    if (address < 0x100000U || ((address | size) & 3U) != 0 ||
        size < SVGA_FIFO_CMD_START + 24U || size > 0xFFFFFFFFU - address) {
        c_puts("[VMSVGA] invalid FIFO aperture\n");
        return false;
    }

    svga_fifo = (volatile uint32_t *)(uintptr_t)address;
    svga_fifo_size = size;
    /* Only initialize the four legacy registers while CONFIG_DONE is off.
     * Do not clear device-owned capability registers in the rest of the page. */
    svga_fifo[SVGA_FIFO_MIN] = SVGA_FIFO_CMD_START;
    svga_fifo[SVGA_FIFO_MAX] = size;
    svga_fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_CMD_START;
    svga_fifo[SVGA_FIFO_STOP] = SVGA_FIFO_CMD_START;
    __asm__ volatile("" ::: "memory");
    svga_write_reg(SVGA_REG_CONFIG_DONE, 1U);
    svga_fifo_ready = true;
    c_puts("[VMSVGA] FIFO aperture="); dbg_u32(address);
    c_puts(" size="); dbg_u32(size); c_putc('\n');
    return true;
}

static void svga_fifo_update(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!vmware_svga_present || !svga_fifo_ready) {
        return;
    }

    if (svga_current_width != 0 && svga_current_height != 0) {
        if (x >= svga_current_width || y >= svga_current_height) {
            return;
        }
        if (width == 0 || width > svga_current_width - x) {
            width = svga_current_width - x;
        }
        if (height == 0 || height > svga_current_height - y) {
            height = svga_current_height - y;
        }
    }
    if (width == 0 || height == 0) {
        return;
    }

    /* STOP belongs to the host after initialization. Never reset it or
     * overwrite pending commands, even when the device stops responding.
    */

    for (uint32_t attempt = 0; attempt < 2U; ++attempt) {
        bool queued = vmware_fifo_write_update(svga_fifo, svga_fifo_size,
                                               x, y, width, height);
        svga_write_reg(SVGA_REG_SYNC, 1U);
        for (uint32_t timeout = 0; timeout < 10000U; ++timeout) {
            if (svga_read_reg(SVGA_REG_BUSY) == 0U) {
                break;
            }
            svga_delay(1U);
        }
        if (queued) {
            return;
        }
    }
    c_puts("[VMSVGA] FIFO stalled; update not queued\n");
}

static bool svga_program_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
    uint32_t attempt;
    uint32_t actual_w = 0;
    uint32_t actual_h = 0;
    uint32_t actual_bpp = 0;
    uint32_t fb;
    uint32_t size;
    uint32_t bytes_per_pixel;
    uint32_t min_pitch;
    uint32_t pitch;

    c_puts("[VMSVGA] program_mode entry w="); dbg_u32(width);
    c_puts(" h="); dbg_u32(height);
    c_puts(" bpp="); dbg_u32(bpp); c_putc('\n');

    if (!vmware_svga_present || svga_fb_addr == 0) {
        c_puts("[VMSVGA] program_mode: NOT present or no FB\n");
        return false;
    }
    if (!svga_mode_sane(width, height, bpp)) {
        c_puts("[VMSVGA] program_mode: insane mode\n");
        return false;
    }

    c_puts("[VMSVGA] DISABLE\n");
    svga_write_reg(SVGA_REG_ENABLE, 0);
    svga_delay(1000U);

    c_puts("[VMSVGA] SET regs\n");
    svga_write_reg(SVGA_REG_WIDTH, width);
    svga_write_reg(SVGA_REG_HEIGHT, height);
    svga_write_reg(SVGA_REG_BITS_PER_PIXEL, bpp);
    svga_write_reg(SVGA_REG_FB_OFFSET, 0);

    c_puts("[VMSVGA] ENABLE\n");
    svga_write_reg(SVGA_REG_ENABLE, 1);
    c_puts("[VMSVGA] FIFO init\n");
    if (!svga_fifo_init()) {
        svga_write_reg(SVGA_REG_ENABLE, 0U);
        return false;
    }
    c_puts("[VMSVGA] delay 10000\n");
    svga_delay(10000U);

    /*
     * I use readback only to learn what the host actually accepted. I do
     * not fail the mode change just because a virtual device has lazy
     * register readback.
     */
    c_puts("[VMSVGA] readback loop\n");
    for (attempt = 0; attempt < 16U; attempt++) {
        svga_delay(2000U);

        actual_w = svga_read_reg(SVGA_REG_WIDTH);
        actual_h = svga_read_reg(SVGA_REG_HEIGHT);
        actual_bpp = svga_read_reg(SVGA_REG_BITS_PER_PIXEL);

        if (attempt == 0U || attempt == 15U) {
            c_puts("[VMSVGA] attempt "); dbg_u32(attempt);
            c_puts(" w="); dbg_u32(actual_w);
            c_puts(" h="); dbg_u32(actual_h);
            c_puts(" bpp="); dbg_u32(actual_bpp); c_putc('\n');
        }

        if (actual_w >= 320U && actual_w <= SVGA_HARD_MAX_WIDTH &&
            actual_h >= 200U && actual_h <= SVGA_HARD_MAX_HEIGHT) {
            break;
        }
    }

    if (actual_w >= 320U && actual_w <= SVGA_HARD_MAX_WIDTH &&
        actual_h >= 200U && actual_h <= SVGA_HARD_MAX_HEIGHT) {
        width = actual_w;
        height = actual_h;
    }

    if (actual_bpp == 24U || actual_bpp == 32U) {
        bpp = actual_bpp;
    }

    fb = svga_read_reg(SVGA_REG_FB_START);
    if (fb != 0 && fb != 0xFFFFFFFFU && fb != 0xFFFFFFF0U) {
        svga_fb_addr = fb;
    }
    c_puts("[VMSVGA] final fb="); dbg_u32(svga_fb_addr);
    c_puts(" FB_START_reg="); dbg_u32(fb); c_putc('\n');

    if (svga_fb_addr == 0 || svga_fb_addr == 0xFFFFFFFFU ||
        svga_fb_addr == 0xFFFFFFF0U) {
        c_puts("[VMSVGA] program_mode FAIL: no FB\n");
        return false;
    }

    size = svga_read_reg(SVGA_REG_FB_SIZE);
    if (size != 0) {
        svga_fb_size = size;
    }

    bytes_per_pixel = svga_bytes_per_pixel(bpp);
    min_pitch = width * bytes_per_pixel;
    pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);

    c_puts("[VMSVGA] readback w="); dbg_u32(width);
    c_puts(" h="); dbg_u32(height);
    c_puts(" bpp="); dbg_u32(bpp);
    c_puts(" pitch_reg="); dbg_u32(pitch);
    c_puts(" min_pitch="); dbg_u32(min_pitch); c_putc('\n');

    /*
     * Some hosts return zero or a stale pitch right after a mode change.
     * The computed tight pitch is enough for AurionOS when the hardware
     * does not report a larger aligned stride.
    */

    if (pitch < min_pitch || pitch > 0xFFFFU) {
        pitch = min_pitch;
    }
    if (pitch == 0) {
        pitch = min_pitch;
    }

    svga_current_width = width;
    svga_current_height = height;
    svga_current_bpp = bpp;
    svga_pitch = pitch;

    if (svga_fb_size == 0) {
        svga_fb_size = pitch * height;
    }

    svga_publish_boot_block(width, height, bpp, pitch);

    /*
     * Mark the whole mode dirty once. If the host only updates on FIFO
     * commands, this is the point where the scanout becomes visible.
     */
    c_puts("[VMSVGA] FIFO UPDATE\n");
    svga_fifo_update(0, 0, width, height);

    c_puts("[VMSVGA] program_mode OK w="); dbg_u32(svga_current_width);
    c_puts(" h="); dbg_u32(svga_current_height);
    c_puts(" bpp="); dbg_u32(svga_current_bpp);
    c_puts(" pitch="); dbg_u32(svga_pitch);
    c_puts(" fb="); dbg_u32(svga_fb_addr); c_putc('\n');

    return true;
}

bool vmware_svga_detect(void)
{
    uint32_t bus;
    uint32_t dev;
    uint32_t func;

    vmware_svga_present = false;
    svga_iobase = 0;
    svga_fb_addr = 0;
    svga_fb_size = 0;
    svga_max_width = 0;
    svga_max_height = 0;
    svga_version = 0;
    svga_current_width = 0;
    svga_current_height = 0;
    svga_current_bpp = 0;
    svga_pitch = 0;
    svga_fifo_ready = false;

    for (bus = 0; bus < 256U; bus++) {
        for (dev = 0; dev < 32U; dev++) {
            for (func = 0; func < 8U; func++) {
                uint32_t vendor_device;
                uint16_t vendor;
                uint16_t device;
                uint32_t bar0;
                uint32_t bar1;
                uint32_t bar1_fb;
                uint32_t command;
                uint16_t io_base;
                uint32_t fb;
                uint32_t size;
                uint32_t max_w;
                uint32_t max_h;

                vendor_device = pci_config_read(
                    (uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x00);
                vendor = (uint16_t)(vendor_device & 0xFFFFU);

                if (vendor != VMWARE_VENDOR_ID && vendor != VBOX_VENDOR_ID) {
                    continue;
                }
                device = (uint16_t)(vendor_device >> 16);
                if (device != VMWARE_SVGA2_ID && device != VBOX_VMSVGA_ID) {
                    continue;
                }

                device = (uint16_t)(vendor_device >> 16);
                if (device != VMWARE_SVGA2_ID) {
                    continue;
                }

                bar0 = pci_config_read(
                    (uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x10);
                if ((bar0 & 0x1U) == 0) {
                    c_puts("[VMSVGA] BAR0 is not I/O space\n");
                    return false;
                }

                io_base = (uint16_t)(bar0 & 0xFFFCU);
                if (io_base == 0) {
                    c_puts("[VMSVGA] invalid BAR0 I/O base\n");
                    return false;
                }

                bar1 = pci_config_read(
                    (uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x14);
                bar1_fb = 0;

                if ((bar1 & 0x1U) == 0) {
                    uint32_t mem_type = bar1 & 0x06U;

                    if (mem_type == 0x04U) {
                        uint32_t high = pci_config_read(
                            (uint8_t)bus,
                            (uint8_t)dev,
                            (uint8_t)func,
                            0x18);

                        if (high != 0) {
                            c_puts("[VMSVGA] framebuffer above 4 GiB\n");
                            return false;
                        }
                    }

                    bar1_fb = bar1 & 0xFFFFFFF0U;
                }

                command = pci_config_read(
                    (uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x04);
                command |= 0x07U; /* I/O space, memory space, bus master */
                pci_config_write(
                    (uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x04, command);

                /*
                 * PCI spec requires a settle delay after enabling I/O space
                 * in the command register.  VMware Workstation's SVGA device
                 * needs this before its BAR0 I/O ports become responsive.
                 * Without it, the first register reads return 0xFFFFFFFF
                 * and the driver silently falls back to VGA (black screen).
                */

                svga_delay(50000U);

                c_puts("[VMSVGA] PCI match b="); dbg_u32(bus);
                c_puts(" d="); dbg_u32(dev);
                c_puts(" f="); dbg_u32(func); c_putc('\n');

                c_puts("[VMSVGA] BAR0="); dbg_u32(bar0);
                c_puts(" BAR1="); dbg_u32(bar1);
                c_puts(" bar1_fb="); dbg_u32(bar1_fb); c_putc('\n');

                svga_iobase = io_base;
                c_puts("[VMSVGA] iobase="); dbg_u32(io_base);
                c_puts(" val_off="); dbg_u32(SVGA_VALUE_PORT); c_putc('\n');

                if (!svga_negotiate_version()) {
                    return false;
                }
                c_puts("[VMSVGA] version="); dbg_u32(svga_version); c_putc('\n');

                fb = svga_read_reg(SVGA_REG_FB_START);
                if (fb == 0 || fb == 0xFFFFFFFFU) {
                    fb = bar1_fb;
                }
                c_puts("[VMSVGA] FB_START="); dbg_u32(svga_read_reg(SVGA_REG_FB_START));
                c_puts(" using fb="); dbg_u32(fb); c_putc('\n');

                if (fb == 0 || fb == 0xFFFFFFFFU || fb == 0xFFFFFFF0U) {
                    c_puts("[VMSVGA] no framebuffer address\n");
                    return false;
                }

                size = svga_read_reg(SVGA_REG_FB_SIZE);
                max_w = svga_read_reg(SVGA_REG_MAX_WIDTH);
                max_h = svga_read_reg(SVGA_REG_MAX_HEIGHT);

                c_puts("[VMSVGA] raw size="); dbg_u32(size);
                c_puts(" max_w="); dbg_u32(max_w);
                c_puts(" max_h="); dbg_u32(max_h); c_putc('\n');

                if (max_w == 0 || max_w < SVGA_MIN_WIDTH ||
                    max_w > SVGA_HARD_MAX_WIDTH) {
                    max_w = SVGA_FALLBACK_WIDTH;
                }
                if (max_h == 0 || max_h < SVGA_MIN_HEIGHT ||
                    max_h > SVGA_HARD_MAX_HEIGHT) {
                    max_h = SVGA_FALLBACK_HEIGHT;
                }

                if (size == 0) {
                    size = max_w * max_h * 4U;
                }

                svga_fb_addr = fb;
                svga_fb_size = size;
                svga_max_width = max_w;
                svga_max_height = max_h;
                vmware_svga_present = true;

                c_puts("[VMSVGA] DETECTED fb="); dbg_u32(svga_fb_addr);
                c_puts(" size="); dbg_u32(svga_fb_size);
                c_puts(" max="); dbg_u32(svga_max_width);
                c_puts("x"); dbg_u32(svga_max_height); c_putc('\n');

                return true;
            }
        }
    }

    return false;
}

bool vmware_svga_available(void)
{
    return vmware_svga_present && svga_fb_addr != 0;
}

void vmware_svga_disable(void)
{
    if (!vmware_svga_present) {
        return;
    }

    svga_write_reg(SVGA_REG_ENABLE, 0);
    svga_delay(1000U);

    vmware_svga_present = false;
    svga_version = 0;
    svga_fb_addr = 0;
    svga_fb_size = 0;
    svga_pitch = 0;
    svga_current_width = 0;
    svga_current_height = 0;
    svga_current_bpp = 0;
    svga_fifo_ready = false;
}

bool vmware_svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
    uint32_t saved_w;
    uint32_t saved_h;
    uint32_t saved_b;
    uint32_t tried_w;
    uint32_t tried_h;
    uint32_t tried_b;
    uint32_t w;
    uint32_t h;
    uint32_t b;

    c_puts("[VMSVGA] set_mode "); dbg_u32(width); c_puts("x");
    dbg_u32(height); c_puts(" bpp="); dbg_u32(bpp); c_putc('\n');

    if (!vmware_svga_available()) {
        return false;
    }

    saved_w = svga_current_width;
    saved_h = svga_current_height;
    saved_b = svga_current_bpp;

    w = width;
    h = height;
    b = bpp;
    svga_clamp_mode(&w, &h, &b);
    tried_w = w;
    tried_h = h;
    tried_b = b;

    if (svga_program_mode(w, h, b)) {
        return true;
    }

    w = SVGA_FALLBACK_WIDTH;
    h = SVGA_FALLBACK_HEIGHT;
    b = 32;
    svga_clamp_mode(&w, &h, &b);
    if (w != tried_w || h != tried_h || b != tried_b) {
        tried_w = w;
        tried_h = h;
        tried_b = b;
        if (svga_program_mode(w, h, b)) {
            return true;
        }
    }

    w = 800;
    h = 600;
    b = 32;
    svga_clamp_mode(&w, &h, &b);
    if (w != tried_w || h != tried_h || b != tried_b) {
        tried_w = w;
        tried_h = h;
        tried_b = b;
        if (svga_program_mode(w, h, b)) {
            return true;
        }
    }

    w = 640;
    h = 480;
    b = 32;
    svga_clamp_mode(&w, &h, &b);
    if (w != tried_w || h != tried_h || b != tried_b) {
        if (svga_program_mode(w, h, b)) {
            return true;
        }
    }

    /*
     * If a mode change fails after a previous mode was active, I try to give
     * the old mode back before surrendering the device.
     */
    if (saved_w != 0 && svga_mode_sane(saved_w, saved_h, saved_b)) {
        if (svga_program_mode(saved_w, saved_h, saved_b)) {
            return false;
        }
    }

    vmware_svga_disable();
    c_puts("[VMSVGA] no usable video mode\n");
    return false;
}

void vmware_svga_get_max_resolution(uint32_t *width, uint32_t *height)
{
    uint32_t w;
    uint32_t h;

    if (!width || !height) {
        return;
    }

    if (!vmware_svga_available()) {
        *width = 1920;
        *height = 1080;
        return;
    }

    w = svga_max_width;
    h = svga_max_height;

    if (w < SVGA_MIN_WIDTH || w > SVGA_HARD_MAX_WIDTH) {
        w = 1920;
    }
    if (h < SVGA_MIN_HEIGHT || h > SVGA_HARD_MAX_HEIGHT) {
        h = 1080;
    }

    *width = w;
    *height = h;
}

uint32_t vmware_svga_get_fb_addr(void)
{
    if (!vmware_svga_available()) {
        return 0;
    }
    return svga_fb_addr;
}

uint32_t vmware_svga_get_fb_size(void)
{
    if (!vmware_svga_available()) {
        return 0;
    }
    return svga_fb_size;
}

uint32_t vmware_svga_get_pitch(void)
{
    uint32_t pitch;
    uint32_t min_pitch;

    if (!vmware_svga_available()) {
        return 0;
    }
    if (svga_pitch != 0) {
        return svga_pitch;
    }
    if (svga_current_width == 0 || svga_current_bpp == 0) {
        return 0;
    }

    pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);
    min_pitch = svga_current_width * svga_bytes_per_pixel(svga_current_bpp);

    if (pitch < min_pitch || pitch > 0xFFFFU) {
        return 0;
    }

    return pitch;
}

/*
 * gpu_flush() calls this after copying the backbuffer into the LFB. The
 * UPDATE command is what makes Workstation present the changed region.
*/

void vmware_svga_update(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    svga_fifo_update(x, y, width, height);
}
