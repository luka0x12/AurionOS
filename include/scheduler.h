/* scheduler.h - real process table, threads, PID allocation, CPU/mem accounting.
 *
 * Cooperative accounting model: the WM main loop charges ticks to whichever
 * task it is currently dispatching. No ISR changes, no context switch, safe to
 * drop into the existing single-loop kernel. A preemptive upgrade later only
 * has to feed sched_tick() from the timer and add context switch; the table,
 * PIDs, threads and accounting stay the same.
*/

#ifndef AURION_SCHEDULER_H
#define AURION_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

#define SCHED_MAX_PROCS            64
#define SCHED_MAX_THREADS_PER_PROC 4
#define SCHED_MAX_NAME             32

typedef enum {
    PROC_UNUSED = 0,
    PROC_KERNEL,
    PROC_SHELL,
    PROC_GUI,
    PROC_SERVICE
} ProcKind;

typedef enum {
    TSTATE_FREE = 0,
    TSTATE_READY,
    TSTATE_RUNNING,
    TSTATE_BLOCKED,
    TSTATE_SLEEPING
} ThreadState;

typedef enum {
    PSTATE_FREE = 0,
    PSTATE_READY,
    PSTATE_RUNNING,
    PSTATE_SLEEPING,
    PSTATE_ZOMBIE
} ProcState;

typedef struct {
    int         tid;
    ThreadState state;
    uint32_t    stack_base;
    uint32_t    stack_size;
    uint32_t    cpu_ticks;
} Thread;

typedef struct {
    int       pid;
    ProcKind  kind;
    ProcState state;
    char      name[SCHED_MAX_NAME];
    int       parent_pid;
    int       priority;
    uint32_t  create_tick;
    uint32_t  cpu_ticks;     /* ticks charged to this process */
    uint32_t  mem_bytes;     /* resident bytes attributed to this process */
    Thread    threads[SCHED_MAX_THREADS_PER_PROC];
    int       thread_count;
    void     *owner;         /* GUI procs: the Window*; others NULL */
    bool      used;
} Proc;

/* lifecycle */
void  sched_init(void);
int   sched_create_process(const char *name, ProcKind kind, void *owner);
void  sched_destroy_process(int pid);
void  sched_destroy_by_owner(void *owner);
Proc *sched_get(int pid);
Proc *sched_current(void);
int   sched_count(void);
int   sched_iter(int *cursor);   /* pass 0 cursor; returns next pid or -1 */

/* cpu accounting (cooperative) */
void     sched_set_current(int pid);
void     sched_set_current_by_owner(void *owner);
void     sched_tick(uint32_t elapsed);
void     sched_charge_elapsed(void);   /* charge wall-clock since last call to current */
uint32_t sched_total_ticks(void);
int      sched_cpu_percent(int pid, uint32_t *last_ticks, uint32_t *last_total);

/* memory accounting */
void sched_mem_add(int pid, uint32_t bytes);
void sched_mem_sub(int pid, uint32_t bytes);
int sched_current_pid(void);
uint32_t sched_proc_bytes(int pid);

/* watchdog liveness: 0 RUNNING, 1 WAIT_INPUT, 2 WAIT_DEVICE, 3 SHUTDOWN */
void sched_heartbeat(void);
void sched_progress_wait(int s);
void sched_progress_run(void);
int sched_progress_state(void);
/* Cooperative idle: poke the watchdog epoch then HLT. Sitting in a GUI
 * poll loop is not a hung scheduler; a tight busy-loop with no heartbeat is. */
void sched_idle_halt(void);

/* threads */
int sched_add_thread(int pid, uint32_t stack_base, uint32_t stack_size);

#endif