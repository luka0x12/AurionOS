/*
 * app_task_manager.c
 *
 * Task Manager + Audio Manager for AurionOS.
 *
 * Every number on screen is read from a live kernel structure:
 *   CPU%    -> scheduler.c cpu_ticks delta over a 1 s window
 *   MEM     -> memory.asm heap stats + bios_ram_bytes (BIOS INT 15h)
 *   DISK    -> fs_usage.c: fs_table byte sum vs ATA IDENTIFY capacity
 *   UPTIME  -> get_ticks() (PIT, 100 Hz)
 *   PROC    -> scheduler.c process table via sched_iter()
 *   THREADS -> Proc.threads[]
 *   AUDIO   -> fs_iterate() scan + mp3 driver state + WAV header parse
 *
 * Nothing is simulated. Unmeasurable values show n/a until they exist.
*/

#include <stdint.h>
#include <stdbool.h>
#include "window_manager.h"
#include "fs.h"
#include "scheduler.h"

extern uint32_t get_ticks(void);
extern int smp_cpu_count(void);
extern void     mem_get_stats(uint32_t *stats);
extern void     wm_terminate_process(int pid);
extern uint32_t bios_ram_bytes;
extern int      load_file_content(const char *fn, char *buf, int max);

extern void  mp3_init(void);
extern bool  mp3_is_playing(void);
extern float mp3_get_position(void);
extern float mp3_get_volume(void);
extern void  mp3_set_volume(float volume);
extern bool  mp3_play(const char *filename);
extern void  mp3_pause(void);
extern void  mp3_resume(void);
extern void  mp3_stop(void);

__attribute__((weak)) void fs_get_usage(uint32_t *used_kb, uint32_t *total_kb) {
    *used_kb = 0;
    *total_kb = 0;
}

#define TM_BLACK   0xFF000000
#define TM_PANEL   0xFF0E0E0E
#define TM_ROW_A   0xFF0A0A0A
#define TM_ROW_B   0xFF000000
#define TM_ROW_SEL 0xFF161616
#define TM_BORDER  0xFF262626
#define TM_TEXT    0xFFE6E6E6
#define TM_DIM     0xFF8A8A8A
#define TM_GREEN   0xFF4ADE80
#define TM_GREEN_D 0xFF1E5A36
#define TM_WARN    0xFFFBBF24
#define TM_CRIT    0xFFEF4444

#define TM_HIST          60
#define TM_SPARK_H       44
#define TM_GRID          0xFF1C1C20
#define TM_MAX           64
#define TM_GLYPH         8
#define TM_SAMPLE_TICKS  100

