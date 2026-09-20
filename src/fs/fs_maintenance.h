#ifndef FS_MAINTENANCE_H
#define FS_MAINTENANCE_H

#include "fs.h"
#include <stdbool.h>

#define FS_MAX_USERS 32
#define FS_DATA_START_LBA 1000U
#define FS_FILE_TABLE_START_LBA FS_DATA_START_LBA
#define FS_USER_TABLE_START_LBA (FS_FILE_TABLE_START_LBA + FS_MAX_FILES)
#define FS_CONTENT_META_START_LBA (FS_USER_TABLE_START_LBA + FS_MAX_USERS)
#define FS_CONTENT_START_LBA (FS_CONTENT_META_START_LBA + FS_MAX_FILE_CONTENTS)
#define FS_CONTENT_END_LBA (FS_CONTENT_START_LBA + FS_MAX_FILES * 16U)
#define FS_CURDIR_LBA 499U
#define FS_VOLUME_LBA 998U
#define FS_RESET_LOG_LBA 999U

/* Stored directory names have one leading slash and no trailing slash. */
int fs_normalize_directory(const char *path, char *out, uint32_t capacity);
/* Creates or reuses a directory in RAM; caller persists the resulting state. */
int fs_ensure_directory(const char *path);
/* Repairs duplicate directories and updates content/parent indices in RAM. */
int fs_repair_duplicate_directories(void);
/* Removes one file from RAM; directories are never removed by this helper. */
int fs_remove_starter_file(const char *path);

bool fs_user_exists(const char *username);
/* Installer-only account update in RAM. Replacement requires explicit consent. */
int fs_setup_account(const char *username, const char *password, bool replace);
void fs_clear_user_state(void);

bool fs_writes_blocked(void);
/* Zero and verify every AurionFS user-data sector, then clear RAM state. */
int fs_reset_user_data(void);
/* Resume an interrupted reset before loading any users or files at boot. */
int fs_recover_reset(void);

#endif
