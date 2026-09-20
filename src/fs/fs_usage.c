/* fs_usage.c - real disk usage for the task manager and df/chkdsk.
 *
 * used  = sum of fs_table[].size; empty slots contribute 0, a 12 KB file
 *         contributes 12 KB. Same number `du` would give for user data.
 * total = IDENTIFY DEVICE word 60/61 (LBA28) or 100-103 (LBA48) times 512,
 *         probed once and cached. A 2 GB image reports 2 GB; nobody has to
 *         keep a #define in sync with the Makefile any more.
 *         If the probe fails (no IDE device, timeout) we fall back to the
 *         build default and say so via fs_capacity_source(), so the UI
 *         never presents a guess as a measurement.
*/

#include <stdint.h>
#include <stdbool.h>
#include "fs.h"

extern FSEntry fs_table[];
extern int     fs_count;
extern bool    fat32_usage_bytes(uint64_t *used_bytes, uint64_t *total_bytes);

#define FS_BUILD_DEFAULT_BYTES (60u * 1024u * 1024u) /* keep == Makefile HDD_SIZE_MB */

#define ATA_STATUS  0x1F7
#define ATA_COMMAND 0x1F7
#define ATA_DATA    0x1F0
#define ATA_DRVHD   0x1F6
#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01
#define ATA_CMD_ID  0xEC

static uint8_t ata_in(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void ata_out(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static uint16_t ata_inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* IDENTIFY is read-only as far as device state goes; safe to issue once
 * after ata.c has finished its own init. Bounded waits, never a hang. */
static uint64_t ata_identify_sectors(void) {
    ata_out(ATA_DRVHD, 0xA0);          /* master */
    for (int i = 0; i < 100000; i++)   /* ~enough even on a slow emulator */
        if (!(ata_in(ATA_STATUS) & ATA_SR_BSY)) break;

    ata_out(ATA_COMMAND, ATA_CMD_ID);

    int waited = 0;
    for (;;) {
        uint8_t st = ata_in(ATA_STATUS);
        if (st & ATA_SR_ERR) return 0;             /* not present */
        if (st & ATA_SR_DRQ) break;
        if (!(st & ATA_SR_BSY) && waited > 200000) return 0;
        waited++;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = ata_inw(ATA_DATA);

    uint64_t lba48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[101] << 16) | (uint64_t)id[100];
    if (lba48 > 0) return lba48;
    uint32_t lba28 = ((uint32_t)id[61] << 16) | id[60];
    if (lba28 > 0) return lba28;
    /* ancient CHS-only device */
    uint32_t cyl = id[1], heads = id[3], sect = id[6];
    if (cyl && heads && sect) return (uint64_t)cyl * heads * sect;
    return 0;
}

static uint64_t g_total_bytes = 0;
static bool     g_from_identify = false;

static void capacity_ensure(void) {
    if (g_total_bytes) return;
    uint64_t s = ata_identify_sectors();
    if (s) { g_total_bytes = s * 512u; g_from_identify = true; }
    else   { g_total_bytes = FS_BUILD_DEFAULT_BYTES; g_from_identify = false; }
}

const char *fs_capacity_source(void) {
    capacity_ensure();
    return g_from_identify ? "ata identify" : "build default";
}

void fs_get_usage(uint32_t *used_kb, uint32_t *total_kb) {
    /* allocated FAT clusters vs filesystem capacity: measured from the
     * disk itself. The file-table byte sum is NOT disk usage (the table
     * only mirrors small files), so a zero here means "no measurement",
     * never "empty disk". Callers show n/a for 0/0.
    */
    uint64_t used_b = 0, total_b = 0;
    if (fat32_usage_bytes(&used_b, &total_b) && total_b > 0) {
        *used_kb  = (uint32_t)(used_b / 1024u);
        *total_kb = (uint32_t)(total_b / 1024u);
        return;
    }
    capacity_ensure();
    (void)fs_count;
    *used_kb  = 0;
    *total_kb = 0;
}

/* apps ask, never poke fs_table directly */
int fs_iterate(int cursor, const char **name, uint32_t *size, int *type) {
    for (int i = cursor; i < fs_count; i++) {
        if (fs_table[i].name[0] == 0) continue;
        if (name) *name = fs_table[i].name;
        if (size) *size = fs_table[i].size;
        if (type) *type = fs_table[i].type;
        return i + 1;
    }
    return -1;
}