static int tm_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void tm_copy(char *d, const char *s, int n) {
    int i = 0;
    while (i < n - 1 && s && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void tm_cat(char *d, const char *s, int cap) {
    int i = tm_strlen(d);
    while (i < cap - 1 && s && *s) d[i++] = *s++;
    d[i] = 0;
}

static void int_to_strn(int val, char *buf, int cap) {
    if (!buf || cap <= 0) return;
    bool neg = (val < 0);
    unsigned int v = neg ? (unsigned int)(-(val + 1)) + 1u : (unsigned int)val;
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0 && i < (int)sizeof(tmp) - 1) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    int j = 0;
    if (neg && j < cap - 1) buf[j++] = '-';
    while (i > 0 && j < cap - 1) buf[j++] = tmp[--i];
    buf[j] = 0;
}

static void fmt_hex(char *buf, uint32_t v) {
    static const char *hx = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hx[(v >> (28 - i * 4)) & 0xF];
    buf[10] = 0;
}

static void fmt_mem_short(char *buf, int cap, uint32_t bytes) {
    buf[0] = 0;
    if (bytes >= 1048576u) {
        int mb   = (int)(bytes / 1048576u);
        int frac = (int)((bytes % 1048576u) / 104858u);
        int_to_strn(mb, buf, cap);
        if (mb < 10 && frac > 0) {
            tm_cat(buf, ".", cap);
            char f[4]; int_to_strn(frac, f, 4); tm_cat(buf, f, cap);
        }
        tm_cat(buf, "MB", cap);
    } else {
        int_to_strn((int)(bytes / 1024u), buf, cap);
        tm_cat(buf, "KB", cap);
    }
}

static void fmt_mem_fit(char *buf, int cap, uint32_t used, uint32_t total, int avail_px) {
    char pretty[40], compact[40], tiny[40];
    pretty[0] = compact[0] = tiny[0] = 0;

    fmt_mem_short(pretty, 40, used);
    tm_cat(pretty, " / ", 40);
    char tb[20]; fmt_mem_short(tb, 20, total); tm_cat(pretty, tb, 40);

    int_to_strn((int)(used / 1048576u), compact, 40);
    tm_cat(compact, "/", 40);
    int_to_strn((int)(total / 1048576u), compact + tm_strlen(compact), 40 - tm_strlen(compact));
    tm_cat(compact, "MB", 40);

    int_to_strn((int)(used / 1048576u), tiny, 40);
    tm_cat(tiny, "/", 40);
    int_to_strn((int)(total / 1048576u), tiny + tm_strlen(tiny), 40 - tm_strlen(tiny));
    tm_cat(tiny, "MB", 40);

    if (tm_strlen(pretty)  * TM_GLYPH <= avail_px) { tm_copy(buf, pretty,  cap); return; }
    if (tm_strlen(compact) * TM_GLYPH <= avail_px) { tm_copy(buf, compact, cap); return; }
    if (tm_strlen(tiny)    * TM_GLYPH <= avail_px) { tm_copy(buf, tiny,    cap); return; }
    int_to_strn((int)(used / 1048576u), buf, cap);
    tm_cat(buf, "M", cap);
}

static void fmt_uptime(char *buf, int cap, uint32_t ticks) {
    uint32_t secs = ticks / 100u;
    uint32_t h = secs / 3600u, m = (secs % 3600u) / 60u, s = secs % 60u;
    char t[8];
    buf[0] = 0;
    if (h) { int_to_strn((int)h, t, 8); tm_cat(buf, t, cap); tm_cat(buf, "h ", cap); }
    int_to_strn((int)m, t, 8); tm_cat(buf, t, cap); tm_cat(buf, "m ", cap);
    int_to_strn((int)s, t, 8); tm_cat(buf, t, cap); tm_cat(buf, "s", cap);
}

static uint32_t load_color(int pct) {
    if (pct < 0)  return TM_DIM;
    if (pct < 60) return TM_GREEN;
    if (pct < 85) return TM_WARN;
    return TM_CRIT;
}

static const char *kind_str(ProcKind k) {
    switch (k) {
        case PROC_KERNEL:  return "SYS";
        case PROC_SHELL:   return "SH";
        case PROC_GUI:     return "GUI";
        case PROC_SERVICE: return "SVC";
        default:           return "?";
    }
}

static const char *pstate_str(ProcState s) {
    switch (s) {
        case PSTATE_READY:    return "READY";
        case PSTATE_RUNNING:  return "RUN";
        case PSTATE_SLEEPING: return "SLEEP";
        case PSTATE_ZOMBIE:   return "ZOMBIE";
        default:              return "-";
    }
}

static const char *tstate_str(ThreadState s) {
    switch (s) {
        case TSTATE_READY:    return "READY";
        case TSTATE_RUNNING:  return "RUN";
        case TSTATE_BLOCKED:  return "BLOCK";
        case TSTATE_SLEEPING: return "SLEEP";
        default:              return "-";
    }
}

typedef struct {
    Window *win;
    int selected, scroll;
    int sys_cpu;
    int idle_pct;
    int hist[TM_HIST], hist_n;
    int snap_pid[TM_MAX];
    uint32_t snap_cpu[TM_MAX];
    uint32_t snap_total[TM_MAX];
    int snap_n;
    int pids[TM_MAX];
    int n;
    int mouse_pending_y;
    uint32_t last_sample_tick;
    uint8_t sampled_this_frame;
    int disp_cpu_pid[TM_MAX];
    int disp_cpu[TM_MAX];
    int disp_cpu_n;
} TmState;

static TmState tm_state;

static int tm_cpu(TmState *st, int pid, Proc *p) {
    uint32_t total = get_ticks();
    int idx = -1;
    for (int i = 0; i < st->snap_n; i++)
        if (st->snap_pid[i] == pid) { idx = i; break; }
    if (idx < 0) {
        idx = (st->snap_n < TM_MAX) ? st->snap_n++ : 0;
        st->snap_pid[idx]   = pid;
        st->snap_cpu[idx]   = p->cpu_ticks;
        st->snap_total[idx] = total;
        return -1;
    }
    uint32_t dt = total - st->snap_total[idx];
    if (dt < TM_SAMPLE_TICKS) return -1;
    if (p->cpu_ticks < st->snap_cpu[idx]) {
        st->snap_cpu[idx]   = p->cpu_ticks;
        st->snap_total[idx] = total;
        return -1;
    }
    uint32_t dp = p->cpu_ticks - st->snap_cpu[idx];
    if (dt == 0) return -1;
    int pct = (int)((uint64_t)dp * 100u / (uint64_t)dt);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    st->snap_cpu[idx]   = p->cpu_ticks;
    st->snap_total[idx] = total;
    return pct;
}

static void draw_hbar(Window *win, int x, int y, int w, int h, int pct, uint32_t fill) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    wm_fill_rect(win, x, y, w, h, TM_PANEL);
    wm_fill_rect(win, x, y, w, 1, TM_BORDER);
    wm_fill_rect(win, x, y + h - 1, w, 1, TM_BORDER);
    int fw = (w - 2) * pct / 100;
    if (fw > 0) wm_fill_rect(win, x + 1, y + 1, fw, h - 2, fill);
}

static void draw_spark(Window *win, int x, int y, int w, int h, TmState *st) {
    wm_fill_rect(win, x, y, w, h, TM_BLACK);
    wm_fill_rect(win, x, y, w, 1, TM_BORDER);
    wm_fill_rect(win, x, y + h - 1, w, 1, TM_BORDER);
    wm_fill_rect(win, x, y, 1, h, TM_BORDER);
    wm_fill_rect(win, x + w - 1, y, 1, h, TM_BORDER);
    int inner = h - 4;
    if (inner < 4) inner = 4;
    int base = y + h - 2;
    for (int q = 1; q <= 3; q++) {
        int gy = base - inner * (q * 25) / 100;
        for (int gx = x + 2; gx < x + w - 2; gx += 2)
            wm_fill_rect(win, gx, gy, 1, 1, TM_GRID);
    }
    if (st->hist_n < 1 || w < 8) return;
    int n     = st->hist_n < TM_HIST ? st->hist_n : TM_HIST;
    int start = (st->hist_n <= TM_HIST) ? 0 : (st->hist_n % TM_HIST);
    int span = w - 4;
    if (span < 1) span = 1;
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int v = st->hist[(start + i) % TM_HIST];
        if (v > peak) peak = v;
    }
    if (peak > 100) peak = 100;
    for (int px = 0; px < span; px++) {
        int idx = (px * n) / span;
        if (idx >= n) idx = n - 1;
        int v = st->hist[(start + idx) % TM_HIST];
        if (v < 0) continue;
        if (v > 100) v = 100;
        int bh = inner * v / 100;
        int bx = x + 2 + px;
        if (bh > 0) wm_fill_rect(win, bx, base - bh + 1, 1, bh, TM_GREEN_D);
        wm_fill_rect(win, bx, base - bh - 1 < y + 2 ? y + 2 : base - bh - 1,
                     1, 2, load_color(v));
    }
    {
        char pk[12];
        tm_copy(pk, "pk ", 12);
        int_to_strn(peak, pk + 3, 9);
        tm_cat(pk, "%", 12);
        int pw = tm_strlen(pk) * TM_GLYPH;
        wm_draw_string(win, x + w - 4 - pw, y + 3, pk, TM_DIM, TM_BLACK);
    }
}

