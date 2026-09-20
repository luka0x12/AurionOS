#include "fs_maintenance.h"

extern FSEntry fs_table[FS_MAX_FILES];
extern FileContent file_contents[FS_MAX_FILE_CONTENTS];
extern int fs_count, file_content_count;
extern int str_cmp(const char *a, const char *b);
extern bool ata_is_available(void);
extern int disk_write_lba(uint32_t lba, uint32_t count, const void *buffer);
extern int disk_read_lba_hdd(uint32_t lba, uint32_t count, void *buffer);
extern void sched_heartbeat(void);

int fs_normalize_directory(const char *path, char *out, uint32_t capacity)
{
    if (!path || !out || capacity < 2)
        return -1;
    if (path[0] && path[1] == ':')
        path += 2;
    if (*path != '/' && *path != '\\')
        return -1;
    uint32_t n = 0;
    while (*path) {
        char c = *path++;
        if (c == '\\') c = '/';
        if (c == '/' && n > 0 && out[n - 1] == '/')
            continue;
        if (n + 1 >= capacity) {
            out[0] = 0;
            return -1;
        }
        out[n++] = c;
    }
    while (n > 1 && out[n - 1] == '/')
        n--;
    out[n] = 0;
    return 0;
}

int fs_ensure_directory(const char *path)
{
    char canonical[FS_MAX_FILENAME];
    if (fs_normalize_directory(path, canonical, sizeof(canonical)) != 0)
        return -1;
    for (int i = 0; i < fs_count; i++) {
        char existing[FS_MAX_FILENAME];
        if (fs_normalize_directory(fs_table[i].name, existing, sizeof(existing)) == 0 &&
            str_cmp(existing, canonical) == 0)
            return fs_table[i].type == 1 ? 0 : -1;
    }
    if (fs_count >= FS_MAX_FILES)
        return -2;
    FSEntry *entry = &fs_table[fs_count++];
    for (uint32_t i = 0; i < sizeof(*entry); i++)
        ((uint8_t *)entry)[i] = 0;
    for (uint32_t i = 0; canonical[i]; i++)
        entry->name[i] = canonical[i];
    entry->type = 1;
    entry->attr = 0x10;
    entry->parent_idx = 0xFFFF;
    return 0;
}

/* Removing a directory aliases parent references to the retained directory.
 * Content slots still refer to file-table indices, so compact both together.
*/

static void remove_entry(int index, int replacement)
{
    for (int c = 0; c < file_content_count;) {
        if (file_contents[c].file_idx == index) {
            for (int j = c; j + 1 < file_content_count; j++)
                file_contents[j] = file_contents[j + 1];
            file_content_count--;
            for (uint32_t j = 0; j < sizeof(FileContent); j++)
                ((uint8_t *)&file_contents[file_content_count])[j] = 0;
            continue;
        }
        if (file_contents[c].file_idx > index)
            file_contents[c].file_idx--;
        c++;
    }
    for (int i = 0; i < fs_count; i++) {
        uint16_t parent = fs_table[i].parent_idx;
        if (parent == index)
            parent = replacement >= 0 ? (uint16_t)replacement : 0xFFFF;
        if (parent != 0xFFFF && parent > index)
            parent--;
        fs_table[i].parent_idx = parent;
    }
    for (int i = index; i + 1 < fs_count; i++)
        fs_table[i] = fs_table[i + 1];
    fs_count--;
    for (uint32_t i = 0; i < sizeof(FSEntry); i++)
        ((uint8_t *)&fs_table[fs_count])[i] = 0;
}

