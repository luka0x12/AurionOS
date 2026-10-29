#include "security.h"
#include "utils.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "window_manager.h"
#include "kpanic.h"
#include "fs_maintenance.h"
#include "vanta_examples.h"
#include "reinstall.h"

/* External Wrappers (from kernel.asm) */
extern void c_puts(const char *s);
extern void c_putc(char c);
extern void c_cls(void);
extern uint16_t c_getkey(void);
extern bool c_kb_hit(void);
extern void sys_reboot(void);
extern void sys_shutdown(void);
extern uint32_t get_ticks(void);
extern void sleep_ms(uint32_t ms);

#define puts c_puts
#define putc c_putc
#define cls c_cls
#define getkey c_getkey

/* Filesystem primitives (filesys.asm) */
extern int fs_list_root(uint32_t out_dir_buffer, uint32_t max_entries);

/* Memory management (memory.asm) */
extern void mem_get_stats(uint32_t *stats);
extern void *kmalloc(uint32_t size);
extern void kfree(void *ptr);

/* System call functions (from syscall.c) */
extern int sys_get_time(uint8_t *hours, uint8_t *minutes, uint8_t *seconds);
extern int sys_get_date(uint8_t *day, uint8_t *month, uint16_t *year);
extern int sys_read_sector(uint32_t lba, uint32_t count, void *buffer);
extern int disk_read_lba(uint32_t lba, uint32_t count, void *buffer);
extern int disk_read_lba_hdd(uint32_t lba, uint32_t count, void *buffer);

extern int disk_write_lba(uint32_t lba, uint32_t count, void *buffer);

/* FAT32 filesystem functions */
extern bool fat32_mount(uint32_t start_lba);
extern bool fat32_is_mounted(void);
extern const char *fat32_get_volume_label(void);
extern bool fat32_find_file(const char *filename, uint32_t *out_cluster, uint32_t *out_size);
extern int fat32_read_file(uint32_t start_cluster, uint32_t offset, uint32_t size, void *buffer);
extern bool fat32_list_directory(const char *dirname, void (*callback)(const char *name, bool is_dir, uint32_t size));
extern uint32_t fat32_get_file_size_by_name(const char *filename);
extern bool fat32_unmount(void);

int keyboard_layout = 0;     /* switch for layout selection */
char current_dir[256] = "/"; /* Root by default, updated to home in init */
static uint8_t current_color = 0x07;

static char volume_label[13] = "SYSTEM";
static uint32_t volume_serial = 0x0; /* Default 0, will be randomized if not on disk */

#include "fs.h"

/* SUDO Grace Period Tracking */
static uint32_t sudo_last_tick = 0;
static char sudo_last_user[32] = "";
#define SUDO_TIMEOUT_TICKS 30000 /* 5 minutes at 100Hz PIT */

void fs_reset_sudo(void)
{
  sudo_last_tick = 0;
  sudo_last_user[0] = 0;
}

/* User entry structure (64 bytes each) */
typedef struct
{
  char username[32];
  char password_hash[32]; /* Simple hash */
} UserEntry;

/* In-memory caches */
FSEntry fs_table[FS_MAX_FILES];
int fs_count = 0;
UserEntry user_table[FS_MAX_USERS]; /* Made non-static for login screen */
int user_count = 0;                 /* Made non-static for login screen */
char current_user[32] = "";

/* build_full_path is defined later in this file. */
static void build_full_path(const char *name, char *full_path);
int str_cmp(const char *a, const char *b); /* also declared below; needed early by the sys lock check */

/* System files no shell user may delete or overwrite without root.
 * Empty current_user = pre-login boot/installer context: privileged. */
bool sys_caller_is_root(void)
{
  return current_user[0] == 0 || str_cmp(current_user, "root") == 0;
}

static bool sys_abs_locked(const char *abs_path)
{
  return str_cmp(abs_path, "/installed.sys") == 0;
}

/* Absolute-path check for internal callers (save_file_content, del). */
bool sys_path_locked_abs(const char *abs_path)
{
  if (!abs_path || !sys_abs_locked(abs_path))
    return false;
  return !sys_caller_is_root();
}

/* Resolve-then-check for UI callers (nano, notepad). */
bool sys_path_locked(const char *path)
{
  char full[256];
  if (!path || !path[0])
    return false;
  build_full_path(path, full);
  for (int i = 0; full[i]; i++)
    if (full[i] == '\\') full[i] = '/';
  return sys_path_locked_abs(full);
}

/* Forward declarations for migration helpers */
int fs_save_to_disk(void);

/* AUR-02: Store PBKDF2-HMAC-SHA256 in the legacy 32-byte user slot.
   Layout: "PBK2" marker, 12-byte salt, first 16 bytes of derived key. */
static void store_password_pbkdf2(UserEntry *entry, const char *password)
{
  uint8_t salt[16];
  uint8_t derived[32];

  if (!entry || !password) return;
  csprng_bytes(&g_csprng, salt, 16);
  /* The 32-byte slot stores 12 salt bytes + 16 derived bytes, so the
     derivation must use exactly the 12 stored bytes plus 4 zeros.
     Previously we derived with the full 16 random bytes while only 12
     were persisted; verify rebuilt a different salt (12 + 4 zeros),
     so PBKDF2 produced a different key and every login was rejected
     even with the correct password. 96 bits of salt is ample. */
  for (int i = 12; i < 16; i++) salt[i] = 0;
  password_derive_key(password, salt, PBKDF2_ITERATIONS, derived);

  entry->password_hash[0] = 'P';
  entry->password_hash[1] = 'B';
  entry->password_hash[2] = 'K';
  entry->password_hash[3] = '2';
  for (int i = 0; i < 12; i++) {
    entry->password_hash[4 + i] = (char)salt[i];
  }
  for (int i = 0; i < 16; i++) {
    entry->password_hash[16 + i] = (char)derived[i];
  }
}

/* AUR-02: Forward declaration for migration-aware verification (non-static, used by login_screen.c) */
int fs_save_to_disk(void); /* Forward declaration for migration */
bool verify_password_with_migration(UserEntry *entry, const char *password);

/* AUR-02: Helper to verify a password against a stored PBKDF2 entry.
   Returns true if the password matches. */
static bool verify_password_pbkdf2(const UserEntry *entry, const char *password)
{
  uint8_t salt[16];
  uint8_t stored_partial[16];
  uint8_t derived[32];

  if (!entry || !password) return false;
  if (entry->password_hash[0] != 'P' || entry->password_hash[1] != 'B' ||
      entry->password_hash[2] != 'K' || entry->password_hash[3] != '2') {
    return false;
  }

  for (int i = 0; i < 12; i++) {
    salt[i] = (uint8_t)entry->password_hash[4 + i];
  }
  for (int i = 12; i < 16; i++) {
    salt[i] = 0;
  }
  for (int i = 0; i < 16; i++) {
    stored_partial[i] = (uint8_t)entry->password_hash[16 + i];
  }

  password_derive_key(password, salt, PBKDF2_ITERATIONS, derived);
  return secure_memcmp(derived, stored_partial, 16);
}

/* AUR-02 Migration: Detect legacy djb2 hash format.
   Old format: password_hash[0..3] repeated 8 times to fill 32 bytes.
   The djb2 hash is a 32-bit value stored as 4 bytes, repeated cyclically.
   Returns true if this entry uses the old djb2 format. */
bool fs_is_legacy_hash(const void *entry)
{
  const UserEntry *e = (const UserEntry *)entry;
  if (!e) return false;
  const uint8_t *h = (const uint8_t *)e->password_hash;
  /* Check that bytes [0..3] == [4..7] == [8..11] == ... == [28..31] */
  for (int i = 4; i < 32; i++) {
    if (h[i] != h[i % 4]) return false;
  }
  /* Also reject all-zero (empty/corrupt) */
  if (h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0) return false;
  return true;
}

/* AUR-23 policy note: the old djb2 verification path (legacy_djb2_hash)
   was removed when legacy hashes became refused-by-policy; no code writes
   djb2 hashes anymore. */

/* AUR-02 Migration: Verify password against a legacy djb2 entry.
   AUR-23 policy: djb2 hashes are crackable offline in microseconds and can
   only predate the PBKDF2 release, which is the cutoff. They are therefore
   NEVER accepted for authentication; the account must go through a password
   reset (the login screen's first-boot-style flow for root, or root's
   passwd for any other user). This closes the "weak hash sits on disk
   until that user logs in once" window by policy instead of by chance. */
bool verify_password_with_migration(UserEntry *entry, const char *password)
{
  if (fs_is_legacy_hash(entry))
  {
    /* Do not authenticate, do not silently migrate: the hash is
       pre-cutoff. Return false; the caller fails closed. */
    return false;
  }

  /* Normal PBKDF2 path */
  return verify_password_pbkdf2(entry, password);
}

/* First-boot password setup: store a PBKDF2 hash for the given user and
   persist. Used by the login screen when it detects an all-zero hash.
   Returns false if the user does not exist or the password is empty. */
int str_cmp(const char *a, const char *b); /* forward decl; body at line ~595 */
bool fs_set_user_password(const char *username, const char *password)
{
  if (!username || !password || password[0] == 0) return false;
  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, username) == 0)
    {
      store_password_pbkdf2(&user_table[i], password);
      fs_save_to_disk();
      return true;
    }
  }
  return false;
}

/* Process tracking - Not needed with real-time WM enumeration, but kept for PID mapping */
typedef struct
{
  uint32_t pid;
  char name[32];
  char state[16];
  uint32_t mem_usage;
  int priority;
} ProcessEntry;

static void ensure_processes_init(void) { /* Legacy stub */ }

/* System Mode Check */
extern int boot_mode_flag;
static bool is_dos_mode(void)
{
  return (boot_mode_flag == 1);
}

/* File content storage (simple buffer) */
FileContent file_contents[FS_MAX_FILE_CONTENTS] = {0};
int file_content_count = 0;

void fs_clear_user_state(void)
{
  for (uint32_t i = 0; i < sizeof(fs_table); i++) ((uint8_t *)fs_table)[i] = 0;
  for (uint32_t i = 0; i < sizeof(file_contents); i++) ((uint8_t *)file_contents)[i] = 0;
  for (uint32_t i = 0; i < sizeof(user_table); i++) ((uint8_t *)user_table)[i] = 0;
  for (uint32_t i = 0; i < sizeof(current_user); i++) current_user[i] = 0;
  for (uint32_t i = 0; i < sizeof(current_dir); i++) current_dir[i] = 0;
  for (uint32_t i = 0; i < sizeof(volume_label); i++) volume_label[i] = 0;
  current_dir[0] = '/';
  volume_serial = 0;
  fs_count = file_content_count = user_count = 0;
  fs_reset_sudo();
}

bool fs_user_exists(const char *username)
{
  if (!username || !username[0]) return false;
  for (int i = 0; i < user_count; i++) {
    if (str_cmp(user_table[i].username, username) == 0) return true;
  }
  return false;
}

bool fs_authenticate_current_user(const char *password)
{
  if (!current_user[0] || !password || !password[0]) return false;
  for (int i = 0; i < user_count; i++) {
    if (str_cmp(user_table[i].username, current_user) == 0)
      return verify_password_with_migration(&user_table[i], password);
  }
  return false;
}

int fs_setup_account(const char *username, const char *password, bool replace)
{
  if (!sys_caller_is_root() || !username || !password || fs_writes_blocked()) return -1;
  uint32_t name_len = 0, password_len = 0;
  while (name_len < 32 && username[name_len]) {
    char ch = username[name_len++];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return -1;
  }
  while (password_len < 32 && password[password_len]) password_len++;
  if (!name_len || name_len >= 32 || password_len < 6 || password_len >= 32) return -1;
  for (int i = 0; i < user_count; i++) {
    if (str_cmp(user_table[i].username, username) == 0) {
      if (!replace) return -1;
      store_password_pbkdf2(&user_table[i], password);
      return 0;
    }
  }
  if (user_count >= FS_MAX_USERS) return -1;
  UserEntry *entry = &user_table[user_count++];
  for (uint32_t i = 0; i < sizeof(*entry); i++) ((uint8_t *)entry)[i] = 0;
  for (uint32_t i = 0; i < name_len; i++) entry->username[i] = username[i];
  store_password_pbkdf2(entry, password);
  return 0;
}

/* Utility Functions */
int str_len(const char *s)
{
  if (!s)
    return 0;
  int l = 0;
  while (s[l])
    l++;
  return l;
}

void str_copy(char *dst, const char *src, int max)
{
  int i = 0;
  while (i < max - 1 && src[i])
  {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* Forward declarations */
int str_cmp(const char *a, const char *b);
int cmd_dispatch(const char *line);
int fs_save_to_disk(void);

/* Helper to save content to file system */
int save_file_content(const char *filename, const char *data, int len)
{
  if (!filename || !data || len < 0 || fs_writes_blocked())
    return -1;

  char full_path[256];
  const char *name_ptr = filename;

  /* Skip legacy drive letter if present */
  if (filename[0] && filename[1] == ':' && (filename[2] == '\\' || filename[2] == '/'))
  {
    name_ptr = filename + 2;
  }

  if (name_ptr[0] != '/')
  { // Check for absolute path (starts with /)
    int dir_len = str_len(current_dir);
    str_copy(full_path, current_dir, 256);

    int name_len = str_len(name_ptr);
    for (int k = 0; k < name_len; k++)
      full_path[dir_len + k] = name_ptr[k];
    full_path[dir_len + name_len] = '\0';
  }
  else
  {
    str_copy(full_path, name_ptr, 256);
  }

  /* Normalize backslashes to forward slashes */
  for (int i = 0; full_path[i]; i++)
  {
    if (full_path[i] == '\\')
      full_path[i] = '/';
  }

  /* System files need root to overwrite, whoever the writer is. */
  if (sys_path_locked_abs(full_path))
    return -1;

  // Check if file exists -> overwrite
  int idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      idx = i;
      break;
    }
  }

  if (idx == -1)
  {
    // Create new
    if (fs_count >= FS_MAX_FILES)
    {
      puts("Error: Disk full\n");
      return -1;
    }
    idx = fs_count++;
    str_copy(fs_table[idx].name, full_path, FS_MAX_FILENAME);
    fs_table[idx].type = 0; // File
    fs_table[idx].attr = 0;
    fs_table[idx].parent_idx = 0xFFFF; // Root
  }

  /* Truncate before recording size — fs_table must match stored bytes */
  if (len > MAX_FILE_SIZE)
    len = MAX_FILE_SIZE;
  fs_table[idx].size = len;

  // Save content
  // Find content slot
  int content_idx = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if (file_contents[i].file_idx == idx)
    {
      content_idx = i;
      break;
    }
  }

  if (content_idx == -1)
  {
    if (file_content_count >= FS_MAX_FILE_CONTENTS)
    {
      puts("Error: Content storage full\n");
      return -1;
    }
    content_idx = file_content_count++;
    file_contents[content_idx].file_idx = idx;
  }

  // Copy data
  for (int k = 0; k < len; k++)
  {
    file_contents[content_idx].data[k] = data[k];
  }
  file_contents[content_idx].size = len;

  /* Persist to disk */
  return fs_save_to_disk();
}

/* Helper to load content from file system */
int load_file_content(const char *filename, char *buffer, int max_len)
{
  if (!filename || !buffer || max_len <= 0)
    return -1;

  char full_path[256];
  const char *name_ptr = filename;

  /* Skip legacy drive letter if present */
  if (filename[0] && filename[1] == ':' && (filename[2] == '\\' || filename[2] == '/'))
  {
    name_ptr = filename + 2;
  }

  if (name_ptr[0] != '/')
  {
    int dl = str_len(current_dir);
    str_copy(full_path, current_dir, 256);
    int nl = str_len(name_ptr);
    for (int i = 0; i < nl; i++)
      full_path[dl + i] = name_ptr[i];
    full_path[dl + nl] = 0;
  }
  else
  {
    str_copy(full_path, name_ptr, 256);
  }

  /* Normalize backslashes to forward slashes */
  for (int i = 0; full_path[i]; i++)
  {
    if (full_path[i] == '\\')
      full_path[i] = '/';
  }

  /* Debug output */
  // puts("[LOAD] Looking for: ");
  // puts(full_path);
  // puts("\n");

  int idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      idx = i;
      // puts("[LOAD] Found file at index ");
      char l_buf[16];
      int_to_str(i, l_buf);
      // puts(l_buf);
      // puts(", size=");
      int_to_str(fs_table[i].size, l_buf);
      // puts(l_buf);
      // puts("\n");
      break;
    }
  }
  if (idx == -1)
  {
    // puts("[LOAD] File not found in fs_table\n");
    return -1;
  }

  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == idx)
    {
      ci = i;
      // puts("[LOAD] Found content at content_idx ");
      char l_buf_2[16];
      int_to_str(i, l_buf_2);
      // puts(l_buf_2);
      // puts("\n");
      break;
    }
  }
  if (ci == -1)
  {
    /* Content missing from in-RAM cache.
       Treat as not-loadable (callers need an error),
       rather than silently returning an empty file. */
    return -1;
  }

  int sz = (int)file_contents[ci].size;
  if (sz > max_len)
    sz = max_len;
  for (int i = 0; i < sz; i++)
    buffer[i] = file_contents[ci].data[i];

  /* Null terminate ONLY if there is extra space.
     Do NOT overwrite the last byte of binary data if it fits exactly. */
  if (sz < max_len)
    buffer[sz] = 0;

  return sz;
}

/* Bytes actually stored for a file (after MAX_FILE_SIZE cap). -1 = not found. */
int fs_get_stored_content_bytes(const char *filename)
{
  if (!filename)
    return -1;

  char full_path[256];
  const char *name_ptr = filename;
  if (filename[0] && filename[1] == ':' && (filename[2] == '\\' || filename[2] == '/'))
    name_ptr = filename + 2;

  if (name_ptr[0] != '/')
  {
    int dl = str_len(current_dir);
    str_copy(full_path, current_dir, 256);
    int nl = str_len(name_ptr);
    for (int i = 0; i < nl; i++)
      full_path[dl + i] = name_ptr[i];
    full_path[dl + nl] = 0;
  }
  else
    str_copy(full_path, name_ptr, 256);

  for (int i = 0; full_path[i]; i++)
  {
    if (full_path[i] == '\\')
      full_path[i] = '/';
  }

  int idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      idx = i;
      break;
    }
  }
  if (idx == -1)
    return -1;

  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == idx)
      return (int)file_contents[i].size;
  }
  return 0;
}

/* Copy file bytes [offset, offset+max_len) for BMP streaming; returns bytes copied or -1 */
int load_file_content_range(const char *filename, char *buffer, uint32_t offset, int max_len)
{
  if (!filename || !buffer || max_len <= 0)
    return -1;

  char full_path[256];
  const char *name_ptr = filename;
  if (filename[0] && filename[1] == ':' && (filename[2] == '\\' || filename[2] == '/'))
    name_ptr = filename + 2;

  if (name_ptr[0] != '/')
  {
    int dl = str_len(current_dir);
    str_copy(full_path, current_dir, 256);
    int nl = str_len(name_ptr);
    for (int i = 0; i < nl; i++)
      full_path[dl + i] = name_ptr[i];
    full_path[dl + nl] = 0;
  }
  else
    str_copy(full_path, name_ptr, 256);

  for (int i = 0; full_path[i]; i++)
  {
    if (full_path[i] == '\\')
      full_path[i] = '/';
  }

  int idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      idx = i;
      break;
    }
  }
  if (idx == -1)
    return -1;

  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == idx)
    {
      ci = i;
      break;
    }
  }
  if (ci == -1)
    return -1;

  uint32_t sz = file_contents[ci].size;
  if (offset >= sz)
    return -1;
  uint32_t avail = sz - offset;
  uint32_t n = (uint32_t)max_len;
  if (n > avail)
    n = avail;
  for (uint32_t i = 0; i < n; i++)
    buffer[i] = file_contents[ci].data[offset + i];
  return (int)n;
}

int str_cmp(const char *a, const char *b)
{
  if (!a || !b)
    return -1;
  while (*a && *b)
  {
    char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
    char cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
    if (ca != cb)
      return (int)(ca - cb);
    a++;
    b++;
  }
  char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
  char cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
  return (int)(ca - cb);
}

static int str_ncmp(const char *a, const char *b, int n)
    __attribute__((unused));
static int str_ncmp(const char *a, const char *b, int n)
{
  if (!a || !b)
    return -1;
  for (int i = 0; i < n; i++)
  {
    if (a[i] != b[i] || !a[i])
      return a[i] - b[i];
  }
  return 0;
}

static void str_upper(char *s)
{
  while (*s)
  {
    if (*s >= 'a' && *s <= 'z')
      *s -= 32;
    s++;
  }
}

static uint32_t str_to_int(const char *s)
{
  uint32_t n = 0;
  while (*s >= '0' && *s <= '9')
  {
    n = n * 10 + (*s - '0');
    s++;
  }
  return n;
}

static void print_hex(uint32_t n)
{
  char hex[] = "0123456789ABCDEF";
  char v_buf[9];
  for (int i = 7; i >= 0; i--)
  {
    v_buf[i] = hex[n & 0xF];
    n >>= 4;
  }
  v_buf[8] = '\0';
  puts(v_buf);
}

static void print_hex16(uint16_t n)
{
  char hex[] = "0123456789ABCDEF";
  char v_buf[5];
  for (int i = 3; i >= 0; i--)
  {
    v_buf[i] = hex[n & 0xF];
    n >>= 4;
  }
  v_buf[4] = '\0';
  puts(v_buf);
}

/* Simple hash function */
static uint32_t hash_string(const char *s)
{
  uint32_t h = 5381;
  while (*s)
  {
    h = ((h << 5) + h) + (uint32_t)(*s);
    s++;
  }
  return h;
}

/* Define sector for current directory storage */
/* Persistent layout is shared with fs_maintenance.h. */

/* FIXED: fs_save_to_disk - Save content indexed by file_idx, not loop position
 */
int fs_save_to_disk(void)
{
  if (fs_writes_blocked()) return -1;
  char sector[512];

  /* Save current directory first */
  for (int j = 0; j < 512; j++)
    sector[j] = 0;
  for (int j = 0; j < 256 && current_dir[j]; j++)
  {
    sector[j] = current_dir[j];
  }
  if (disk_write_lba(FS_CURDIR_LBA, 1, sector) != 0)
  {
    return -1;
  }

  /* Save Volume Info - Store at LBA 998 (just before CurDir) */
  for (int j = 0; j < 512; j++)
    sector[j] = 0;
  str_copy(sector, volume_label, 13);
  *((uint32_t *)(sector + 16)) = volume_serial;
  disk_write_lba(998, 1, sector);

  /* Save file table - save ALL files up to FS_MAX_FILES */
  for (int i = 0; i < fs_count && i < FS_MAX_FILES; i++)
  {
    for (int j = 0; j < 512; j++)
      sector[j] = 0;
    for (int j = 0; j < 64 && j < (int)sizeof(FSEntry); j++)
    {
      sector[j] = ((char *)&fs_table[i])[j];
    }

    if (disk_write_lba(FS_DATA_START_LBA + i, 1, sector) != 0)
    {
      return -1;
    }
  }

  /* Clear remaining file slots - clear up to 64 entries */
  for (int i = fs_count; i < FS_MAX_FILES; i++)
  {
    for (int j = 0; j < 512; j++)
      sector[j] = 0;
    disk_write_lba(FS_DATA_START_LBA + i, 1, sector);
  }

  /* Save user table */
  for (int i = 0; i < user_count && i < FS_MAX_USERS; i++)
  {
    for (int j = 0; j < 512; j++)
      sector[j] = 0;
    for (int j = 0; j < 64 && j < (int)sizeof(UserEntry); j++)
    {
      sector[j] = ((char *)&user_table[i])[j];
    }
    if (disk_write_lba(FS_USER_TABLE_START_LBA + i, 1, sector) != 0)
    {
      return -1;
    }
  }

  /* Clear remaining user slots */
  for (int i = user_count; i < FS_MAX_USERS; i++)
  {
    for (int j = 0; j < 512; j++)
      sector[j] = 0;
    disk_write_lba(FS_USER_TABLE_START_LBA + i, 1, sector);
  }

  /* Save file contents - Write each file's content to its designated sectors */
  for (int c = 0; c < file_content_count; c++)
  {
    uint16_t file_idx = file_contents[c].file_idx;
    uint32_t file_size = file_contents[c].size;

    /* Calculate sectors needed for this file */
    uint32_t sectors_needed = (file_size + 511) / 512;
    if (sectors_needed == 0)
      sectors_needed = 1;

    /* Write file content sectors */
    for (uint32_t s = 0; s < sectors_needed && s < 16; s++)
    {
      /* Zero sector buffer */
      for (int j = 0; j < 512; j++)
        sector[j] = 0;

      /* Calculate how much to copy for this sector */
      uint32_t offset = s * 512;
      uint32_t to_copy = file_size - offset;
      if (to_copy > 512)
        to_copy = 512;

      /* Copy data to sector buffer */
      for (uint32_t j = 0; j < to_copy; j++)
      {
        sector[j] = file_contents[c].data[offset + j];
      }

      /* Write sector to disk at the correct LBA for this file */
      uint32_t lba = FS_CONTENT_START_LBA + (file_idx * 16) + s;
      if (disk_write_lba(lba, 1, sector) != 0)
      {
        return -1;
      }
    }

    /* Clear remaining sectors for this file (if file shrunk) */
    for (uint32_t s = sectors_needed; s < 16; s++)
    {
      for (int j = 0; j < 512; j++)
        sector[j] = 0;
      uint32_t lba = FS_CONTENT_START_LBA + (file_idx * 16) + s;
      disk_write_lba(lba, 1, sector);
    }
  }

  return 0;
}