static void stat_card(Window *win, int x, int y, int w, const char *label,
                      const char *value, int pct, uint32_t fill) {
    wm_fill_rect(win, x, y, w, 44, TM_PANEL);
    wm_fill_rect(win, x, y, w, 1, TM_BORDER);
    wm_fill_rect(win, x, y, 1, 44, TM_BORDER);
    wm_fill_rect(win, x + w - 1, y, 1, 44, TM_BORDER);
    wm_fill_rect(win, x, y + 43, w, 1, TM_BORDER);
    wm_draw_string(win, x + 8, y + 5,  label, TM_DIM,  TM_PANEL);
    wm_draw_string(win, x + 8, y + 17, value, TM_TEXT, TM_PANEL);
    draw_hbar(win, x + 8, y + 33, w - 16, 6, pct, fill);
}

typedef struct {
    int pad, card_w, card_y, spark_y, hdr_y, list_top, row_h;
    int visible, detail_y, detail_h, footer_h;
} TmLayout;

static void compute_layout(int cw, int ch, TmLayout *L) {
    (void)cw;
    L->pad      = 10;
    L->card_y   = 28;
    L->spark_y  = L->card_y + 44 + 6;
    L->hdr_y    = L->spark_y + TM_SPARK_H + 8;
    L->list_top = L->hdr_y + 15 + 3;
    L->row_h    = 17;
    L->footer_h = 18;
    L->detail_h = 92;
    int free_h = ch - L->list_top - L->footer_h;
    if (free_h < L->detail_h + L->row_h) {
        L->detail_h = free_h - L->row_h;
        if (L->detail_h < 32) L->detail_h = 32;
    }
    L->detail_y = ch - L->footer_h - L->detail_h;
    int list_h  = L->detail_y - L->list_top;
    L->visible  = list_h / L->row_h;
    if (L->visible < 1) L->visible = 1;
}

