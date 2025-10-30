#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Req 3: MLFQS용 3단계 준비 큐 */
static struct list ready_queues[3]; /* Q0, Q1, Q2 */

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List of process in sleep */
static struct list sleep_list;
static int64_t next_tick_to_wakeup = INT64_MAX;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
    void *eip;             /* Return address. */
    thread_func *function; /* Function to call. */
    void *aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs = false;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void check_preemption (void); // 선점 확인하고 필요시 yield 하는 헬퍼함수

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
/* threads/thread.c */

void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    list_init (&ready_list);
    list_init (&all_list);
    list_init (&sleep_list);

    /* --- Project 1: Scheduling --- */
    /* Req 3: MLFQS 큐 초기화 */
    if (thread_mlfqs)
    {
        list_init (&ready_queues[0]);
        list_init (&ready_queues[1]);
        list_init (&ready_queues[2]);
    }
    /* --- End Project 1 --- */

    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
    /* Create the idle thread. */
    struct semaphore idle_started;
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started);

    /* Start preemptive thread scheduling. */
    intr_enable ();

    /* Wait for the idle thread to initialize idle_thread. */
    sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
/* threads/thread.c */

void
thread_tick (void)
{
    struct thread *t = thread_current ();

    /* Update statistics. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* --- Project 1: Scheduling --- */
    if (thread_mlfqs)
    {
        /* --- Req 3: MLFQS 로직 --- */
        
        /* 1. Ready 큐 스레드들 에이징 (승급) */
        /* Q1, Q2만 승급 대상 */
        for (int i = 1; i <= 2; i++)
        {
            struct list_elem *e = list_begin (&ready_queues[i]);
            while (e != list_end (&ready_queues[i]))
            {
                struct thread *rt = list_entry (e, struct thread, elem);
                struct list_elem *next_e = list_next(e);
                rt->age++;
                if (rt->age >= 20)
                {
                    rt->age = 0;
                    rt->queue_level--; /* 승급 */
                    list_remove (e);
                    list_push_back (&ready_queues[rt->queue_level], &rt->elem);
                }
                e = next_e;
            }
        }

        /* 2. 현재 실행 중인 스레드 타임 슬라이스 (강등) */
        if (t != idle_thread)
        {
            t->time_slice_remaining--;
            if (t->time_slice_remaining <= 0)
            {
                t->age = 0;
                if (t->queue_level < 2)
                    t->queue_level++; /* 강등 */
                
                /* 새 큐의 타임 슬라이스 설정 */
                if (t->queue_level == 0) t->time_slice_remaining = 2;
                else if (t->queue_level == 1) t->time_slice_remaining = 4;
                else t->time_slice_remaining = 8;
                
                intr_yield_on_return (); /* 타임 슬라이스 소진 시 yield */
            }
        }
    }
    else
    {
        /* --- Req 2: 우선순위 + 에이징 로직 --- */
        
        /* 1. Ready 큐 스레드들 에이징 (승급) */
        struct list_elem *e = list_begin (&ready_list);
        while (e != list_end (&ready_list))
        {
            struct thread *rt = list_entry (e, struct thread, elem);
            struct list_elem *next_e = list_next(e);
            rt->age++;
            if (rt->age >= 20 && rt->priority < PRI_DEFAULT)
            {
                rt->age = 0;
                rt->priority++;
                /* 우선순위가 변경되었으므로 리스트에서 제거 후 재삽입 */
                list_remove (e);
                list_insert_ordered (&ready_list, &rt->elem, thread_priority_cmp, NULL);
            }
            e = next_e;
        }

        /* 2. 표준 Round-Robin 타임 슬라이스 */
        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return ();
    }

    /* 3. 에이징/승급으로 인해 선점 조건이 만족될 수 있음 */
    check_preemption ();
    /* --- End Project 1 --- */
}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;
    enum intr_level old_level;

    ASSERT (function != NULL);

    /* Allocate thread. */
    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
    old_level = intr_disable ();

    /* Stack frame for kernel_thread(). */
    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    /* Stack frame for switch_entry(). */
    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void))kernel_thread;

    /* Stack frame for switch_threads(). */
    sf = alloc_frame (t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    intr_set_level (old_level);

    /* Add to run queue. */
    thread_unblock (t);
    
    return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
    ASSERT (!intr_context ());
    ASSERT (intr_get_level () == INTR_OFF);

    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
/* threads/thread.c */

void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;

    ASSERT (is_thread (t));

    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED);

    /* --- Project 1: Scheduling --- */
    t->age = 0; /* Req 2 & 3: Ready 큐에 들어갈 때 age 초기화 */

    if (thread_mlfqs)
    {
        /* Req 3: MLFQS 큐의 뒤에 삽입 (FIFO) */
        list_push_back (&ready_queues[t->queue_level], &t->elem);
    }
    else
    {
        /* Req 1: 우선순위 큐에 정렬 삽입 */
        list_insert_ordered (&ready_list, &t->elem, thread_priority_cmp, NULL);
    }
    /* --- End Project 1 --- */

    t->status = THREAD_READY;

    /* --- Project 1: Scheduling --- */
    /* Req 1 & 3: 
       Unblock된 스레드가 현재 스레드보다 우선순위 높으면 선점.
       (단, 인터럽트 컨텍스트에서는 yield 불가) */
    if (!intr_context())
        check_preemption ();
    /* --- End Project 1 --- */
    
    intr_set_level (old_level);
}

