/* scheduler.c - process table + cooperative CPU/memory accounting.
 * Static table, no kmalloc, no allocation failure paths. PIDs: 0 idle,
 * 1 kernel, 2 shell are created lazily on first use; GUI/service processes
 * get monotonically increasing real PIDs from 100 upward.
 */
#include "scheduler.h"
#include <stdint.h>
#include <stdbool.h>

extern uint32_t get_ticks(void);

static Proc     g_proc[SCHED_MAX_PROCS];
static int      g_current = 1;
static uint32_t g_total_ticks = 0;
static int      g_next_pid = 100;
static bool     g_inited = false;
static volatile uint32_t g_wd_epoch = 0;
static volatile int g_prog_state = 0; /* 0 RUNNING, 1 WAIT_INPUT, 2 WAIT_DEVICE, 3 SHUTDOWN */

static Proc *find_slot_free(void) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) if (!g_proc[i].used) return &g_proc[i];
    return 0;
}
static Proc *find_pid(int pid) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        if (g_proc[i].used && g_proc[i].pid == pid) return &g_proc[i];
    return 0;
}
static void name_copy(char *d, const char *s) {
    int i = 0; while (i < SCHED_MAX_NAME - 1 && s && s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}

static int add_thread_internal(Proc *p, uint32_t base, uint32_t size, ThreadState st) {
    if (p->thread_count >= SCHED_MAX_THREADS_PER_PROC) return -1;
    Thread *t = &p->threads[p->thread_count];
    t->tid = p->pid * 10 + p->thread_count;   /* unique, stable, real */
    t->state = st;
    t->stack_base = base;
    t->stack_size = size;
    t->cpu_ticks = 0;
    p->thread_count++;
    return t->tid;
}

static Proc *create_raw(int pid, const char *name, ProcKind kind, ProcState st, void *owner) {
    Proc *p = find_slot_free();
    if (!p) return 0;
    for (uint32_t i = 0; i < (uint32_t)sizeof(Proc); i++) ((char *)p)[i] = 0;
    p->used = true;
    p->pid = pid;
    p->kind = kind;
    p->state = st;
    name_copy(p->name, name);
    p->parent_pid = (kind == PROC_GUI || kind == PROC_SERVICE) ? 1 : 0;
    p->priority = (kind == PROC_KERNEL) ? 0 : 10;
    p->create_tick = get_ticks();
    p->owner = owner;
    return p;
}

/* Lazy init so the table works even if no explicit boot call is wired yet. */
static void ensure_init(void) {
    if (g_inited) return;
    g_inited = true;
    g_total_ticks = 0; g_next_pid = 100; g_current = 1;
    Proc *idle = create_raw(0, "idle", PROC_KERNEL, PSTATE_SLEEPING, 0);
    if (idle) add_thread_internal(idle, 0, 0, TSTATE_SLEEPING);
    Proc *k = create_raw(1, "kernel", PROC_KERNEL, PSTATE_RUNNING, 0);
    if (k) {
        add_thread_internal(k, 0x00100000, 16384, TSTATE_RUNNING);  /* main */
        add_thread_internal(k, 0x00104000, 8192, TSTATE_READY);     /* irq bottom half */
    }
    Proc *sh = create_raw(2, "shell", PROC_SHELL, PSTATE_READY, 0);
    if (sh) add_thread_internal(sh, 0x00110000, 8192, TSTATE_READY);
}

void sched_init(void) { ensure_init(); }

int sched_create_process(const char *name, ProcKind kind, void *owner) {
    ensure_init();
    int pid = g_next_pid++;
    if (g_next_pid > 30000) g_next_pid = 100;
    Proc *p = create_raw(pid, name, kind, PSTATE_READY, owner);
    if (!p) return -1;
    add_thread_internal(p, 0, 0, TSTATE_READY);
    return pid;
}

void sched_destroy_process(int pid) {
    Proc *p = find_pid(pid);
    if (!p) return;
    if (p->pid == 0 || p->pid == 1 || p->pid == 2) return;   /* never kill core */
    p->used = false;
    if (g_current == pid) g_current = 1;
}

void sched_destroy_by_owner(void *owner) {
    if (!owner) return;
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        if (g_proc[i].used && g_proc[i].owner == owner) { sched_destroy_process(g_proc[i].pid); return; }
}

Proc *sched_get(int pid)     { ensure_init(); return find_pid(pid); }
Proc *sched_current(void)    { ensure_init(); return find_pid(g_current); }
int   sched_count(void)      { ensure_init(); int n = 0; for (int i = 0; i < SCHED_MAX_PROCS; i++) if (g_proc[i].used) n++; return n; }

/* Owner tag for the heap allocator: every kmalloc is charged to whoever
 * runs right now. No heap use in here (static table only), so this is
 * safe to call from inside kmalloc itself, even during early boot. */
int sched_current_pid(void) {
    ensure_init();
    Proc *p = find_pid(g_current);
    return p ? p->pid : 1;
}

extern uint32_t mem_proc_bytes(int pid);

/* Real per-process heap bytes: live heap walk, not estimates. */
uint32_t sched_proc_bytes(int pid) { return mem_proc_bytes(pid); }

int sched_iter(int *cursor) {
    ensure_init();
    while (*cursor < SCHED_MAX_PROCS) { int i = (*cursor)++; if (g_proc[i].used) return g_proc[i].pid; }
    return -1;
}

void sched_set_current(int pid) { if (find_pid(pid)) g_current = pid; }
void sched_set_current_by_owner(void *owner) {
    if (!owner) return;
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        if (g_proc[i].used && g_proc[i].owner == owner) { g_current = g_proc[i].pid; return; }
}

void sched_tick(uint32_t elapsed) {
    ensure_init();
    g_total_ticks += elapsed;
    g_wd_epoch += elapsed;
    Proc *p = find_pid(g_current);
    if (p) {
        p->cpu_ticks += elapsed;
        if (p->thread_count > 0) p->threads[0].cpu_ticks += elapsed;
    }
}

/* One shared charge state for all WM dispatch sites, so every wall-clock
 * tick is attributed to exactly one process and none are double-counted. */
static uint32_t g_last_charge = 0;
static bool     g_charge_valid = false;

void sched_charge_elapsed(void) {
    ensure_init();
    uint32_t now = get_ticks();
    if (g_charge_valid) sched_tick(now - g_last_charge);
    g_last_charge = now;
    g_charge_valid = true;
}

uint32_t sched_total_ticks(void) { return g_total_ticks; }

/* Watchdog epoch: advances on charged ticks AND explicit heartbeats, so
 * provably-alive loops (timed sleeps, DHCP polls) move it while a truly
 * hogged scheduler freezes it. g_total_ticks is accounting (CPU%); this
 * one is liveness only. See kpanic_watchdog. */
uint32_t sched_epoch(void) { ensure_init(); return g_wd_epoch; }

void sched_heartbeat(void) { ensure_init(); g_wd_epoch++; }

void sched_idle_halt(void) {
    ensure_init();
    g_wd_epoch++;
    __asm__ volatile("sti\nhlt");
}

void sched_progress_wait(int s) {
    ensure_init();
    if (s >= 0 && s <= 3) g_prog_state = s;
}

void sched_progress_run(void) { ensure_init(); g_prog_state = 0; }

int sched_progress_state(void) { ensure_init(); return g_prog_state; }

/* Arming hook for kpanic_watchdog (weak default: dormant). Armed exactly
 * when the system claims to be making progress: legitimate waits opt out
 * via sched_progress_wait, everything else must move the epoch. */
bool sched_preempt_active(void) { ensure_init(); return g_prog_state == 0; }

void sched_mem_add(int pid, uint32_t bytes) { Proc *p = find_pid(pid); if (p) p->mem_bytes += bytes; }
void sched_mem_sub(int pid, uint32_t bytes) {
    Proc *p = find_pid(pid);
    if (p) { if (bytes > p->mem_bytes) p->mem_bytes = 0; else p->mem_bytes -= bytes; }
}

int sched_add_thread(int pid, uint32_t base, uint32_t size) {
    Proc *p = find_pid(pid);
    if (!p) return -1;
    return add_thread_internal(p, base, size, TSTATE_READY);
}

/* Returns 0..100, or -1 when no time has elapsed yet (caller shows n/a). */
int sched_cpu_percent(int pid, uint32_t *last_ticks, uint32_t *last_total) {
    Proc *p = find_pid(pid);
    if (!p) return -1;
    uint32_t total = g_total_ticks;
    uint32_t dt = total - *last_total;
    if (dt == 0) return -1;
    uint32_t dp = p->cpu_ticks - *last_ticks;
    uint32_t pct = (uint32_t)((uint64_t)dp * 100u / (uint64_t)dt);
    if (pct > 100) pct = 100;
    *last_ticks = p->cpu_ticks;
    *last_total = total;
    return (int)pct;
}