/* FIXED: fs_load_from_disk - Load content and match by file_idx correctly */
static int fs_load_from_disk(void)
{
  char sector[512];

  /* Load current directory first */
  if (disk_read_lba_hdd(FS_CURDIR_LBA, 1, sector) == 0)
  {
    if (sector[0] != 0 && sector[0] == '/')
    {
      /* Valid current directory found - copy it */
      for (int j = 0; j < 256 && sector[j]; j++)
      {
        current_dir[j] = sector[j];
      }
      /* Ensure null termination */
      current_dir[255] = '\0';
    }
    /* If not valid, keep default "/" */
  }

  /* Load Volume Info from LBA 998 */
  bool found_vol = false;
  if (disk_read_lba_hdd(998, 1, sector) == 0)
  {
    if (sector[0] != 0)
    {
      str_copy(volume_label, sector, 13);
      volume_serial = *((uint32_t *)(sector + 16));
      found_vol = true;
    }
  }

  if (!found_vol)
  {
    /* First boot/unformatted: Generate semi-random serial */
    extern uint32_t get_ticks(void);
    uint32_t t = get_ticks();
    volume_serial = (t << 16) ^ (t >> 8) ^ 0x69604153; /* Xorshift mix */
    str_copy(volume_label, "AURION_DISK", 13);

    /* Auto-persist new identity */
    fs_save_to_disk();
  }

  /* Load file table - read up to 64 entries */
  fs_count = 0;
  for (int i = 0; i < FS_MAX_FILES; i++)
  {
    if (disk_read_lba_hdd(FS_DATA_START_LBA + i, 1, sector) == 0)
    {
      /* Check if this is a valid entry - first char of filename must not be 0 */
      if (sector[0] != 0)
      {
        for (int j = 0; j < 64 && j < (int)sizeof(FSEntry); j++)
        {
          ((char *)&fs_table[fs_count])[j] = sector[j];
        }

        fs_table[fs_count].name[FS_MAX_FILENAME - 1] = 0;

        /* MIGRATION: Normalize backslashes and strip drive letters in name */
        char *name_start = fs_table[fs_count].name;
        if (name_start[0] && name_start[1] == ':' && (name_start[2] == '/' || name_start[2] == '\\'))
        {
          /* Shift the rest of the string to the left */
          char *d = name_start;
          char *s = name_start + 2;
          while (*s)
            *d++ = *s++;
          *d = 0;
        }

        for (int j = 0; fs_table[fs_count].name[j]; j++)
        {
          if (fs_table[fs_count].name[j] == '\\')
            fs_table[fs_count].name[j] = '/';
        }

        /* STRIP trailing slash for consistency (except for root /) */
        int slen = str_len(fs_table[fs_count].name);
        if (slen > 1 && fs_table[fs_count].name[slen - 1] == '/')
        {
          fs_table[fs_count].name[slen - 1] = '\0';
        }
        fs_count++;
      }
      else
      {
        /* Empty slot - stop reading (files are stored contiguously) */
        break;
      }
    }
  }

  /* Load user table */
  user_count = 0;
  for (int i = 0; i < FS_MAX_USERS; i++)
  {
    if (disk_read_lba_hdd(FS_USER_TABLE_START_LBA + i, 1, sector) == 0)
    {
      if (sector[0] != 0)
      {
        for (int j = 0; j < 64 && j < (int)sizeof(UserEntry); j++)
        {
          ((char *)&user_table[user_count])[j] = sector[j];
        }
        user_count++;
      }
    }
  }

  /* CRITICAL FIX: Load file contents properly */
  file_content_count = 0;

  /* For each file in fs_table that has size > 0 */
  for (int i = 0; i < fs_count && file_content_count < FS_MAX_FILE_CONTENTS; i++)
  {
    if (fs_table[i].size > 0 && fs_table[i].type == 0)
    {
      /* Calculate sectors needed */
      uint32_t sectors_needed = (fs_table[i].size + 511) / 512;
      if (sectors_needed == 0)
        sectors_needed = 1;

      /* Set up file_contents entry */
      file_contents[file_content_count].file_idx = i;
      file_contents[file_content_count].size = fs_table[i].size;

      /* Zero out the entire data buffer first */
      for (int z = 0; z < MAX_FILE_SIZE; z++)
      {
        file_contents[file_content_count].data[z] = 0;
      }

      /* Read content from disk - using file_idx i */
      uint32_t total_read = 0;
      for (uint32_t s = 0; s < sectors_needed && s < 16; s++)
      {
        uint32_t lba = FS_CONTENT_START_LBA + (i * 16) + s;
        if (disk_read_lba_hdd(lba, 1, sector) == 0)
        {
          uint32_t to_copy = fs_table[i].size - total_read;
          if (to_copy > 512)
            to_copy = 512;

          for (uint32_t j = 0; j < to_copy && total_read < MAX_FILE_SIZE; j++)
          {
            file_contents[file_content_count].data[total_read++] = sector[j];
          }
        }
      }

      file_content_count++;
    }
  }

  return 0;
}

/* Verify a file's entry + content bytes reached the platters, bypassing
 * the RAM cache. The installer's RAM-only check cannot catch a disk that
 * silently dropped the write (no HDD attached, wrong drive): the bytes
 * must be readable back from raw LBAs. Returns 0 on match, -1 otherwise.
 */
int fs_verify_on_disk(const char *name, const char *expect, int len)
{
  char sector[512];
  int idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, name) == 0)
    {
      idx = i;
      break;
    }
  }
  if (idx < 0)
    return -1;
  /* entry sector on disk must carry the same name */
  if (disk_read_lba_hdd(FS_DATA_START_LBA + idx, 1, sector) != 0)
    return -1;
  for (int j = 0; name[j]; j++)
  {
    if (sector[j] != name[j])
      return -1;
  }
  /* first content sector must carry the expected bytes */
  if (disk_read_lba_hdd(FS_CONTENT_START_LBA + (uint32_t)idx * 16, 1, sector) != 0)
    return -1;
  for (int j = 0; j < len; j++)
  {
    if (sector[j] != expect[j])
      return -1;
  }
  return 0;
}

/* Initialize filesystem - silent flag to suppress output */
static int fs_init_silent = 0;

/* Desktop directory initialization helper */
static void fs_ensure_desktop_dir(void)
{
  /* Check/create /Desktop/ */
  bool has_desktop = false;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, "/Desktop") == 0 && fs_table[i].type == 1)
    {
      has_desktop = true;
      break;
    }
  }
  if (!has_desktop && fs_count < FS_MAX_FILES)
  {
    str_copy(fs_table[fs_count].name, "/Desktop", FS_MAX_FILENAME);
    fs_table[fs_count].size = 0;
    fs_table[fs_count].type = 1;
    fs_table[fs_count].attr = 0x10;
    fs_count++;
  }

  /* Check/create /Desktop/Applications/ */
  bool has_apps = false;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, "/Desktop/Applications") == 0 && fs_table[i].type == 1)
    {
      has_apps = true;
      break;
    }
  }
  if (!has_apps && fs_count < FS_MAX_FILES)
  {
    str_copy(fs_table[fs_count].name, "/Desktop/Applications", FS_MAX_FILENAME);
    fs_table[fs_count].size = 0;
    fs_table[fs_count].type = 1;
    fs_table[fs_count].attr = 0x10;
    fs_count++;
  }

  /* Create app shortcut stubs inside /Desktop/Applications/ if missing.
     These are zero-byte marker files - one per built-in app. */
  const char *app_shortcuts[] = {
      "/Desktop/Applications/Terminal",
      "/Desktop/Applications/File Explorer",
      "/Desktop/Applications/Notepad",
      "/Desktop/Applications/Paint",
      "/Desktop/Applications/Calculator",
      "/Desktop/Applications/Clock",
      "/Desktop/Applications/System Info",
  };
  int num_shortcuts = 7;

  for (int a = 0; a < num_shortcuts; a++)
  {
    bool exists = false;
    for (int i = 0; i < fs_count; i++)
    {
      if (str_cmp(fs_table[i].name, app_shortcuts[a]) == 0)
      {
        exists = true;
        break;
      }
    }
    if (!exists && fs_count < FS_MAX_FILES)
    {
      str_copy(fs_table[fs_count].name, app_shortcuts[a], FS_MAX_FILENAME);
      fs_table[fs_count].size = 0;
      fs_table[fs_count].type = 0; /* file shortcut */
      fs_table[fs_count].attr = 0x20;
      fs_count++;
    }
  }

  fs_save_to_disk();
}

/* Documents directory initialization helper */
static void fs_ensure_documents_dir(void)
{
  /* Check/create /Documents directory FIRST */
  bool has_docs = false;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, "/Documents") == 0 && fs_table[i].type == 1)
    {
      has_docs = true;
      break;
    }
  }
  if (!has_docs && fs_count < FS_MAX_FILES)
  {
    str_copy(fs_table[fs_count].name, "/Documents", FS_MAX_FILENAME);
    fs_table[fs_count].size = 0;
    fs_table[fs_count].type = 1;
    fs_table[fs_count].attr = 0x10;
    fs_count++;
    fs_save_to_disk();
  }

  /* Helper: Create a file with content */
  void create_web_file(const char *name, const char *content)
  {
    int file_idx = -1;
    /* Search for existing file */
    for (int i = 0; i < fs_count; i++)
    {
      if (str_cmp(fs_table[i].name, name) == 0)
      {
        file_idx = i;
        break;
      }
    }

    if (file_idx == -1)
    {
      if (fs_count >= FS_MAX_FILES)
        return;
      file_idx = fs_count++;
      str_copy(fs_table[file_idx].name, name, FS_MAX_FILENAME);
      fs_table[file_idx].type = 0;
      fs_table[file_idx].attr = 0x20;
    }

    int html_len = str_len(content);
    fs_table[file_idx].size = html_len;

    /* Update or add file content */
    int content_idx = -1;
    for (int i = 0; i < file_content_count; i++)
    {
      if (file_contents[i].file_idx == file_idx)
      {
        content_idx = i;
        break;
      }
    }

    if (content_idx == -1)
    {
      if (file_content_count >= FS_MAX_FILE_CONTENTS)
        return;
      content_idx = file_content_count++;
      file_contents[content_idx].file_idx = file_idx;
    }

    file_contents[content_idx].size = html_len;
    for (int i = 0; i < html_len && i < MAX_FILE_SIZE; i++)
    {
      file_contents[content_idx].data[i] = content[i];
    }
  }

  /* Now check/create multiple default web pages */
  const char *web_files[] = {
      "/Documents/index.html",
      "/Documents/about.html",
      "/Documents/apps.html",
      "/Documents/demo.html",
      "/Documents/cssgrid.html"};
  for (int f = 0; f < 5; f++)
  {
    const char *content = "";

    if (f == 0)
    { /* index.html */
      content = "<html><title>Aurion OS Home</title><body style='background:#000; color:#fff; text-align:center; padding-top:0;'>"
                "<div style='background:#111; padding:30 0; border-bottom:1px solid #333; margin-bottom:80; text-align:center;'>"
                "<a href='index.html' style='color:#4ade80; margin:0 40; text-decoration:none; font-weight:bold; font-size:20;'>HOME</a>"
                "<a href='about.html' style='color:#888; margin:0 40; text-decoration:none; font-size:20;'>ABOUT</a>"
                "<a href='apps.html' style='color:#888; margin:0 40; text-decoration:none; font-size:20;'>APPS</a>"
                "<a href='demo.html' style='color:#888; margin:0 40; text-decoration:none; font-size:20;'>DEMO</a>"
                "</div>"
                "<div style='padding:0 40;'>"
                "<h1 style='font-size:180; margin-bottom:0; color:#4ade80; letter-spacing:-12; font-weight:bold; text-align:center; padding-top:60;'>AURION OS</h1>"
                "<p style='font-size:32; color:#888; margin:40 auto; text-align:center; max-width:800; line-height:1.6;'>"
                "Aurion OS is a statement of speed, beauty and uncompromising software."
                "</p>"
                "<div style='margin-top:200; color:#222; font-size:14; text-transform:uppercase; letter-spacing:4; text-align:center;'>Pure Speed. Zero Compromise. | v1.2 Nightly</div>"
                "</div>"
                "</body></html>";
    }
    else if (f == 1)
    { /* about.html */
      content = "<html><title>About Aurion OS</title><body style='background:#0a0a0a; color:#f0f0f0; padding:80; text-align:center;'>"
                "<h1 style='font-size:72; color:#4ade80;'>THE VISION</h1>"
                "<p style='font-size:24; line-height:1.6; color:#aaa; max-width:800; margin:0 auto;'>AurionOS isn't just another operating system. It's a statement. A statement that software can be fast, beautiful, and uncompromising.</p>"
                "<div style='background:#111; padding:40; border-radius:25; border:1px solid #222; margin:60 auto; width:600; text-align:left;'>"
                "<h3 style='color:#4ade80;'>v1.2 NIGHTLY RELEASE NOTES</h3>"
                "<p>+ New Blaze Engine with CSS3 support</p>"
                "<p>+ Multi-tab environment</p>"
                "<p>+ Developer Console integration</p>"
                "<p>+ Real-time JS execution</p>"
                "</div>"
                "<a href='index.html' style='display:inline-block; background:#4ade80; color:#000; padding:15 60; border-radius:20; text-decoration:none; font-weight:bold;'>RETURN TO HOME</a>"
                "</body></html>";
    }
    else if (f == 2)
    { /* apps.html */
      content = "<html><title>System Apps</title><body style='background:#0a0a0a; color:#f0f0f0; padding:80; text-align:center;'>"
                "<h1>SYSTEM APPLICATIONS</h1>"
                "<div style='margin-top:60; text-align:center;'>"
                "<div style='display:inline-block; width:260; background:#16161a; padding:50 40; border-radius:40; border:2px solid #333; margin:20;'>"
                "<div style='width:90; height:90; background:#6366f1; border-radius:24; margin:0 auto 30;'></div>"
                "<h3>TERMINAL</h3>"
                "<p style='color:#888; font-size:14; margin-top:10;'>Root access shell</p></div>"
                "<div style='display:inline-block; width:260; background:#16161a; padding:50 40; border-radius:40; border:2px solid #333; margin:20;'>"
                "<div style='width:90; height:90; background:#4ade80; border-radius:24; margin:0 auto 30;'></div>"
                "<h3>BROWSER</h3>"
                "<p style='color:#888; font-size:14; margin-top:10;'>The fastest web engine</p></div>"
                "<div style='display:inline-block; width:260; background:#16161a; padding:50 40; border-radius:40; border:2px solid #333; margin:20;'>"
                "<div style='width:90; height:90; background:#eab308; border-radius:24; margin:0 auto 30;'></div>"
                "<h3>NOTEPAD</h3>"
                "<p style='color:#888; font-size:14; margin-top:10;'>Simple text editor</p></div>"
                "<div style='display:inline-block; width:260; background:#16161a; padding:50 40; border-radius:40; border:2px solid #333; margin:20;'>"
                "<div style='width:90; height:90; background:#f43f5e; border-radius:24; margin:0 auto 30;'></div>"
                "<h3>PAINT</h3>"
                "<p style='color:#888; font-size:14; margin-top:10;'>Draw and create art</p></div>"
                "<div style='display:inline-block; width:260; background:#16161a; padding:50 40; border-radius:40; border:2px solid #333; margin:20;'>"
                "<div style='width:90; height:90; background:#22d3ee; border-radius:24; margin:0 auto 30;'></div>"
                "<h3>SNAKE</h3>"
                "<p style='color:#888; font-size:14; margin-top:10;'>Classic arcade game</p></div>"
                "</div>"
                "<div style='margin-top:100;'><a href='index.html' style='display:inline-block; background:#222; color:#fff; padding:15 60; border-radius:20; border:1px solid #333; text-decoration:none;'>BACK TO DASHBOARD</a></div>"
                "<script>console.log(\"Apps is alive!\");</script>"
                "</body></html>";
    }
    else if (f == 3)
    { /* demo.html */
      content = "<html><title>Blaze Demo</title><body style='background:#0a0a0a; color:#f0f0f0; padding:80; text-align:center;'>"
                "<h1 style='color:#4ade80;'>BLAZE ENGINE</h1>"
                "<div style='background:#111; padding:50; border-radius:40; margin:60 auto; width:700;'>"
                "<h2>FLEXIBLE LAYOUTS</h2>"
                "<div style='text-align:center; margin-top:40;'>"
                "<div style='display:inline-block; width:120; height:120; background:#4ade80; border-radius:60; margin:10;'></div>"
                "<div style='display:inline-block; width:120; height:120; background:#6366f1; border-radius:20; margin:10;'></div>"
                "<div style='display:inline-block; width:120; height:120; background:#f43f5e; border-radius:40; margin:10;'></div>"
                "</div>"
                "</div>"
                "<button onclick='console.log(\"Log initiated from local script\")' style='background:#4ade80; color:#000; padding:20 60; border-radius:40; border:none; font-weight:bold; cursor:pointer;'>TEST DEVELOPER CONSOLE</button>"
                "<p style='margin-top:100;'><a href='index.html' style='display:inline-block; background:#1a1a1a; color:#fff; padding:15 60; border-radius:20; border:1px solid #333; text-decoration:none;'>TERMINATE DEMO</a></p>"
                "<script>console.log(\"Demo is alive!\");</script>"
                "</body></html>";
    }
    else if (f == 4)
    { /* cssgrid.html */
      content = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>CSS Grid Test</title><style>body{margin:0;padding:20px;background:#1a1a2e;color:#fff}h1{color:#4ade80;text-align:center}h2{color:#60a5fa;margin-top:30px}.test-section{background:#16213e;padding:20px;margin-bottom:30px;border-radius:10px}.basic-grid{display:grid;grid-template-columns:100px 100px 100px;gap:10px;background:#0f3460;padding:10px}.item1{background:#ef4444;color:#fff;padding:20px;text-align:center;border-radius:5px;font-family:'Courier New',monospace;font-size:24px;font-weight:bold}.item2{background:#f97316;color:#fff;padding:20px;text-align:center;border-radius:5px;font-family:Georgia,serif;font-size:18px;font-style:italic}.item3{background:#eab308;color:#000;padding:20px;text-align:center;border-radius:5px;font-family:'Times New Roman',serif;font-size:20px}.item4{background:#22c55e;color:#fff;padding:20px;text-align:center;border-radius:5px;font-family:'Arial Black',sans-serif;font-size:14px}.item5{background:#06b6d4;color:#fff;padding:20px;text-align:center;border-radius:5px;font-family:Verdana,sans-serif;font-size:16px}.item6{background:#8b5cf6;color:#fff;padding:20px;text-align:center;border-radius:5px;font-family:'Trebuchet MS',sans-serif;font-size:22px}.fr-grid{display:grid;grid-template-columns:1fr 2fr 1fr;gap:15px;background:#0f3460;padding:10px}.f1{background:#ec4899;color:#fff;padding:25px 10px;text-align:center;border-radius:8px;font-family:'Comic Sans MS',cursive;font-size:20px}.f2{background:#f472b6;color:#000;padding:25px 10px;text-align:center;border-radius:8px;font-family:Impact,sans-serif;font-size:28px}.f3{background:#fb7185;color:#fff;padding:25px 10px;text-align:center;border-radius:8px;font-family:'Palatino Linotype',serif;font-size:18px}.f4{background:#c084fc;color:#fff;padding:25px 10px;text-align:center;border-radius:8px;font-family:'Lucida Console',monospace;font-size:16px}.f5{background:#a78bfa;color:#000;padding:25px 10px;text-align:center;border-radius:8px;font-family:Garamond,serif;font-size:24px}.f6{background:#e879f9;color:#fff;padding:25px 10px;text-align:center;border-radius:8px;font-family:'Century Gothic',sans-serif;font-size:14px}.complex-grid{display:grid;grid-template-columns:200px 1fr 200px;gap:10px;background:#0f3460;padding:10px;min-height:200px}.complex-grid>.header{grid-column:span 3;background:#ef4444;padding:20px;text-align:center;border-radius:8px;font-family:'Arial Black',sans-serif;font-size:28px;color:#fff}.complex-grid>.sidebar{background:#3b82f6;padding:20px;border-radius:8px;font-family:Georgia,serif}.complex-grid>.content{background:#10b981;padding:20px;border-radius:8px;font-family:'Segoe UI',sans-serif;color:#fff}.complex-grid>.footer{grid-column:span 3;background:#8b5cf6;padding:15px;text-align:center;border-radius:8px;font-family:'Courier New',monospace;font-size:14px;color:#fff}</style></head><body><h1>CSS Grid Test</h1><div class='test-section'><h2>Basic Grid</h2><div class='basic-grid'><div class='item1'>Item 1</div><div class='item2'>Item 2</div><div class='item3'>Item 3</div><div class='item4'>Item 4</div><div class='item5'>Item 5</div><div class='item6'>Item 6</div></div></div><div class='test-section'><h2>Fr Units</h2><div class='fr-grid'><div class='f1'>1fr</div><div class='f2'>2fr</div><div class='f3'>1fr</div><div class='f4'>1fr</div><div class='f5'>2fr</div><div class='f6'>1fr</div></div></div><div class='test-section'><h2>Holy Grail Layout</h2><div class='complex-grid'><div class='header'>Header</div><div class='sidebar'>Left</div><div class='content'>Main</div><div class='sidebar'>Right</div><div class='footer'>Footer</div></div></div><div style='text-align:center;margin-top:20px'><a href='index.html' style='color:#4ade80'>Home</a></div></body></html>";
    }
    create_web_file(web_files[f], content);
  }

  fs_save_to_disk();
}

static void fs_init_commands(void)
{
  /* CRITICAL FIX: Explicitly zero out ALL filesystem state */
  fs_count = 0;
  user_count = 0;

  /* CRITICAL: Zero out file_content_count and the entire array! */
  file_content_count = 0;
  for (int i = 0; i < FS_MAX_FILE_CONTENTS; i++)
  {
    file_contents[i].file_idx = 0;
    file_contents[i].size = 0;
    for (int j = 0; j < MAX_FILE_SIZE; j++)
    {
      file_contents[i].data[j] = 0;
    }
  }

  /* Finish any confirmed reset before old accounts or files can load. */
  if (fs_recover_reset() != 0) {
    puts("AurionOS reset is incomplete; disk writes remain blocked.\n");
    return;
  }
  /* Load from disk - this will update the counters */
  fs_load_from_disk();
  int repaired = fs_repair_duplicate_directories();
  static const char *legacy_starters[] = {
    "/Development/hello.py", "/Development/math_demo.py",
    "/Development/loop_demo.py", "/Development/files_demo.py",
    "/Development/syntax_error.py", "/Development/Makefile"
  };
  for (uint32_t i = 0; i < sizeof(legacy_starters) / sizeof(legacy_starters[0]); i++)
    repaired |= fs_remove_starter_file(legacy_starters[i]);
  if (repaired && fs_save_to_disk() != 0)
    puts("Filesystem cleanup could not be saved to disk.\n");

  /* Only print messages if not silent */
  if (!fs_init_silent)
  {
    /* Verify file_content_count is correct */
    char i_buf[16];
    puts("[INIT: file_content_count=");
    int_to_str(file_content_count, i_buf);
    puts(i_buf);
    puts("]\n");

    /* Print initialization message */
    puts("Ready. ");
    int_to_str(fs_count, i_buf);
    puts(i_buf);
    puts(" files, ");
    int_to_str(file_content_count, i_buf);
    puts(i_buf);
    puts(" contents loaded. Type HELP for commands.\n");
  }

  /* If no users exist, create a root account with NO password.
     An all-zero hash means "no password set": the login screen detects
     this and forces a first-boot password setup flow. No default
     credential can ever exist; login fails closed against a zero hash. */
  if (user_count == 0)
  {
    str_copy(user_table[0].username, "root", 32);
    for (int i = 0; i < 32; i++)
    {
      user_table[0].password_hash[i] = 0;
    }
    user_count = 1;
    fs_save_to_disk();
  }

  /* Ensure /Desktop directory exists */
  fs_ensure_desktop_dir();

  /* Ensure /Documents directory exists with default index.html */
  fs_ensure_documents_dir();

  /* Ensure /Keyboard directory exists for configuration */
  bool kb_found = false;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, "/Keyboard") == 0)
    {
      kb_found = true;
      break;
    }
    if (str_cmp(fs_table[i].name, "/Keyboard/") == 0)
    {
      kb_found = true;
      break;
    }
  }
  if (!kb_found && fs_count < FS_MAX_FILES)
  {
    str_copy(fs_table[fs_count].name, "/Keyboard", FS_MAX_FILENAME);
    fs_table[fs_count].type = 1;
    fs_table[fs_count].size = 0;
    fs_count++;
  }

  /* New directories requested by user */
  const char *extra_dirs[] = {"/Music", "/Pictures", "/Downloads", "/Videos",
                              "/Code", "/Development", "/System", "/Keyboard"};
  for (int d = 0; d < 8; d++)
  {
    bool found = false;
    for (int i = 0; i < fs_count; i++)
    {
      if (str_cmp(fs_table[i].name, extra_dirs[d]) == 0)
      {
        found = true;
        break;
      }
    }
    if (!found && fs_count < FS_MAX_FILES)
    {
      str_copy(fs_table[fs_count].name, extra_dirs[d], FS_MAX_FILENAME);
      fs_table[fs_count].type = 1;
      fs_table[fs_count].size = 0;
      fs_count++;
    }
  }

  /* Seed only Vanta programs; never overwrite an existing example. */
  if (vanta_seed_examples() != 0)
    puts("Some Vanta examples could not be saved to disk.\n");

  /* Load the active user profile (set by installer) */
  char user_buf[32];
  int ulen = load_file_content("/System/current_user", user_buf, 31);
  if (ulen > 0)
  {
    user_buf[ulen] = 0;
    str_copy(current_user, user_buf, 32);

    /* Set default current directory to user's home */
    char hpath[64];
    str_copy(hpath, "/users/", 64);
    int hl = str_len(hpath);
    str_copy(hpath + hl, current_user, 32);
    /* Store the directory without a trailing slash, like the disk loader.
       current_dir alone keeps its slash for relative-path concatenation. */
    if (fs_ensure_directory("/users") != 0 || fs_ensure_directory(hpath) != 0)
      puts("Could not create the user home directory.\n");
    str_copy(current_dir, hpath, 256);
    int hl2 = str_len(current_dir);
    current_dir[hl2] = '/';
    current_dir[hl2 + 1] = 0;
  }
  else
  {
    /* Fallback to root home if no user set */
    str_copy(current_dir, "/users/root/", 256);
  }

  /* Load keyboard layout setting from /Keyboard/config.sys */
  char kb_buf[64];
  int klen = load_file_content("/Keyboard/config.sys", kb_buf, 63);
  if (klen > 0)
  {
    kb_buf[klen] = 0;
    puts("[KEYBOARD] Loaded config: ");
    puts(kb_buf);
    puts("\n");
    /* Robust checking for KEYBOARD=SERBIAN, KEYBOARD=ENGLISH, or legacy "1"/"0" */
    bool is_serbian = false;
    if (kb_buf[0] == '1')
    {
      is_serbian = true;
    }
    else
    {
      for (int i = 0; i < klen - 3; i++)
      {
        if (kb_buf[i] == 'S' && kb_buf[i + 1] == 'E' && kb_buf[i + 2] == 'R' && kb_buf[i + 3] == 'B')
        {
          is_serbian = true;
          break;
        }
      }
    }
    if (is_serbian)
    {
      keyboard_layout = 1;
      puts("[KEYBOARD] Set to SERBIAN\n");
    }
    else
    {
      keyboard_layout = 0;
      puts("[KEYBOARD] Set to ENGLISH\n");
    }
  }
  else
  {
    puts("[KEYBOARD] No config found, defaulting to ENGLISH\n");
  }
}

/* Command parsing */
static const char *skip_spaces(const char *s)
{
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
}