static void
update_next_tick_to_wakeup (int64_t tick)
{
    next_tick_to_wakeup = 
        (next_tick_to_wakeup > tick) ? tick : next_tick_to_wakeup;
}

int64_t
get_next_tick_to_wakeup (void)
{
    return next_tick_to_wakeup;
}

/* Wakes up this thread after ticks */
void
thread_sleep (int64_t tick)
{
    struct thread *cur;
    enum intr_level old_level;

    old_level = intr_disable ();
    cur = thread_current ();

    ASSERT (cur != idle_thread);

    update_next_tick_to_wakeup (cur->wakeup_tick = tick);
    list_push_back (&sleep_list, &cur->elem);

    thread_block ();

    intr_set_level (old_level);
}

void
thread_wakeup (int64_t current_tick)
{
    struct list_elem *e;

    next_tick_to_wakeup = INT64_MAX;

    e = list_begin (&sleep_list);
    while (e != list_end (&sleep_list))
    {
        struct thread *t = list_entry (e, struct thread, elem);
        if (current_tick >= t->wakeup_tick)
        {
            e = list_remove (&t->elem);
            thread_unblock (t);
        }
        else
        {
            e = list_next (e);
            update_next_tick_to_wakeup (t->wakeup_tick);
        }
    }
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
    return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
    struct thread *t = running_thread ();

    /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
    ASSERT (is_thread (t));
    ASSERT (t->status == THREAD_RUNNING);

    return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
    return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
    ASSERT (!intr_context ());

#ifdef USERPROG
    process_exit ();
#endif

    /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
    intr_disable ();
    list_remove (&thread_current ()->allelem);
    thread_current ()->status = THREAD_DYING;
    schedule ();
    NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
/* threads/thread.c */

void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (cur != idle_thread)
    {
        /* --- Project 1: Scheduling --- */
        cur->age = 0; /* Req 2 & 3: Ready 큐에 들어가므로 age 초기화 */
        if (thread_mlfqs)
        {
            /* Req 3: MLFQS 큐의 뒤에 삽입 (FIFO) */
            list_push_back (&ready_queues[cur->queue_level], &cur->elem);
        }
        else
        {
            /* Req 1: 우선순위 큐에 정렬 삽입 */
            list_insert_ordered (&ready_list, &cur->elem, thread_priority_cmp, NULL);
        }
        /* --- End Project 1 --- */
    }
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
    struct list_elem *e;

    ASSERT (intr_get_level () == INTR_OFF);

    for (e = list_begin (&all_list); e != list_end (&all_list);
         e = list_next (e))
        {
            struct thread *t = list_entry (e, struct thread, allelem);
            func (t, aux);
        }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
/* threads/thread.c */

void
thread_set_priority (int new_priority)
{
    /* --- Project 1: Scheduling --- */
    if (thread_mlfqs)
        return; /* Req 3: MLFQS는 우선순위 수동 변경 무시 */

    thread_current ()->priority = new_priority;

    /* Req 1: 우선순위를 낮췄을 때, 대기 중인 스레드가 더 높으면 선점 */
    check_preemption ();
    /* --- End Project 1 --- */
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
    return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED)
{
    /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void)
{
    /* Not yet implemented. */
    return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void)
{
    /* Not yet implemented. */
    return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void)
{
    /* Not yet implemented. */
    return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED)
{
    struct semaphore *idle_started = idle_started_;
    idle_thread = thread_current ();
    sema_up (idle_started);

    for (;;)
        {
            /* Let someone else run. */
            intr_disable ();
            thread_block ();

            /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
            asm volatile ("sti; hlt" : : : "memory");
        }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux)
{
    ASSERT (function != NULL);

    intr_enable (); /* The scheduler runs with interrupts off. */
    function (aux); /* Execute the thread function. */
    thread_exit (); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
    uint32_t *esp;

    /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
    asm ("mov %%esp, %0" : "=g"(esp));
    return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
/* threads/thread.c */

static void
init_thread (struct thread *t, const char *name, int priority)
{
    /* ... (memset, strlcpy, stack) ... */
    t->status = THREAD_BLOCKED;
    strlcpy (t->name, name, sizeof t->name);
    t->stack = (uint8_t *)t + PGSIZE;
    t->magic = THREAD_MAGIC;

    /* --- Project 1: Scheduling --- */
    t->age = 0; /* Req 2 & 3: Age는 0에서 시작 */

    if (thread_mlfqs)
    {
        /* Req 3: MLFQS 스레드는 Q0에서 시작, 타임 슬라이스 2 */
        t->priority = PRI_DEFAULT; /* MLFQS에서는 priority 무시 */
        t->queue_level = 0;
        t->time_slice_remaining = 2; 
    }
    else
    {
        /* Req 1: 우선순위 스케줄링 */
        t->priority = priority;
    }
    /* --- End Project 1 --- */

    list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size)
{
    /* Stack data is always allocated in word-size units. */
    ASSERT (is_thread (t));
    ASSERT (size % sizeof (uint32_t) == 0);

    t->stack -= size;
    return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
/* threads/thread.c */

static struct thread *
next_thread_to_run (void)
{
    /* --- Project 1: Scheduling --- */
    if (thread_mlfqs)
    {
        /* Req 3: Q0 -> Q1 -> Q2 순서로 탐색 */
        if (!list_empty (&ready_queues[0]))
            return list_entry (list_pop_front (&ready_queues[0]), struct thread, elem);
        if (!list_empty (&ready_queues[1]))
            return list_entry (list_pop_front (&ready_queues[1]), struct thread, elem);
        if (!list_empty (&ready_queues[2]))
            return list_entry (list_pop_front (&ready_queues[2]), struct thread, elem);
    }
    else
    {
        /* Req 1: 우선순위 큐의 맨 앞 (가장 높은 우선순위) */
        if (!list_empty (&ready_list))
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
    }
    /* --- End Project 1 --- */

    return idle_thread;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread ();

    ASSERT (intr_get_level () == INTR_OFF);

    /* Mark us as running. */
    cur->status = THREAD_RUNNING;

    /* Start new time slice. */
    thread_ticks = 0;

#ifdef USERPROG
    /* Activate the new address space. */
    process_activate ();
#endif

    /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
        {
            ASSERT (prev != cur);
            palloc_free_page (prev);
        }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void)
{
    struct thread *cur = running_thread ();
    struct thread *next = next_thread_to_run ();
    struct thread *prev = NULL;

    ASSERT (intr_get_level () == INTR_OFF);
    ASSERT (cur->status != THREAD_RUNNING);
    ASSERT (is_thread (next));

    if (cur != next)
        prev = switch_threads (cur, next);
    thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void)
{
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire (&tid_lock);
    tid = next_tid++;
    lock_release (&tid_lock);

    return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

bool
thread_priority_cmp (const struct list_elem *a,
                     const struct list_elem *b,
                     void *aux UNUSED)
{
    struct thread *ta = list_entry (a, struct thread, elem);
    struct thread *tb = list_entry (b, struct thread, elem);
    
    return ta->priority > tb->priority;
}

/* Req 1 & 3: Ready 큐에서 가장 우선순위 높은 스레드를 반환 (제거 안 함) */
static struct thread *
get_highest_priority_ready_thread (void)
{
    if (thread_mlfqs)
    {
        if (!list_empty(&ready_queues[0]))
            return list_entry(list_front(&ready_queues[0]), struct thread, elem);
        if (!list_empty(&ready_queues[1]))
            return list_entry(list_front(&ready_queues[1]), struct thread, elem);
        if (!list_empty(&ready_queues[2]))
            return list_entry(list_front(&ready_queues[2]), struct thread, elem);
    }
    else
    {
        if (!list_empty(&ready_list))
            return list_entry(list_front(&ready_list), struct thread, elem);
    }
    return NULL;
}

/* Req 1 & 3: 선점(Preemption) 확인 */
static void
check_preemption (void)
{
    /* 인터럽트 핸들러 내에서는 yield 불가 */
    if (intr_context()) 
        return;

    struct thread *cur = thread_current();
    struct thread *highest_ready = get_highest_priority_ready_thread();

    if (highest_ready == NULL)
        return; /* Ready 큐가 비어있음 */

    if (thread_mlfqs)
    {
        /* Req 3: MLFQS 선점 (상위 큐가 하위 큐 선점) */
        if (highest_ready->queue_level < cur->queue_level)
            thread_yield ();
    }
    else
    {
        /* Req 1: 우선순위 선점 */
        if (highest_ready->priority > cur->priority)
            thread_yield ();
    }
}