static void task_manager_on_draw(Window *win) {
    TmState *st = (TmState *)win->user_data;
    if (!st) st = &tm_state;
    int cw = win->w, ch = win->h;
    TmLayout L;
    compute_layout(cw, ch, &L);

    st->sampled_this_frame = 0;
    uint32_t now = get_ticks();
    if (now - st->last_sample_tick >= TM_SAMPLE_TICKS) {
        st->last_sample_tick   = now;
        st->sampled_this_frame = 1;
        Proc *idle = sched_get(0);
        st->idle_pct = idle ? tm_cpu(st, 0, idle) : -1;
        st->sys_cpu  = (st->idle_pct < 0) ? -1 : (100 - st->idle_pct);
        if (st->sys_cpu >= 0) {
            st->hist[st->hist_n % TM_HIST] = st->sys_cpu;
            st->hist_n++;
        }
    }

    uint32_t ms[4] = {0, 0, 0, 0};
    mem_get_stats(ms);
    uint32_t heap_b  = ms[1];
    uint32_t total_b = bios_ram_bytes ? bios_ram_bytes : (ms[0] + ms[1]);

    uint32_t disk_used_kb = 0, disk_total_kb = 0;
    fs_get_usage(&disk_used_kb, &disk_total_kb);

    st->n = 0;
    int cur = 0, pid;
    while ((pid = sched_iter(&cur)) >= 0 && st->n < TM_MAX)
        st->pids[st->n++] = pid;
    if (st->selected >= st->n) st->selected = st->n - 1;
    if (st->selected < 0)      st->selected = 0;

    if (st->sampled_this_frame) {
        st->disp_cpu_n = 0;
        for (int i = 0; i < st->n && st->disp_cpu_n < TM_MAX; i++) {
            Proc *p = sched_get(st->pids[i]);
            if (!p) continue;
            st->disp_cpu_pid[st->disp_cpu_n] = p->pid;
            st->disp_cpu[st->disp_cpu_n] =
                (p->pid == 0) ? st->idle_pct : tm_cpu(st, p->pid, p);
            st->disp_cpu_n++;
        }
    }

    /* Rows show live heap bytes per owner pid: open a textured app and
     * watch it move, close it and watch it drop. The MEMORY card is the
     * heap total, so rows always sum honestly below it.
    */

    uint32_t used_b   = heap_b;

    wm_fill_rect(win, 0, 0, cw, ch, TM_BLACK);
    wm_fill_rect(win, 0, 0, cw, 2, TM_GREEN);
    wm_draw_string(win, 10, 9, "TASK MANAGER", TM_GREEN, TM_BLACK);
    char cpu_txt[16];
    if (st->sys_cpu < 0) tm_copy(cpu_txt, "n/a", 16);
    else { int_to_strn(st->sys_cpu, cpu_txt, 12); tm_cat(cpu_txt, "%", 16); }
    wm_draw_string(win, cw - 12 - tm_strlen(cpu_txt) * TM_GLYPH, 9,
                   cpu_txt, load_color(st->sys_cpu), TM_BLACK);

    int gap = 8;
    L.card_w = (cw - L.pad * 2 - gap * 3) / 4;
    int avail = L.card_w - 16;
    char vbuf[48];

    char cpu_label[12];
    tm_copy(cpu_label, "CPU x", 12);
    int_to_strn(smp_cpu_count(), cpu_label + 5, 7);
    stat_card(win, L.pad, L.card_y, L.card_w, cpu_label,
              st->sys_cpu < 0 ? "n/a" : cpu_txt,
              st->sys_cpu < 0 ? 0 : st->sys_cpu, load_color(st->sys_cpu));

    fmt_mem_fit(vbuf, 48, used_b, total_b, avail);
    int ram_pct = total_b ? (int)((uint64_t)used_b * 100u / total_b) : 0;
    stat_card(win, L.pad + (L.card_w + gap), L.card_y, L.card_w,
              "MEMORY", vbuf, ram_pct, TM_GREEN);

    if (disk_total_kb)
        fmt_mem_fit(vbuf, 48, disk_used_kb * 1024u, disk_total_kb * 1024u, avail);
    else
        tm_copy(vbuf, "n/a", 48);
    int disk_pct = disk_total_kb
        ? (int)((uint64_t)disk_used_kb * 100u / disk_total_kb) : 0;
    stat_card(win, L.pad + (L.card_w + gap) * 2, L.card_y, L.card_w,
              "DISK", vbuf, disk_pct,
              disk_total_kb ? load_color(disk_pct) : TM_GREEN_D);

    fmt_uptime(vbuf, 48, get_ticks());
    stat_card(win, L.pad + (L.card_w + gap) * 3, L.card_y, L.card_w,
              "UPTIME", vbuf, 0, TM_GREEN_D);

    wm_draw_string(win, L.pad, L.spark_y + 7, "CPU HISTORY", TM_DIM, TM_BLACK);
    int spark_x = L.pad + 92;
    int spark_w = cw - L.pad - spark_x;
    draw_spark(win, spark_x, L.spark_y, spark_w, TM_SPARK_H, st);

    wm_fill_rect(win, 0, L.hdr_y, cw, 1, TM_BORDER);
    int hy = L.hdr_y + 4;
    wm_draw_string(win, L.pad,       hy, "PID",   TM_DIM, TM_BLACK);
    wm_draw_string(win, L.pad + 48,  hy, "NAME",  TM_DIM, TM_BLACK);
    wm_draw_string(win, L.pad + 232, hy, "KIND",  TM_DIM, TM_BLACK);
    wm_draw_string(win, L.pad + 280, hy, "STATE", TM_DIM, TM_BLACK);
    wm_draw_string(win, L.pad + 344, hy, "THR",   TM_DIM, TM_BLACK);
    wm_draw_string(win, L.pad + 384, hy, "CPU",   TM_DIM, TM_BLACK);
    wm_draw_string(win, L.pad + 432, hy, "MEM",   TM_DIM, TM_BLACK);
    wm_fill_rect(win, 0, hy + 15, cw, 1, TM_BORDER);

    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + L.visible)
        st->scroll = st->selected - L.visible + 1;
    if (st->scroll < 0) st->scroll = 0;

    if (st->mouse_pending_y >= 0) {
        int my = st->mouse_pending_y;
        st->mouse_pending_y = -1;
        if (my >= L.list_top && my < L.detail_y) {
            int row = (my - L.list_top) / L.row_h;
            int idx = st->scroll + row;
            if (idx >= 0 && idx < st->n) st->selected = idx;
        }
    }

    int y = L.list_top;
    for (int i = st->scroll; i < st->n && i < st->scroll + L.visible; i++) {
        Proc *p = sched_get(st->pids[i]);
        if (!p) continue;
        uint32_t bg = (i == st->selected) ? TM_ROW_SEL
                    : ((i & 1) ? TM_ROW_A : TM_ROW_B);
        wm_fill_rect(win, 0, y, cw, L.row_h, bg);
        if (i == st->selected) wm_fill_rect(win, 0, y, 2, L.row_h, TM_GREEN);

        char col[24];
        int_to_strn(p->pid, col, 24);
        wm_draw_string(win, L.pad,       y + 2, col, TM_DIM, bg);
        wm_draw_string(win, L.pad + 48,  y + 2, p->name, TM_TEXT, bg);
        wm_draw_string(win, L.pad + 232, y + 2, kind_str(p->kind), TM_DIM, bg);
        uint32_t stc = (p->state == PSTATE_RUNNING) ? TM_GREEN : TM_DIM;
        wm_draw_string(win, L.pad + 280, y + 2, pstate_str(p->state), stc, bg);
        int_to_strn(p->thread_count, col, 24);
        wm_draw_string(win, L.pad + 344, y + 2, col, TM_DIM, bg);

        int pct = -1;
        for (int d = 0; d < st->disp_cpu_n; d++)
            if (st->disp_cpu_pid[d] == p->pid) { pct = st->disp_cpu[d]; break; }
        if (pct < 0) {
            wm_draw_string(win, L.pad + 384, y + 2, "n/a", TM_DIM, bg);
        } else {
            int_to_strn(pct, col, 24);
            tm_cat(col, "%", 24);
            wm_draw_string(win, L.pad + 384, y + 2, col, load_color(pct), bg);
        }

        uint32_t mb = sched_proc_bytes(p->pid);
        fmt_mem_short(col, 24, mb);
        wm_draw_string(win, L.pad + 432, y + 2, col, TM_DIM, bg);
        y += L.row_h;
    }

    wm_fill_rect(win, 0, L.detail_y, cw, 1, TM_BORDER);
    wm_fill_rect(win, 0, L.detail_y + 1, cw, L.detail_h - 1, TM_PANEL);
    Proc *sel = (st->selected >= 0 && st->selected < st->n)
        ? sched_get(st->pids[st->selected]) : 0;
    if (sel) {
        char line[64];
        tm_copy(line, "PROCESS ", 64);
        int_to_strn(sel->pid, line + tm_strlen(line), 64 - tm_strlen(line));
        tm_cat(line, "  ", 64);
        tm_cat(line, sel->name, 64);
        wm_draw_string(win, L.pad, L.detail_y + 5, line, TM_GREEN, TM_PANEL);

        tm_copy(line, "parent ", 64);
        int_to_strn(sel->parent_pid, line + tm_strlen(line), 64 - tm_strlen(line));
        tm_cat(line, "   prio ", 64);
        int_to_strn(sel->priority, line + tm_strlen(line), 64 - tm_strlen(line));
        wm_draw_string(win, L.pad, L.detail_y + 18, line, TM_DIM, TM_PANEL);

        if (L.detail_h >= 64) {
            int ty0 = L.detail_y + 33;
            wm_draw_string(win, L.pad,       ty0, "TID",       TM_DIM, TM_PANEL);
            wm_draw_string(win, L.pad + 56,  ty0, "STATE",     TM_DIM, TM_PANEL);
            wm_draw_string(win, L.pad + 128, ty0, "STACK",     TM_DIM, TM_PANEL);
            wm_draw_string(win, L.pad + 248, ty0, "SIZE",      TM_DIM, TM_PANEL);
            wm_draw_string(win, L.pad + 328, ty0, "CPU TICKS", TM_DIM, TM_PANEL);
            int ty   = ty0 + 13;
            int maxt = sel->thread_count;
            if (maxt > 3) maxt = 3;
            for (int t = 0; t < maxt && ty < L.detail_y + L.detail_h - 4; t++) {
                Thread *th = &sel->threads[t];
                int_to_strn(th->tid, line, 64);
                wm_draw_string(win, L.pad,       ty, line, TM_TEXT, TM_PANEL);
                wm_draw_string(win, L.pad + 56,  ty, tstate_str(th->state), TM_DIM, TM_PANEL);
                fmt_hex(line, th->stack_base);
                wm_draw_string(win, L.pad + 128, ty, line, TM_DIM, TM_PANEL);
                int_to_strn((int)th->stack_size, line, 64);
                wm_draw_string(win, L.pad + 248, ty, line, TM_DIM, TM_PANEL);
                int_to_strn((int)th->cpu_ticks, line, 64);
                wm_draw_string(win, L.pad + 328, ty, line, TM_DIM, TM_PANEL);
                ty += 13;
            }
        }
    } else {
        wm_draw_string(win, L.pad, L.detail_y + 5, "No process selected", TM_DIM, TM_PANEL);
    }

    wm_fill_rect(win, 0, ch - L.footer_h, cw, 1, TM_BORDER);
    char foot[64];
    tm_copy(foot, "Up/Dn select  End kill  F5 refresh  ", 64);
    char nbuf[8];
    int_to_strn(st->n, nbuf, 8);
    tm_cat(foot, nbuf, 64);
    tm_cat(foot, " procs", 64);
    wm_draw_string(win, L.pad, ch - L.footer_h + 5, foot, TM_DIM, TM_BLACK);
}