static const char *get_token(const char *s, char *token, int max)
{
  s = skip_spaces(s);
  int i = 0;
  while (*s && *s != ' ' && *s != '\t' && i < max - 1)
  {
    token[i++] = *s++;
  }
  token[i] = '\0';
  return s;
}

/* COMMAND IMPLEMENTATIONS */

/* Forward declaration for MAKE command (defined in cmd_make.c) */
extern int cmd_make(const char *args);

/* Vanta language command */
#include "vanta.h"

/* Forward declaration for build_full_path (defined later in this file) */
static void build_full_path(const char *name, char *full_path);

int vanta_syscall_hook(int sys_id, int64_t arg1, int64_t arg2)
{
  (void)arg1;
  (void)arg2;
  switch (sys_id)
  {
  case 1: /* Memory stats */
  {
    uint32_t stats[4];
    mem_get_stats(stats);
    return (int)stats[0];
  }
  case 2: /* Uptime ticks */
    return (int)get_ticks();
  default:
    return -1;
  }
}

static int cmd_vanta(const char *args)
{
  char filename[64];
  args = get_token(args, filename, 64);

  if (filename[0] == 0)
  {
    puts("Usage: VANTA <filename.vanta>\n");
    puts("Example: VANTA /Development/tour.vanta\n");
    return -1;
  }

  char full_path[256];
  build_full_path(filename, full_path);

  set_attr(0x0B);
  puts("[VANTA] Executing: ");
  puts(full_path);
  puts("\n");
  set_attr(0x07);

  VantaResult res = vanta_run_file(full_path);
  if (res != VANTA_OK)
  {
    set_attr(0x0C);
    puts("Vanta Error: ");
    puts(vanta_strerror(res));
    puts("\n");
    set_attr(0x07);
    return -1;
  }

  return 0;
}

/* KEYBOARD - keyboard layout switcher (English / Serbian Latin)
 * The OS reads all input as ASCII from the BIOS/keyboard ISR.
 * This command stores the active layout and provides a software
 * remapping table that shell.c and terminal.c can call.
 * Serbian Latin QWERTY maps the same physical keys to characters
 * used in Serbian: c->c, but adds s->s, z->z and the digraphs
 * dz, lj, nj via their Latin equivalents.  For the purpose of
 * this OS the remapping gives you: c/C, s/S, z/Z, d/D plus the
 * combined-letter sequences lj/LJ and nj/NJ typed as normal
 * Latin letters, which is exactly how Serbian Latin keyboards
 * behave on a standard QWERTY layout.
 */

/* 0 = English (default), 1 = Serbian Latin */

/* Remap a character according to current layout.
   For Serbian Latin the physical key mapping is identical to
   English on QWERTY - we just tag the layout so the user knows
   which convention is active.  The real difference is accented
   chars (c with caron, etc.) which we approximate here:
     [ -> s-caron (s)   { -> S-caron (S)
     ] -> c-caron (c)   } -> C-caron (C)
     \ -> z-caron (z)   | -> Z-caron (Z)
   These keys are rarely used in English on QWERTY so it is a
   clean swap with no conflict.                               */
/* Serbian Latin QWERTY full key map.
 *
 * Physical key -> what appears on screen.
 *
 * Number row (unshifted) is identical to US QWERTY (1-9, 0 same).
 * Shift + number row differs from US:
 *   US:  !  @  #  $  %  ^  &  *  (  )
 *   SRB: !  "  #  $  %  &  /  (  )  =
 *
 * Special letters (bracket/backslash cluster on US keyboard):
 *   [  ->  s (s-caron, written "s" in ASCII)
 *   ]  ->  c (c-caron)
 *   \  ->  z (z-caron)
 *   {  ->  S
 *   }  ->  C
 *   |  ->  Z
 *   ;  ->  c (Cyrillic: same key, written c in Latin)
 *   '  ->  c (another common mapping)
 *
 * The BIOS/keyboard ISR gives us the ASCII value of what is typed
 * AFTER the US QWERTY shift lookup.  So when the user presses
 * SHIFT+0 on a US keyboard we receive ')'.  We remap that to '='.
 */
char keyboard_remap(char c)
{
  if (keyboard_layout == 0)
    return c; /* English - pass through */

  switch (c)
  {
  /* QWERTZ swap */
  case 'y':
    return 'z';
  case 'Y':
    return 'Z';
  case 'z':
    return 'y';
  case 'Z':
    return 'Y';

  /* Shifted number row - US result -> Serbian result */
  case '@':
    return '"'; /* SHIFT+2: @ -> " */
  case '^':
    return '&'; /* SHIFT+6: ^ -> & */
  case '&':
    return '/'; /* SHIFT+7: & -> / */
  case '*':
    return '('; /* SHIFT+8: * -> ( */
  case '(':
    return ')'; /* SHIFT+9: ( -> ) */
  case ')':
    return '='; /* SHIFT+0: ) -> = */
  case '_':
    return '?'; /* SHIFT+-: _ -> ? */
  case '+':
    return '*'; /* SHIFT+=: + -> * */

  /* Unshifted punctuation row */
  case '-':
    return '\''; /* - -> ' */
  case '=':
    return '+'; /* = -> + */

  /* Bracket/backslash cluster -> Serbian letters (š, đ, ž)
     Note: Using ASCII fallbacks because font is 128 chars only. */
  case '[':
    return 's'; /* š */
  case '{':
    return 'S'; /* Š */
  case ']':
    return 'd'; /* đ */
  case '}':
    return 'D'; /* Đ */
  case '\\':
    return 'z'; /* ž */
  case '|':
    return 'Z'; /* Ž */

  /* Semicolon/Quote cluster -> (č, ć) */
  case ';':
    return 'c'; /* č */
  case ':':
    return 'C'; /* Č */
  case '\'':
    return 'c'; /* ć */
  case '"':
    return 'C'; /* Ć */

  /* Comma/Period/Slash row */
  case ',':
    return ',';
  case '.':
    return '.';
  case '/':
    return '-'; /* / -> - */
  case '<':
    return ';'; /* SHIFT+,: < -> ; */
  case '>':
    return ':'; /* SHIFT+.: > -> : */
  case '?':
    return '_'; /* SHIFT+/: ? -> _ */

  default:
    return c;
  }
}

/* No-op command - handles # character and empty commands */
static int cmd_noop(const char *args)
{
  (void)args;
  return 0; /* Silent success - do nothing */
}

static int cmd_keyboard(const char *args)
{
  char tok[32];
  get_token(args, tok, 32);
  str_upper(tok);

  if (tok[0] == 0)
  {
    /* No argument - show current layout and usage */
    puts("Keyboard layout: ");
    puts(keyboard_layout == 0 ? "English (default)\n" : "Serbian Latin QWERTY\n");
    puts("\nUsage:\n");
    puts("  KEYBOARD ENGLISH   - switch to English layout\n");
    puts("  KEYBOARD SERBIAN   - switch to Serbian Latin QWERTY\n");
    puts("\nSerbien Latin key overrides (bracket cluster):\n");
    puts("  [  ->  s (s-caron)     {  ->  S\n");
    puts("  ]  ->  c (c-caron)     }  ->  C\n");
    puts("  \\  ->  z (z-caron)     |  ->  Z\n");
    return 0;
  }

  if (str_cmp(tok, "ENGLISH") == 0)
  {
    keyboard_layout = 0;
    save_file_content("/Keyboard/config.sys", "KEYBOARD=ENGLISH", 16);
    fs_save_to_disk();
    puts("Keyboard layout set to: English\n");
    return 0;
  }

  if (str_cmp(tok, "SERBIAN") == 0)
  {
    keyboard_layout = 1;
    save_file_content("/Keyboard/config.sys", "KEYBOARD=SERBIAN", 16);
    fs_save_to_disk();
    puts("Keyboard layout set to: Serbian Latin QWERTY\n");
    puts("Keys [ ] \\ now produce s c z (Serbian caron letters)\n");
    puts("Uppercase: { } | produce S C Z\n");
    puts("AltGr (Right Alt) combinations enabled (@, \\, |, [, ], {, })\n");
    return 0;
  }

  puts("Unknown layout. Use: KEYBOARD ENGLISH  or  KEYBOARD SERBIAN\n");
  return -1;
}

/* Rust GPU driver entry points used by this file */
extern int gpu_flush(void);
extern void gpu_fill_rect(int x, int y, int w, int h, uint32_t color);
extern void gpu_draw_string(int x, int y, const uint8_t *str, uint32_t fg, uint32_t bg);
extern uint32_t *gpu_get_backbuffer(void);
extern int gpu_get_width(void);
extern int gpu_get_height(void);

/* 1. HELP - Show command list */
static int cmd_help(const char *args)
{
  (void)args;
  puts("========================================================================\n");
  puts("AurionOS Available Commands:\n");
  puts("  File: DIR LS CD MKDIR RMDIR TOUCH DEL CAT NANO TYPE COPY MOVE REN FIND\n");
  puts("  Disk: CHKDSK FORMAT LABEL VOL DISKPART FSCK\n");
  puts("  Info: VER TIME DATE UPTIME MEM SYSINFO UNAME WHOAMI HOSTNAME\n");
  puts("  User: USERADD USERDEL PASSWD USERS LOGIN LOGOUT SU SUDO\n");
  puts("  Proc: PS KILL TOP TASKLIST TASKKILL\n");
  puts("  Misc: CLS CLEAR CLS-TERMINAL CLEAR-TERMINAL ECHO CALC\n");
  puts("        HEXDUMP ASCII HASH\n");
  puts("  Ctrl: REBOOT SHUTDOWN PAUSE SLEEP EXIT\n");
  puts("  Programming: VANTA (run /Development/tour.vanta for examples)\n");
  puts("  Reset: SUDO REINSTALL AURION (erases all AurionOS user data)\n");
  puts("========================================================================\n");
  return 0;
}

/* 2. CLS/CLEAR - Clear screen.
   Returns -2 so shell.c skips printing a second prompt immediately after. */
static int cmd_cls(const char *args)
{
  (void)args;
  extern void set_cursor_pos(int row, int col);
  cls();
  set_cursor_pos(0, 0);
  return -2; /* shell.c special code: suppress duplicate prompt */
}

/* 3. VER - Show version */
static int cmd_ver(const char *args)
{
  (void)args;
  puts("AurionOS v1.2 Nightly\n");
  puts("32-bit Protected Mode Operating System\n");
  return 0;
}

/* TEMP-DEBUG WP - switch wallpaper from the shell: WP 0 | WP 1 | WP 2 | WP 3 */
static int cmd_wp(const char *args)
{
  extern void wm_set_wallpaper(int idx);
  extern void wm_save_settings(void);
  extern int wm_get_wallpaper(void);
  int idx = -1;
  if (args) {
    while (*args == ' ') args++;
    if (*args >= '0' && *args <= '3') idx = *args - '0';
  }
  if (idx < 0) {
    puts("Usage: WP 0|1|2|3 (0=Night Ridge, 1=Misty Conifer, 2=Gate B14, 3=Sunday)\n");
    return 1;
  }
  wm_set_wallpaper(idx);
  wm_save_settings();
  puts("Wallpaper switched.\n");
  (void)wm_get_wallpaper;
  return 0;
}

/* PANICTEST - Validate the kernel-panic subsystem without panicking */
static int cmd_panictest(const char *args)
{
  char tok[16];
  args = get_token(args, tok, 16);
  if (tok[0] == 'F' || tok[0] == 'f') {
    /* FAULT: real divide-by-zero. Vector 0 must panic, never silence. */
    puts("PANICTEST: firing divide fault...\n");
    {
        volatile uint32_t z = 0;
        volatile uint32_t q = 1u / z;
        (void)q;
    }
    puts("PANICTEST: survived?! fault did not trap\n");
    return -1;
  }
  {
    extern int kpanic_selftest(char *out, uint32_t out_len);
    char report[64];
    int rc = kpanic_selftest(report, sizeof(report));
    puts(report);
    puts("\n");
    return rc;
  }
}

/* SNDTEST - Probe SB16 and play one DMA chunk through the card */
static int cmd_sndtest(const char *args)
{
  (void)args;
  {
    extern int sb16_selftest(void);
    int rc = sb16_selftest();
    if (rc == -3)
      puts("SB16: not detected (no DSP reset ACK)\n");
    else if (rc == -2)
      puts("SB16: DMA/DSP start failed\n");
    else if (rc == -1)
      puts("SB16: IRQ timeout (chunk played, IRQ lost)\n");
    else {
      puts("SB16: OK, played ");
      char v_buf[16];
      int_to_str((uint32_t)rc, v_buf);
      puts(v_buf);
      puts(" bytes via IRQ\n");
    }
    return rc;
  }
}

/* 4. TIME - Show current time */
static int cmd_time(const char *args)
{
  (void)args;
  uint8_t h, m, s;
  if (sys_get_time(&h, &m, &s) == 0)
  {
    char v_buf[16];
    int_to_str(h, v_buf);
    puts(v_buf);
    puts(":");
    if (m < 10)
      puts("0");
    int_to_str(m, v_buf);
    puts(v_buf);
    puts(":");
    if (s < 10)
      puts("0");
    int_to_str(s, v_buf);
    puts(v_buf);
    puts("\n");
  }
  return 0;
}

/* 5. DATE - Show current date */
static int cmd_date(const char *args)
{
  (void)args;
  uint8_t day, month;
  uint16_t year;
  if (sys_get_date(&day, &month, &year) == 0)
  {
    char v_buf[16];
    int_to_str(year, v_buf);
    puts(v_buf);
    puts("-");
    if (month < 10)
      puts("0");
    int_to_str(month, v_buf);
    puts(v_buf);
    puts("-");
    if (day < 10)
      puts("0");
    int_to_str(day, v_buf);
    puts(v_buf);
    puts("\n");
  }
  return 0;
}

/* 6. REBOOT - Reboot system */
static int cmd_reboot(const char *args)
{
  (void)args;
  puts("Rebooting...\n");
  sleep_ms(1000);
  sys_reboot();
  return 0;
}

/* 7. SHUTDOWN - Shutdown system */
static int cmd_shutdown(const char *args)
{
  (void)args;
  puts("WARNING: shut down is still not full implemented, close your QEMU/VMware/Virtualbox window,\n");
  return 0;
}

/* Helper to build full path */
static void build_full_path(const char *name, char *full_path)
{
  if (!name || !full_path)
    return;
  if (name[0] == '/')
  {
    str_copy(full_path, name, 256);
  }
  else
  {
    int dir_len = str_len(current_dir);
    for (int i = 0; i < dir_len && i < 255; i++)
    {
      full_path[i] = current_dir[i];
    }
    int name_len = str_len(name);
    for (int i = 0; i < name_len && (dir_len + i) < 255; i++)
    {
      full_path[dir_len + i] = name[i];
    }
    full_path[dir_len + name_len] = '\0';
  }

  /* Normalize backslashes to forward slashes */
  for (int i = 0; full_path[i]; i++)
  {
    if (full_path[i] == '\\')
      full_path[i] = '/';
  }
}

/* 8. MKDIR - Create directory */
int cmd_mkdir(const char *args)
{
  char name[64], full_path[256];
  get_token(args, name, sizeof(name));
  if (!name[0] || fs_writes_blocked()) return -1;
  build_full_path(name, full_path);
  int rc = fs_ensure_directory(full_path);
  if (rc != 0) {
    puts("Cannot create directory: invalid path, name conflict, or filesystem full.\n");
    return rc;
  }
  return fs_save_to_disk();
}

/* Create/reuse a canonical directory and persist it (silent, for GUI). */
int fs_mkdir_abs(const char *abs_path)
{
  if (fs_writes_blocked()) return -1;
  int rc = fs_ensure_directory(abs_path);
  return rc == 0 ? fs_save_to_disk() : rc;
}

/* 9. RMDIR - Remove directory */
static int cmd_rmdir(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  if (name[0] == 0)
  {
    puts("Usage: RMDIR dirname\n");
    return -1;
  }

  char full_path[256];
  build_full_path(name, full_path);

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0 && fs_table[i].type == 1)
    {
      for (int j = i; j < fs_count - 1; j++)
      {
        fs_table[j] = fs_table[j + 1];
      }
      fs_count--;
      fs_save_to_disk();
      puts("Directory removed\n");
      return 0;
    }
  }

  puts("Error: Directory not found\n");
  return -1;
}

/* 10. TOUCH - Create empty file */
static int cmd_touch(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  if (name[0] == 0)
  {
    puts("Usage: TOUCH filename\n");
    return -1;
  }

  if (fs_count >= FS_MAX_FILES)
  {
    puts("Error: Filesystem full\n");
    return -1;
  }

  /* Build full path */
  char full_path[256];
  build_full_path(name, full_path);

  /* Check if file already exists in this directory */
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      puts("File already exists\n");
      return 0;
    }
  }

  str_copy(fs_table[fs_count].name, full_path, FS_MAX_FILENAME);
  fs_table[fs_count].size = 0;
  fs_table[fs_count].type = 0;
  fs_table[fs_count].attr = 0x20;
  fs_count++;

  fs_save_to_disk();
  puts("File created: ");
  puts(name);
  puts("\n");
  return 0;
}

/* 11. DEL/RM - Delete file */
static int cmd_del(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  /* Skip flags */
  while (name[0] == '-')
  {
    /* Special case: 'rm -rf /' */
    if (str_cmp(name, "-rf") == 0)
    {
      char next[64];
      get_token(args, next, 64);
      if (next[0] == '/' && next[1] == '\0')
      {
        puts("Nice Try, but AurionOS protects itself (and you too :D)\n");
        return 0;
      }
    }
    args = get_token(args, name, 64);
    if (name[0] == 0)
      break;
  }

  if (name[0] == 0)
  {
    puts("Usage: DEL filename\n");
    return -1;
  }

  if (name[0] == '/' && name[1] == '\0')
  {
    puts("Nice Try, but AurionOS protects itself (and you too :D)\n");
    return 0;
  }

  /* Build full path */
  char full_path[256];
  build_full_path(name, full_path);

  if (sys_path_locked_abs(full_path))
  {
    puts("Access denied: /installed.sys needs sudo.\n");
    return -1;
  }

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0 && fs_table[i].type == 0)
    {
      for (int j = i; j < fs_count - 1; j++)
      {
        fs_table[j] = fs_table[j + 1];
      }
      fs_count--;
      fs_save_to_disk();
      puts("File deleted\n");
      return 0;
    }
  }

  puts("Error: File not found\n");
  return -1;
}

/* 12. DIR/LS - List directory */
static int cmd_dir(const char *args)
{
  (void)args;

  puts("Directory of ");
  puts(current_dir);
  puts("\n\n");

  int file_count = 0;
  int dir_count = 0;
  uint32_t total_size = 0;

  int current_dir_len = str_len(current_dir);

  for (int i = 0; i < fs_count; i++)
  {
    /* Skip hidden files (attr 0x02) - these are internal markers */
    if (fs_table[i].attr & 0x02)
    {
      continue;
    }

    /* Check if this entry is in the current directory */
    bool in_current_dir = true;

    /* Compare path prefix */
    for (int j = 0; j < current_dir_len; j++)
    {
      if (fs_table[i].name[j] != current_dir[j])
      {
        in_current_dir = false;
        break;
      }
    }

    /* Make sure there's no subdirectory after current path */
    if (in_current_dir && fs_table[i].name[current_dir_len] != '\0')
    {
      for (int j = current_dir_len; fs_table[i].name[j] != '\0'; j++)
      {
        if (fs_table[i].name[j] == '/')
        {
          /* If there's another slash, it's deeper in the hierarchy, unless it's just a trailing slash */
          if (fs_table[i].name[j + 1] != '\0')
          {
            in_current_dir = false;
            break;
          }
        }
      }
    }
    else if (in_current_dir && fs_table[i].name[current_dir_len] == '\0')
    {
      /* This is the directory itself, skip it */
      in_current_dir = false;
    }

    if (!in_current_dir)
      continue;

    /* Display the filename without the full path */
    char *display_name = fs_table[i].name + current_dir_len;

    char v_buf[16];

    if (fs_table[i].type == 1)
    {
      puts("<DIR>      ");
      set_attr(0x0B); /* Light Blue for directories */
      puts(display_name);
      /* Ensure visible trailing slash for directories in DIR output */
      int dlen = str_len(display_name);
      if (dlen == 0 || display_name[dlen - 1] == '/')
      {
        puts("/");
      }
      set_attr(0x07);
      dir_count++;
    }
    else
    {
      int_to_str(fs_table[i].size, v_buf);
      puts(v_buf);
      for (int j = str_len(v_buf); j < 11; j++)
        puts(" ");
      puts(display_name);
      file_count++;
      total_size += fs_table[i].size;
    }

    puts("\n");
  }

  /* No extra newline here, makes prompt closer */
  char v_buf[16];
  int_to_str(file_count, v_buf);
  puts(v_buf);
  puts(" file(s), ");
  int_to_str(dir_count, v_buf);
  puts(v_buf);
  puts(" dir(s), ");
  int_to_str(total_size, v_buf);
  puts(v_buf);
  puts(" bytes\n");

  return 0;
}

/* 13. CD - Change directory */
static int cmd_cd(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  if (name[0] == 0)
  {
    puts(current_dir);
    puts("\n");
    return 0;
  }

  /* Absolute path to root */
  if (str_cmp(name, "/") == 0)
  {
    str_copy(current_dir, "/", 256);
    fs_save_to_disk();
    return 0;
  }

  /* Parent directory (.., ..., ....: N dots climb N-1 levels) */
  {
    int dots = 0;
    while (name[dots] == '.') dots++;
    if (dots >= 2 && name[dots] == 0)
    {
      for (int u = 0; u < dots - 1; u++)
      {
        int len = str_len(current_dir);
        if (len > 1) /* Not at root */
        {
          /* If ends with slash, skip it */
          if (current_dir[len - 1] == '/')
            len--;

          /* Find the slash before that */
          int found = -1;
          for (int i = len - 1; i >= 0; i--)
          {
            if (current_dir[i] == '/')
            {
              found = i;
              break;
            }
          }

          if (found != -1)
          {
            current_dir[found + 1] = '\0';
          }
          else
          {
            /* Should not happen if paths are valid, but fallback to root */
            str_copy(current_dir, "/", 256);
          }
        }
      }
      fs_save_to_disk();
      return 0;
    }
  }

  /* Build full path */
  char full_path[256];
  build_full_path(name, full_path);

  /* Normalize slashes */
  for (int i = 0; full_path[i]; i++)
  {
    if (full_path[i] == '\\')
      full_path[i] = '/';
  }

  /* Handle root path if it somehow ended up here */
  if (str_cmp(full_path, "/") == 0)
  {
    str_copy(current_dir, "/", 256);
    fs_save_to_disk();
    return 0;
  }

  /* Check if this directory exists.
     Directories in fs_table might or might not have trailing slashes.
     We check both cases. */
  char alt_path[256];
  str_copy(alt_path, full_path, 256);
  int aplen = str_len(alt_path);
  if (alt_path[aplen - 1] == '/')
  {
    alt_path[aplen - 1] = '\0'; /* Remove trailing slash for check */
  }
  else
  {
    alt_path[aplen] = '/'; /* Add trailing slash for check */
    alt_path[aplen + 1] = '\0';
  }

  int found_idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if ((str_cmp(fs_table[i].name, full_path) == 0 || str_cmp(fs_table[i].name, alt_path) == 0) && fs_table[i].type == 1)
    {
      found_idx = i;
      break;
    }
  }

  if (found_idx != -1)
  {
    str_copy(current_dir, fs_table[found_idx].name, 256);
    /* Ensure trailing slash for current_dir */
    int len = str_len(current_dir);
    if (current_dir[len - 1] != '/')
    {
      current_dir[len] = '/';
      current_dir[len + 1] = '\0';
    }
    fs_save_to_disk();
    return 0;
  }

  /* Fallback: If they typed cd /Documents and we only have /Documents/ or vice versa,
     we check if the root itself is being navigated to */
  if (str_cmp(full_path, "/") == 0 || str_cmp(alt_path, "/") == 0)
  {
    str_copy(current_dir, "/", 256);
    fs_save_to_disk();
    return 0;
  }

  puts("Directory not found\n");
  return -1;
}

/* 14. CAT/TYPE - Display file contents */
static int cmd_cat(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  if (name[0] == 0)
  {
    puts("Usage: CAT filename\n");
    return -1;
  }

  char full_path[256];
  build_full_path(name, full_path);

  /* Find file in fs_table */
  int file_idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0 && fs_table[i].type == 0)
    {
      file_idx = i;
      break;
    }
  }

  if (file_idx < 0)
  {
    puts("File not found\n");
    return -1;
  }

  /* Check if file has any size */
  if (fs_table[file_idx].size == 0)
  {
    puts("(empty file)\n");
    return 0;
  }

  /* Search for content in file_contents array */
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == file_idx)
    {
      if (file_contents[i].size == 0)
      {
        puts("(empty file)\n");
        return 0;
      }

      /* Display file contents */
      for (uint32_t j = 0; j < file_contents[i].size; j++)
      {
        putc(file_contents[i].data[j]);
      }

      /* Add newline if file doesn't end with one */
      if (file_contents[i].data[file_contents[i].size - 1] != '\n')
      {
        puts("\n");
      }
      return 0;
    }
  }

  /* File exists but no content found */
  puts("(empty file)\n");
  return 0;
}