int fs_repair_duplicate_directories(void)
{
    int changed = 0;
    for (int i = 0; i < fs_count; i++) {
        if (fs_table[i].type != 1)
            continue;
        char canonical[FS_MAX_FILENAME];
        if (fs_normalize_directory(fs_table[i].name, canonical, sizeof(canonical)) != 0)
            continue;
        for (uint32_t k = 0; k < FS_MAX_FILENAME; k++) {
            char c = canonical[k];
            if (fs_table[i].name[k] != c) changed = 1;
            fs_table[i].name[k] = c;
            if (!c) break;
        }
        for (int j = 0; j < i; j++) {
            if (fs_table[j].type == 1 && str_cmp(fs_table[j].name, canonical) == 0) {
                remove_entry(i, j);
                changed = 1;
                i--;
                break;
            }
        }
    }
    return changed;
}

int fs_remove_starter_file(const char *path)
{
    int removed = 0;
    for (int i = 0; i < fs_count;) {
        if (fs_table[i].type == 0 && str_cmp(fs_table[i].name, path) == 0) {
            remove_entry(i, -1);
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

#define RESET_BATCH_SECTORS 16U
_Static_assert(FS_CONTENT_END_LBA <= 10000U, "AurionFS reset must not overlap raw system assets");
static uint8_t reset_zero[RESET_BATCH_SECTORS * 512U];
static uint8_t reset_verify[RESET_BATCH_SECTORS * 512U];
static bool reset_blocked;
static const char reset_magic[] = "AURION_RESET_V1";

bool fs_writes_blocked(void)
{
    return reset_blocked;
}

static int zero_sectors(uint32_t lba, uint32_t count)
{
    if (count == 0 || count > RESET_BATCH_SECTORS)
        return -1;
    if (disk_write_lba(lba, count, reset_zero) != 0 ||
        disk_read_lba_hdd(lba, count, reset_verify) != 0)
        return -1;
    for (uint32_t i = 0; i < count * 512U; i++) {
        if (reset_verify[i] != 0)
            return -1;
    }
    return 0;
}

static int finish_reset(void)
{
    /* All file records, users, metadata and allocated/unallocated content
     * slots are erased. Raw system assets start at LBA 10000. */
    for (uint32_t lba = FS_DATA_START_LBA; lba < FS_CONTENT_END_LBA;) {
        uint32_t count = FS_CONTENT_END_LBA - lba;
        if (count > RESET_BATCH_SECTORS) count = RESET_BATCH_SECTORS;
        if (zero_sectors(lba, count) != 0)
            return -1;
        lba += count;
        sched_heartbeat();
    }
    if (zero_sectors(FS_CURDIR_LBA, 1) != 0 ||
        zero_sectors(FS_VOLUME_LBA, 1) != 0)
        return -1;
    /* Clear the restart record LAST, only after all user data was verified. */
    if (zero_sectors(FS_RESET_LOG_LBA, 1) != 0)
        return -1;
    fs_clear_user_state();
    return 0;
}

int fs_reset_user_data(void)
{
    if (!ata_is_available())
        return -1;
    uint8_t record[512] = {0};
    for (uint32_t i = 0; i < sizeof(reset_magic); i++)
        record[i] = (uint8_t)reset_magic[i];
    reset_blocked = true;
    if (disk_write_lba(FS_RESET_LOG_LBA, 1, record) != 0 ||
        disk_read_lba_hdd(FS_RESET_LOG_LBA, 1, reset_verify) != 0)
        return -1;
    for (uint32_t i = 0; i < sizeof(record); i++) {
        if (record[i] != reset_verify[i])
            return -1;
    }
    /* Keep normal writes blocked until reboot, including if reset fails. */
    return finish_reset();
}

int fs_recover_reset(void)
{
    if (!ata_is_available())
        return 0;
    if (disk_read_lba_hdd(FS_RESET_LOG_LBA, 1, reset_verify) != 0) {
        reset_blocked = true;
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(reset_magic); i++) {
        if (reset_verify[i] != (uint8_t)reset_magic[i])
            return reset_blocked ? -1 : 0;
    }
    reset_blocked = true;
    if (finish_reset() != 0)
        return -1;
    reset_blocked = false;
    return 0;
}