static void task_manager_on_key(Window *win, uint16_t key) {
    TmState *st = (TmState *)win->user_data;
    if (!st) st = &tm_state;
    uint8_t scan = (key >> 8) & 0xFF;
    if (scan == 0x48) {
        if (st->selected > 0) st->selected--;
    } else if (scan == 0x50) {
        if (st->selected < st->n - 1) st->selected++;
    } else if (scan == 0x49) {
        st->selected -= 8;
        if (st->selected < 0) st->selected = 0;
    } else if (scan == 0x51) {
        st->selected += 8;
        if (st->selected > st->n - 1) st->selected = st->n - 1;
    } else if (scan == 0x3F) {
        st->hist_n = 0;
        st->snap_n = 0;
    } else if (scan == 0x4F) {
        if (st->selected >= 0 && st->selected < st->n)
            wm_terminate_process(st->pids[st->selected]);
    }
}

static void task_manager_on_mouse(Window *win, int mx, int my, bool down, bool rmb) {
    (void)mx; (void)rmb;
    TmState *st = (TmState *)win->user_data;
    if (!st) st = &tm_state;
    if (down) st->mouse_pending_y = my;
}

void app_task_manager_create(void) {
    sched_init();
    int sw = wm_get_screen_w();
    int sh = wm_get_screen_h();
    int w = 560, h = 480;
    Window *win = wm_create_window("Task Manager",
                                   (sw - w) / 2, (sh - WM_TASKBAR_HEIGHT - h) / 2, w, h);
    if (!win) return;
    TmState *st = &tm_state;
    for (uint32_t i = 0; i < (uint32_t)sizeof(TmState); i++) ((char *)st)[i] = 0;
    st->win             = win;
    st->sys_cpu         = -1;
    st->mouse_pending_y = -1;
    for (int i = 0; i < TM_HIST; i++) st->hist[i] = -1;
    win->user_data = st;
    win->on_draw   = task_manager_on_draw;
    win->on_key    = task_manager_on_key;
    win->on_mouse  = task_manager_on_mouse;
}

/* audio manager below. playlist = real files on disk, duration parsed from
 * the WAV header, shuffle/repeat act on track end, progress = pos/duration.
 * state lives in win->app_data (4096 B), fits at ~1.2 KB.
*/

#define AM_MAX_TRACKS 16
#define AM_NAME_MAX   64