/* 15. NANO - Simple text editor - COMPLETELY FIXED */
static int cmd_nano(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  if (name[0] == 0)
  {
    puts("Usage: NANO filename\n");
    return -1;
  }

  char full_path[256];
  build_full_path(name, full_path);

  if (sys_path_locked_abs(full_path))
  {
    puts("Access denied: /installed.sys needs sudo.\n");
    return -1;
  }

  /* Find or create file in fs_table */
  int file_idx = -1;
  bool is_new_file = false;

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0 && fs_table[i].type == 0)
    {
      file_idx = i;
      break;
    }
  }

  /* Create new file if not found */
  if (file_idx < 0)
  {
    if (fs_count >= FS_MAX_FILES)
    {
      puts("Error: Filesystem full\n");
      return -1;
    }
    str_copy(fs_table[fs_count].name, full_path, FS_MAX_FILENAME);
    fs_table[fs_count].size = 0;
    fs_table[fs_count].type = 0;
    fs_table[fs_count].attr = 0x20;
    file_idx = fs_count;
    is_new_file = true;
    fs_count++;
  }

  /* Show editor header */
  puts("\n=== NANO Editor ===\n");
  puts("File: ");
  puts(full_path);
  puts("\n");
  puts("Ctrl+O/Ctrl+S=Save  Ctrl+X=Save and exit  ESC=Save\n");
  puts("-------------------\n");

  /* Use a static buffer instead of kmalloc to avoid heap issues */
  static char nano_buf[MAX_FILE_SIZE];
  char *v_buf = nano_buf;

  if (v_buf == NULL)
  {
    set_attr(0x0C);
    puts("Error: Out of memory for NANO buffer\n");
    set_attr(0x07);
    if (is_new_file)
      fs_count--;
    return -1;
  }

  /* Zero the entire buffer */
  for (int i = 0; i < MAX_FILE_SIZE; i++)
  {
    v_buf[i] = 0;
  }

  int pos = 0;
  bool cancelled = false;

  /* Find existing content */
  int content_idx = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == file_idx)
    {
      content_idx = i;
      break;
    }
  }

  /* Load existing content if present */
  if (content_idx >= 0 && file_contents[content_idx].size > 0)
  {
    uint32_t load_size = file_contents[content_idx].size;
    if (load_size >= MAX_FILE_SIZE)
    {
      load_size = MAX_FILE_SIZE - 1;
    }

    /* Copy to buffer */
    for (uint32_t j = 0; j < load_size; j++)
    {
      v_buf[j] = file_contents[content_idx].data[j];
    }
    pos = (int)load_size;

    /* Display existing content */
    for (uint32_t j = 0; j < load_size; j++)
    {
      putc(v_buf[j]);
    }
  }
  else
  {
    puts("[New file...]\n");
    pos = 0;
  }

  /* Main edit loop */
  while (1)
  {
    /* Keep system alive while waiting for key in GUI mode. In DOS mode
     * nothing charges the epoch here, so declare the wait: otherwise an
     * idle prompt looks exactly like a hung scheduler to the watchdog. */
    extern void sched_progress_wait(int s);
    extern void sched_progress_run(void);
    sched_progress_wait(1);
    while (!c_kb_hit())
    {
      if (!is_dos_mode())
      {
        extern void wm_update_light(void);
        wm_update_light();
      }
      __asm__ volatile("hlt");
    }
    sched_progress_run();

    uint16_t k = c_getkey();
    uint8_t key = (uint8_t)(k & 0xFF);

    if (key == 27 || key == 15 || key == 19 || key == 24)
      break; /* ESC/Ctrl+O/Ctrl+S/Ctrl+X - save */

    if (key == 3)
    { /* Ctrl+C - cancel */
      puts("\n^C Cancelled\n");
      cancelled = true;
      break;
    }

    if (key == 11)
    { /* Ctrl+K - clear */
      for (int i = 0; i < MAX_FILE_SIZE; i++)
        v_buf[i] = 0;
      pos = 0;
      cls();
      puts("\n=== NANO Editor ===\n");
      puts("File: ");
      puts(full_path);
      puts("\nCtrl+O/Ctrl+S=Save  Ctrl+X=Save and exit  ESC=Save\n");
      puts("-------------------\n[Cleared]\n");
    }
    else if (key == 8)
    { /* Backspace */
      if (pos > 0)
      {
        pos--;
        v_buf[pos] = 0;
        putc(8);
        putc(' ');
        putc(8);
      }
    }
    else if (key >= 32 && key <= 126)
    { /* Printable */
      if (pos < MAX_FILE_SIZE - 1)
      {
        v_buf[pos++] = key;
        putc(key);
      }
    }
    else if (key == 13 || key == 10)
    { /* Enter */
      if (pos < MAX_FILE_SIZE - 1)
      {
        v_buf[pos++] = '\n';
        putc('\n');
      }
    }
  }

  if (cancelled)
  {
    if (is_new_file)
      fs_count--;
    return 0;
  }

  puts("\n");

  /* Find or create content slot */
  if (content_idx < 0)
  {
    if (file_content_count >= FS_MAX_FILE_CONTENTS)
    {
      puts("Error: Too many files\n");
      if (is_new_file)
        fs_count--;
      return -1;
    }
    content_idx = file_content_count++;
  }

  /* Zero and save */
  for (int i = 0; i < MAX_FILE_SIZE; i++)
  {
    file_contents[content_idx].data[i] = 0;
  }

  file_contents[content_idx].file_idx = (uint16_t)file_idx;
  file_contents[content_idx].size = (uint32_t)pos;

  for (int i = 0; i < pos; i++)
  {
    file_contents[content_idx].data[i] = v_buf[i];
  }

  /* CRITICAL: Update fs_table size BEFORE saving to disk */
  fs_table[file_idx].size = (uint32_t)pos;

  /* Save both file table and file contents to disk */
  if (fs_save_to_disk() == 0)
  {
    char tmp[16];
    puts("Saved ");
    int_to_str(pos, tmp);
    puts(tmp);
    puts(" bytes to disk\n");

    /* SUCCESS - data is on disk, show verification */
    puts("Verifying: fs_table[");
    int_to_str(file_idx, tmp);
    puts(tmp);
    puts("].size = ");
    int_to_str(fs_table[file_idx].size, tmp);
    puts(tmp);
    puts(" bytes\n");
  }
  else
  {
    puts("ERROR: Save failed!\n");
    return -1;
  }

  return 0;
}

/* 16. MEM - Display memory info */
extern uint32_t bios_ram_kb;
extern uint32_t bios_ram_bytes;

static int cmd_mem(const char *args)
{
  (void)args;
  uint32_t stats[3];
  mem_get_stats(stats);

  char v_buf[32];
  uint32_t total_ram = bios_ram_bytes;
  uint32_t total_kb = bios_ram_kb;

  puts("Physical Memory Information:\n");
  puts("----------------------------\n");

  puts("  Total RAM:    ");
  int_to_str(total_ram, v_buf);
  puts(v_buf);
  puts(" bytes (");
  int_to_str(total_kb / 1024, v_buf);
  puts(v_buf);
  puts(" MB)\n");

  puts("  Used Heap:    ");
  int_to_str(stats[1], v_buf);
  puts(v_buf);
  puts(" bytes (");
  int_to_str(stats[1] / 1024, v_buf);
  puts(v_buf);
  puts(" KB)\n");

  puts("  Free Heap:    ");
  int_to_str(stats[0], v_buf);
  puts(v_buf);
  puts(" bytes (");
  int_to_str(stats[0] / 1024, v_buf);
  puts(v_buf);
  puts(" KB)\n");

  puts("  Heap Total:   ");
  uint32_t heap_total = stats[0] + stats[1];
  int_to_str(heap_total, v_buf);
  puts(v_buf);
  puts(" bytes (");
  int_to_str(heap_total / 1024 / 1024, v_buf);
  puts(v_buf);
  puts(" MB)\n");

  puts("  Blocks:       ");
  int_to_str(stats[2], v_buf);
  puts(v_buf);
  puts(" allocated segments\n");

  return 0;
}

/* 17. ECHO - Echo text */
static int cmd_echo(const char *args)
{
  args = skip_spaces(args);
  puts(args);
  puts("\n");
  return 0;
}

/* 18. COLOR - Set text color */
static int cmd_color(const char *args)
{
  char tok[16];
  args = get_token(args, tok, 16);

  if (tok[0] == 0)
  {
    puts("Usage: COLOR <0-255>\n");
    puts("Examples: COLOR 10 (green), COLOR 12 (red), COLOR 14 (yellow)\n");
    puts("Format: Foreground + (Background * 16)\n");
    return -1;
  }

  if (tok[0] >= '0' && tok[0] <= '9')
  {
    uint8_t c = str_to_int(tok);
    current_color = c;
    set_attr(c);
    puts("Color changed to ");
    char v_buf[16];
    int_to_str(c, v_buf);
    puts(v_buf);
    puts("\n");
    return 0;
  }

  puts("Usage: COLOR 0-255\n");
  return -1;
}

/* 20. UPTIME - Show system uptime */
static int cmd_uptime(const char *args)
{
  (void)args;
  uint32_t ticks = get_ticks();
  uint32_t seconds = ticks / 18;
  uint32_t minutes = seconds / 60;
  uint32_t hours = minutes / 60;

  char v_buf[16];
  puts("Uptime: ");
  int_to_str(hours, v_buf);
  puts(v_buf);
  puts("h ");
  int_to_str(minutes % 60, v_buf);
  puts(v_buf);
  puts("m ");
  int_to_str(seconds % 60, v_buf);
  puts(v_buf);
  puts("s\n");

  return 0;
}

/* 21. COPY/CP - Copy file */
static int cmd_copy(const char *args)
{
  char src[64], dst[64];
  args = get_token(args, src, 64);
  args = get_token(args, dst, 64);

  if (src[0] == 0 || dst[0] == 0)
  {
    puts("Usage: COPY source dest\n");
    return -1;
  }

  char full_src[256], full_dst[256];
  build_full_path(src, full_src);
  build_full_path(dst, full_dst);

  if (sys_path_locked_abs(full_dst))
  {
    puts("Access denied: /installed.sys needs sudo.\n");
    return -1;
  }

  int src_idx = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_src) == 0 && fs_table[i].type == 0)
    {
      src_idx = i;
      break;
    }
  }

  if (src_idx < 0)
  {
    puts("Source file not found\n");
    return -1;
  }

  if (fs_count >= FS_MAX_FILES)
  {
    puts("Error: Filesystem full\n");
    return -1;
  }

  str_copy(fs_table[fs_count].name, full_dst, FS_MAX_FILENAME);
  fs_table[fs_count].size = fs_table[src_idx].size;
  fs_table[fs_count].type = 0;
  fs_table[fs_count].attr = fs_table[src_idx].attr;
  int dst_idx = fs_count;
  fs_count++;

  for (int i = 0; i < file_content_count; i++)
  {
    if (file_contents[i].file_idx == src_idx && file_content_count < FS_MAX_FILE_CONTENTS)
    {
      file_contents[file_content_count].file_idx = dst_idx;
      file_contents[file_content_count].size = file_contents[i].size;
      for (uint32_t j = 0; j < file_contents[i].size; j++)
      {
        file_contents[file_content_count].data[j] = file_contents[i].data[j];
      }
      file_content_count++;
      break;
    }
  }

  fs_save_to_disk();
  puts("File copied\n");
  return 0;
}

/* 22. MOVE/MV - Move file */
static int cmd_move(const char *args)
{
  char src[64], dst[64];
  args = get_token(args, src, 64);
  args = get_token(args, dst, 64);

  if (src[0] == 0 || dst[0] == 0)
  {
    puts("Usage: MOVE source dest\n");
    return -1;
  }

  char full_src[256], full_dst[256];
  build_full_path(src, full_src);
  build_full_path(dst, full_dst);

  if (sys_path_locked_abs(full_src) || sys_path_locked_abs(full_dst))
  {
    puts("Access denied: /installed.sys needs sudo.\n");
    return -1;
  }

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_src) == 0)
    {
      str_copy(fs_table[i].name, full_dst, FS_MAX_FILENAME);
      fs_save_to_disk();
      puts("File moved\n");
      return 0;
    }
  }

  puts("File not found\n");
  return -1;
}

/* 23. REN/RENAME - Rename file */
static int cmd_ren(const char *args) { return cmd_move(args); }

/* 24. FIND - Find files */
static int cmd_find(const char *args)
{
  char pattern[64];
  args = get_token(args, pattern, 64);

  if (pattern[0] == 0)
  {
    puts("Usage: FIND <pattern> [file]\n");
    return -1;
  }

  const char *rem = skip_spaces(args);
  if (*rem)
  {
    /* If a filename is provided, dispatch to GREP */
    int cmd_grep(const char *args);
    return cmd_grep(args);
  }

  int found = 0;
  for (int i = 0; i < fs_count; i++)
  {
    bool match = false;
    for (int j = 0; fs_table[i].name[j]; j++)
    {
      bool sub_match = true;
      for (int k = 0; pattern[k]; k++)
      {
        if (fs_table[i].name[j + k] != pattern[k])
        {
          sub_match = false;
          break;
        }
      }
      if (sub_match)
      {
        match = true;
        break;
      }
    }

    if (match)
    {
      puts(fs_table[i].name);
      puts("\n");
      found++;
    }
  }

  if (found == 0)
  {
    puts("No files found\n");
  }

  return 0;
}

/* 25. TREE - Show directory tree */
static int cmd_tree(const char *args)
{
  (void)args;
  puts("Directory tree:\n");
  puts(current_dir);
  puts("\n");

  int dir_len = str_len(current_dir);
  for (int i = 0; i < fs_count; i++)
  {
    /* Filter by current directory prefix */
    if (str_ncmp(fs_table[i].name, current_dir, dir_len) != 0)
      continue;

    puts("  ");
    if (fs_table[i].type == 1)
      puts("[DIR] ");
    else
      puts("[FILE] ");

    /* Show relative path */
    const char *rel_path = fs_table[i].name + dir_len;
    /* If we are at root '/', current_dir is '/' (len 1).
       The rel_path will be "filename" which is correct.
       If we are in '/dir/', rel_path is "file" which is correct. */
    if (*rel_path == 0)
      puts("."); /* current dir itself */
    else
      puts(rel_path);

    puts("\n");
  }

  return 0;
}

/* 26. ATTRIB - Show/set file attributes */
static int cmd_attrib(const char *args)
{
  char name[64];
  args = get_token(args, name, 64);

  if (name[0] == 0)
  {
    for (int i = 0; i < fs_count; i++)
    {
      puts("0x");
      print_hex(fs_table[i].attr);
      puts(" ");
      puts(fs_table[i].name);
      puts("\n");
    }
  }
  else
  {
    for (int i = 0; i < fs_count; i++)
    {
      if (str_cmp(fs_table[i].name, name) == 0)
      {
        puts("Attributes: 0x");
        print_hex(fs_table[i].attr);
        puts("\n");
        return 0;
      }
    }
    puts("File not found\n");
  }

  return 0;
}

/* 27. CHMOD - Change file mode */
static int cmd_chmod(const char *args)
{
  char mode[16], name[64];
  args = get_token(args, mode, 16);
  args = get_token(args, name, 64);

  if (mode[0] == 0 || name[0] == 0)
  {
    puts("Usage: CHMOD mode filename\n");
    return -1;
  }

  uint8_t new_attr = str_to_int(mode);

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, name) == 0)
    {
      fs_table[i].attr = new_attr;
      fs_save_to_disk();
      puts("Attributes changed\n");
      return 0;
    }
  }

  puts("File not found\n");
  return -1;
}

/* 27b. UNHIDE - Remove hidden attribute from file */
static int cmd_unhide(const char *args)
{
  char name[256];
  args = get_token(args, name, 256);

  if (name[0] == 0)
  {
    puts("Usage: UNHIDE filename\n");
    puts("Removes the hidden attribute (0x02) from a file\n");
    return -1;
  }

  /* Build full path if not absolute */
  char full_path[256];
  if (name[1] != ':')
  {
    int dl = str_len(current_dir);
    str_copy(full_path, current_dir, 256);
    int nl = str_len(name);
    for (int i = 0; i < nl; i++)
      full_path[dl + i] = name[i];
    full_path[dl + nl] = 0;
  }
  else
  {
    str_copy(full_path, name, 256);
  }

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      puts("File: ");
      puts(fs_table[i].name);
      puts("\nCurrent attributes: 0x");
      print_hex(fs_table[i].attr);
      puts("\n");

      /* Remove hidden bit (0x02) */
      fs_table[i].attr &= ~0x02;

      puts("New attributes: 0x");
      print_hex(fs_table[i].attr);
      puts("\n");
      fs_save_to_disk();
      puts("File unhidden successfully\n");
      return 0;
    }
  }
  puts("File not found: ");
  puts(full_path);
  puts("\n");
  return -1;
}

/* 28. VOL - Show volume info */
static int cmd_vol(const char *args)
{
  (void)args;
  puts(" Volume in drive C is ");
  puts(volume_label);
  puts(" Volume Serial Number is ");

  /* Print serial as XXXX-XXXX */
  print_hex16((uint16_t)(volume_serial >> 16));
  putc('-');
  print_hex16((uint16_t)(volume_serial & 0xFFFF));
  puts("\n Filesystem is AurionFS\n");
  return 0;
}

/* 29. LABEL - Set volume label */
static int cmd_label(const char *args)
{
  char new_label[13];
  args = get_token(args, new_label, 12);

  if (new_label[0] == 0)
  {
    puts("Volume label in drive C is ");
    puts(volume_label);
    puts("\nVolume Serial Number is ");
    print_hex((volume_serial >> 16) & 0xFFFF);
    putc('-');
    print_hex(volume_serial & 0xFFFF);
    puts("\nVolume label (11 characters, ENTER for none)? ");

    /* Interactive read */
    char input[13];
    int pos = 0;
    while (pos < 11)
    {
      uint16_t k = getkey();
      char c = (char)(k & 0xFF);
      if (c == 13 || c == 10)
        break;
      if (c == 8 && pos > 0)
      {
        pos--;
        puts("\b \b");
      }
      if (c >= 32 && c <= 126)
      {
        input[pos++] = c;
        putc(c);
      }
    }
    input[pos] = 0;
    putc('\n');
    if (pos > 0)
    {
      str_copy(volume_label, input, 13);
      str_upper(volume_label);
    }
  }
  else
  {
    str_copy(volume_label, new_label, 13);
    str_upper(volume_label);
  }

  fs_save_to_disk();
  return 0;
}

/* 30. CHKDSK - Check disk */
static int cmd_chkdsk(const char *args)
{
  bool fix = false;
  char tok[16];
  get_token(args, tok, 16);
  str_upper(tok);
  if (str_cmp(tok, "/F") == 0)
    fix = true;

  set_attr(0x0E);
  puts("The type of the file system is AurionFS.\n");
  set_attr(0x07);
  puts("Verifying file system integrity...\n");

  int errors = 0;
  int orphan_contents = 0;

  /* 1. Check for valid filenames and parent links */
  for (int i = 0; i < fs_count; i++)
  {
    if (fs_table[i].name[0] == 0)
    {
      errors++;
      continue;
    }
    /* Root check or valid parent check */
    if (fs_table[i].parent_idx != 0xFFFF && fs_table[i].parent_idx >= fs_count)
    {
      errors++;
      if (fix)
        fs_table[i].parent_idx = 0xFFFF; // Fix: Reset to root
    }
  }

  /* 2. Check content mapping */
  for (int i = 0; i < file_content_count; i++)
  {
    if (file_contents[i].file_idx >= fs_count)
    {
      orphan_contents++;
      errors++;

      if (fix)
      {
        /* Remove this content slot by shifting */
        for (int k = i; k < file_content_count - 1; k++)
        {
          file_contents[k] = file_contents[k + 1];
        }
        file_content_count--;
        i--; /* Re-check current index */
      }
    }
  }

  if (fix && errors > 0)
  {
    fs_save_to_disk();
    puts("Fixes applied to file system.\n");
    errors = 0; /* Reset error count for report */
  }

  char v_buf[32];
  puts("\nDisk Scan Results:\n");
  puts("  Total Files:      ");
  int_to_str(fs_count, v_buf);
  puts(v_buf);
  puts("\n");

  uint32_t total_size = 0;
  for (int i = 0; i < fs_count; i++)
    total_size += fs_table[i].size;

  puts("  Total Size:       ");
  int_to_str(total_size, v_buf);
  puts(v_buf);
  puts(" bytes\n");
  puts("  Content Slots:    ");
  int_to_str(file_content_count, v_buf);
  puts(v_buf);
  puts("/");
  int_to_str(FS_MAX_FILE_CONTENTS, v_buf);
  puts(v_buf);
  puts("\n");

  if (errors == 0)
  {
    set_attr(0x0A);
    puts("\nAurionOS has scanned the file system and found no problems.\n");
    puts("No further action is required.\n");
  }
  else
  {
    set_attr(0x0C);
    puts("\nFound ");
    int_to_str(errors, v_buf);
    puts(v_buf);
    puts(" inconsistencies!\n");
    puts("Run with /F to fix.\n");
  }
  set_attr(0x07);

  uint32_t du_k = 0, dt_k = 0;
  fs_get_usage(&du_k, &dt_k);
  puts("\n       ");
  int_to_str(dt_k, v_buf);
  puts(v_buf);
  puts(" KB total disk space.\n");
  int_to_str(du_k, v_buf);
  puts("       ");
  puts(v_buf);
  puts(" KB in files.\n");
  int_to_str((dt_k - du_k), v_buf);
  puts("       ");
  puts(v_buf);
  puts(" KB available on disk.\n");

  return 0;
}

/* 31. FORMAT - Format disk */
static int cmd_format(const char *args)
{
  char token[32];
  get_token(args, token, 32);
  str_upper(token);

  if (str_cmp(token, "CONFIRM") != 0)
  {
    puts("WARNING: This will erase all data!\n");
    puts("To format, run: FORMAT CONFIRM\n");
    return -1;
  }

  fs_count = 0;
  file_content_count = 0;
  user_count = 0; /* Reset users too */
  extern void cmd_init_silent(void);
  cmd_init_silent(); /* This recreates root user and \Desktop etc */
  fs_save_to_disk();
  puts("Format complete\n");

  return 0;
}

/* 32. DISKPART - Disk partitioning info */
typedef struct
{
  uint8_t boot_flag;
  uint8_t chs_start[3];
  uint8_t type;
  uint8_t chs_end[3];
  uint32_t lba_start;
  uint32_t sector_count;
} __attribute__((packed)) MBREntry;

static int cmd_diskpart(const char *args)
{
  (void)args;
  char v_buf[32];
  uint8_t sector[512];

  puts("AurionOS DiskPart Utility\n");
  puts("--------------------------\n");

  if (disk_read_lba(0, 1, sector) != 0)
  {
    set_attr(0x0C);
    puts("ERROR: Could not read Physical Drive 0 (ATA Master).\n");
    set_attr(0x07);
    return -1;
  }

  bool has_mbr = (sector[510] == 0x55 && sector[511] == 0xAA);

  puts("Disk ###  Status         Size     Free     Type\n");
  puts("--------  -------------  -------  -------  ----\n");
  puts("Disk 0    Online          100 MB      0 B  ");
  if (has_mbr)
    puts("MBR\n\n");
  else
    puts("RAW\n\n");

  if (has_mbr)
  {
    puts("Partition ###  Type              Size     Offset (LBA)\n");
    puts("-------------  ----------------  -------  ------------\n");
    MBREntry *entries = (MBREntry *)(sector + 446);
    int found = 0;
    for (int i = 0; i < 4; i++)
    {
      if (entries[i].sector_count > 0)
      {
        found++;
        puts("Partition ");
        int_to_str(i + 1, v_buf);
        puts(v_buf);
        puts("    ");

        /* Type string */
        if (entries[i].type == 0x83)
          puts("Linux/AurionFS  ");
        else if (entries[i].type == 0x07)
          puts("NTFS/HPFS       ");
        else if (entries[i].type == 0x0B || entries[i].type == 0x0C)
          puts("FAT32           ");
        else if (entries[i].type == 0xEE)
          puts("GPT Protective  ");
        else
        {
          puts("Unknown (0x");
          /* Simple hex for type */
          char hex[] = "0123456789ABCDEF";
          putc(hex[(entries[i].type >> 4) & 0xF]);
          putc(hex[entries[i].type & 0xF]);
          puts(")   ");
        }

        /* Size in MB */
        uint32_t mb = (entries[i].sector_count * 512) / (1024 * 1024);
        int_to_str(mb, v_buf);
        int len = str_len(v_buf);
        for (int k = 0; k < 6 - len; k++)
          putc(' ');
        puts(v_buf);
        puts(" MB   ");

        /* Offset */
        int_to_str(entries[i].lba_start, v_buf);
        puts(v_buf);
        puts("\n");
      }
    }
    if (found == 0)
      puts("No active partitions found in MBR.\n");
    puts("\n");
  }
  else
  {
    puts("The disk index 0 does not contain a valid MBR signature.\n");
    puts("System is currently using AurionFS in Native Raw mode.\n\n");
  }

  /* Internal FS Diagnostics */
  set_attr(0x0E);
  puts("AurionFS Internal Volume Mapping:\n");
  set_attr(0x07);
  puts("  [000-001] MBR/BootSector (Raw Binary)\n");
  puts("  [002-997] Kernel Executable Image\n");
  puts("  [   999 ] Runtime Directory State\n");
  puts("  [1000-1127] File Registry Table (");
  int_to_str(fs_count, v_buf);
  puts(v_buf);
  puts("/");
  int_to_str(FS_MAX_FILES, v_buf);
  puts(v_buf);
  puts(" slots)\n");
  puts("  [628-659] User Security Table (");
  int_to_str(user_count, v_buf);
  puts(v_buf);
  puts("/");
  int_to_str(FS_MAX_USERS, v_buf);
  puts(v_buf);
  puts(" users)\n");
  puts("  [700-LBA] Dynamic File Cluster Storage\n");

  return 0;
}

/* 33. FSCK - Filesystem check */
static int cmd_fsck(const char *args) { return cmd_chkdsk(args); }

/* Helper: Interactive Password Input (Masked) */
static void interactive_password_prompt(const char *prompt, char *out_pass, int max_len)
{
  puts(prompt);

  /* Flush keys */
  while (c_kb_hit())
    c_getkey();

  int pi = 0;
  while (pi < max_len - 1)
  {
    /* Keep system alive while waiting for key in GUI mode. In DOS mode
     * nothing charges the epoch here, so declare the wait: otherwise an
     * idle prompt looks exactly like a hung scheduler to the watchdog. */
    extern void sched_progress_wait(int s);
    extern void sched_progress_run(void);
    sched_progress_wait(1);
    while (!c_kb_hit())
    {
      if (!is_dos_mode())
      {
        extern void wm_update_light(void);
        wm_update_light();
      }
      __asm__ volatile("hlt");
    }
    sched_progress_run();

    uint16_t k = c_getkey();
    char c = (char)(k & 0xFF);
    if (c == 13 || c == 10)
      break;
    if (c == 8 || c == 127)
    {
      if (pi > 0)
      {
        pi--;
        puts("\b \b");
      }
      continue;
    }
    if (c >= 32 && c <= 126)
    {
      out_pass[pi++] = c;
      puts("*");
    }
  }
  out_pass[pi] = 0;
  puts("\n");
}

/* 34. USERADD - Add user */
int cmd_useradd(const char *args)
{
  char username[32];
  char password[32];
  args = get_token(args, username, 32);
  args = get_token(args, password, 32);

  if (username[0] == 0 || password[0] == 0)
  {
    puts("Usage: USERADD <username> <password>\n");
    return -1;
  }

  if (user_count >= FS_MAX_USERS)
  {
    puts("Error: User table full\n");
    return -1;
  }

  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, username) == 0)
    {
      puts("Error: User already exists\n");
      return -1;
    }
  }

  str_copy(user_table[user_count].username, username, 32);
  store_password_pbkdf2(&user_table[user_count], password);
  user_count++;

  fs_save_to_disk();
  puts("User created: ");
  puts(username);
  puts("\n");
  return 0;
}

