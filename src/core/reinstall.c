#include "reinstall.h"
#include "fs_maintenance.h"

extern void c_puts(const char *text);
extern void c_putc(char c);
extern int c_kb_hit(void);
extern uint16_t c_getkey(void);
extern volatile int terminal_last_char;
extern char keyboard_remap(char c);
extern void sleep_ms(uint32_t ms);
extern void wm_update_light(void);
extern void sched_progress_wait(int state);
extern void sched_progress_run(void);
extern void sys_reboot(void);
extern void fs_reset_sudo(void);
extern bool fs_authenticate_current_user(const char *password);
extern char current_user[32];
extern int boot_mode_flag;
extern int str_cmp(const char *a, const char *b);

static void wipe_password(char *password, uint32_t size)
{
    volatile char *p = password;
    while (size--) *p++ = 0;
}

/* Read through Enter even when full: a long answer must never confirm by
 * its truncated prefix. Escape/Ctrl+C cancel instead of supplying input. */
static int read_answer(char *out, uint32_t capacity, bool masked)
{
    uint32_t used = 0;
    bool overflow = false;
    for (;;) {
        sched_progress_wait(1);
        while (!c_kb_hit()) {
            if (boot_mode_flag != 1) wm_update_light();
            sleep_ms(1);
        }
        sched_progress_run();
        /* The GUI has already mapped terminal keys; direct keyboard reads
         * still need the selected layout, just like installer passwords. */
        bool mapped = terminal_last_char != 0;
        char c = (char)(c_getkey() & 0xFF);
        if (!mapped) c = keyboard_remap(c);
        if (c == 27 || c == 3) {
            out[0] = 0;
            c_puts("\n");
            return -1;
        }
        if (c == '\r' || c == '\n') {
            out[used] = 0;
            c_puts("\n");
            return overflow ? -1 : (int)used;
        }
        if (c == 8 || c == 127) {
            if (used && !overflow) {
                used--;
                c_puts("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (used + 1 < capacity && !overflow) {
                out[used++] = c;
                c_putc(masked ? '*' : c);
            } else {
                overflow = true;
            }
        }
    }
}

static bool aurion_argument(const char *args)
{
    if (!args) return false;
    while (*args == ' ' || *args == '\t') args++;
    char word[16];
    uint32_t n = 0;
    while (*args && *args != ' ' && *args != '\t') {
        if (n + 1 >= sizeof(word)) return false;
        word[n++] = *args++;
    }
    word[n] = 0;
    while (*args == ' ' || *args == '\t') args++;
    return !*args && str_cmp(word, "aurion") == 0;
}

int cmd_reinstall(const char *args)
{
    (void)args;
    c_puts("Use: sudo reinstall aurion\n");
    return -1;
}

int cmd_reinstall_from_sudo(const char *args)
{
    if (!aurion_argument(args)) {
        c_puts("Usage: sudo reinstall aurion\n");
        return -1;
    }
    if (!current_user[0]) {
        c_puts("Log in before reinstalling AurionOS.\n");
        return -1;
    }
    fs_reset_sudo();
    c_puts("[sudo] password for ");
    c_puts(current_user);
    c_puts(": ");
    char password[32] = {0};
    int got = read_answer(password, sizeof(password), true);
    bool authenticated = got > 0 && fs_authenticate_current_user(password);
    wipe_password(password, sizeof(password));
    if (!authenticated) {
        c_puts("Authentication failed or cancelled. Nothing was erased.\n");
        return -1;
    }
    c_puts("This erases ALL AurionOS accounts, passwords, files and settings.\n");
    c_puts("AurionOS will restart at first-time setup.\n");
    c_puts("Are you sure? yes/no: ");
    char answer[16];
    if (read_answer(answer, sizeof(answer), false) <= 0 || str_cmp(answer, "yes") != 0) {
        c_puts("Reinstall cancelled. Nothing was erased.\n");
        return 0;
    }
    c_puts("Erasing and verifying AurionOS user data...\n");
    if (fs_reset_user_data() != 0) {
        c_puts("Disk erase failed. Setup was not restarted.\n");
        if (fs_writes_blocked())
            c_puts("Disk writes are blocked; an interrupted reset resumes on reboot.\n");
        return -1;
    }
    c_puts("All AurionOS user data cleared. Restarting setup...\n");
    sys_reboot();
    return 0;
}