typedef struct {
    Window *win;
    bool   is_playing;
    float  volume;
    float  position_s;
    float  duration_s;      /* 0 = driver/file gave no real duration */
    char   playlist[AM_MAX_TRACKS][AM_NAME_MAX];
    int    playlist_count;
    int    current_track_idx;
    int    selected_idx;
    bool   shuffle_mode;
    bool   repeat_mode;
    uint8_t was_playing;    /* edge detect: driver stopped by itself = ended */
    uint8_t user_paused;    /* we stopped it, so don't advance */
    uint32_t rng_state;
    int    sweep;           /* indeterminate bar phase */
} AudioManagerState;

static uint32_t rd32le(const char *p) {
    return (uint32_t)(uint8_t)p[0] | ((uint32_t)(uint8_t)p[1] << 8) |
           ((uint32_t)(uint8_t)p[2] << 16) | ((uint32_t)(uint8_t)p[3] << 24);
}

/* real duration from the RIFF header; 0 when it is not a parseable WAV */
static float wav_duration_s(const char *path) {
    char hdr[512];
    int n = load_file_content(path, hdr, 512);
    if (n < 44) return 0.0f;
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F') return 0.0f;
    if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E') return 0.0f;
    uint32_t byte_rate = 0, data_size = 0;
    int p = 12;
    while (p + 8 <= n) {
        uint32_t csz = rd32le(hdr + p + 4);
        if (hdr[p] == 'f' && hdr[p+1] == 'm' && hdr[p+2] == 't' && hdr[p+3] == ' ') {
            if (p + 16 <= n) byte_rate = rd32le(hdr + p + 12);
        } else if (hdr[p] == 'd' && hdr[p+1] == 'a' && hdr[p+2] == 't' && hdr[p+3] == 'a') {
            data_size = csz;
        }
        p += 8 + (int)csz + (int)(csz & 1);
    }
    if (!byte_rate || !data_size) return 0.0f;
    return (float)data_size / (float)byte_rate;
}

static bool am_is_audio_name(const char *name) {
    int len = tm_strlen(name);
    if (len < 5 || name[len - 4] != '.') return false;
    char e0 = name[len - 3], e1 = name[len - 2], e2 = name[len - 1];
    if (e0 >= 'A' && e0 <= 'Z') e0 = (char)(e0 - 'A' + 'a');
    if (e1 >= 'A' && e1 <= 'Z') e1 = (char)(e1 - 'A' + 'a');
    if (e2 >= 'A' && e2 <= 'Z') e2 = (char)(e2 - 'A' + 'a');
    if (e0 == 'w' && e1 == 'a' && e2 == 'v') return true;
    if (e0 == 'm' && e1 == 'p' && e2 == '3') return true;
    if (e0 == 'o' && e1 == 'g' && e2 == 'g') return true;
    return false;
}

static void am_scan_tracks(AudioManagerState *st) {
    st->playlist_count = 0;
    const char *name;
    uint32_t size;
    int type;
    int cursor = 0;
    while (st->playlist_count < AM_MAX_TRACKS &&
           (cursor = fs_iterate(cursor, &name, &size, &type)) >= 0) {
        if (type != 0 || !am_is_audio_name(name)) continue;
        tm_copy(st->playlist[st->playlist_count], name, AM_NAME_MAX);
        st->playlist_count++;
    }
}

static uint32_t am_rand(AudioManagerState *st) {
    st->rng_state ^= st->rng_state << 13;
    st->rng_state ^= st->rng_state >> 17;
    st->rng_state ^= st->rng_state << 5;
    return st->rng_state;
}

static void am_play_idx(AudioManagerState *st, int idx) {
    if (idx < 0 || idx >= st->playlist_count) return;
    if (!mp3_play(st->playlist[idx])) return;
    st->current_track_idx = idx;
    st->selected_idx      = idx;
    st->user_paused       = 0;
    st->duration_s        = wav_duration_s(st->playlist[idx]);
}

static void am_on_track_end(AudioManagerState *st) {
    int cur = st->current_track_idx;
    if (st->repeat_mode) { am_play_idx(st, cur); return; }
    if (st->shuffle_mode && st->playlist_count > 1) {
        int nxt = cur;
        while (nxt == cur) nxt = (int)(am_rand(st) % (uint32_t)st->playlist_count);
        am_play_idx(st, nxt);
        return;
    }
    if (cur + 1 < st->playlist_count) { am_play_idx(st, cur + 1); return; }
    st->current_track_idx = -1;   /* playlist finished; say so, don't loop */
}

static void audio_float_to_str(float val, char *buf, int cap, int decimals) {
    if (!buf || cap <= 0) return;
    if (val < 0.0f) { if (cap > 1) { buf[0] = '-'; buf[1] = 0; } val = -val; }
    int_to_strn((int)val, buf, cap);
    if (decimals > 0) {
        int len = tm_strlen(buf);
        if (len < cap - 2 - decimals) {
            buf[len++] = '.';
            unsigned frac = (unsigned)((val - (float)(int)val) * 1000.0f);
            for (int d = 0; d < decimals; d++) {
                buf[len++] = (char)('0' + (frac / 100u) % 10u);
                frac = (frac % 100u) * 10u;
            }
            buf[len] = 0;
        }
    }
}

/* Same visual language as the task manager above: near-black panel,
 * hairline TM_BORDER rules, green accent reserved for "what is live":
 * the playing track, progress fill, active toggle. No blue anywhere.
*/