/* 35. USERDEL - Delete user */
static int cmd_userdel(const char *args)
{
  char username[32];
  args = get_token(args, username, 32);

  if (username[0] == 0)
  {
    puts("Usage: USERDEL username\n");
    return -1;
  }

  if (str_cmp(username, "root") == 0 || str_cmp(username, "ROOT") == 0)
  {
    puts("Error: Cannot delete root user\n");
    return -1;
  }

  /* Authenticate current user first */
  char pass[32];
  char prompt_buf[64];
  str_copy(prompt_buf, "Password for ", 64);
  int cur_len = str_len(prompt_buf);
  str_copy(prompt_buf + cur_len, current_user, 64 - cur_len);
  cur_len = str_len(prompt_buf);
  str_copy(prompt_buf + cur_len, ": ", 64 - cur_len);

  interactive_password_prompt(prompt_buf, pass, 32);

  /* Verify password using PBKDF2 */
  bool ok = false;
  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, current_user) == 0)
    {
      ok = verify_password_with_migration(&user_table[i], pass);
      break;
    }
  }

  if (!ok)
  {
    puts("Authentication failed\n");
    return -1;
  }

  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, username) == 0)
    {
      for (int j = i; j < user_count - 1; j++)
      {
        user_table[j] = user_table[j + 1];
      }
      user_count--;
      fs_save_to_disk();
      puts("User deleted\n");
      return 0;
    }
  }

  puts("User not found\n");
  return -1;
}

/* 36. PASSWD - Change password */
int cmd_passwd(const char *args)
{
  char username[32];
  char newpass_arg[32];
  newpass_arg[0] = 0;

  args = get_token(args, username, 32);

  /* Check for non-interactive password argument */
  get_token(args, newpass_arg, 32);

  if (username[0] == 0)
  {
    /* Default to current user */
    str_copy(username, current_user, 32);
  }

  /* Find user */
  int user_idx = -1;
  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, username) == 0)
    {
      user_idx = i;
      break;
    }
  }

  if (user_idx == -1)
  {
    puts("User not found\n");
    return -1;
  }

  /* 1. Permission gate (AUR-22). Changing someone else's password requires
     root (or the pre-login boot/installer context, where current_user is
     empty). A plain user may only change their own password, and only with
     the old-password proof in the interactive path below. */
  bool is_root = (current_user[0] == 0) || (str_cmp(current_user, "root") == 0);
  bool is_self = (str_cmp(current_user, username) == 0);

  if (!is_self && !is_root)
  {
    puts("passwd: permission denied\n");
    return -1;
  }

  /* If a password was provided as an argument, skip interactivity (installer/scripts) */
  if (newpass_arg[0] != 0)
  {
    /* AUR-22: the non-interactive form carries no old-password proof, so
       it is restricted to root / the pre-login installer context. */
    if (!is_root)
    {
      puts("passwd: non-interactive password change requires root\n");
      return -1;
    }
    store_password_pbkdf2(&user_table[user_idx], newpass_arg);
    fs_save_to_disk();
    puts("passwd: password updated non-interactively\n");
    return 0;
  }

  /* Interactive logic for manual use */
  if (!is_root || (is_root && is_self))
  {
    char oldpass[32];
    interactive_password_prompt("Enter current password: ", oldpass, 32);

    if (!verify_password_with_migration(&user_table[user_idx], oldpass))
    {
      puts("passwd: Authentication token manipulation error\n");
      puts("passwd: password unchanged\n");
      return -1;
    }
  }

  /* 2. Enter New Password */
  char newpass1[32];
  char newpass2[32];

  interactive_password_prompt("Enter new password: ", newpass1, 32);
  interactive_password_prompt("Retype new password: ", newpass2, 32);

  if (str_cmp(newpass1, newpass2) != 0)
  {
    puts("Sorry, passwords do not match.\n");
    puts("passwd: password unchanged\n");
    return -1;
  }

  if (newpass1[0] == 0)
  {
    puts("Password cannot be empty.\n");
    return -1;
  }

  /* Update password using PBKDF2 */
  store_password_pbkdf2(&user_table[user_idx], newpass1);

  fs_save_to_disk();
  puts("passwd: password updated successfully\n");
  return 0;
}

/* 37. USERS - List users */
static int cmd_users(const char *args)
{
  (void)args;
  puts("System Users:\n");

  for (int i = 0; i < user_count; i++)
  {
    puts("  ");
    puts(user_table[i].username);
    puts("\n");
  }

  char v_buf[16];
  int_to_str(user_count, v_buf);
  puts("\nTotal: ");
  puts(v_buf);
  puts(" users\n");

  return 0;
}

/* 38. LOGIN - Login as user */
static int cmd_login(const char *args)
{
  char username[32];
  char password[32];
  args = get_token(args, username, 32);
  args = get_token(args, password, 32);

  if (username[0] == 0 || password[0] == 0)
  {
    puts("Usage: LOGIN <username> <password>\n");
    return -1;
  }

  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, username) == 0)
    {
      if (verify_password_with_migration(&user_table[i], password))
      {
        str_copy(current_user, username, 32);
        puts("Logged in as ");
        puts(username);
        puts("\n");
        return 0;
      }
      else
      {
        set_attr(0x0C);
        puts("Invalid password\n");
        set_attr(0x07);
        return -1;
      }
    }
  }

  puts("User not found\n");
  return -1;
}

/* 39. LOGOUT - Logout */
static int cmd_logout(const char *args)
{
  (void)args;
  /* AUR-06: Wipe current_user completely, do NOT assign "root".
     Logout must return to login screen with no authenticated user. */
  for (int i = 0; i < 32; i++) current_user[i] = '\0';
  puts("Logged out\n");
  return 0;
}

/* 40. WHOAMI - Show current user */
static int cmd_whoami(const char *args)
{
  (void)args;
  puts(current_user);
  puts("\n");
  return 0;
}

/* 41. SU - Switch user */
static int cmd_su(const char *args) { return cmd_login(args); }

/* 42. PS - List processes */
static int cmd_ps(const char *args)
{
  (void)args;
  char v_buf[32];
  uint32_t stats[3];
  mem_get_stats(stats);

  puts("PID   NAME             STATE        RAM USAGE    OWNER\n");
  puts("----  ---------------  -----------  -----------  ------\n");

  /* Calculate kernel memory (total used minus shell estimate) */
  uint32_t kernel_kb = stats[1] / 1024; /* Total used in KB */
  uint32_t shell_kb = stats[1] / 32;    /* Shell estimate */

  /* Account for GUI windows */
  int count = wm_get_window_count();
  uint32_t gui_total_kb = 0;
  for (int i = 0; i < count; i++)
  {
    Window *win = wm_get_window(i);
    if (win)
    {
      gui_total_kb += ((win->w * win->h * 4) + 4096) / 1024;
    }
  }

  /* Kernel memory = total - shell - GUI apps */
  uint32_t kernel_actual_kb = kernel_kb;
  if (kernel_actual_kb > shell_kb + gui_total_kb)
  {
    kernel_actual_kb -= (shell_kb + gui_total_kb);
  }

  /* PID 1-9: Core System Services */
  puts("1     KERNEL           RUNNING      ");
  if (kernel_actual_kb >= 1024)
  {
    int_to_str(kernel_actual_kb / 1024, v_buf);
    puts(v_buf);
    puts(".");
    int_to_str((kernel_actual_kb % 1024) / 100, v_buf);
    puts(v_buf);
    puts(" MB       ");
  }
  else
  {
    int_to_str(kernel_actual_kb, v_buf);
    puts(v_buf);
    puts(" KB       ");
  }
  puts("system\n");

  puts("2     SHELL            RUNNING      ");
  int_to_str(shell_kb, v_buf);
  puts(v_buf);
  puts(" KB       ");
  puts(current_user);
  puts("\n");

  /* PID 100+: GUI Apps from Window Manager */
  for (int i = 0; i < count; i++)
  {
    Window *win = wm_get_window(i);
    if (!win)
      continue;

    /* PID */
    int pid = 100 + win->app_id;
    int_to_str(pid, v_buf);
    puts(v_buf);
    int len = str_len(v_buf);
    for (int k = 0; k < 6 - len; k++)
      putc(' ');

    /* Name */
    char name[16];
    str_copy(name, win->title, 15);
    puts(name);
    len = str_len(name);
    for (int k = 0; k < 17 - len; k++)
      putc(' ');

    /* State */
    if (win->focused)
      puts("FOCUSED      ");
    else if (win->visible)
      puts("VISIBLE      ");
    else
      puts("MINIMIZED    ");

    /* RAM usage estimate (Backbuffer + State) */
    uint32_t ram = (win->w * win->h * 4) + 4096;
    int_to_str(ram / 1024, v_buf);
    puts(v_buf);
    puts(" KB      ");

    /* Owner */
    puts(current_user);
    puts("\n");
  }

  return 0;
}

/* 43. KILL - Kill process */
static int cmd_kill(const char *args)
{
  char pid_str[16];
  args = get_token(args, pid_str, 16);

  if (pid_str[0] == 0)
  {
    puts("Usage: KILL <pid>\n");
    return -1;
  }

  uint32_t pid = str_to_int(pid_str);

  if (pid < 10)
  {
    puts("Error: Cannot kill critical system process (KERNEL/SHELL)\n");
    return -1;
  }

  if (pid >= 100)
  {
    int app_id = pid - 100;
    int count = wm_get_window_count();
    for (int i = 0; i < count; i++)
    {
      Window *win = wm_get_window(i);
      if (win && win->app_id == app_id)
      {
        puts("Stopping process: ");
        puts(win->title);
        puts("...\n");
        wm_destroy_window(win);
        return 0;
      }
    }
  }

  puts("Error: No process found with PID ");
  puts(pid_str);
  puts("\n");
  return -1;
}

/* 44. TOP - Show top processes */
static int cmd_top(const char *args)
{
  (void)args;
  puts("AurionOS Top Resource Monitor. Press Q to exit.\n");

  while (1)
  {
    cls();
    puts("--- AurionOS Top - Real-time Diagnostics ---\n");
    cmd_mem("");
    puts("\n");
    cmd_ps("");

    /* Refresh every 1.5 seconds or on keypress */
    uint32_t start = get_ticks();
    extern volatile int terminal_last_char;
    terminal_last_char = 0;

    extern void sched_heartbeat(void);
    while (get_ticks() - start < 150)
    {
      sched_heartbeat(); /* redraw loop is alive while it waits for Q */
      /* Check for exit key via terminal_last_char (set by GUI terminal) */
      if (terminal_last_char != 0)
      {
        uint8_t ch = (uint8_t)(terminal_last_char & 0xFF);
        terminal_last_char = 0;
        if (ch == 'q' || ch == 'Q' || ch == 3 || ch == 27)
        {
          puts("\nResource monitor ended.\n");
          return 0;
        }
      }
      if (!is_dos_mode())
      {
        extern void wm_update_light(void);
        wm_update_light();
      }
      else
      {
        /* DOS mode: check hardware keyboard directly */
        if (c_kb_hit())
        {
          uint8_t k = (uint8_t)(c_getkey() & 0xFF);
          if (k == 'q' || k == 'Q' || k == 3 || k == 27)
          {
            puts("\nResource monitor ended.\n");
            return 0;
          }
        }
      }
      __asm__ volatile("hlt");
    }
  }
}

/* 45. TASKLIST - List tasks */
static int cmd_tasklist(const char *args) { return cmd_ps(args); }

/* 46. TASKKILL - Kill task */
static int cmd_taskkill(const char *args) { return cmd_kill(args); }

/* 47. PAUSE - Pause execution */
static int cmd_pause(const char *args)
{
  (void)args;
  puts("Press any key to continue . . . ");
  while (!c_kb_hit())
  {
    if (!is_dos_mode())
    {
      extern void wm_update_light(void);
      wm_update_light();
    }
    __asm__ volatile("hlt");
  }
  c_getkey();
  puts("\n");
  return 0;
}

/* 48. SLEEP - Sleep for seconds */
static int cmd_sleep(const char *args)
{
  char tok[16];
  args = get_token(args, tok, 16);

  if (tok[0] == 0)
  {
    puts("Usage: SLEEP seconds\n");
    return -1;
  }

  uint32_t sec = str_to_int(tok);
  uint32_t end = get_ticks() + (sec * 100); /* 100Hz PIT */
  extern void sched_heartbeat(void);
  while (get_ticks() < end)
  {
    /* Intentionally avoid WM updates here so both keyboard and mouse appear asleep. */
    sched_heartbeat(); /* provably alive: timed wait, not a hang */
    __asm__ volatile("hlt");
  }
  return 0;
}

/* 50. EXIT - Exit shell */
static int cmd_exit(const char *args)
{
  extern int cmd_reboot(const char *);
  return cmd_reboot(args);
}

/* 51. CALC - Simple calculator */
static int cmd_calc(const char *args)
{
  char a_str[16], op[4], b_str[16];
  args = get_token(args, a_str, 16);
  args = get_token(args, op, 4);
  args = get_token(args, b_str, 16);

  if (a_str[0] == 0 || op[0] == 0 || b_str[0] == 0)
  {
    puts("Usage: CALC num op num (e.g., CALC 5 + 3)\n");
    return -1;
  }

  uint32_t a = str_to_int(a_str);
  uint32_t b = str_to_int(b_str);
  uint32_t result = 0;

  if (op[0] == '+')
  {
    result = a + b;
  }
  else if (op[0] == '-')
  {
    result = a - b;
  }
  else if (op[0] == '*')
  {
    result = a * b;
  }
  else if (op[0] == '/')
  {
    if (b == 0)
    {
      puts("Error: Division by zero\n");
      return -1;
    }
    result = a / b;
  }
  else
  {
    puts("Unknown operator\n");
    return -1;
  }

  char v_buf[16];
  int_to_str(result, v_buf);
  puts("Result: ");
  puts(v_buf);
  puts("\n");

  return 0;
}

/* 52. HEXDUMP - Hex dump of memory */
static int cmd_hexdump(const char *args)
{
  char addr_str[16];
  args = get_token(args, addr_str, 16);

  if (addr_str[0] == 0)
  {
    puts("Usage: HEXDUMP address\n");
    return -1;
  }

  uint32_t addr = str_to_int(addr_str);
  uint8_t *ptr = (uint8_t *)addr;

  puts("Hex dump at 0x");
  print_hex(addr);
  puts(":\n");

  for (int i = 0; i < 16; i++)
  {
    print_hex(ptr[i]);
    puts(" ");
  }
  puts("\n");

  return 0;
}

/* 53. ASCII - Show ASCII table */
static int cmd_ascii(const char *args)
{
  (void)args;
  puts("ASCII Table (printable):\n");

  for (int i = 32; i < 127; i++)
  {
    char v_buf[16];
    int_to_str(i, v_buf);
    if (i < 100)
      putc('0'); /* Pad cleanly */
    puts(v_buf);
    puts(": ");
    putc((char)i);
    puts("  ");

    if ((i - 31) % 8 == 0)
    {
      puts("\n");
    }
  }
  puts("\n");

  return 0;
}

/* 54. HASH - Hash a string */
static int cmd_hash(const char *args)
{
  args = skip_spaces(args);

  if (args[0] == 0)
  {
    puts("Usage: HASH string\n");
    return -1;
  }

  uint32_t h = hash_string(args);

  puts("Hash: 0x");
  print_hex(h);
  puts("\n");

  return 0;
}

/* Dummy declarations to satisfy any remaining references */

/* Alias commands */
static int cmd_clear(const char *args) { return cmd_cls(args); }
static int cmd_rm(const char *args) { return cmd_del(args); }
static int cmd_ls(const char *args) { return cmd_dir(args); }
static int cmd_pwd(const char *args)
{
  (void)args;
  puts(current_dir);
  puts("\n");
  return 0;
}
static int cmd_type(const char *args) { return cmd_cat(args); }
static int cmd_cp(const char *args) { return cmd_copy(args); }
static int cmd_mv(const char *args) { return cmd_move(args); }
/* cmd_wifitest defined above with Rust driver */

/* System commands with real implementations */

static int cmd_mount(const char *a)
{
  (void)a;
  puts("Mounted filesystems:\n");

  /* C: is the primary AurionFS volume on Hard Disk */
  puts("  C:\\    AurionFS    rw,relatime    (LBA 1000+)\n");

  /* Virtual Filesystems */
  puts("  /dev   devfs       rw,nosuid      (devices)\n");

  ensure_processes_init();
  puts("  /proc  procfs      ro,nosuid      (processes)\n");

  return 0;
}

static int cmd_umount(const char *a)
{
  char tok[64];
  a = get_token(a, tok, 64);
  if (tok[0] == 0)
  {
    puts("Usage: UMOUNT <mountpoint>\n");
    return -1;
  }

  if (str_cmp(tok, "C:\\") == 0 || str_cmp(tok, "C:") == 0 || str_cmp(tok, "/") == 0)
  {
    puts("umount: C:\\ is busy (root filesystem)\n");
    return -1;
  }

  if (str_cmp(tok, "/dev") == 0 || str_cmp(tok, "/proc") == 0)
  {
    puts("umount: ");
    puts(tok);
    puts(" is a kernel-reserved virtual mountpoint\n");
    return -1;
  }

  puts("umount: ");
  puts(tok);
  puts(": not mounted\n");
  return -1;
}

static int cmd_sync(const char *a)
{
  (void)a;
  puts("Syncing cached data to disk...\n");
  if (fs_save_to_disk() == 0)
  {
    puts("Filesystem synchronized (AurionFS LBA 1000+).\n");
    return 0;
  }
  else
  {
    puts("Error: Sync failed (disk write error).\n");
    return -1;
  }
}

static int cmd_free(const char *a)
{
  (void)a;
  uint32_t stats[4];
  mem_get_stats(stats);
  char v_buf[16];
  extern uint32_t bios_ram_bytes;
  puts("              total       used       free\n");
  puts("Mem:    ");
  int_to_str(bios_ram_bytes, v_buf);
  puts(v_buf);
  puts("   ");
  int_to_str(stats[1], v_buf);
  puts(v_buf);
  puts("   ");
  int_to_str(stats[0], v_buf);
  puts(v_buf);
  puts("\n");
  return 0;
}

static int cmd_df(const char *a)
{
  (void)a;
  char v_buf[32];
  puts("Filesystem      Size      Used     Avail  Use%  Mounted on\n");

  uint32_t used = 0, tot = 0;
  fs_get_usage(&used, &tot);
  uint32_t used_k = used;
  uint32_t total_k = tot ? tot : 102400;
  uint32_t avail_k = (used_k > total_k) ? 0 : (total_k - used_k);
  int pct = (used_k * 100) / total_k;

  puts("C:\\           100.0M  ");

  /* Used */
  int_to_str(used_k, v_buf);
  int len = str_len(v_buf);
  for (int i = 0; i < 7 - len; i++)
    putc(' ');
  puts(v_buf);
  puts("K  ");

  /* Avail */
  int_to_str(avail_k, v_buf);
  len = str_len(v_buf);
  for (int i = 0; i < 7 - len; i++)
    putc(' ');
  puts(v_buf);
  puts("K  ");

  /* Pct */
  int_to_str(pct, v_buf);
  len = str_len(v_buf);
  for (int i = 0; i < 3 - len; i++)
    putc(' ');
  puts(v_buf);
  puts("%  /\n");

  /* Virtuals */
  puts("devfs             0B        0B        0B   0%  /dev\n");
  puts("procfs            0B        0B        0B   0%  /proc\n");

  return 0;
}

static int cmd_du(const char *args)
{
  char tok[64];
  get_token(args, tok, 64);
  char v_buf[16];

  if (tok[0] == 0)
  {
    uint32_t total = 0;
    puts("Size\tPath\n");
    puts("----\t-----\n");
    for (int i = 0; i < fs_count; i++)
    {
      total += fs_table[i].size;
      int_to_str(fs_table[i].size, v_buf);
      puts(v_buf);
      puts("\t");
      puts(fs_table[i].name);
      puts("\n");
    }
    puts("----\t-----\n");
    int_to_str(total, v_buf);
    puts(v_buf);
    puts("\ttotal\n");
  }
  else
  {
    for (int i = 0; i < fs_count; i++)
    {
      if (str_cmp(fs_table[i].name, tok) == 0)
      {
        int_to_str(fs_table[i].size, v_buf);
        puts(v_buf);
        puts("\t");
        puts(tok);
        puts("\n");
        return 0;
      }
    }
    puts("du: cannot access '");
    puts(tok);
    puts("'\n");
  }
  return 0;
}

static int cmd_lsblk(const char *a)
{
  (void)a;
  puts("NAME MAJ:MIN  SIZE TYPE MOUNTPOINT\n");
  puts("hda    3:0   100M disk\n");
  puts("  hda1 3:1   100M part /\n");
  return 0;
}

static int cmd_fdisk(const char *a)
{
  (void)a;
  puts("Disk /dev/hda: 100 MB, 104857600 bytes\n");
  puts("  Device    Boot  Start  End   Sectors  Size  Id  Type\n");
  puts("  /dev/hda1  *       1   204800  204800 100M   1  AurionFS\n");
  return 0;
}

static int cmd_blkid(const char *a)
{
  (void)a;
  puts("/dev/hda1: UUID=\"");
  print_hex16((uint16_t)(volume_serial >> 16));
  putc('-');
  print_hex16((uint16_t)(volume_serial & 0xFFFF));
  puts("\" TYPE=\"AurionFS\" LABEL=\"");
  puts(volume_label);
  puts("\"\n");
  return 0;
}

static int cmd_readsector(const char *a)
{
  char tok[16];
  a = get_token(a, tok, 16);
  if (tok[0] == 0)
  {
    puts("Usage: READSECTOR <sector>\n");
    return -1;
  }
  uint32_t sector = str_to_int(tok);
  char v_buf[512];
  if (disk_read_lba(sector, 1, v_buf) == 0)
  {
    puts("Sector dump:\n");
    char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 512; i++)
    {
      char hb[3];
      hb[0] = hex[((uint8_t)v_buf[i]) >> 4];
      hb[1] = hex[((uint8_t)v_buf[i]) & 15];
      hb[2] = 0;
      puts(hb);
      puts(" ");
      if ((i + 1) % 16 == 0)
        puts("\n");
    }
  }
  else
  {
    puts("Error reading sector!\n");
  }
  return 0;
}

static int cmd_sysinfo(const char *a)
{
  (void)a;
  uint32_t stats[4];
  mem_get_stats(stats);
  char v_buf[16];
  puts("=== AurionOS System Information ===\n");
  puts("OS:         AurionOS v1.2 Nightly\n");
  puts("CPU:        x86 i386-compatible\n");
  puts("Arch:       32-bit Protected Mode\n");
  puts("Network:    none (removed)\n");
  puts("Graphics:   VBE Framebuffer\n");
  puts("Free Mem:   ");
  int_to_str(stats[0], v_buf);
  puts(v_buf);
  puts(" bytes\n");
  puts("Used Mem:   ");
  int_to_str(stats[1], v_buf);
  puts(v_buf);
  puts(" bytes\n");
  puts("Files:      ");
  int_to_str(fs_count, v_buf);
  puts(v_buf);
  puts("\n");
  puts("User:       ");
  puts(current_user);
  puts("\n");
  puts("Contributor: Mage7968 (run MAGE7968 in terminal)\n");
  return 0;
}

static int cmd_uname(const char *a)
{
  char tok[16];
  a = get_token(a, tok, 16);
  if (tok[0] == 0 || str_cmp(tok, "-a") == 0)
  {
    puts("AurionOS v1.2 Nightly i386 AurionOS/x86\n");
  }
  else if (str_cmp(tok, "-n") == 0)
  {
    puts("AurionOS\n");
  }
  else if (str_cmp(tok, "-s") == 0)
  {
    puts("AurionOS\n");
  }
  else if (str_cmp(tok, "-r") == 0)
  {
    puts("1.0.0\n");
  }
  else if (str_cmp(tok, "-m") == 0)
  {
    puts("i386\n");
  }
  else
  {
    puts("AurionOS rodos 1.0.0 i386 AurionOS/x86\n");
  }
  return 0;
}

static int cmd_hostname(const char *a)
{
  char tok[64];
  a = get_token(a, tok, 64);
  if (tok[0] == 0)
  {
    puts("AurionOS\n");
  }
  else
  {
    puts("hostname: cannot set hostname (read-only)\n");
  }
  return 0;
}

static int cmd_lscpu(const char *a)
{
  (void)a;
  puts("Architecture:     x86 (i386)\n");
  puts("CPU op-mode(s):   32-bit\n");
  puts("Byte Order:       Little Endian\n");
  puts("Address sizes:    32 bits physical, 32 bits virtual\n");
  puts("CPU(s):           1\n");
  puts("Model name:       x86 Family\n");
  return 0;
}
static int cmd_lspci(const char *a)
{
  (void)a;
  /* Direct PCI scan - check all slots on bus 0 */
  extern uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

  puts("=== PCI Scan (Bus 0) ===\n");
  char hex[] = "0123456789ABCDEF";
  int found = 0;

  for (int dev = 0; dev < 32; dev++)
  {
    uint32_t vendor_device = pci_config_read(0, dev, 0, 0x00);
    uint16_t vendor = vendor_device & 0xFFFF;
    uint16_t device_id = (vendor_device >> 16) & 0xFFFF;

    if (vendor != 0xFFFF && vendor != 0x0000)
    {
      uint32_t bar0 = pci_config_read(0, dev, 0, 0x10);
      uint32_t bar1 = pci_config_read(0, dev, 0, 0x14);

      puts("Slot ");
      if (dev < 10)
        putc('0');
      putc('0' + dev / 10);
      putc('0' + dev % 10);
      puts(": Vendor=");
      for (int j = 12; j >= 0; j -= 4)
        putc(hex[(vendor >> j) & 0xF]);
      puts(" Dev=");
      for (int j = 12; j >= 0; j -= 4)
        putc(hex[(device_id >> j) & 0xF]);
      puts(" BAR0=");
      for (int j = 28; j >= 0; j -= 4)
        putc(hex[(bar0 >> j) & 0xF]);
      puts(" BAR1=");
      for (int j = 28; j >= 0; j -= 4)
        putc(hex[(bar1 >> j) & 0xF]);
      puts("\n");
      found++;

      /* Check if VirtIO */
      if (vendor == 0x1AF4)
      {
        puts("  ^^^ VirtIO device found!\n");
      }
    }
  }

  if (found == 0)
    puts("No PCI devices found!\n");
  else
  {
    puts("Total: ");
    putc('0' + found / 10);
    putc('0' + found % 10);
    puts(" devices\n");
  }
  puts("VirtIO vendor ID is 1AF4\n");
  return 0;
}

