/*
 * VGA fallback driver for AurionOS.
 *
 * Programs VGA Mode 13h (320x200x256) and the 256-color DAC palette directly
 * through the VGA registers. This is the legacy path that vbe_graphics.c takes
 * when the VBE/VESA probe finds no usable mode ("[VGA] Falling back to Mode
 * 13h"), so it stays independent of the modern VBE driver.
 *
 * Also restores the saved text-mode screen when the shell boots back from a
 * GUI exit (gui_check_and_restore_screen).
*/

#include <stdint.h>

/* VGA port I/O. Everything here talks to the legacy VGA sequencer, CRTC,
   graphics controller and DAC through standard I/O ports, so only byte access
   is needed. */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Set VGA Mode 13h (320x200x256) by programming the VGA registers directly.
   Every register write is followed by a write to port 0x80, the conventional
   ISA I/O delay that gives slow VGA chips time to latch each value. */
void set_mode_13h(void)
{
    /* Miscellaneous Output Register */
    outb(0x3C2, 0x63);
    outb(0x80, 0);

    /* Sequencer registers */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03); /* Reset */
    outb(0x80, 0);
    outb(0x3C4, 0x01);
    outb(0x3C5, 0x01); /* Clocking Mode */
    outb(0x80, 0);
    outb(0x3C4, 0x02);
    outb(0x3C5, 0x0F); /* Map Mask */
    outb(0x80, 0);
    outb(0x3C4, 0x03);
    outb(0x3C5, 0x00); /* Character Map */
    outb(0x80, 0);
    outb(0x3C4, 0x04);
    outb(0x3C5, 0x0E); /* Memory Mode */
    outb(0x80, 0);

    /* Unlock CRTC */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & 0x7F);
    outb(0x80, 0);

    /* CRTC registers for 320x200 */
    static const uint8_t crtc[] = {
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
        0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF};
    for (int i = 0; i < 25; i++)
    {
        outb(0x3D4, i);
        outb(0x3D5, crtc[i]);
        outb(0x80, 0);
    }

    /* Graphics Controller */
    static const uint8_t gc[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF};
    for (int i = 0; i < 9; i++)
    {
        outb(0x3CE, i);
        outb(0x3CF, gc[i]);
        outb(0x80, 0);
    }

    /* Attribute Controller */
    inb(0x3DA); /* Reset flip-flop */
    static const uint8_t ac[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x41, 0x00, 0x0F, 0x00, 0x00};
    for (int i = 0; i < 21; i++)
    {
        inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, ac[i]);
        outb(0x80, 0);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20); /* Enable display */
}

/* Write one DAC palette entry (r/g/b are 8-bit, DAC takes 6-bit per channel). */
static void set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    outb(0x3C8, idx);
    outb(0x3C9, r >> 2);
    outb(0x3C9, g >> 2);
    outb(0x3C9, b >> 2);
}

/* Program the standard 256-color VGA palette: 16 EGA colors, a 16-step
   grayscale ramp, then a 6x6x6 color cube for the remainder. */
void setup_palette(void)
{
    /* Standard 16 colors */
    set_palette(0, 0, 0, 0);        /* Black */
    set_palette(1, 0, 0, 170);      /* Blue */
    set_palette(2, 0, 170, 0);      /* Green */
    set_palette(3, 0, 170, 170);    /* Cyan */
    set_palette(4, 170, 0, 0);      /* Red */
    set_palette(5, 170, 0, 170);    /* Magenta */
    set_palette(6, 170, 85, 0);     /* Brown */
    set_palette(7, 170, 170, 170);  /* Light Gray */
    set_palette(8, 85, 85, 85);     /* Dark Gray */
    set_palette(9, 85, 85, 255);    /* Light Blue */
    set_palette(10, 85, 255, 85);   /* Light Green */
    set_palette(11, 85, 255, 255);  /* Light Cyan */
    set_palette(12, 255, 85, 85);   /* Light Red */
    set_palette(13, 255, 85, 255);  /* Light Magenta */
    set_palette(14, 255, 255, 85);  /* Yellow */
    set_palette(15, 255, 255, 255); /* White */

    /* Grayscale ramp 16-31 */
    for (int i = 0; i < 16; i++)
    {
        uint8_t v = i * 17;
        set_palette(16 + i, v, v, v);
    }

    /* Color cube 32-255 */
    int idx = 32;
    for (int r = 0; r < 6 && idx < 256; r++)
    {
        for (int g = 0; g < 6 && idx < 256; g++)
        {
            for (int b = 0; b < 6 && idx < 256; b++)
            {
                set_palette(idx++, r * 51, g * 51, b * 51);
            }
        }
    }
}

/* Screen backup for GUI exit via reboot. The magic is set by the GUI before it
   hands control back through a reboot; the shell reads it on the next boot and
   restores the exact text screen from the low-memory backup region.
*/

#define VGA_TEXT_MEM ((volatile uint16_t *)0xB8000)
#define SCREEN_BACKUP_ADDR ((volatile uint16_t *)0x90000)   /* Safe area in low memory */
#define GUI_REBOOT_FLAG_ADDR ((volatile uint32_t *)0x9F000) /* Flag location */
#define GUI_CURSOR_ROW_ADDR ((volatile uint32_t *)0x9F004)  /* Cursor row backup */
#define GUI_CURSOR_COL_ADDR ((volatile uint32_t *)0x9F008)  /* Cursor col backup */
#define GUI_REBOOT_MAGIC 0x47554952                         /* "GUIR" - GUI Reboot magic */

/* External cursor variables from io.asm */
extern uint32_t cursor_row;
extern uint32_t cursor_col;

/* External function to update hardware cursor */
extern void set_cursor_hardware(void);

/* Restore text screen after reboot (called from shell init) */
int gui_check_and_restore_screen(void)
{
    if (*GUI_REBOOT_FLAG_ADDR == GUI_REBOOT_MAGIC)
    {
        /* Clear the flag first */
        *GUI_REBOOT_FLAG_ADDR = 0;

        /* Restore the screen exactly as it was */
        for (int i = 0; i < 80 * 25; i++)
        {
            VGA_TEXT_MEM[i] = SCREEN_BACKUP_ADDR[i];
        }

        /* Restore cursor position */
        cursor_row = *GUI_CURSOR_ROW_ADDR;
        cursor_col = *GUI_CURSOR_COL_ADDR;

        /* Update hardware cursor to match */
        set_cursor_hardware();

        return 1; /* Restored */
    }
    return 0; /* Normal boot */
}