static void audio_manager_on_draw(Window *win) {
    AudioManagerState *st = (AudioManagerState *)win->app_data;
    if (!st) return;

    st->is_playing  = mp3_is_playing();
    st->volume      = mp3_get_volume();
    st->position_s  = st->is_playing ? mp3_get_position() / 1000.0f : 0.0f;

    /* driver stopped on its own while we were playing: the track ended */
    if (st->was_playing && !st->is_playing && !st->user_paused &&
        st->current_track_idx >= 0)
        am_on_track_end(st);
    st->was_playing = st->is_playing ? 1 : 0;

    int cw = win->w, ch = win->h;
    wm_fill_rect(win, 0, 0, cw, ch, TM_PANEL);
    wm_fill_rect(win, 0, 0, cw, 2, TM_GREEN);
    wm_draw_string(win, 10, 9, "AUDIO MANAGER", TM_GREEN, TM_BLACK);
    wm_fill_rect(win, 10, 36, cw - 20, 1, TM_BORDER);

    if (st->current_track_idx >= 0 && st->current_track_idx < st->playlist_count) {
        wm_draw_string(win, 10, 46, "Now Playing:", TM_DIM, 0x00000000);
        char buf[AM_NAME_MAX];
        tm_copy(buf, st->playlist[st->current_track_idx], AM_NAME_MAX);
        if (tm_strlen(buf) > 38) { buf[35] = '.'; buf[36] = '.'; buf[37] = '.'; buf[38] = 0; }
        wm_draw_string(win, 10, 64, buf, TM_TEXT, 0x00000000);

        char pos_str[24];
        audio_float_to_str(st->position_s, pos_str, 24, 1);
        tm_cat(pos_str, "s", 24);
        if (st->duration_s > 0.5f) {
            char dur_str[24];
            tm_cat(pos_str, " / ", 24);
            audio_float_to_str(st->duration_s, dur_str, 24, 1);
            tm_cat(pos_str, dur_str, 24);
            tm_cat(pos_str, "s", 24);
        } else {
            tm_cat(pos_str, " (dur n/a)", 24);
        }
        wm_draw_string(win, 10, 82, pos_str, TM_DIM, 0x00000000);

        int bar_x = 10, bar_y = 100, bar_w = cw - 20, bar_h = 14;
        wm_fill_rect(win, bar_x, bar_y, bar_w, bar_h, TM_ROW_A);
        wm_fill_rect(win, bar_x, bar_y, bar_w, 1, TM_BORDER);
        wm_fill_rect(win, bar_x, bar_y + bar_h - 1, bar_w, 1, TM_BORDER);
        wm_fill_rect(win, bar_x, bar_y, 1, bar_h, TM_BORDER);
        wm_fill_rect(win, bar_x + bar_w - 1, bar_y, 1, bar_h, TM_BORDER);

        if (st->duration_s > 0.5f) {
            float frac = st->position_s / st->duration_s;
            if (frac > 1.0f) frac = 1.0f;
            int fw = (int)((float)(bar_w - 4) * frac);
            if (fw > 0) wm_fill_rect(win, bar_x + 2, bar_y + 2, fw, bar_h - 4, TM_GREEN);
        } else if (st->is_playing) {
            /* no real duration: honest indeterminate sweep, not a fake bar */
            int segw = (bar_w - 4) / 5;
            st->sweep = (st->sweep + 2) % (bar_w - 4 - segw);
            wm_fill_rect(win, bar_x + 2 + st->sweep, bar_y + 2, segw, bar_h - 4, TM_GREEN_D);
        }
    } else {
        wm_draw_string(win, 10, 46,
                       st->playlist_count == 0 ? "No audio files on disk"
                                               : "Select a track",
                       TM_DIM, 0x00000000);
    }

    int vol_y = 128;
    wm_draw_string(win, 10, vol_y, "Volume:", TM_DIM, 0x00000000);
    int vol_bar_x = 80, vol_bar_w = 120, vol_bar_h = 14;
    wm_fill_rect(win, vol_bar_x, vol_y, vol_bar_w, vol_bar_h, TM_ROW_A);
    wm_fill_rect(win, vol_bar_x, vol_y, vol_bar_w, 1, TM_BORDER);
    wm_fill_rect(win, vol_bar_x, vol_y + vol_bar_h - 1, vol_bar_w, 1, TM_BORDER);
    wm_fill_rect(win, vol_bar_x, vol_y, 1, vol_bar_h, TM_BORDER);
    wm_fill_rect(win, vol_bar_x + vol_bar_w - 1, vol_y, 1, vol_bar_h, TM_BORDER);
    int vol_pos = (int)((float)(vol_bar_w - 4) * st->volume);
    if (vol_pos > 0)
        wm_fill_rect(win, vol_bar_x + 2, vol_y + 2, vol_pos, vol_bar_h - 4, TM_GREEN);
    char vol_buf[8];
    int_to_strn((int)(st->volume * 100), vol_buf, 8);
    wm_draw_string(win, vol_bar_x + vol_bar_w + 8, vol_y, vol_buf, TM_TEXT, 0x00000000);
    wm_draw_string(win, vol_bar_x + vol_bar_w + 30, vol_y, "%", TM_DIM, 0x00000000);

    int btn_y = 158, btn_spacing = 85;
    bool have = st->playlist_count > 0;

    int prev_x = 10;
    wm_fill_rect(win, prev_x, btn_y, 70, 28, have ? TM_ROW_SEL : TM_ROW_A);
    wm_draw_string(win, prev_x + 10, btn_y + 9, "Prev", have ? TM_TEXT : TM_DIM, 0);

    int play_x = prev_x + btn_spacing;
    bool playing = st->is_playing;
    wm_fill_rect(win, play_x, btn_y, 70, 28, playing ? TM_GREEN : TM_ROW_SEL);
    wm_draw_string(win, play_x + 10, btn_y + 9, playing ? "Pause" : "Play",
                   playing ? TM_BLACK : TM_TEXT, 0);

    int next_x = play_x + btn_spacing;
    wm_fill_rect(win, next_x, btn_y, 70, 28, have ? TM_ROW_SEL : TM_ROW_A);
    wm_draw_string(win, next_x + 10, btn_y + 9, "Next", have ? TM_TEXT : TM_DIM, 0);

    int stop_x = next_x + btn_spacing;
    wm_fill_rect(win, stop_x, btn_y, 70, 28, playing ? TM_ROW_SEL : TM_ROW_A);
    wm_draw_string(win, stop_x + 10, btn_y + 9, "Stop", playing ? TM_TEXT : TM_DIM, 0);

    wm_fill_rect(win, 10, 198, cw - 20, 1, TM_BORDER);

    wm_draw_string(win, 10, 208, "Playlist:", TM_DIM, 0x00000000);
    char count_buf[32];
    int_to_strn(st->playlist_count, count_buf, 32);
    wm_draw_string(win, 80, 208, count_buf, TM_TEXT, 0x00000000);
    wm_draw_string(win, 80 + tm_strlen(count_buf) * 8, 208, " tracks", TM_DIM, 0x00000000);

    int list_y = 226, row_h = 20;
    int visible_rows = (ch - list_y - 10) / row_h;
    if (visible_rows < 1) visible_rows = 1;
    int end_idx = visible_rows;
    if (end_idx > st->playlist_count) end_idx = st->playlist_count;

    for (int i = 0; i < end_idx; i++) {
        int ry = list_y + i * row_h;
        if (i == st->selected_idx)
            wm_fill_rect(win, 8, ry - 1, cw - 16, row_h, TM_ROW_SEL);
        if (i == st->current_track_idx && st->is_playing)
            wm_fill_rect(win, 10, ry + 6, 4, 8, TM_GREEN);
        char buf[AM_NAME_MAX];
        tm_copy(buf, st->playlist[i], AM_NAME_MAX);
        if (tm_strlen(buf) > 38) { buf[35] = '.'; buf[36] = '.'; buf[37] = '.'; buf[38] = 0; }
        uint32_t tc = (i == st->current_track_idx) ? TM_GREEN : TM_TEXT;
        wm_draw_string(win, 20, ry, buf, tc, 0x00000000);
    }
    if (st->playlist_count == 0)
        wm_draw_string(win, 20, list_y, "No .wav/.mp3/.ogg files in filesystem",
                       TM_DIM, 0x00000000);

    int mode_y = ch - 28;
    wm_draw_string(win, 10,  mode_y, "[S] Shuffle", st->shuffle_mode ? TM_GREEN : TM_DIM, 0);
    wm_draw_string(win, 120, mode_y, "[R] Repeat",  st->repeat_mode  ? TM_GREEN : TM_DIM, 0);
    wm_draw_string(win, cw - 200, mode_y, "Up/Dn select  Enter play", TM_DIM, 0);
}