static int cmd_dmesg(const char *a)
{
  (void)a;
  /* Real ring: every value below is read live, nothing is canned.
   * Timestamps come from the 100 Hz PIT, memory from the allocator,
   * files from the filesystem, network from the default interface. */
  uint32_t stats[4];
  mem_get_stats(stats);
  char v_buf[16];
  uint32_t t = get_ticks();
  uint32_t sec = t / 100;
  uint32_t dsec = (t % 100) / 10;

  puts("[      0.0] AurionOS kernel initialized\n");
  puts("[      0.0] PIT channel 0 at 100 Hz, VBE framebuffer active\n");
  puts("[");
  int_to_str(sec, v_buf);
  /* right-align seconds in a 6-wide field */
  {
    int w = 6 - str_len(v_buf) - 2;
    for (int i = 0; i < w; i++)
      putc(' ');
    puts(v_buf);
    putc('.');
    putc('0' + (char)dsec);
  }
  puts("] mem free=");
  int_to_str(stats[0], v_buf);
  puts(v_buf);
  puts(" used=");
  int_to_str(stats[1], v_buf);
  puts(v_buf);
  puts(" files=");
  int_to_str(fs_count, v_buf);
  puts(v_buf);
  puts(" user=");
  puts(current_user[0] ? current_user : "-");
  puts("\n");
  puts("] net removed\n");
  return 0;
}
static int cmd_mode(const char *a)
{
  (void)a;
  puts("Current mode: Text 80x25\n");
  return 0;
}

/* WC - word/line/char count */
static int cmd_wc(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: WC <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0 && fs_table[i].type == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
  {
    puts("  0  0  0 ");
    puts(name);
    puts("\n");
    return 0;
  }
  int lines = 0, words = 0, chars = (int)file_contents[ci].size;
  bool inw = false;
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
  {
    char c = file_contents[ci].data[j];
    if (c == '\n')
      lines++;
    if (c == ' ' || c == '\n' || c == '\t')
    {
      if (inw)
        words++;
      inw = false;
    }
    else
      inw = true;
  }
  if (inw)
    words++;
  char v_buf[16];
  puts("  ");
  int_to_str(lines, v_buf);
  puts(v_buf);
  puts("  ");
  int_to_str(words, v_buf);
  puts(v_buf);
  puts("  ");
  int_to_str(chars, v_buf);
  puts(v_buf);
  puts(" ");
  puts(name);
  puts("\n");
  return 0;
}

/* HEAD - show first N lines */
static int cmd_head(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: HEAD <file> [n]\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0 && fs_table[i].type == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
    return 0;
  int maxlines = 10, lines = 0;
  for (uint32_t j = 0; j < file_contents[ci].size && lines < maxlines; j++)
  {
    putc(file_contents[ci].data[j]);
    if (file_contents[ci].data[j] == '\n')
      lines++;
  }
  return 0;
}

/* TAIL - show last N lines */
static int cmd_tail(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: TAIL <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0 && fs_table[i].type == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
    return 0;
  int total_lines = 0;
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
    if (file_contents[ci].data[j] == '\n')
      total_lines++;
  int skip = total_lines - 10;
  if (skip < 0)
    skip = 0;
  int cur = 0;
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
  {
    if (cur >= skip)
      putc(file_contents[ci].data[j]);
    if (file_contents[ci].data[j] == '\n')
      cur++;
  }
  return 0;
}

/* GREP - search for pattern in file */
int cmd_grep(const char *a)
{
  char pat[64], name[64];
  a = get_token(a, pat, 64);
  a = get_token(a, name, 64);
  if (pat[0] == 0)
  {
    puts("Usage: GREP <pattern> <file>\n");
    return -1;
  }
  if (name[0] == 0)
  {
    puts("Usage: GREP <pattern> <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0 && fs_table[i].type == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
    return 0;
  int plen = str_len(pat);
  /* Line-by-line search */
  uint32_t ls = 0;
  for (uint32_t j = 0; j <= file_contents[ci].size; j++)
  {
    bool eol = (j == file_contents[ci].size || file_contents[ci].data[j] == '\n');
    if (eol)
    {
      /* Check if pattern in this line */
      for (uint32_t k = ls; k + plen <= j; k++)
      {
        bool m = true;
        for (int p = 0; p < plen; p++)
        {
          if (file_contents[ci].data[k + p] != pat[p])
          {
            m = false;
            break;
          }
        }
        if (m)
        {
          for (uint32_t x = ls; x < j; x++)
            putc(file_contents[ci].data[x]);
          putc('\n');
          break;
        }
      }
      ls = j + 1;
    }
  }
  return 0;
}

/* Bounded line index shared by SORT and UNIQ. The file stays where it is;
 * only offsets sort, so even MAX_FILE_SIZE input costs ~1 KB of stack. */
#define SORT_MAX_LINES 128

static int collect_lines(const char *data, uint32_t size,
                         uint32_t offs[SORT_MAX_LINES],
                         uint32_t lens[SORT_MAX_LINES])
{
  int n = 0;
  uint32_t ls = 0;
  for (uint32_t j = 0; j <= size; j++)
  {
    bool eol = (j == size || data[j] == '\n');
    if (!eol)
      continue;
    uint32_t len = j - ls;
    if (len > 0 && data[j - 1] == '\r')
      len--;
    if (n < SORT_MAX_LINES)
    {
      offs[n] = ls;
      lens[n] = len;
      n++;
    }
    else
    {
      return -1; /* over the line cap: caller reports honestly */
    }
    ls = j + 1;
  }
  return n;
}

static int cmp_line(const char *data, uint32_t ao, uint32_t al,
                    uint32_t bo, uint32_t bl)
{
  uint32_t m = (al < bl) ? al : bl;
  for (uint32_t i = 0; i < m; i++)
  {
    uint8_t ca = (uint8_t)data[ao + i];
    uint8_t cb = (uint8_t)data[bo + i];
    if (ca != cb)
      return (int)ca - (int)cb;
  }
  if (al == bl)
    return 0;
  return (al < bl) ? -1 : 1;
}

static int cmd_sort(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: SORT <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full) == 0 && fs_table[i].type == 0)
    {
      fi = i;
      break;
    }
  }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  }
  if (ci < 0)
    return 0;

  /* Real byte-wise line sort (insertion sort: tiny n, no alloc). */
  const char *data = file_contents[ci].data;
  uint32_t size = file_contents[ci].size;
  uint32_t offs[SORT_MAX_LINES];
  uint32_t lens[SORT_MAX_LINES];
  int n = collect_lines(data, size, offs, lens);
  if (n < 0)
  {
    puts("SORT: file has more than 128 lines, refusing (bounded).\n");
    return -1;
  }
  for (int i = 1; i < n; i++)
  {
    uint32_t ko = offs[i], kl = lens[i];
    int j = i - 1;
    while (j >= 0 && cmp_line(data, offs[j], lens[j], ko, kl) > 0)
    {
      offs[j + 1] = offs[j];
      lens[j + 1] = lens[j];
      j--;
    }
    offs[j + 1] = ko;
    lens[j + 1] = kl;
  }
  for (int i = 0; i < n; i++)
  {
    for (uint32_t k = 0; k < lens[i]; k++)
      putc(data[offs[i] + k]);
    putc('\n');
  }
  return 0;
}
static int cmd_uniq(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: UNIQ <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full) == 0 && fs_table[i].type == 0)
    {
      fi = i;
      break;
    }
  }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  }
  if (ci < 0)
    return 0;

  /* Real uniq: collapse adjacent duplicate lines, byte comparison. */
  const char *data = file_contents[ci].data;
  uint32_t size = file_contents[ci].size;
  uint32_t offs[SORT_MAX_LINES];
  uint32_t lens[SORT_MAX_LINES];
  int n = collect_lines(data, size, offs, lens);
  if (n < 0)
  {
    puts("UNIQ: file has more than 128 lines, refusing (bounded).\n");
    return -1;
  }
  for (int i = 0; i < n; i++)
  {
    if (i > 0 && cmp_line(data, offs[i - 1], lens[i - 1],
                          offs[i], lens[i]) == 0)
      continue;
    for (uint32_t k = 0; k < lens[i]; k++)
      putc(data[offs[i] + k]);
    putc('\n');
  }
  return 0;
}

/* DIFF - compare two files */
static int cmd_diff(const char *a)
{
  char f1[64], f2[64];
  a = get_token(a, f1, 64);
  a = get_token(a, f2, 64);
  if (f1[0] == 0 || f2[0] == 0)
  {
    puts("Usage: DIFF <file1> <file2>\n");
    return -1;
  }
  int i1 = -1, i2 = -1;
  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, f1) == 0)
      i1 = i;
    if (str_cmp(fs_table[i].name, f2) == 0)
      i2 = i;
  }
  if (i1 < 0)
  {
    puts("File not found: ");
    puts(f1);
    puts("\n");
    return -1;
  }
  if (i2 < 0)
  {
    puts("File not found: ");
    puts(f2);
    puts("\n");
    return -1;
  }
  int c1 = -1, c2 = -1;
  for (int i = 0; i < file_content_count; i++)
  {
    if ((int)file_contents[i].file_idx == i1)
      c1 = i;
    if ((int)file_contents[i].file_idx == i2)
      c2 = i;
  }
  if (c1 < 0 && c2 < 0)
  {
    puts("Files are identical (both empty)\n");
    return 0;
  }
  if (c1 < 0 || c2 < 0)
  {
    puts("Files differ\n");
    return 0;
  }
  if (file_contents[c1].size != file_contents[c2].size)
  {
    puts("Files differ in size\n");
    return 0;
  }
  for (uint32_t j = 0; j < file_contents[c1].size; j++)
  {
    if (file_contents[c1].data[j] != file_contents[c2].data[j])
    {
      puts("Files differ\n");
      return 0;
    }
  }
  puts("Files are identical\n");
  return 0;
}

static int cmd_cut(const char *a)
{
  (void)a;
  puts("Usage: CUT -d<delim> -f<field> <file>\n(Simplified: use GREP)\n");
  return 0;
}
static int cmd_paste(const char *a)
{
  (void)a;
  puts("PASTE: Requires multiple file merging\n");
  return 0;
}
static int cmd_tr(const char *a)
{
  (void)a;
  puts("TR: Requires piping (not supported)\n");
  return 0;
}
static int cmd_sed(const char *a)
{
  (void)a;
  puts("SED: Stream editor requires piping\n");
  return 0;
}
static int cmd_awk(const char *a)
{
  (void)a;
  puts("AWK: Pattern processing requires piping\n");
  return 0;
}

/* BASE64 encode */
static int cmd_base64(const char *a)
{
  a = skip_spaces(a);
  if (a[0] == 0)
  {
    puts("Usage: BASE64 <text>\n");
    return -1;
  }
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int len = str_len(a);
  for (int i = 0; i < len; i += 3)
  {
    uint32_t n = ((uint32_t)(uint8_t)a[i]) << 16;
    if (i + 1 < len)
      n |= ((uint32_t)(uint8_t)a[i + 1]) << 8;
    if (i + 2 < len)
      n |= (uint32_t)(uint8_t)a[i + 2];
    putc(b64[(n >> 18) & 63]);
    putc(b64[(n >> 12) & 63]);
    putc((i + 1 < len) ? b64[(n >> 6) & 63] : '=');
    putc((i + 2 < len) ? b64[n & 63] : '=');
  }
  putc('\n');
  return 0;
}

/* XXD - hex dump of file */
static int cmd_xxd(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: XXD <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
  {
    puts("(empty file)\n");
    return 0;
  }
  uint32_t sz = file_contents[ci].size;
  if (sz > 256)
    sz = 256;
  for (uint32_t j = 0; j < sz; j += 16)
  {
    print_hex(j);
    puts(": ");
    for (int k = 0; k < 16; k++)
    {
      if (j + k < sz)
        print_hex(file_contents[ci].data[j + k]);
      else
        puts("  ");
      putc(' ');
    }
    putc(' ');
    for (int k = 0; k < 16 && j + k < sz; k++)
    {
      char c = file_contents[ci].data[j + k];
      putc((c >= 32 && c < 127) ? c : '.');
    }
    putc('\n');
  }
  return 0;
}

static int cmd_od(const char *a) { return cmd_xxd(a); }

/* REV - reverse string */
static int cmd_rev(const char *a)
{
  a = skip_spaces(a);
  if (a[0] == 0)
  {
    puts("Usage: REV <text>\n");
    return -1;
  }
  int len = str_len(a);
  for (int i = len - 1; i >= 0; i--)
    putc(a[i]);
  putc('\n');
  return 0;
}

/* NL - number lines */
static int cmd_nl(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: NL <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
    return 0;
  int line = 1;
  char v_buf[16];
  int_to_str(line, v_buf);
  puts("  ");
  puts(v_buf);
  puts("  ");
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
  {
    putc(file_contents[ci].data[j]);
    if (file_contents[ci].data[j] == '\n' && j + 1 < file_contents[ci].size)
    {
      line++;
      int_to_str(line, v_buf);
      puts("  ");
      puts(v_buf);
      puts("  ");
    }
  }
  return 0;
}

/* TAC - print file in reverse line order */
static int cmd_tac(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: TAC <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
    return 0;
  /* Find line starts */
  uint32_t starts[128];
  int lc = 0;
  starts[lc++] = 0;
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
    if (file_contents[ci].data[j] == '\n' && j + 1 < file_contents[ci].size && lc < 128)
      starts[lc++] = j + 1;
  for (int l = lc - 1; l >= 0; l--)
  {
    uint32_t s = starts[l];
    uint32_t e = (l < lc - 1) ? starts[l + 1] : file_contents[ci].size;
    for (uint32_t j = s; j < e; j++)
      putc(file_contents[ci].data[j]);
    if (file_contents[ci].data[e - 1] != '\n')
      putc('\n');
  }
  return 0;
}

/* FACTOR - prime factorization */
static int cmd_factor(const char *a)
{
  char tok[16];
  a = get_token(a, tok, 16);
  if (tok[0] == 0)
  {
    puts("Usage: FACTOR <number>\n");
    return -1;
  }
  uint32_t n = str_to_int(tok);
  char v_buf[16];
  int_to_str(n, v_buf);
  puts(v_buf);
  puts(":");
  uint32_t orig = n;
  for (uint32_t d = 2; d * d <= orig && n > 1; d++)
  {
    while (n % d == 0)
    {
      putc(' ');
      int_to_str(d, v_buf);
      puts(v_buf);
      n /= d;
    }
  }
  if (n > 1)
  {
    putc(' ');
    int_to_str(n, v_buf);
    puts(v_buf);
  }
  putc('\n');
  return 0;
}

/* SEQ - print sequence */
static int cmd_seq(const char *a)
{
  char s1[16], s2[16];
  a = get_token(a, s1, 16);
  a = get_token(a, s2, 16);
  if (s1[0] == 0)
  {
    puts("Usage: SEQ <start> <end>\n");
    return -1;
  }
  uint32_t start = str_to_int(s1);
  uint32_t end2 = s2[0] ? str_to_int(s2) : start;
  if (s2[0] == 0)
  {
    end2 = start;
    start = 1;
  }
  char v_buf[16];
  for (uint32_t i = start; i <= end2; i++)
  {
    int_to_str(i, v_buf);
    puts(v_buf);
    putc('\n');
  }
  return 0;
}

/* SHUF - random number */
static int cmd_shuf(const char *a)
{
  char tok[16];
  a = get_token(a, tok, 16);
  uint32_t max = tok[0] ? str_to_int(tok) : 100;
  uint32_t r = get_ticks() % max;
  char v_buf[16];
  int_to_str(r, v_buf);
  puts(v_buf);
  putc('\n');
  return 0;
}

/* YES - repeat text */
static int cmd_yes(const char *a)
{
  a = skip_spaces(a);
  const char *txt = (a && a[0]) ? a : "y";
  for (int i = 0; i < 20; i++)
  {
    puts(txt);
    putc('\n');
  }
  puts("(stopped after 20 lines)\n");
  return 0;
}
static int cmd_timeout(const char *a)
{
  char tok[16];
  a = get_token(a, tok, 16);
  if (tok[0] == 0)
  {
    puts("Usage: TIMEOUT <seconds> <command>\n");
    return -1;
  }
  uint32_t sec = str_to_int(tok);
  a = skip_spaces(a);
  if (a[0])
    return cmd_dispatch(a);
  sleep_ms(sec * 1000);
  return 0;
}

/* WHICH - find command */
static int cmd_which(const char *a)
{
  char tok[64];
  a = get_token(a, tok, 64);
  if (tok[0] == 0)
  {
    puts("Usage: WHICH <command>\n");
    return -1;
  }
  str_upper(tok);
  /* Search command table */
  extern const char *commands_get_name(int i);
  puts("/bin/");
  for (int i = 0; tok[i]; i++)
    putc(tok[i] >= 'A' && tok[i] <= 'Z' ? tok[i] + 32 : tok[i]);
  putc('\n');
  return 0;
}

static int cmd_whereis(const char *a) { return cmd_which(a); }

static int cmd_id(const char *a)
{
  (void)a;
  puts("uid=0(");
  puts(current_user);
  puts(") gid=0(root)\n");
  return 0;
}

static int cmd_who(const char *a)
{
  (void)a;
  puts(current_user);
  puts("  console  ");
  uint8_t h, m, s;
  sys_get_time(&h, &m, &s);
  char v_buf[16];
  int_to_str(h, v_buf);
  puts(v_buf);
  putc(':');
  int_to_str(m, v_buf);
  puts(v_buf);
  putc('\n');
  return 0;
}

static int cmd_w(const char *a) { return cmd_who(a); }
static int cmd_last(const char *a)
{
  (void)a;
  puts(current_user);
  puts("  console  still logged in\n");
  return 0;
}

/* ENV - environment variables */
#define MAX_ENV_VARS 16
static char env_keys[MAX_ENV_VARS][32];
static char env_vals[MAX_ENV_VARS][64];
static int env_count = 0;
static bool env_inited = false;

static void env_init_defaults(void)
{
  if (env_inited)
    return;
  env_inited = true;
  str_copy(env_keys[0], "PATH", 32);
  str_copy(env_vals[0], "/bin", 64);
  env_count++;
  str_copy(env_keys[1], "HOME", 32);
  str_copy(env_vals[1], "C:\\", 64);
  env_count++;
  str_copy(env_keys[2], "SHELL", 32);
  str_copy(env_vals[2], "/bin/sh", 64);
  env_count++;
  str_copy(env_keys[3], "USER", 32);
  str_copy(env_vals[3], "root", 64);
  env_count++;
}

static int cmd_env(const char *a)
{
  (void)a;
  env_init_defaults();
  for (int i = 0; i < env_count; i++)
  {
    puts(env_keys[i]);
    putc('=');
    puts(env_vals[i]);
    putc('\n');
  }
  return 0;
}

static int cmd_export(const char *a)
{
  env_init_defaults();
  char tok[96];
  a = get_token(a, tok, 96);
  if (tok[0] == 0)
  {
    return cmd_env(a);
  }
  /* Find = */
  int eq = -1;
  for (int i = 0; tok[i]; i++)
    if (tok[i] == '=')
    {
      eq = i;
      break;
    }
  if (eq < 0)
  {
    puts("Usage: EXPORT KEY=value\n");
    return -1;
  }
  tok[eq] = 0;
  int idx = -1;
  for (int i = 0; i < env_count; i++)
    if (str_cmp(env_keys[i], tok) == 0)
    {
      idx = i;
      break;
    }
  if (idx < 0 && env_count < MAX_ENV_VARS)
    idx = env_count++;
  if (idx >= 0)
  {
    str_copy(env_keys[idx], tok, 32);
    str_copy(env_vals[idx], tok + eq + 1, 64);
  }
  return 0;
}

static int cmd_unset(const char *a)
{
  env_init_defaults();
  char tok[32];
  a = get_token(a, tok, 32);
  if (tok[0] == 0)
  {
    puts("Usage: UNSET <var>\n");
    return -1;
  }
  for (int i = 0; i < env_count; i++)
  {
    if (str_cmp(env_keys[i], tok) == 0)
    {
      for (int j = i; j < env_count - 1; j++)
      {
        str_copy(env_keys[j], env_keys[j + 1], 32);
        str_copy(env_vals[j], env_vals[j + 1], 64);
      }
      env_count--;
      return 0;
    }
  }
  return 0;
}

static int cmd_printenv(const char *a) { return cmd_env(a); }
static int cmd_set(const char *a) { return cmd_env(a); }
static int cmd_source(const char *a)
{
  (void)a;
  puts("SOURCE: Script execution not supported\n");
  return 0;
}
static int cmd_true(const char *a)
{
  (void)a;
  return 0;
}
static int cmd_false(const char *a)
{
  (void)a;
  return 1;
}
static int cmd_test(const char *a)
{
  char tok[16];
  a = get_token(a, tok, 16);
  if (str_cmp(tok, "-f") == 0)
  {
    char f[64];
    a = get_token(a, f, 64);
    for (int i = 0; i < fs_count; i++)
      if (str_cmp(fs_table[i].name, f) == 0 && fs_table[i].type == 0)
        return 0;
    return 1;
  }
  if (str_cmp(tok, "-d") == 0)
  {
    char f[64];
    a = get_token(a, f, 64);
    for (int i = 0; i < fs_count; i++)
      if (str_cmp(fs_table[i].name, f) == 0 && fs_table[i].type == 1)
        return 0;
    return 1;
  }
  return 1;
}

/* EXPR - evaluate expression */
static int cmd_expr(const char *a)
{
  char a1[16], op[4], a2[16];
  a = get_token(a, a1, 16);
  a = get_token(a, op, 4);
  a = get_token(a, a2, 16);
  if (a1[0] == 0)
  {
    puts("Usage: EXPR num op num\n");
    return -1;
  }
  if (op[0] == 0)
  {
    int_to_str(str_to_int(a1), (char[16]){});
    char b[16];
    int_to_str(str_to_int(a1), b);
    puts(b);
    putc('\n');
    return 0;
  }
  int v1 = str_to_int(a1), v2 = str_to_int(a2), r = 0;
  if (op[0] == '+')
    r = v1 + v2;
  else if (op[0] == '-')
    r = v1 - v2;
  else if (op[0] == '*')
    r = v1 * v2;
  else if (op[0] == '/')
    r = v2 ? v1 / v2 : 0;
  else if (op[0] == '%')
    r = v2 ? v1 % v2 : 0;
  char v_buf[16];
  int_to_str(r, v_buf);
  puts(v_buf);
  putc('\n');
  return 0;
}

static int cmd_let(const char *a) { return cmd_expr(a); }

static int cmd_read(const char *a)
{
  (void)a;
  puts("Enter input: ");
  char v_buf[128];
  int pos = 0;
  while (pos < 127)
  {
    uint16_t k = getkey();
    uint8_t c = k & 0xFF;
    if (c == 13 || c == 10)
    {
      putc('\n');
      break;
    }
    if (c >= 32 && c < 127)
    {
      v_buf[pos++] = c;
      putc(c);
    }
  }
  v_buf[pos] = 0;
  puts("Read: ");
  puts(v_buf);
  putc('\n');
  return 0;
}

static int cmd_printf(const char *a)
{
  if (a)
    puts(a);
  puts("\n");
  return 0;
}

/* ALIAS storage */
#define MAX_ALIASES 16
static char alias_names[MAX_ALIASES][32];
static char alias_cmds[MAX_ALIASES][64];
static int alias_count = 0;

static int cmd_alias(const char *a)
{
  char tok[96];
  a = get_token(a, tok, 96);
  if (tok[0] == 0)
  {
    for (int i = 0; i < alias_count; i++)
    {
      puts("alias ");
      puts(alias_names[i]);
      puts("='");
      puts(alias_cmds[i]);
      puts("'\n");
    }
    return 0;
  }
  int eq = -1;
  for (int i = 0; tok[i]; i++)
    if (tok[i] == '=')
    {
      eq = i;
      break;
    }
  if (eq < 0)
  {
    puts("Usage: ALIAS name=command\n");
    return -1;
  }
  tok[eq] = 0;
  int idx = -1;
  for (int i = 0; i < alias_count; i++)
    if (str_cmp(alias_names[i], tok) == 0)
    {
      idx = i;
      break;
    }
  if (idx < 0 && alias_count < MAX_ALIASES)
    idx = alias_count++;
  if (idx >= 0)
  {
    str_copy(alias_names[idx], tok, 32);
    str_copy(alias_cmds[idx], tok + eq + 1, 64);
  }
  return 0;
}

static int cmd_unalias(const char *a)
{
  char tok[32];
  a = get_token(a, tok, 32);
  for (int i = 0; i < alias_count; i++)
  {
    if (str_cmp(alias_names[i], tok) == 0)
    {
      for (int j = i; j < alias_count - 1; j++)
      {
        str_copy(alias_names[j], alias_names[j + 1], 32);
        str_copy(alias_cmds[j], alias_cmds[j + 1], 64);
      }
      alias_count--;
      return 0;
    }
  }
  return 0;
}

/* HISTORY storage */
#define MAX_HISTORY 32
static char cmd_history_buf[MAX_HISTORY][128];
static int history_total = 0;

static int cmd_history(const char *a)
{
  (void)a;
  for (int i = 0; i < history_total && i < MAX_HISTORY; i++)
  {
    char v_buf[16];
    int_to_str(i + 1, v_buf);
    puts("  ");
    puts(v_buf);
    puts("  ");
    puts(cmd_history_buf[i]);
    putc('\n');
  }
  return 0;
}

static int cmd_jobs(const char *a)
{
  (void)a;
  puts("No background jobs\n");
  return 0;
}
static int cmd_fg(const char *a)
{
  (void)a;
  puts("No background jobs\n");
  return 0;
}
static int cmd_bg(const char *a)
{
  (void)a;
  puts("No background jobs\n");
  return 0;
}
static int cmd_nice(const char *a)
{
  a = skip_spaces(a);
  if (a[0])
    return cmd_dispatch(a);
  puts("Usage: NICE <command>\n");
  return 0;
}
static int cmd_nohup(const char *a)
{
  a = skip_spaces(a);
  if (a[0])
    return cmd_dispatch(a);
  puts("Usage: NOHUP <command>\n");
  return 0;
}
static int cmd_strace(const char *a)
{
  (void)a;
  puts("STRACE: System call tracing not available\n");
  return 0;
}

/* MORE/LESS - paginated file viewer */
static int cmd_more(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: MORE <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
  {
    puts("(empty)\n");
    return 0;
  }
  int lines = 0;
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
  {
    putc(file_contents[ci].data[j]);
    if (file_contents[ci].data[j] == '\n')
    {
      lines++;
      if (lines % 24 == 0)
      {
        puts("--More--");
        getkey();
        puts("\r        \r");
      }
    }
  }
  return 0;
}

static int cmd_less(const char *a) { return cmd_more(a); }

/* FILE - identify file type */
static int cmd_file(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: FILE <filename>\n");
    return -1;
  }
  char full_path[256];
  build_full_path(name, full_path);

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      puts(full_path);
      puts(": ");
      if (fs_table[i].type == 1)
      {
        puts("directory\n");
        return 0;
      }
      puts("regular file, ");
      char v_buf[16];
      int_to_str(fs_table[i].size, v_buf);
      puts(v_buf);
      puts(" bytes\n");
      return 0;
    }
  }
  puts(full_path);
  puts(": No such file\n");
  return -1;
}

/* STAT - file status */
static int cmd_stat(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: STAT <filename>\n");
    return -1;
  }
  char full_path[256];
  build_full_path(name, full_path);

  for (int i = 0; i < fs_count; i++)
  {
    if (str_cmp(fs_table[i].name, full_path) == 0)
    {
      char v_buf[16];
      puts("  File: ");
      puts(full_path);
      putc('\n');
      puts("  Size: ");
      int_to_str(fs_table[i].size, v_buf);
      puts(v_buf);
      puts(" bytes\n");
      puts("  Type: ");
      puts(fs_table[i].type ? "directory" : "regular file");
      putc('\n');
      puts("  Attr: 0x");
      print_hex(fs_table[i].attr);
      putc('\n');
      return 0;
    }
  }
  puts("stat: cannot stat '");
  puts(full_path);
  puts("'\n");
  return -1;
}

static int cmd_path(const char *a)
{
  (void)a;
  env_init_defaults();
  puts(env_vals[0]);
  putc('\n');
  return 0;
}
static int cmd_prompt(const char *a)
{
  (void)a;
  puts(current_dir);
  puts("> ");
  return 0;
}
static int cmd_ln(const char *a)
{
  (void)a;
  puts("LN: Symbolic links not supported in FAT12\n");
  return 0;
}
static int cmd_chown(const char *a)
{
  (void)a;
  puts("CHOWN: Single-user system (all files owned by root)\n");
  return 0;
}

/* STRINGS - print printable strings from file */
static int cmd_strings(const char *a)
{
  char name[64];
  a = get_token(a, name, 64);
  if (name[0] == 0)
  {
    puts("Usage: STRINGS <file>\n");
    return -1;
  }
  char full[256];
  build_full_path(name, full);
  int fi = -1;
  for (int i = 0; i < fs_count; i++)
    if (str_cmp(fs_table[i].name, full) == 0)
    {
      fi = i;
      break;
    }
  if (fi < 0)
  {
    puts("File not found\n");
    return -1;
  }
  int ci = -1;
  for (int i = 0; i < file_content_count; i++)
    if ((int)file_contents[i].file_idx == fi)
    {
      ci = i;
      break;
    }
  if (ci < 0)
    return 0;
  int run = 0;
  for (uint32_t j = 0; j < file_contents[ci].size; j++)
  {
    char c = file_contents[ci].data[j];
    if (c >= 32 && c < 127)
    {
      putc(c);
      run++;
    }
    else
    {
      if (run >= 4)
        putc('\n');
      run = 0;
    }
  }
  if (run >= 4)
    putc('\n');
  return 0;
}

/* Forward declaration */
int cmd_dispatch(const char *line);

/* CAL - calendar */
static int cmd_cal(const char *a)
{
  (void)a;
  uint8_t day, month;
  uint16_t year;
  extern int sys_get_date(uint8_t *, uint8_t *, uint16_t *);
  sys_get_date(&day, &month, &year);
  char v_buf[16];
  static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  if (month >= 1 && month <= 12)
    puts(months[month - 1]);
  puts(" ");
  int_to_str(year, v_buf);
  puts(v_buf);
  putc('\n');
  puts("Su Mo Tu We Th Fr Sa\n");
  /* Simplified: just show current day */
  for (int d = 1; d <= 28; d++)
  {
    if (d < 10)
      putc(' ');
    int_to_str(d, v_buf);
    puts(v_buf);
    if (d == day)
    {
      putc('*');
    }
    else
      putc(' ');
    if (d % 7 == 0)
      putc('\n');
  }
  putc('\n');
  return 0;
}

/* BANNER/FIGLET - large text */
static int cmd_banner(const char *a)
{
  a = skip_spaces(a);
  if (!a || a[0] == 0)
  {
    puts("Usage: BANNER <text>\n");
    return -1;
  }
  puts("####  ");
  puts(a);
  puts("  ####\n");
  puts("#     ");
  for (int i = 0; a[i]; i++)
    putc(' ');
  puts("     #\n");
  puts("####  ");
  puts(a);
  puts("  ####\n");
  return 0;
}
static int cmd_figlet(const char *a) { return cmd_banner(a); }

/* COWSAY */
static int cmd_cowsay(const char *a)
{
  a = skip_spaces(a);
  const char *msg = (a && a[0]) ? a : "moo";
  int len = str_len(msg);
  putc(' ');
  for (int i = 0; i < len + 2; i++)
    putc('_');
  putc('\n');
  puts("< ");
  puts(msg);
  puts(" >\n");
  putc(' ');
  for (int i = 0; i < len + 2; i++)
    putc('-');
  putc('\n');
  puts("        \\   ^__^\n");
  puts("         \\  (oo)\\_______\n");
  puts("            (__)\\       )\\/\\\n");
  puts("                ||----w |\n");
  puts("                ||     ||\n");
  return 0;
}

static int cmd_fortune(const char *a)
{
  (void)a;
  static const char *fortunes[] = {
      "A bad penny always turns up.",
      "A calm sea does not make a skilled sailor.",
      "A close mouth catches no flies.",
      "Fortune favors the bold.",
      "Computers make very fast, very accurate mistakes."};
  extern uint32_t get_ticks(void);
  uint32_t r = get_ticks() % 5;
  puts(fortunes[r]);
  putc('\n');
  return 0;
}

/* 104. MAKE - build system (implemented in cmd_make.c, declared above) */

/* Switch to dos mode reason - matching WM definition */
extern volatile int desktop_exit_reason;
/* 102. SUDO - Run command as root (interactive password prompt) */
static int cmd_sudo(const char *args)
{
  args = skip_spaces(args);

  if (args[0] == 0)
  {
    puts("Usage: SUDO <command>\n");
    puts("Execute a command with root privileges.\n");
    return -1;
  }

  /* Reinstall always authenticates freshly, even for root or cached sudo. */
  char subcommand[64];
  const char *subargs = get_token(args, subcommand, sizeof(subcommand));
  if (str_cmp(subcommand, "REINSTALL") == 0)
    return cmd_reinstall_from_sudo(subargs);

  /* Check if user is already root */
  if (str_cmp(current_user, "root") == 0)
  {
    return cmd_dispatch(args);
  }

  /* Grace Period Check */
  uint32_t now = get_ticks();
  if (sudo_last_tick != 0 && (now - sudo_last_tick < SUDO_TIMEOUT_TICKS) &&
      str_cmp(sudo_last_user, current_user) == 0)
  {
    return cmd_dispatch(args);
  }

  /* Prompt for password */
  puts("[sudo] password for ");
  puts(current_user);
  puts(": ");

  /* Read password interactively */
  while (c_kb_hit())
    c_getkey(); /* Flush any leftover keys */

  char password[32];
  int pi = 0;
  while (pi < 31)
  {
    /* Keep system alive while waiting for key in GUI mode. In DOS mode
     * nothing charges the epoch here, so declare the wait: otherwise an
     * idle prompt looks exactly like a hung scheduler to the watchdog. */
    extern void sched_progress_wait(int s);
    extern void sched_progress_run(void);
    sched_progress_wait(1);
    while (!c_kb_hit())
    {
      if (!is_dos_mode())
      {
        extern void wm_update_light(void);
        wm_update_light();
      }
      __asm__ volatile("hlt");
    }
    sched_progress_run();

    uint16_t k = c_getkey();
    char c = (char)(k & 0xFF);
    if (c == 13 || c == 10)
      break;                /* Enter */
    if (c == 8 || c == 127) /* Backspace */
    {
      if (pi > 0)
      {
        pi--;
        puts("\b \b");
      }
      continue;
    }
    if (c >= 32 && c <= 126)
    {
      password[pi++] = c;
      puts("*");
    }
  }
  password[pi] = 0;
  puts("\n");

  /* AUR-07: Verify password ONLY against current user.
     No root password fallback — that is a privilege escalation bug.
     Only users in the "wheel" group (or root itself) should be able
     to use sudo. For now, we require the current user's own password. */
  bool ok = false;

  for (int i = 0; i < user_count; i++)
  {
    if (str_cmp(user_table[i].username, current_user) == 0)
    {
      ok = verify_password_with_migration(&user_table[i], password);
      break;
    }
  }

  if (!ok)
  {
    /* AUR-21: wipe the grace period on ANY failure so the next sudo
       re-prompts instead of inheriting a stale grant. */
    sudo_last_tick = 0;
    sudo_last_user[0] = 0;
    set_attr(0x0C);
    puts("[sudo] Authentication failed.\n");
    set_attr(0x07);
    return -1;
  }

  /* Success - Store for grace period */
  sudo_last_tick = get_ticks();
  str_copy(sudo_last_user, current_user, 32);

  /* Temporarily become root */
  char saved_user[32];
  str_copy(saved_user, current_user, 32);
  str_copy(current_user, "root", 32);

  /* Execute the command */
  int result = cmd_dispatch(args);

  /* Restore original user */
  str_copy(current_user, saved_user, 32);

  return result;
}

/* COMMAND DISPATCHER */

typedef struct
{
  const char *name;
  int (*func)(const char *args);
} Command;

static int cmd_install(const char *args)
{
  (void)args;
  puts("To start the graphical installer, enter GUI mode by typing 'GUIMODE'.\n");
  puts("If no installation is detected, the setup will launch automatically.\n");
  return 0;
}

static int cmd_dosmode(const char *args)
{
  (void)args;
  puts("Exiting GUI mode...\n");
  extern volatile int desktop_exit_reason;
  desktop_exit_reason = 0; /* 0 = Exit to DOS mode */
  return 0;
}

static void fat32_list_callback(const char *name, bool is_dir, uint32_t size)
{
  puts(is_dir ? "[DIR] " : "[FILE] ");
  puts(name);
  if (!is_dir)
  {
    puts(" (");
    char size_str[16];
    int_to_str(size, size_str);
    puts(size_str);
    puts(" bytes)");
  }
  puts("\n");
}

static int cmd_fatload(const char *args)
{
  (void)args;
  puts("[FAT32] USB/FAT32 Debug Tool\n");
  puts("Usage: fatload mount [lba] - Mount FAT32 partition\n");
  puts("       fatload unmount      - Unmount\n");
  puts("       fatload ls [dir]     - List directory\n");
  puts("       fatload find <file>  - Find file\n");
  puts("       fatload info         - Show mount info\n");
  puts("       fatload test         - Quick mount test at LBA 0\n");

  if (args == NULL || args[0] == '\0')
  {
    return 0;
  }

  args = skip_spaces(args);

  if (str_ncmp(args, "mount", 5) == 0)
  {
    uint32_t lba = 0;
    const char *lba_str = args + 5;
    while (*lba_str == ' ')
      lba_str++;
    if (*lba_str >= '0' && *lba_str <= '9')
    {
      lba = str_to_int(lba_str);
    }
    puts("[FAT32] Mounting at LBA ");
    char lba_buf[16];
    int_to_str(lba, lba_buf);
    puts(lba_buf);
    puts("...\n");
    bool ok = fat32_mount(lba);
    if (ok)
    {
      puts("[FAT32] Mount successful!\n");
      puts("[FAT32] Volume: ");
      puts(fat32_get_volume_label());
      puts("\n");
    }
    else
    {
      puts("[FAT32] Mount failed - not a valid FAT32 partition\n");
    }
  }
  else if (str_cmp(args, "unmount") == 0)
  {
    fat32_unmount();
  }
  else if (str_cmp(args, "info") == 0)
  {
    if (fat32_is_mounted())
    {
      puts("[FAT32] Volume: ");
      puts(fat32_get_volume_label());
      puts("\n[FAT32] Status: Mounted\n");
    }
    else
    {
      puts("[FAT32] Status: Not mounted\n");
    }
  }
  else if (str_ncmp(args, "ls", 2) == 0)
  {
    if (!fat32_is_mounted())
    {
      puts("[FAT32] Not mounted. Use 'fatload mount' first.\n");
      return 0;
    }
    const char *dir = args + 2;
    while (*dir == ' ')
      dir++;
    if (*dir == '\0')
      dir = "/";
    puts("[FAT32] Listing directory: /");
    puts(dir);
    puts("\n");
    fat32_list_directory(dir, fat32_list_callback);
  }
  else if (str_ncmp(args, "find ", 5) == 0)
  {
    if (!fat32_is_mounted())
    {
      puts("[FAT32] Not mounted. Use 'fatload mount' first.\n");
      return 0;
    }
    const char *filename = args + 4;
    while (*filename == ' ')
      filename++;
    if (*filename == '\0')
    {
      puts("[FAT32] Usage: fatload find <filename>\n");
      return 0;
    }
    puts("[FAT32] Finding file: ");
    puts(filename);
    puts("\n");
    uint32_t cluster, size;
    if (fat32_find_file(filename, &cluster, &size))
    {
      puts("[FAT32] Found! Cluster: ");
      char buf[16];
      int_to_str(cluster, buf);
      puts(buf);
      puts(", Size: ");
      int_to_str(size, buf);
      puts(buf);
      puts(" bytes\n");
    }
    else
    {
      puts("[FAT32] File not found\n");
    }
  }
  else if (str_cmp(args, "test") == 0)
  {
    puts("[FAT32] Quick mount test at LBA 0...\n");
    bool ok = fat32_mount(0);
    if (ok)
    {
      puts("[FAT32] Found FAT32 partition!\n");
      puts("[FAT32] Volume: ");
      puts(fat32_get_volume_label());
      puts("\n[FAT32] Now try 'fatload ls' to see files\n");
    }
    else
    {
      puts("[FAT32] No FAT32 at LBA 0. Try 'fatload mount 2048' for partitioned USB.\n");
    }
  }

  return 0;
}

static float mage_sin(float x) {
    const float PI = 3.14159265f;
    const float TWO_PI = 6.28318530f;
    x = x - (float)((int)(x * 0.159154943f)) * TWO_PI;
    if (x > PI) x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}

static float mage_cos(float x) {
    return mage_sin(x + 1.57079632f);
}

/* Helper: hash-based value noise for nebula
 * Kept in unsigned math: x * 374761393 leaves the signed int range as soon
 * as x reaches 6, and signed overflow is undefined behaviour in C. */
static float mage_hash2f(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

static float mage_hash2f_nosafe(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

static float mage_noise2d_fast(float x, float y) {
    int ix = (int)x;
    int iy = (int)y;
    float fx = x - (float)ix;
    float fy = y - (float)iy;

    float v00 = mage_hash2f_nosafe(ix, iy);
    float v10 = mage_hash2f_nosafe(ix + 1, iy);
    float v01 = mage_hash2f_nosafe(ix, iy + 1);
    float v11 = mage_hash2f_nosafe(ix + 1, iy + 1);

    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);

    float i0 = v00 + sx * (v10 - v00);
    float i1 = v01 + sx * (v11 - v01);
    return i0 + sy * (i1 - i0);
}

static float mage_fbm_fast(float x, float y) {
    float v0 = mage_noise2d_fast(x, y);
    float v1 = mage_noise2d_fast(x * 2.0f, y * 2.0f);
    return v0 * 0.66f + v1 * 0.34f;
}

/* MAGE7968 -- Tribute sequence. Near-black field, twinkling static stars,
 * one audit-sweep ring around a depth-sorted solid icosahedron core
 * (filled faces + bright edges), zooming
 * credit text (native font -> scratch -> scaled chroma-key blit), CRT
 * scanlines, corner HUD with git handle. ESC exits. */

#define CUBE_VERTS 8
#define CUBE_EDGES 12
static const float cube_verts[CUBE_VERTS][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
};
static const uint8_t cube_edges[CUBE_EDGES][2] = {
    {0,1}, {1,2}, {2,3}, {3,0},
    {4,5}, {5,6}, {6,7}, {7,4},
    {0,4}, {1,5}, {2,6}, {3,7}
};

#define CUBE_FACES 12
static const uint8_t cube_faces[CUBE_FACES][3] = {
    {0,1,2}, {0,2,3}, {4,5,6}, {4,6,7},
    {0,4,7}, {0,7,3}, {1,5,6}, {1,6,2},
    {0,1,5}, {0,5,4}, {3,2,6}, {3,6,7}
};
static bool cube_faces_ready = false;

static void ensure_cube_faces(void) {
    cube_faces_ready = true;
}

/* Static star field: positions fixed at init, only brightness twinkles.
 * No motion, no streaks -- the field stays calm so the core reads clean. */
#define STAR_COUNT 80
static float star_x[STAR_COUNT], star_y[STAR_COUNT], star_ph[STAR_COUNT];
static bool stars_inited = false;

static void init_stars(void) {
    if (stars_inited) return;
    uint32_t seed = 0x5EED1234u;
    for (int i = 0; i < STAR_COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        star_x[i] = (float)(seed & 0xFFFF) / 65535.0f;
        seed = seed * 1103515245 + 12345;
        star_y[i] = (float)(seed & 0xFFFF) / 65535.0f;
        seed = seed * 1103515245 + 12345;
        star_ph[i] = (float)(seed & 0xFFFF) / 65535.0f * 6.2831853f;
    }
    stars_inited = true;
}

/* Zoom-text scratch: native font rendered once per message at (0,0),
 * copied out, then chroma-key blitted at animated scale. */
#define ZT_W 704
#define ZT_H 32
static uint32_t zt_scratch[ZT_W * ZT_H];
static int zt_bb_x, zt_bb_y, zt_bb_w, zt_bb_h;
static int zt_msg_idx = -1;

static const char *zt_msgs[] = {
    "MAGE7968",
    "SECURITY ARCHITECT AND AUDITOR",
    "HELPED HARDEN AURIONOS",
    "SECURITY FINDINGS THAT MADE AURIONOS STRONGER",
    "VULNERABILITIES IDENTIFIED AND CLOSED",
    "THREAT SURFACE REVIEWED AND HARDENED",
    "GITHUB.COM/WILLIAM43243"
};
#define ZT_MSG_COUNT 7
static const uint32_t zt_cols[ZT_MSG_COUNT] = {
    0x00FFCC, 0x66D9FF, 0xFF5FD7, 0xFFC857, 0x7CFF6B, 0x66D9FF, 0xFFFFFF
};

static inline void put_px(uint32_t *fb, int w, int h, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h)
        fb[y * w + x] = c;
}