static void audio_manager_on_key(Window *win, uint16_t key) {
    AudioManagerState *st = (AudioManagerState *)win->app_data;
    if (!st) return;
    uint8_t scan  = (key >> 8) & 0xFF;
    uint8_t ascii = key & 0xFF;

    if (scan == 0x48) {
        if (st->selected_idx > 0) st->selected_idx--;
    } else if (scan == 0x50) {
        if (st->selected_idx < st->playlist_count - 1) st->selected_idx++;
    } else if (ascii == 13) {
        if (st->is_playing && st->current_track_idx == st->selected_idx) {
            mp3_pause();
            st->user_paused = 1;
        } else {
            am_play_idx(st, st->selected_idx);
        }
    } else if (ascii == ' ') {
        if (st->current_track_idx >= 0) {
            if (st->is_playing) { mp3_pause(); st->user_paused = 1; }
            else                { mp3_resume(); st->user_paused = 0; }
        }
    } else if (ascii == 's' || ascii == 'S') {
        st->shuffle_mode = !st->shuffle_mode;
    } else if (ascii == 'r' || ascii == 'R') {
        st->repeat_mode = !st->repeat_mode;
    } else if (ascii == 27) {
        mp3_stop();
        st->user_paused = 1;
        st->current_track_idx = -1;
    } else if (ascii == '+' || ascii == '=') {
        float v = mp3_get_volume() + 0.1f;
        mp3_set_volume(v > 1.0f ? 1.0f : v);
    } else if (ascii == '-') {
        float v = mp3_get_volume() - 0.1f;
        mp3_set_volume(v < 0.0f ? 0.0f : v);
    } else if (scan == 0x4F && st->playlist_count) {       /* End = next */
        am_play_idx(st, (st->current_track_idx + 1) % st->playlist_count);
    } else if (scan == 0x48 + 1 && st->playlist_count) {   /* unused guard */
    }
}

void app_audio_manager_create(void) {
    mp3_init();
    int sw = wm_get_screen_w();
    int sh = wm_get_screen_h();
    int w = 400, h = 480;
    Window *win = wm_create_window("Audio Manager",
                                   (sw - w) / 2, (sh - WM_TASKBAR_HEIGHT - h) / 2, w, h);
    if (!win) return;

    AudioManagerState *st = (AudioManagerState *)win->app_data;
    win->user_data = st;
    for (uint32_t i = 0; i < (uint32_t)sizeof(AudioManagerState); i++) ((char *)st)[i] = 0;
    st->win               = win;
    st->volume            = mp3_get_volume();
    st->current_track_idx = -1;
    st->rng_state         = get_ticks() | 1u;

    am_scan_tracks(st);

    win->on_draw = audio_manager_on_draw;
    win->on_key  = audio_manager_on_key;
}