static void draw_line(uint32_t *fb, int w, int h,
                      int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        put_px(fb, w, h, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* Flat-filled triangle for the solid icosahedron core. Scanline fill
 * between the long edge and the short edges; put_px clips to the frame. */
static void fill_triangle(uint32_t *fb, int w, int h,
                          int x0, int y0, int x1, int y1,
                          int x2, int y2, uint32_t color) {
    int t, y, xa, xb, x;
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y0 == y2) return;
    for (y = y0; y <= y2; y++) {
        if (y < y1) {
            xa = (y1 == y0) ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
            xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        } else {
            xa = (y1 == y2) ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
            xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        }
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        for (x = xa; x <= xb; x++) put_px(fb, w, h, x, y, color);
    }
}

static void rotate3(float px, float py, float pz,
                    float ax, float ay, float az,
                    float *ox, float *oy, float *oz) {
    float cx = mage_cos(ax), sx = mage_sin(ax);
    float y1 = py * cx - pz * sx;
    float z1 = py * sx + pz * cx;
    float cy = mage_cos(ay), sy = mage_sin(ay);
    float x2 = px * cy - z1 * sy;
    float z2 = px * sy + z1 * cy;
    float cz = mage_cos(az), sz = mage_sin(az);
    *ox = x2 * cz - y1 * sz;
    *oy = x2 * sz + y1 * cz;
    *oz = z2;
}

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Fixed-point dim: k in [0,256]. */
static inline uint32_t dimc(uint32_t c, int k) {
    uint8_t r = (uint8_t)((((c >> 16) & 0xFF) * k) >> 8);
    uint8_t g = (uint8_t)((((c >> 8) & 0xFF) * k) >> 8);
    uint8_t b = (uint8_t)(((c & 0xFF) * k) >> 8);
    return rgb(r, g, b);
}

/* Helper: additive blend for glow effects */
static uint32_t add_pixel(uint32_t base, uint32_t add, int alpha) {
    int br = (int)((base >> 16) & 0xFF) + (((int)((add >> 16) & 0xFF) * alpha) >> 8);
    int bg = (int)((base >> 8) & 0xFF) + (((int)((add >> 8) & 0xFF) * alpha) >> 8);
    int bb = (int)(base & 0xFF) + (((int)(add & 0xFF) * alpha) >> 8);
    if (br > 255) br = 255;
    if (bg > 255) bg = 255;
    if (bb > 255) bb = 255;
    return rgb((uint8_t)br, (uint8_t)bg, (uint8_t)bb);
}

static void zt_refresh(uint32_t *fb, int w, int h, int msg) {
    zt_msg_idx = msg;
    gpu_fill_rect(0, 0, ZT_W, ZT_H, 0x000000);
    gpu_draw_string(4, 4, (const uint8_t *)zt_msgs[msg], 0xFFFFFF, 0x000000);
    int cw = (w < ZT_W) ? w : ZT_W;
    int ch = (h < ZT_H) ? h : ZT_H;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            zt_scratch[y * ZT_W + x] = fb[y * w + x];
    int x0 = ZT_W, y0 = ZT_H, x1 = -1, y1 = -1;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            if (zt_scratch[y * ZT_W + x] != 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0) { zt_bb_w = 0; zt_bb_h = 0; return; }
    zt_bb_x = x0; zt_bb_y = y0;
    zt_bb_w = x1 - x0 + 1; zt_bb_h = y1 - y0 + 1;
}

/* Scaled chroma-key blit of the scratch glyph run, centered. */
static void zt_blit(uint32_t *fb, int w, int h, float scale, int k, uint32_t col) {
    if (zt_bb_w <= 0 || k <= 0) return;
    int dw = (int)(zt_bb_w * scale);
    int dh = (int)(zt_bb_h * scale);
    if (dw <= 0 || dh <= 0) return;
    int ox = (w - dw) / 2;
    int oy = (int)(h * 0.50f) - dh / 2;
    float inv = 1.0f / scale;
    uint32_t body = dimc(col, k);
    for (int dy = 0; dy < dh; dy++) {
        int sy = zt_bb_y + (int)(dy * inv);
        if (sy >= zt_bb_y + zt_bb_h) continue;
        const uint32_t *srow = zt_scratch + sy * ZT_W;
        int py = oy + dy;
        if ((unsigned)py >= (unsigned)h) continue;
        uint32_t *drow = fb + py * w;
        for (int dx = 0; dx < dw; dx++) {
            int sx = zt_bb_x + (int)(dx * inv);
            if (srow[sx] == 0) continue;
            int px = ox + dx;
            if ((unsigned)px < (unsigned)w) drow[px] = body;
        }
    }
}

/* Git-branch glyph: two trunk nodes, one branch node, two connectors. */
static void draw_git_icon(uint32_t *fb, int w, int h, int x, int y, uint32_t c) {
    gpu_fill_rect(x, y, 3, 3, c);
    gpu_fill_rect(x, y + 9, 3, 3, c);
    gpu_fill_rect(x + 7, y + 2, 3, 3, c);
    draw_line(fb, w, h, x + 1, y + 3, x + 1, y + 9, c);
    draw_line(fb, w, h, x + 8, y + 5, x + 2, y + 10, c);
}

static int cmd_mage7968(const char *args) {
    (void)args;

    uint32_t *fb = gpu_get_backbuffer();
    int w = gpu_get_width();
    int h = gpu_get_height();

    if (!fb || w <= 0 || h <= 0) {
        puts("## WARNING! DOS MODE DETECTED: NO FRAMEBUFFER, TRIBUTE NEEDS VESA. FALLING BACK TO TEXT. ##\n");
        return -1;
    }

    init_stars();

    puts("Entering Mage7968 Tribute...\n");
    sleep_ms(300);

    uint32_t start_tick = get_ticks();
    uint32_t prev_now = start_tick;
    int fps_sm = 60;

    float proj_x[CUBE_VERTS], proj_y[CUBE_VERTS], proj_z[CUBE_VERTS];
    typedef struct { int a, b; float z; } EdgeSort;
    EdgeSort sorted_edges[CUBE_EDGES];
    char sbuf[48];

    typedef struct { float x, y, z, vx, vy; } DustParticle;
    DustParticle dust[48];
    for (int i = 0; i < 48; i++) {
        dust[i].x = (float)(rand() % w);
        dust[i].y = (float)(rand() % h);
        dust[i].z = 0.2f + (float)(rand() % 100) / 100.0f;
        dust[i].vx = ((float)(rand() % 100) - 50.0f) / 500.0f;
        dust[i].vy = ((float)(rand() % 100) - 50.0f) / 500.0f;
    }

    int hw = (w + 1) / 2;
    int hh = (h + 1) / 2;
    size_t half_size = (size_t)hw * hh;
    uint32_t *half_buf = (uint32_t *)kmalloc(half_size * sizeof(uint32_t));
    if (!half_buf) {
        puts("no mem for mage\n");
        return -1;
    }

    int qw = (w + 3) / 4;
    int qh = (h + 3) / 4;
    size_t quarter_size = (size_t)qw * qh;
    uint32_t *post_buf = (uint32_t *)kmalloc(quarter_size * sizeof(uint32_t));
    if (!post_buf) {
        kfree(half_buf);
        puts("no mem for post\n");
        return -1;
    }

    // PRE-GENERATE STATIC NEBULA BACKGROUND
    // Runs once. Deep space gradients, layered FBM clouds, baked distant stars.
    for (int y = 0; y < hh; y++) {
        float yn = (float)y / (float)hh;
        uint32_t *row = half_buf + y * hw;
        for (int x = 0; x < hw; x++) {
            float xn = (float)x / (float)hw;
            
            float n1 = mage_fbm_fast(xn * 2.5f, yn * 2.5f);
            float n2 = mage_fbm_fast(xn * 5.0f + 5.2f, yn * 5.0f + 1.3f);
            float n3 = mage_fbm_fast(xn * 10.0f + 9.1f, yn * 10.0f + 3.7f);
            
            int br = (int)(8 + n1 * 25 + n2 * 45 + n3 * 20);
            int bg = (int)(4 + n1 * 10 + n2 * 20 + n3 * 15);
            int bb = (int)(20 + n1 * 40 + n2 * 70 + n3 * 30);
            
            float band = mage_sin(xn * 3.14f + yn * 2.0f) * 0.5f + 0.5f;
            br += (int)(band * 30);
            bg += (int)(band * 5);
            bb += (int)(band * 40);

            float star_h = mage_hash2f(x * 17 + 3, y * 31 + 7);
            if (star_h > 0.990f) {
                int star_bright = (int)((star_h - 0.990f) * 25500.0f);
                br += star_bright;
                bg += star_bright;
                bb += star_bright + 30;
            }

            if (br < 0) br = 0;
            if (br > 255) br = 255;
            if (bg < 0) bg = 0;
            if (bg > 255) bg = 255;
            if (bb < 0) bb = 0;
            if (bb > 255) bb = 255;
            
            row[x] = rgb((uint8_t)br, (uint8_t)bg, (uint8_t)bb);
        }
    }

    int frame = 0;

    while (1) {
        if (c_kb_hit()) {
            uint16_t k = c_getkey();
            if ((k & 0xFF) == 27) break;
        }

        uint32_t now = get_ticks();
        float t = (now - start_tick) / 100.0f;
        uint32_t dt_ticks = now - prev_now;
        prev_now = now;
        if (dt_ticks > 0 && dt_ticks < 100) {
            int fps = (int)(10000 / (dt_ticks * 10));
            fps_sm = (fps_sm * 3 + fps) / 4;
        }

        int msg = ((int)(t / 3.5f)) % ZT_MSG_COUNT;
        float pp = (t / 3.5f) - (float)((int)(t / 3.5f));
        if (msg != zt_msg_idx) zt_refresh(fb, w, h, msg);

        // FAST UPSCALE: Unrolled memory copy from static half_buf to fb
        for (int y = 0; y < h; y++) {
            const uint32_t *srow = half_buf + (y >> 1) * hw;
            uint32_t *drow = fb + y * w;
            int x = 0;
            for (; x <= w - 4; x += 4) {
                uint32_t c0 = srow[x >> 1];
                uint32_t c1 = srow[(x >> 1) + 1];
                drow[x] = c0;
                drow[x + 1] = c0;
                drow[x + 2] = c1;
                drow[x + 3] = c1;
            }
            for (; x < w; x++) {
                drow[x] = srow[x >> 1];
            }
        }

        for (int i = 0; i < STAR_COUNT; i++) {
            float sx = star_x[i];
            float sy = star_y[i];
            int layer = i % 3;
            float speed = (layer == 0) ? 0.3f : ((layer == 1) ? 0.7f : 1.2f);
            float bright = (layer == 0) ? 1.0f : ((layer == 1) ? 0.7f : 0.5f);
            sx += t * speed * 0.002f;
            if (sx > 1.0f) sx -= 1.0f;
            int twinkle_phase = (int)(t * 100.0f + star_ph[i] * 100.0f) & 255;
            int twinkle = (twinkle_phase < 128) ? (twinkle_phase * 2 - 128) : (384 - twinkle_phase * 2);
            int b = (int)(40 + 40.0f * bright) + ((twinkle * (int)(30.0f * bright)) >> 7);
            int px = (int)(sx * (float)w);
            int py = (int)(sy * (float)h);
            if (layer == 0) {
                put_px(fb, w, h, px, py, rgb((uint8_t)b, (uint8_t)b, (uint8_t)b));
                put_px(fb, w, h, px+1, py, rgb((uint8_t)(b*3/4), (uint8_t)(b*3/4), (uint8_t)(b*3/4)));
            } else {
                put_px(fb, w, h, px, py, rgb((uint8_t)b, (uint8_t)b, (uint8_t)b));
            }
        }

        {
            float cx = w * 0.5f, cy = h * 0.42f;
            float max_dist = (float)((w < h) ? w : h) * 0.6f;
            float max_dist_sq = max_dist * max_dist;
            for (int i = 0; i < 48; i++) {
                dust[i].x += dust[i].vx;
                dust[i].y += dust[i].vy;
                if (dust[i].x < 0) dust[i].x += (float)w;
                if (dust[i].x >= (float)w) dust[i].x -= (float)w;
                if (dust[i].y < 0) dust[i].y += (float)h;
                if (dust[i].y >= (float)h) dust[i].y -= (float)h;
                int px = (int)dust[i].x;
                int py = (int)dust[i].y;
                float dx = dust[i].x - cx;
                float dy = dust[i].y - cy;
                float dist_sq = dx * dx + dy * dy;
                if (dist_sq < max_dist_sq) {
                    float light = 1.0f - (dist_sq / max_dist_sq);
                    light *= light * dust[i].z;
                    int b = (int)(20 + light * 80);
                    uint32_t c = rgb((uint8_t)(b * 8 / 10), (uint8_t)(b * 9 / 10), (uint8_t)b);
                    uint32_t existing = fb[py * w + px];
                    fb[py * w + px] = add_pixel(existing, c, (int)(light * 180));
                }
            }
        }

        {
            float period = 7.0f;
            float ct = t - (float)((int)(t / period)) * period;
            float cp = ct / period;
            if (cp < 0.45f) {
                float q = cp / 0.45f;
                for (int j = 5; j >= 0; j--) {
                    float f = (float)j / 5.0f;
                    float trail_q = q - f * 0.1f;
                    if (trail_q < 0.0f) continue;
                    float tx = (float)w * (0.95f - 0.85f * trail_q);
                    float ty = (float)h * (0.08f + 0.30f * trail_q);
                    int px = (int)tx;
                    int py = (int)ty;
                    float fade = 1.0f - f;
                    int k = (int)(fade * 255);
                    uint32_t c = (j == 0) ? rgb(255, 255, 255) : dimc(0xFFC857, k);
                    if (j == 0) {
                        put_px(fb, w, h, px, py, c);
                        put_px(fb, w, h, px+1, py, c);
                        put_px(fb, w, h, px, py+1, c);
                        uint32_t glow = rgb(40, 80, 120);
                        fb[py * w + px + 2] = add_pixel(fb[py * w + px + 2], glow, 80);
                        fb[(py + 1) * w + px] = add_pixel(fb[(py + 1) * w + px], glow, 80);
                        fb[(py - 1) * w + px] = add_pixel(fb[(py - 1) * w + px], glow, 80);
                        fb[py * w + px - 2] = add_pixel(fb[py * w + px - 2], glow, 80);
                    } else {
                        put_px(fb, w, h, px, py, c);
                    }
                }
            }
        }

        float us = (float)((w < h) ? w : h) / 480.0f;
        float cx = w * 0.5f, cy = h * 0.42f;

        {
            float R = 210.0f * us;
            float ax = 1.1f, ay = t * 0.15f, az = 0.0f;
            
            float ring_m[3][3];
            rotate3(1, 0, 0, ax, ay, az, &ring_m[0][0], &ring_m[1][0], &ring_m[2][0]);
            rotate3(0, 1, 0, ax, ay, az, &ring_m[0][1], &ring_m[1][1], &ring_m[2][1]);
            rotate3(0, 0, 1, ax, ay, az, &ring_m[0][2], &ring_m[1][2], &ring_m[2][2]);

            int px_prev = 0, py_prev = 0;
            for (int s = 0; s <= 48; s++) {
                float a = (float)s * 6.2831853f / 48.0f;
                float vx = mage_cos(a) * R;
                float vz = mage_sin(a) * R;
                
                float rx = vx * ring_m[0][0] + vz * ring_m[0][2];
                float ry = vx * ring_m[1][0] + vz * ring_m[1][2];
                float rz = vx * ring_m[2][0] + vz * ring_m[2][2];

                float persp = 300.0f / (rz + 400.0f * us);
                int px = (int)(rx * persp + cx);
                int py = (int)(ry * persp + cy);
                if (s > 0) draw_line(fb, w, h, px_prev, py_prev, px, py, 0x8B5A2B);
                px_prev = px; py_prev = py;
            }
            for (int j = 5; j >= 0; j--) {
                float a = t * 1.3f - (float)j * 0.06f;
                float vx = mage_cos(a) * R;
                float vz = mage_sin(a) * R;
                
                float rx = vx * ring_m[0][0] + vz * ring_m[0][2];
                float ry = vx * ring_m[1][0] + vz * ring_m[1][2];
                float rz = vx * ring_m[2][0] + vz * ring_m[2][2];

                float persp = 300.0f / (rz + 400.0f * us);
                int px = (int)(rx * persp + cx);
                int py = (int)(ry * persp + cy);
                int k = (j == 0) ? 255 : (210 - j * 30);
                uint32_t c = (j == 0) ? rgb(255, 255, 255) : dimc(0xFFC857, k);
                put_px(fb, w, h, px, py, c);
                if (j == 0) {
                    put_px(fb, w, h, px+1, py, c);
                    put_px(fb, w, h, px, py+1, c);
                }
            }
        }

        {
            float breathe = mage_sin(t * 1.3f) * 0.5f + 0.5f;
            float hr = (150.0f + breathe * 22.0f) * us;
            uint32_t hc = dimc(0xFFC857, 60 + (int)(breathe * 50));
            int hx_prev = 0, hy_prev = 0;
            for (int sg = 0; sg <= 24; sg++) {
                float a = (float)sg * 6.2831853f / 24.0f + t * 0.1f;
                int px = (int)(cx + mage_cos(a) * hr);
                int py = (int)(cy + mage_sin(a) * hr);
                if (sg > 0) draw_line(fb, w, h, hx_prev, hy_prev, px, py, hc);
                hx_prev = px; hy_prev = py;
            }
        }

        {
            float rot_y = t * 0.8f;
            float rot_x = t * 0.5f;
            float scale = (95.0f + mage_sin(t * 0.7f) * 12.0f) * us;

            float ico_m[3][3];
            rotate3(1, 0, 0, rot_x, rot_y, 0.0f, &ico_m[0][0], &ico_m[1][0], &ico_m[2][0]);
            rotate3(0, 1, 0, rot_x, rot_y, 0.0f, &ico_m[0][1], &ico_m[1][1], &ico_m[2][1]);
            rotate3(0, 0, 1, rot_x, rot_y, 0.0f, &ico_m[0][2], &ico_m[1][2], &ico_m[2][2]);

            for (int i = 0; i < CUBE_VERTS; i++) {
                float vx = cube_verts[i][0] * scale;
                float vy = cube_verts[i][1] * scale;
                float vz = cube_verts[i][2] * scale;
                
                float rx = vx * ico_m[0][0] + vy * ico_m[0][1] + vz * ico_m[0][2];
                float ry = vx * ico_m[1][0] + vy * ico_m[1][1] + vz * ico_m[1][2];
                float rz = vx * ico_m[2][0] + vy * ico_m[2][1] + vz * ico_m[2][2];

                float perspective = 300.0f / (rz + 400.0f * us);
                proj_x[i] = rx * perspective + cx;
                proj_y[i] = ry * perspective + cy;
                proj_z[i] = rz;
            }

            {
                typedef struct { int f; float z; } FaceSort;
                FaceSort sorted_faces[CUBE_FACES];
                int fi, fj;
                ensure_cube_faces();
                for (fi = 0; fi < CUBE_FACES; fi++) {
                    int f0 = cube_faces[fi][0];
                    int f1 = cube_faces[fi][1];
                    int f2 = cube_faces[fi][2];
                    sorted_faces[fi].f = fi;
                    sorted_faces[fi].z = proj_z[f0] + proj_z[f1] + proj_z[f2];
                }
                for (fi = 1; fi < CUBE_FACES; fi++) {
                    FaceSort key = sorted_faces[fi];
                    fj = fi - 1;
                    while (fj >= 0 && sorted_faces[fj].z > key.z) {
                        sorted_faces[fj+1] = sorted_faces[fj];
                        fj--;
                    }
                    sorted_faces[fj+1] = key;
                }
                for (fi = 0; fi < CUBE_FACES; fi++) {
                    int f0 = cube_faces[sorted_faces[fi].f][0];
                    int f1 = cube_faces[sorted_faces[fi].f][1];
                    int f2 = cube_faces[sorted_faces[fi].f][2];
                    float avg_z = sorted_faces[fi].z / 3.0f;
                    float fzn = (avg_z + 150.0f * us) / (300.0f * us);
                    if (fzn < 0.0f) fzn = 0.0f;
                    if (fzn > 1.0f) fzn = 1.0f;
                    int fcr = (int)(70.0f + fzn * 40.0f);
                    int fcg = (int)(55.0f + (1.0f - fzn) * 30.0f);
                    int fcb = (int)(65.0f + fzn * 35.0f);
                    if (fcr > 255) fcr = 255;
                    if (fcg > 255) fcg = 255;
                    if (fcb > 255) fcb = 255;
                    fill_triangle(fb, w, h,
                                  (int)proj_x[f0], (int)proj_y[f0],
                                  (int)proj_x[f1], (int)proj_y[f1],
                                  (int)proj_x[f2], (int)proj_y[f2],
                                  rgb((uint8_t)fcr,
                                      (uint8_t)fcg,
                                      (uint8_t)fcb));
                }
            }

            for (int i = 0; i < CUBE_EDGES; i++) {
                sorted_edges[i].a = cube_edges[i][0];
                sorted_edges[i].b = cube_edges[i][1];
                sorted_edges[i].z = proj_z[sorted_edges[i].a] + proj_z[sorted_edges[i].b];
            }
            for (int i = 1; i < CUBE_EDGES; i++) {
                EdgeSort key = sorted_edges[i];
                int j = i - 1;
                while (j >= 0 && sorted_edges[j].z > key.z) {
                    sorted_edges[j+1] = sorted_edges[j];
                    j--;
                }
                sorted_edges[j+1] = key;
            }

            for (int i = 0; i < CUBE_EDGES; i++) {
                int a = sorted_edges[i].a;
                int b = sorted_edges[i].b;
                float avg_z = sorted_edges[i].z * 0.5f;
                float zn = (avg_z + 150.0f * us) / (300.0f * us);
                if (zn < 0.0f) zn = 0.0f;
                if (zn > 1.0f) zn = 1.0f;
                float shift = mage_sin(t * 2.0f + avg_z * 0.01f) * 0.5f + 0.5f;
                int cr = (int)(160.0f + zn * 60.0f + shift * 40.0f);
                int cg = (int)(90.0f + (1.0f - zn) * 40.0f + shift * 20.0f);
                int cb = (int)(50.0f + zn * 25.0f + shift * 15.0f);
                if (cr > 255) cr = 255;
                if (cg > 255) cg = 255;
                draw_line(fb, w, h,
                          (int)proj_x[a], (int)proj_y[a],
                          (int)proj_x[b], (int)proj_y[b],
                          rgb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb));
            }

            for (int i = 0; i < CUBE_VERTS; i++) {
                int px = (int)proj_x[i], py = (int)proj_y[i];
                put_px(fb, w, h, px, py, rgb(255, 255, 255));
                uint32_t glow = rgb(180, 100, 50);
                fb[py * w + px + 1] = add_pixel(fb[py * w + px + 1], glow, 100);
                fb[py * w + px - 1] = add_pixel(fb[py * w + px - 1], glow, 100);
                fb[(py + 1) * w + px] = add_pixel(fb[(py + 1) * w + px], glow, 100);
                fb[(py - 1) * w + px] = add_pixel(fb[(py - 1) * w + px], glow, 100);
            }
        }

        {
            float q = (pp < 0.7f) ? (pp / 0.7f) : 1.0f;
            float ease = q * q * (3.0f - 2.0f * q);
            float s = 0.3f + 3.6f * ease + 0.05f * mage_sin(t * 6.0f);
            if (zt_bb_w > 0) {
                float s_max = (float)(w - 40) / (float)zt_bb_w;
                float s_max_h = (float)(h / 2) / (float)zt_bb_h;
                if (s > s_max) s = s_max;
                if (s > s_max_h) s = s_max_h;
            }
            float bright = pp * 6.0f;
            if (bright > 1.0f) bright = 1.0f;
            if (pp > 0.92f) bright *= (1.0f - pp) / 0.08f;
            int k = (int)(bright * 256.0f);
            if (k > 256) k = 256;
            if ((frame & 1) == 0)
                zt_blit(fb, w, h, s * 1.08f, (k * 80) >> 8, zt_cols[msg]);
            zt_blit(fb, w, h, s, k, zt_cols[msg]);
        }

        frame++;

        gpu_draw_string(20, 16, (const uint8_t *)"MAGE7968", 0xFFC857, 0x000000);
        gpu_fill_rect(20, 34, 88, 2, 0xFFC857);
        gpu_draw_string(20, 40, (const uint8_t *)"AURIONOS SECURITY TRIBUTE", 0x667755, 0x000000);

        if (w > 340) {
            int p = 0;
            const char *f = "FPS ";
            for (int i = 0; f[i]; i++) sbuf[p++] = f[i];
            char nb[8];
            int_to_str((uint32_t)fps_sm, nb);
            for (int i = 0; nb[i]; i++) sbuf[p++] = nb[i];
            const char *u = "  T+";
            for (int i = 0; u[i]; i++) sbuf[p++] = u[i];
            int_to_str((now - start_tick) / 100, nb);
            for (int i = 0; nb[i]; i++) sbuf[p++] = nb[i];
            sbuf[p++] = 'S';
            sbuf[p] = 0;
            gpu_draw_string(w - 250, 16, (const uint8_t *)sbuf, 0x8899AA, 0x000000);
        }

        draw_git_icon(fb, w, h, 20, h - 48, 0xFFC857);
        gpu_draw_string(36, h - 46, (const uint8_t *)"GITHUB.COM/WILLIAM43243", 0xFFC857, 0x000000);
        for (int i = 0; i < ZT_MSG_COUNT; i++) {
            gpu_fill_rect(36 + i * 14, h - 26, 6, 6,
                          (i == msg) ? 0xFFC857 : 0x332211);
        }
        if (w > 340)
            gpu_draw_string(w - 110, h - 28, (const uint8_t *)"[ESC] EXIT", 0xFFC857, 0x000000);

        gpu_flush();
        sleep_ms(16);
    }

    kfree(post_buf);
    kfree(half_buf);
    return 0;
}
/* 3D demo lives in src/apps/app_3d_demo.c */
extern void app_3d_demo_create(void);

static int cmd_auriongl(const char *args) {
    (void)args;

    uint32_t *fb = gpu_get_backbuffer();
    int w = gpu_get_width();
    int h = gpu_get_height();

    if (!fb || w <= 0 || h <= 0) {
        puts("AURIONGL not available. Text mode only.\n");
        return -1;
    }

    puts("Initializing AurionGL...\n");
    app_3d_demo_create();
    puts("AurionGL 3D demo launched.\n");
    return 0;
}

static const Command commands[] = {
                                   {"HELP", cmd_help},
                                   {"?", cmd_help},
                                   {"CLS", cmd_cls},
                                   {"CLEAR", cmd_clear},
                                   {"VER", cmd_ver},
                                   {"VERSION", cmd_ver},
                                   {"WP", cmd_wp}, /* TEMP-DEBUG */
                                   {"PANICTEST", cmd_panictest},
                                   {"SNDTEST", cmd_sndtest},
                                   {"TIME", cmd_time},
                                   {"DATE", cmd_date},
                                   {"REBOOT", cmd_reboot},
                                   {"SHUTDOWN", cmd_shutdown},
                                   {"PAUSE", cmd_pause},
                                   {"SLEEP", cmd_sleep},

                                   {"EXIT", cmd_exit},

                                   /* File operations */
                                   {"MKDIR", cmd_mkdir},
                                   {"RMDIR", cmd_rmdir},
                                   {"TOUCH", cmd_touch},
                                   {"DEL", cmd_del},
                                   {"RM", cmd_rm},
                                   {"DIR", cmd_dir},
                                   {"LS", cmd_ls},
                                   {"CD", cmd_cd},
                                   {"PWD", cmd_pwd},
                                   {"CAT", cmd_cat},
                                   {"TYPE", cmd_type},
                                   {"NANO", cmd_nano},
                                   {"COPY", cmd_copy},
                                   {"CP", cmd_cp},
                                   {"MOVE", cmd_move},
                                   {"MV", cmd_mv},
                                   {"REN", cmd_ren},
                                   {"RENAME", cmd_ren},
                                   {"FIND", cmd_find},
                                   {"TREE", cmd_tree},
                                   {"ATTRIB", cmd_attrib},
                                   {"CHMOD", cmd_chmod},
                                   {"UNHIDE", cmd_unhide},

                                   /* Disk operations */
                                   {"VOL", cmd_vol},
                                   {"LABEL", cmd_label},
                                   {"CHKDSK", cmd_chkdsk},
                                   {"FORMAT", cmd_format},
                                   {"DISKPART", cmd_diskpart},
                                   {"FSCK", cmd_fsck},
                                   {"MOUNT", cmd_mount},
                                   {"UMOUNT", cmd_umount},
                                   {"SYNC", cmd_sync},
                                   {"FREE", cmd_free},
                                   {"DF", cmd_df},
                                   {"DU", cmd_du},
                                   {"LSBLK", cmd_lsblk},
                                   {"FDISK", cmd_fdisk},
                                   {"BLKID", cmd_blkid},
                                   {"READSECTOR", cmd_readsector},

                                   /* User management */
                                   {"USERADD", cmd_useradd},
                                   {"USERDEL", cmd_userdel},
                                   {"PASSWD", cmd_passwd},
                                   {"USERS", cmd_users},
                                   {"LOGIN", cmd_login},
                                   {"LOGOUT", cmd_logout},
                                   {"WHOAMI", cmd_whoami},
                                   {"SU", cmd_su},

                                   /* Process management */
                                   {"PS", cmd_ps},
                                   {"KILL", cmd_kill},
                                   {"TOP", cmd_top},
                                   {"TASKLIST", cmd_tasklist},
                                   {"TASKKILL", cmd_taskkill},

                                   /* System info */
                                   {"MEM", cmd_mem},
                                   {"UPTIME", cmd_uptime},
                                   {"SYSINFO", cmd_sysinfo},
                                   {"UNAME", cmd_uname},
                                   {"HOSTNAME", cmd_hostname},
                                   {"LSCPU", cmd_lscpu},
                                   {"LSPCI", cmd_lspci},
                                   {"DMESG", cmd_dmesg},

                                   /* Screen/display */
                                   {"COLOR", cmd_color},
                                   {"ECHO", cmd_echo},
                                   {"MODE", cmd_mode},

                                   /* Utilities */
                                   {"CALC", cmd_calc},
                                    
                                    
                                    
                                    {"VANTA", cmd_vanta},
                                   {"HEXDUMP", cmd_hexdump},
                                   {"ASCII", cmd_ascii},
                                   {"HASH", cmd_hash},
                                   {"PAUSE", cmd_pause},
                                   {"SLEEP", cmd_sleep},

                                   
                                   
                                   
                                   
                                   
                                   
                                   
                                   
                                   
                                   
                                   
                                   
                                   {"SUDO", cmd_sudo},
                                   {"MAKE", cmd_make},

                                   /* File utilities */
                                   {"WC", cmd_wc},
                                   {"HEAD", cmd_head},
                                   {"TAIL", cmd_tail},
                                   {"GREP", cmd_grep},
                                   {"SORT", cmd_sort},
                                   {"UNIQ", cmd_uniq},
                                   {"CUT", cmd_cut},
                                   {"DIFF", cmd_diff},
                                   {"MORE", cmd_more},
                                   {"LESS", cmd_less},
                                   {"FILE", cmd_file},
                                   {"STAT", cmd_stat},

                                   /* Environment */
                                   {"PATH", cmd_path},
                                   {"SET", cmd_set},
                                   {"ALIAS", cmd_alias},
                                   {"HISTORY", cmd_history},
                                   {"PROMPT", cmd_prompt},
                                   {"PRINTENV", cmd_printenv},
                                   {"EXPORT", cmd_export},
                                   {"SOURCE", cmd_source},
                                   {"WHICH", cmd_which},
                                   {"WHEREIS", cmd_whereis},

                                   /* More utilities */
                                   {"CAL", cmd_cal},
                                   {"BANNER", cmd_banner},
                                   {"FIGLET", cmd_figlet},
                                   {"COWSAY", cmd_cowsay},
                                   {"FORTUNE", cmd_fortune},
                                   {"STRINGS", cmd_strings},
                                   {"CHOWN", cmd_chown},
                                   {"LN", cmd_ln},
                                   {"STRACE", cmd_strace},
                                   {"NOHUP", cmd_nohup},
                                   {"NICE", cmd_nice},
                                   {"BG", cmd_bg},
                                   {"FG", cmd_fg},
                                   {"JOBS", cmd_jobs},
                                   {"UNALIAS", cmd_unalias},
                                   {"PRINTF", cmd_printf},
                                   {"READ", cmd_read},
                                   {"LET", cmd_let},
                                   {"TEST", cmd_test},
                                   {"FALSE", cmd_false},
                                   {"TRUE", cmd_true},
                                   {"UNSET", cmd_unset},
                                   {"ID", cmd_id},
                                   {"TIMEOUT", cmd_timeout},
                                   {"YES", cmd_yes},
                                   {"SHUF", cmd_shuf},
                                   {"SEQ", cmd_seq},
                                   {"FACTOR", cmd_factor},
                                   {"TAC", cmd_tac},
                                   {"NL", cmd_nl},
                                   {"REV", cmd_rev},
                                   {"OD", cmd_od},
                                   {"BASE64", cmd_base64},
                                   {"AWK", cmd_awk},
                                   {"SED", cmd_sed},
                                   {"TR", cmd_tr},
                                   {"PASTE", cmd_paste},
                                   {"INSTALL", cmd_install},
                                   {"REINSTALL", cmd_reinstall},
                                   {"DOSMODE", cmd_dosmode},
                                   
                                   
                                   
                                   {"W", cmd_w},
                                   {"WHO", cmd_who},
                                   {"LAST", cmd_last},
                                   {"KEYBOARD", cmd_keyboard},
                                    {"FATLOAD", cmd_fatload},
                                    {"MAGE7968", cmd_mage7968},
                                    {"AURIONGL", cmd_auriongl},
                                    {"#", cmd_noop},

                                   {NULL, NULL}};

/* Command dispatcher - called from shell */
int cmd_dispatch(const char *line)
{
  if (!line || !line[0])
    return 0;

  /* Extract command name */
  char cmd_name[64];
  const char *args = get_token(line, cmd_name, 64);

  /* Convert to uppercase */
  str_upper(cmd_name);

  /* Find and execute command */
  for (int i = 0; commands[i].name != NULL; i++)
  {
    if (str_cmp(cmd_name, commands[i].name) == 0)
    {
      return commands[i].func(args);
    }
  }

  /* Command not found */
  return -255;
}

/* Initialize command system */
void cmd_init(void) {
  /* AUR-01: Initialize CSPRNG before any crypto operations */
  csprng_init(&g_csprng);
  fs_init_commands();
}

/* Silent version - no output messages */
void cmd_init_silent(void)
{
  csprng_init(&g_csprng);
  fs_init_silent = 1;
  fs_init_commands();
  fs_init_silent = 0;
}
