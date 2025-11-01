/* threads/thread.c */

/* 필요한 헤더 파일들을 포함합니다. */
#include "threads/thread.h"   // 스레드 자료구조 및 함수 프로토타입
#include <debug.h>            // ASSERT, NOT_REACHED 등 디버깅 매크로
#include <stddef.h>           // NULL, offsetof 등 표준 정의
#include <random.h>           // 난수 생성 (사용되지는 않음)
#include <stdio.h>            // printf 등 표준 입출력 함수
#include <string.h>           // strlcpy 등 문자열 처리 함수
#include "threads/flags.h"     // EFLAGS 레지스터 관련 플래그
#include "threads/interrupt.h" // 인터럽트 처리 함수 (intr_enable, intr_disable)
#include "threads/intr-stubs.h"// 인터럽트 스텁
#include "threads/palloc.h"    // 페이지 할당자 (palloc_get_page)
#include "threads/switch.h"    // 컨텍스트 스위칭 함수 (switch_threads)
#include "threads/synch.h"     // 동기화 객체 (lock, semaphore)
#include "threads/vaddr.h"     // 가상 주소 관련 함수 (pg_round_down)
#include "devices/timer.h"     // 타이머 관련 (timer_ticks) - sleep/wakeup에 필요



/* 스레드 스택 오버플로우 감지를 위한 매직 넘버 (thread.h 참조) */
#define THREAD_MAGIC 0xcd6abf4b

/* * ready_list: 실행 준비가 되었지만(READY 상태) CPU를 점유하지 않은 스레드들의 리스트.
 * Priority 스케줄러 (thread_mlfqs == false) 일 때만 사용됩니다.
 * 이 리스트는 항상 우선순위가 높은 스레드가 앞에 오도록 정렬됩니다.
 */
static struct list ready_list;

/* * [FIX 2] ready_queue: MLFQS 스케줄러용 3단계(Q0, Q1, Q2) 큐 배열.
 * thread_mlfqs == true 일 때만 사용됩니다.
 * 각 큐는 FIFO(First-In, First-Out)로 동작합니다.
 */
static struct list ready_queue[3];

/* * all_list: 시스템 내의 모든 스레드(RUNNING, READY, BLOCKED)를 포함하는 리스트.
 * 스레드 생성 시 추가되고, 종료 시 제거됩니다. (thread_foreach에서 사용)
 */
static struct list all_list;

/* * sleep_list: thread_sleep()에 의해 잠들어 있는(BLOCKED) 스레드들의 리스트.
 * 깨어날 시간이 되면 thread_wakeup()에 의해 ready 상태가 됩니다.
 */
static struct list sleep_list;

/* * next_tick_to_wakeup: sleep_list에 있는 스레드 중 가장 빨리 깨어날 시간(tick).
 * 불필요한 타이머 인터럽트 처리를 줄이기 위해 사용됩니다. (최적화)
 */
static int64_t next_tick_to_wakeup = INT64_MAX; // 가장 큰 값으로 초기화

/* idle_thread: 실행할 스레드가 아무것도 없을 때 실행되는 스레드. */
static struct thread *idle_thread;

/* initial_thread: Pintos 커널이 시작될 때(init.c:main()) 실행되는 첫 번째 스레드. */
static struct thread *initial_thread;

/* tid_lock: 스레드 ID(tid)를 할당할 때(allocate_tid) 발생하는 경쟁 상태를 방지하기 위한 락. */
static struct lock tid_lock;

/* * kernel_thread_frame: 커널 스레드가 처음 생성될 때 스택에 쌓이는 프레임 구조체.
 * kernel_thread 함수를 실행하기 위해 사용됩니다.
 */
struct kernel_thread_frame
{
    void *eip;             /* 반환 주소 (실제로는 switch_entry에서 사용) */
    thread_func *function; /* 스레드가 실행할 함수 */
    void *aux;             /* 함수에 전달할 인자 */
};

/* 통계 정보 */
static long long idle_ticks;   /* idle 스레드가 실행된 틱 수 */
static long long kernel_ticks; /* 커널 스레드가 실행된 틱 수 */
static long long user_ticks;   /* (USERPROG) 유저 프로그램이 실행된 틱 수 (이 코드에서는 kernel_ticks로 통합됨) */

/* 스케줄링 관련 */
#define TIME_SLICE 4          /* 기본 타임 슬라이스 (Priority 스케줄러의 Round-Robin용) */
static unsigned thread_ticks; /* 마지막 스케줄링 이후 경과한 틱 수 */

/* * thread_mlfqs: MLFQS 스케줄러 사용 여부를 결정하는 플래그.
 * 커널 부팅 시 "-o mlfqs" 옵션으로 true가 됩니다.
 * false이면 기본 Priority 스케줄러 + Aging이 동작합니다.
 */
bool thread_mlfqs;

/* 내부(static) 함수 프로토타입 선언 */
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

/*
 * [Priority 스케줄러용]
 * 스레드 우선순위 비교 함수 (list_insert_ordered 용)
 * priority가 높은 스레드(숫자가 큰)가 리스트의 앞쪽에 오도록 합니다.
 * (a->priority > b->priority)일 때 true를 반환합니다.
 */
bool
thread_priority_compare_func (const struct list_elem *a,
                           const struct list_elem *b,
                           void *aux UNUSED)
{
    /* list_elem을 포함하는 struct thread 포인터를 가져옵니다. */
    struct thread *thread_a = list_entry (a, struct thread, elem);
    struct thread *thread_b = list_entry (b, struct thread, elem);

    /* a의 우선순위가 b보다 높으면 true (리스트의 앞쪽에 위치) */
    return thread_a->priority > thread_b->priority;
}

/* * thread_init: 스레딩 시스템을 초기화합니다.
 * 현재 실행 중인 코드를 'main' 스레드로 변환하고,
 * 각종 리스트와 락을 초기화합니다.
 */
void
thread_init (void)
{
    /* 인터럽트가 꺼진 상태에서만 호출되어야 함을 확인 */
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);        // TID 할당용 락 초기화
    list_init (&ready_list);      // (Priority용) ready_list 초기화
    list_init (&all_list);        // all_list 초기화
    list_init (&sleep_list);      // sleep_list 초기화
    
    /* [FIX 3] MLFQS 큐 3개 초기화 */
    if (thread_mlfqs) // MLFQS 모드일 경우에만
    {
        list_init (&ready_queue[0]); // Q0 초기화
        list_init (&ready_queue[1]); // Q1 초기화
        list_init (&ready_queue[2]); // Q2 초기화
    }
    
    /* 현재 실행 중인 코드를 'initial_thread'로 설정 */
    initial_thread = running_thread (); // 현재 스택 포인터를 기반으로 스레드 구조체 주소 계산
    init_thread (initial_thread, "main", PRI_DEFAULT); // 'main' 스레드로 초기화 (기본 우선순위)
    initial_thread->status = THREAD_RUNNING; // 상태를 RUNNING으로 설정
    initial_thread->tid = allocate_tid ();     // TID 할당 (1)
}

/* * thread_start: 선점형 스케줄링을 시작합니다.
 * 'idle' 스레드를 생성하고 인터럽트를 활성화합니다.
 */
void
thread_start (void)
{
    /* 'idle' 스레드 생성 (가장 낮은 우선순위) */
    struct semaphore idle_started; // idle 스레드 초기화 완료 동기화용 세마포어
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started); // idle 스레드 생성

    /* 인터럽트 활성화 (이제 타이머 인터럽트가 발생하여 스케줄링이 시작됨) */
    intr_enable ();

    /* * idle 스레드가 초기화(idle_thread 변수 설정)를 완료하고 
     * sema_up을 호출할 때까지 대기합니다.
     */
    sema_down (&idle_started);
}

/* * thread_tick: 매 타이머 틱마다 호출되는 인터럽트 핸들러.
 * 스레드 통계 업데이트, 스케줄링 로직(Aging, Demotion, Preemption)을 수행합니다.
 */
void
thread_tick (void)
{
    struct thread *t = thread_current (); // 현재 실행 중인 스레드

    /* 통계 업데이트 */
    if (t == idle_thread) // idle 스레드 실행 중이면
        idle_ticks++;       // idle 틱 증가
    else // 아니면 (커널/유저 스레드)
        kernel_ticks++;     // 커널 틱 증가 (이 프로젝트에서는 유저 틱을 구분하지 않음)

    if (thread_mlfqs) // MLFQS 스케줄러 모드일 경우
    {
        /* --- MLFQS Logic --- */
        
        /* 현재 스레드(idle이 아닐 경우)의 현재 타임 슬라이스 사용량 1 증가 */
        if (t != idle_thread)
            t->time_in_slice++;

        /* 1. 승급 (Promotion / Aging) */
        
        /* 1-1. Sleep List 순회 (잠자는 스레드도 age 증가) */
        struct list_elem *e = list_begin (&sleep_list);
        while (e != list_end (&sleep_list)) // sleep List 를 돌면서, aging 승급처리
        {
            struct thread *s_t = list_entry (e, struct thread, elem);
            s_t->age++; // age 1 증가
            if (s_t->age >= 20) // age가 20이 되면
            {
                s_t->age = 0; // age 리셋
                if (s_t->q_level > 0) // Q0이 아니면
                    s_t->q_level--; /* 깨어날 때 승급된 레벨(큐)로 들어감 */
            }
            e = list_next(e);
        }

        /* 1-2. Ready Queues 순회 (Q1, Q2만. Q0는 승급 대상 아님) */
        for (int i = 1; i <= 2; i++) // Q1(i=1), Q2(i=2) 레디큐를 돌면서 aging, 승급처리
        {
            e = list_begin (&ready_queue[i]);
            while (e != list_end (&ready_queue[i]))
            {
                struct thread *r_t = list_entry (e, struct thread, elem);
                r_t->age++; // age 1 증가

                if (r_t->age >= 20) // age가 20이 되면
                {
                    /* * 리스트 순회 중 요소를 제거해야 하므로, 
                     * 다음 요소를 미리 저장 (iterator invalidation 방지) 
                     */
                    struct list_elem *next_e = list_next(e);                   
                    
                    r_t->age = 0;            // age 리셋
                    r_t->q_level--;      // 큐 레벨 승급 (e.g., Q2 -> Q1)
                    
                    /* 승급 시 타임 슬라이스 초기화 (요구사항 기반 가정) */
                    r_t->time_in_slice = 0; 
                    
                    list_remove(&r_t->elem); // 현재 큐(Q1 또는 Q2)에서 제거
                    // 승급된 큐(Q0 또는 Q1)의 맨 뒤에 추가 (FIFO)
                    list_push_back(&ready_queue[r_t->q_level], &r_t->elem); 
                    
                    e = next_e; // 미리 저장해둔 다음 요소로 이동
                }
                else
                {
                    e = list_next (e); // 다음 요소로 이동
                }
            }
        }
        
        /* 2. 강등 (Demotion): 현재 스레드가 타임 슬라이스를 모두 소모했는지 확인 */
        int slice_limit = 0;
        // 큐 레벨에 따라 타임 슬라이스 제한 설정
        if (t->q_level == 0) slice_limit = 2;      // Q0 = 2 틱
        else if (t->q_level == 1) slice_limit = 4; // Q1 = 4 틱
        else if (t->q_level == 2) slice_limit = 8; // Q2 = 8 틱

        // idle 스레드가 아니고, 타임 슬라이스를 모두 소모했다면
        if (t != idle_thread && t->time_in_slice >= slice_limit)
        {
            t->time_in_slice = 0; /* 슬라이스 카운터 리셋 */
            t->age = 0;            /* 강등 시 age 리셋 */
            if (t->q_level < 2) // Q2가 아니라면
                t->q_level++;   // 하위 큐로 강등 (e.g., Q0 -> Q1)
            
            /* * CPU 양보 요청. (인터럽트 핸들러 종료 시 스케줄링 발생)
             * 강등되었으므로, 같은 큐의 다른 스레드나 하위 큐의 스레드에게 기회를 줌.
             */
            intr_yield_on_return(); 
        }

        /* 3. 선점 (Preemption): 더 높은 우선순위 큐에 스레드가 있는지 확인 */
        if (t != idle_thread) // idle 스레드가 아닐 때만
        {
            bool yield_for_preempt = false;
            
            /* 현재 Q1에서 실행 중인데 Q0에 스레드가 있다면 */
            if (t->q_level == 1 && !list_empty(&ready_queue[0]))
            {
                yield_for_preempt = true; // 선점 양보
            }
            /* 현재 Q2에서 실행 중인데 Q0 또는 Q1에 스레드가 있다면 */
            else if (t->q_level == 2)
            {
                if (!list_empty(&ready_queue[0]) || !list_empty(&ready_queue[1]))
                   yield_for_preempt = true; // 선점 양보
            }
            
            if (yield_for_preempt) {
                 intr_yield_on_return(); /* CPU 양보 요청 (상위 큐 스레드 실행) */
            }
        }
    }
    else /* --- Priority + Aging Logic --- */ // Priority 스케줄러 모드일 경우
    {
        /* ready_list에 있는 모든 스레드의 age 증가 및 승급(우선순위 상승) 처리 */
        struct list_elem *e = list_begin (&ready_list);
        while (e != list_end (&ready_list))
        {
            struct thread *ready_t = list_entry (e, struct thread, elem);
            ready_t->age++; // age 1 증가

            if (ready_t->age >= 20) // age가 20이 되면
            {
                // 우선순위가 최대치가 아니면 1 증가
                if (ready_t->priority < PRI_MAX)
                {
                    ready_t->priority++;
                }
                ready_t->age = 0; // age 리셋
                
                /*
                 * 우선순위가 변경되었으므로 리스트 정렬을 다시 해야 함.
                 * 순회 중 안전한 제거/삽입을 위해 다음 요소를 미리 저장.
                 */
                struct list_elem *next_e = list_next(e);
                list_remove(&ready_t->elem); // 리스트에서 제거
                // 정렬된 위치에 다시 삽입
                list_insert_ordered(&ready_list, &ready_t->elem, thread_priority_compare_func, NULL);
                e = next_e; // 다음 요소로 이동
            }
            else
            {
                e = list_next (e); // 다음 요소로 이동
            }
        }

        /* * Priority 스케줄러에서도 Round-Robin을 위해 TIME_SLICE를 적용.
         * (요구사항에는 없지만, Pintos 기본 동작)
         */
        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return (); // 타임 슬라이스 소모 시 CPU 양보
            
    } 
}



/* 스레드 통계 출력 함수 */
void
thread_print_stats (void)
{
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

/* * thread_create: 새 커널 스레드를 생성합니다.
 * NAME: 스레드 이름, PRIORITY: (Priority 스케줄러용) 초기 우선순위
 * FUNCTION: 실행할 함수, AUX: 함수 인자
 * 생성된 스레드의 TID를 반환, 실패 시 TID_ERROR
 */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
    struct thread *t; // 새로 생성할 스레드 구조체 포인터
    struct kernel_thread_frame *kf; // 커널 스레드 프레임
    struct switch_entry_frame *ef;  // switch_entry 용 프레임
    struct switch_threads_frame *sf;// switch_threads 용 프레임
    tid_t tid; // 반환할 스레드 ID
    enum intr_level old_level; // 인터럽트 이전 상태 저장

    ASSERT (function != NULL); // 실행할 함수는 NULL일 수 없음

    /* 1. 스레드 구조체를 위한 메모리 할당 (1 페이지) */
    t = palloc_get_page (PAL_ZERO); // 페이지 할당 및 0으로 초기화
    if (t == NULL)
        return TID_ERROR; // 할당 실패

    /* 2. 스레드 초기화 */
    init_thread (t, name, priority); // 스레드 기본 정보 설정
    tid = t->tid = allocate_tid ();    // TID 할당

    /* * 3. 스레드 스택 설정 (첫 실행을 위해)
     * 인터럽트를 비활성화하여 스택 설정 중 스케줄링 방지
     */
    old_level = intr_disable ();

    /* kernel_thread 함수용 스택 프레임 설정 */
    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL; // 반환 주소 (사용되지 않음)
    kf->function = function; // 실행할 함수
    kf->aux = aux;           // 인자

    /* switch_entry 함수용 스택 프레임 설정 */
    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void))kernel_thread; // 스레드가 처음 실행될 때 kernel_thread 함수 호출

    /* switch_threads 함수용 스택 프레임 설정 (컨텍스트 스위칭 진입점) */
    sf = alloc_frame (t, sizeof *sf);
    sf->eip = switch_entry; // 스케줄링 시 switch_entry 부터 실행
    sf->ebp = 0; // 베이스 포인터 초기화

    intr_set_level (old_level); // 이전 인터럽트 상태 복원

    /* 4. 스레드를 ready 큐에 추가 (실행 가능 상태로 만듦) */
    thread_unblock (t);
    
    /* * [FIX 1] 생성 직후 선점 로직 (Priority/MLFQS 공통)
     * 새 스레드(t)가 현재 스레드(thread_current())보다 우선순위가 높으면
     * 즉시 CPU를 양보(yield)해야 합니다.
     */
    bool preempt = false; // 선점 여부 플래그
    if (thread_mlfqs)
    {
        /* MLFQS: 새 스레드의 큐 레벨이 더 높으면 (숫자가 낮으면) 선점 */
        if (t->q_level < thread_current()->q_level)
            preempt = true;
    }
    else
    {
        /* Priority: 새 스레드의 우선순위가 더 높으면 (숫자가 크면) 선점 */
        if (t->priority > thread_current ()->priority)
            preempt = true;
    }

    if (preempt) // 선점해야 한다면
    {
        thread_yield (); // 즉시 CPU 양보 (스케줄러가 새 스레드를 선택할 것)
    }

    return tid; // 새 스레드의 TID 반환
}

/* * thread_block: 현재 스레드를 BLOCKED 상태로 만들고 스케줄링합니다.
 * 이 함수는 인터럽트가 비활성화된 상태에서 호출되어야 합니다.
 */
void
thread_block (void)
{
    /* 인터럽트 컨텍스트(핸들러) 내에서 호출되면 안 됨 */
    ASSERT (!intr_context ());
    /* 인터럽트가 비활성화된 상태여야 함 */
    ASSERT (intr_get_level () == INTR_OFF);

    thread_current ()->status = THREAD_BLOCKED; // 상태를 BLOCKED로 변경
    schedule (); // 다른 스레드 스케줄링
}

/* * thread_unblock: BLOCKED 상태의 스레드 T를 READY 상태로 전환합니다.
 * T는 반드시 BLOCKED 상태여야 합니다.
 * 이 함수 자체가 선점을 유발하지는 않습니다. (호출자가 선점 로직 처리)
 */
void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;

    ASSERT (is_thread (t)); // 유효한 스레드인지 확인

    old_level = intr_disable (); // 원자적 연산을 위해 인터럽트 비활성화
    
    ASSERT (t->status == THREAD_BLOCKED); // BLOCKED 상태인지 확인
    
    /* [FIX 8] ready 큐에 추가될 때 age를 0으로 초기화 (요구사항) */
    t->age = 0;
    
    if (thread_mlfqs)
    {
        /* [FIX 5-1] MLFQS: 스레드의 큐 레벨에 맞게 큐의 '뒤'에 추가 (FIFO) */
        list_push_back (&ready_queue[t->q_level], &t->elem);
    }
    else
    {
        /* [FIX 5-2] Priority: 우선순위 정렬 큐에 추가 */
        list_insert_ordered (&ready_list, &t->elem, thread_priority_compare_func, NULL);
    }
    
    /*
     * [주석] 선점 로직 관련:
     * unblock된 스레드(t)가 현재 스레드보다 우선순위가 높다면 선점이 필요합니다.
     * 이 로직은 보통 이 함수를 호출하는 synch.c의 sema_up, cond_signal 등에서
     * intr_yield_on_return() 또는 thread_yield()를 호출하여 처리합니다.
     * (이 코드의 주석 [FIX 1 in sema_up]이 이를 암시합니다)
     */
    
    t->status = THREAD_READY; // 스레드 상태를 READY로 변경
    intr_set_level (old_level); // 이전 인터럽트 상태 복원
}

/* (Sleep/Wakeup) 깨어날 최소 틱 업데이트 */
static void
update_next_tick_to_wakeup (int64_t tick)
{
    // 기존 최소 틱보다 더 빠른 틱이면 업데이트
    next_tick_to_wakeup = 
        (next_tick_to_wakeup > tick) ? tick : next_tick_to_wakeup;
}

/* (Sleep/Wakeup) 깨어날 최소 틱 반환 (timer.c에서 사용) */
int64_t
get_next_tick_to_wakeup (void)
{
    return next_tick_to_wakeup;
}

/* * thread_sleep: 현재 스레드를 'tick' 틱까지 재웁니다.
 * 'tick'은 절대적인 시간(timer_ticks() 기준)입니다.
 */
void
thread_sleep (int64_t tick)
{
    struct thread *cur;
    enum intr_level old_level;

    old_level = intr_disable (); // 원자적 연산
    cur = thread_current ();

    ASSERT (cur != idle_thread); // idle 스레드는 잠들 수 없음

    update_next_tick_to_wakeup (cur->wakeup_tick = tick); // 깨어날 시간 설정 및 최소 틱 업데이트
    list_push_back (&sleep_list, &cur->elem); // sleep_list에 추가

    thread_block (); // 스레드 블락 (스케줄링 발생)

    intr_set_level (old_level); // (깨어난 후) 인터럽트 복원
}

/* * thread_wakeup: 'current_tick'에 깨어나야 할 스레드들을 깨웁니다.
 * (timer_interrupt -> timer_ticks -> 이 함수가 호출됨)
 */
void
thread_wakeup (int64_t current_tick)
{
    struct list_elem *e;

    next_tick_to_wakeup = INT64_MAX; // 최소 틱 초기화 (순회하며 다시 계산)

    e = list_begin (&sleep_list);
    while (e != list_end (&sleep_list))
    {
        struct thread *t = list_entry (e, struct thread, elem);
        
        if (current_tick >= t->wakeup_tick) // 깨어날 시간이 되었거나 지났다면
        {
            e = list_remove (&t->elem); // sleep_list에서 제거 (안전한 순회)
            thread_unblock (t);         // ready 큐로 이동 (READY 상태로 변경)
        }
        else // 아직 깨어날 시간이 아니라면
        {
            e = list_next (e); // 다음 스레드로 이동
            update_next_tick_to_wakeup (t->wakeup_tick); // 최소 틱 업데이트
        }
    }
}

/* 현재 실행 중인 스레드의 이름을 반환 */
const char *
thread_name (void)
{
    return thread_current ()->name;
}

/* * thread_current: 현재 실행 중인 스레드의 'struct thread' 포인터를 반환.
 * 스택 오버플로우 감지용 ASSERT 포함.
 */
struct thread *
thread_current (void)
{
    struct thread *t = running_thread (); // 스택 포인터로 스레드 구조체 주소 계산

    /* 스택 오버플로우 등으로 t가 유효하지 않은 스레드를 가리키는지 확인 */
    ASSERT (is_thread (t));
    /* 현재 스레드는 항상 RUNNING 상태여야 함 */
    ASSERT (t->status == THREAD_RUNNING);

    return t;
}

/* 현재 실행 중인 스레드의 TID 반환 */
tid_t
thread_tid (void)
{
    return thread_current ()->tid;
}

/* * thread_exit: 현재 스레드를 종료합니다.
 * 스레드 상태를 DYING으로 바꾸고 스케줄링을 호출합니다.
 * 실제 메모리 해제는 다음에 실행되는 스레드가 thread_schedule_tail에서 수행합니다.
 */
void
thread_exit (void)
{
    ASSERT (!intr_context ()); // 인터럽트 핸들러에서 호출 금지

#ifdef USERPROG
    process_exit (); // (USERPROG) 프로세스 관련 자원 해제
#endif

    intr_disable (); // 원자적 연산
    list_remove (&thread_current ()->allelem); // all_list에서 제거
    thread_current ()->status = THREAD_DYING; // 상태를 DYING으로 변경
    schedule (); // 다른 스레드 스케줄링 (이 함수로 절대 돌아오지 않음)
    NOT_REACHED ();
}

/* * thread_yield: CPU를 자발적으로 양보합니다.
 * 현재 스레드를 ready 큐에 다시 넣고 스케줄링합니다.
 */
void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ()); // 인터럽트 핸들러에서 호출 금지

    old_level = intr_disable (); // 원자적 연산
    
    if (cur != idle_thread) // idle 스레드는 ready 큐에 넣지 않음
    {
        /* [요구사항] 큐에 추가될 때 age는 항상 0으로 리셋 */
        cur->age = 0; 
        
        if (thread_mlfqs)
        {
            /* [FIX 6-1] MLFQS: 현재 큐의 '뒤'에 추가 (FIFO) */
            list_push_back (&ready_queue[cur->q_level], &cur->elem);
        }
        else
        {
            /* [FIX 6-2] Priority: 우선순위 정렬 큐에 추가 */
            list_insert_ordered (&ready_list, &cur->elem, thread_priority_compare_func, NULL);
        }
    }
    
    cur->status = THREAD_READY; // 상태를 READY로 변경
    schedule (); // 스케줄링
    intr_set_level (old_level); // (다시 스케줄링 되었을 때) 인터럽트 복원
}

/* * thread_foreach: all_list의 모든 스레드에 대해 'func' 함수를 실행.
 * 인터럽트가 꺼진 상태에서 호출되어야 함.
 */
void
thread_foreach (thread_action_func *func, void *aux)
{
    struct list_elem *e;

    ASSERT (intr_get_level () == INTR_OFF);

    for (e = list_begin (&all_list); e != list_end (&all_list);
         e = list_next (e))
        {
            struct thread *t = list_entry (e, struct thread, allelem);
            func (t, aux); // 각 스레드 t에 대해 func(t, aux) 호출
        }
}

/* * thread_set_priority: 현재 스레드의 우선순위를 NEW_PRIORITY로 설정.
 * [Priority 스케줄러] 우선순위가 낮아진 경우, 선점을 유발할 수 있음.
 */
void
thread_set_priority (int new_priority)
{
    thread_current ()->priority = new_priority; // 우선순위 멤버 변수 변경

    /*
     * [FIX 2] 선점 로직 (Priority 스케줄러일 때만 동작)
     * MLFQS는 이 함수로 우선순위를 설정하지 않습니다.
     */
    // Priority 스케줄러 모드이고, ready_list가 비어있지 않다면
    if (!thread_mlfqs && !list_empty (&ready_list))
    {
        /* ready_list의 맨 앞 스레드 = 대기 중인 스레드 중 가장 우선순위가 높은 스레드 */
        struct thread *highest_ready = 
            list_entry (list_front (&ready_list), struct thread, elem);

        /*
         * 만약 나의 새 우선순위(new_priority)가 
         * 대기 중인 최고 우선순위 스레드보다 낮다면,
         * 즉시 CPU를 양보(yield)합니다.
         */
        if (new_priority < highest_ready->priority)
        {
            thread_yield ();
        }
    }
}


/* * thread_get_priority: 현재 스레드의 우선순위를 반환.
 * [MLFQS] 큐 레벨을 기반으로 우선순위를 반환.
 * [Priority] 'priority' 멤버 변수를 반환.
 */
int
thread_get_priority (void)
{
    /* [FIX] MLFQS 활성화 시, q_level에 따라 우선순위 반환 */
    if (thread_mlfqs)
    {
        struct thread *cur = thread_current();
        // 큐 레벨(0, 1, 2)을 Pintos 우선순위(MAX, DEFAULT, MIN)로 매핑
        if (cur->q_level == 0) {
            return PRI_MAX; // Q0 -> Highest priority
        } else if (cur->q_level == 1) {
            return PRI_DEFAULT; // Q1 -> Default priority
        } else { // cur->q_level == 2
            return PRI_MIN; // Q2 -> Lowest priority
        }
    }
    else /* Priority 스케줄러일 때는 저장된 priority 값 반환 */
    {
       return thread_current ()->priority;
    }
}

/* (MLFQS 고급 기능 - 이 프로젝트에서는 미구현) */
void
thread_set_nice (int nice UNUSED)
{
    /* Not yet implemented. */
}

/* (MLFQS 고급 기능 - 이 프로젝트에서는 미구현) */
int
thread_get_nice (void)
{
    /* Not yet implemented. */
    return 0;
}

/* (MLFQS 고급 기능 - 이 프로젝트에서는 미구현) */
int
thread_get_load_avg (void)
{
    /* Not yet implemented. */
    return 0;
}

/* (MLFQS 고급 기능 - 이 프로젝트에서는 미구현) */
int
thread_get_recent_cpu (void)
{
    /* Not yet implemented. */
    return 0;
}

/* * idle: idle 스레드가 실행하는 함수.
 * 아무 스레도 실행 준비가 안됐을 때 실행됩니다.
 * CPU를 HLT(Halt) 상태로 만들어 전력 소모를 줄입니다.
 */
static void
idle (void *idle_started_ UNUSED)
{
    struct semaphore *idle_started = idle_started_;
    idle_thread = thread_current (); // 전역 변수 idle_thread에 자신을 등록
    sema_up (idle_started); // thread_start()가 계속 진행되도록 세마포어 up

    for (;;) // 무한 루프
        {
            /* * 다른 스레드가 ready 큐에 추가될 때까지 
             * 자신을 BLOCKED 상태로 만듭니다. (인터럽트 비활성화)
             */
            intr_disable ();
            thread_block ();

            /* * (스케줄러에 의해 다시 실행되면)
             * sti; hlt: 인터럽트를 활성화하고(sti) 즉시 CPU를 중지(hlt)시킵니다.
             * 이 두 명령어는 원자적으로 실행됩니다.
             * 다음 인터럽트가 발생할 때까지 대기합니다.
             */
            asm volatile ("sti; hlt" : : : "memory");
        }
}

/* kernel_thread: 커널 스레드의 실제 진입점. */
static void
kernel_thread (thread_func *function, void *aux)
{
    ASSERT (function != NULL); // 함수 유효성 검사

    intr_enable (); /* 스케줄러는 인터럽트 비활성화 상태로 실행되므로, 여기서 활성화. */
    function (aux); /* 스레드 생성 시 받은 함수 실행 */
    thread_exit (); /* 함수가 반환되면 스레드 자동 종료 */
}

/* * running_thread: 현재 실행 중인 스레드의 'struct thread' 포인터 반환.
 * CPU의 스택 포인터(ESP)를 읽어, 해당 페이지의 시작 주소를 반환합니다.
 * (Pintos는 'struct thread'가 페이지의 시작 부분에 위치한다고 가정)
 */
struct thread *
running_thread (void)
{
    uint32_t *esp;

    /* 어셈블리: ESP 레지스터 값을 'esp' 변수에 저장 */
    asm ("mov %%esp, %0" : "=g"(esp));
    
    /* * esp를 페이지 경계로 내림(round down)하여 
     * 해당 페이지의 시작 주소, 즉 'struct thread'의 주소를 반환.
     */
    return pg_round_down (esp);
}

/* 스레드 T가 유효한 스레드인지(매직 넘버 확인) 검사 */
static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

/* * init_thread: 스레드 구조체 T를 기본값(BLOCKED 상태)으로 초기화합니다.
 */
static void
init_thread (struct thread *t, const char *name, int priority)
{
    ASSERT (t != NULL);
    ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT (name != NULL);

    memset (t, 0, sizeof *t); // 스레드 구조체를 0으로 초기화
    t->status = THREAD_BLOCKED; // 초기 상태는 BLOCKED
    strlcpy (t->name, name, sizeof t->name); // 스레드 이름 복사
    t->stack = (uint8_t *)t + PGSIZE; // 스택 포인터 초기화 (페이지 최상단)
    
    /* 스레드의 기본 필드 설정 */
    t->priority = priority; // (Priority 스케줄러용) 우선순위 설정
    t->age = 0;             // (Aging/MLFQS용) age 초기화
 
    /* MLFQS 필드 초기화 */
    if (thread_mlfqs)
    {
        t->q_level = 0;    /* 모든 스레드는 Q0에서 시작 (요구사항) */
        t->time_in_slice = 0; // 현재 슬라이스 사용량 초기화
    }

    t->magic = THREAD_MAGIC; // 스택 오버플로우 감지용 매직 넘버 설정
    
    /* * all_list에 스레드 추가.
     * (주의: init_thread는 생성 시 딱 한 번만 호출되어야 함)
     */
    list_push_back (&all_list, &t->allelem);
}


/* * alloc_frame: 스레드 T의 스택 최상단에 SIZE 바이트의 프레임을 할당.
 * 스택 포인터를 내리고(t->stack -= size) 해당 주소를 반환.
 */
static void *
alloc_frame (struct thread *t, size_t size)
{
    /* 스택은 항상 4바이트(word) 단위로 정렬되어야 함 */
    ASSERT (is_thread (t));
    ASSERT (size % sizeof (uint32_t) == 0);

    t->stack -= size; // 스택 포인터 감소 (스택은 아래로 자람)
    return t->stack; // 할당된 프레임의 시작 주소 반환
}

/* * next_thread_to_run: 스케줄러가 다음에 실행할 스레드를 선택합니다.
 * [MLFQS] Q0 -> Q1 -> Q2 순서로 큐를 확인하여 스레드를 꺼냅니다.
 * [Priority] ready_list의 맨 앞(최고 우선순위) 스레드를 꺼냅니다.
 * 실행할 스레드가 없으면 idle_thread를 반환합니다.
 */
static struct thread *
next_thread_to_run (void)
{
    if (thread_mlfqs)
    {
        /* [FIX 7-1] MLFQS: Q0, Q1, Q2 순서대로 큐가 비어있는지 확인 */
        if (!list_empty (&ready_queue[0])) // Q0 확인
            return list_entry (list_pop_front (&ready_queue[0]), struct thread, elem);
        if (!list_empty (&ready_queue[1])) // Q1 확인
            return list_entry (list_pop_front (&ready_queue[1]), struct thread, elem);
        if (!list_empty (&ready_queue[2])) // Q2 확인
            return list_entry (list_pop_front (&ready_queue[2]), struct thread, elem);
    }
    else
    {
        /* [FIX 7-2] Priority: ready_list 확인 (이미 정렬되어 있음) */
        if (!list_empty (&ready_list))
            // 맨 앞(최고 우선순위) 스레드를 꺼냄
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
    }
    
    /* 실행할 스레드가 아무것도 없으면 idle 스레드 반환 */
    return idle_thread;
}

/* * thread_schedule_tail: 컨텍스트 스위치(switch_threads) 직후 호출되는 함수.
 * 새 스레드(cur)의 상태를 RUNNING으로 설정하고,
 * 이전 스레드(prev)가 DYING 상태이면 메모리를 해제합니다.
 */
void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread (); // 현재 (새로 스위치된) 스레드

    ASSERT (intr_get_level () == INTR_OFF); // 인터럽트 비활성화 상태 확인

    /* Mark us as running. */
    cur->status = THREAD_RUNNING; // 상태를 RUNNING으로 설정

    /* Start new time slice. */
    thread_ticks = 0; // (Priority용) 타임 슬라이스 카운터 리셋

    /* * 이전에 실행되던 스레드(prev)가 종료(DYING) 상태이면,
     * 해당 스레드의 'struct thread' 페이지를 해제합니다.
     * (initial_thread는 palloc으로 할당된 게 아니므로 제외)
     */
    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
        {
            ASSERT (prev != cur); // 이전 스레드와 현재 스레드는 달라야 함
            palloc_free_page (prev); // 페이지 해제
        }
}

/* * schedule: 스케줄러의 핵심.
 * 현재 스레드는 RUNNING이 아닌 다른 상태 (READY, BLOCKED, DYING)여야 합니다.
 * next_thread_to_run()을 호출하여 다음 스레드를 선택하고,
 * switch_threads()를 호출하여 컨텍스트 스위치를 수행합니다.
 */
static void
schedule (void)
{
    struct thread *cur = running_thread (); // 현재 스레드
    struct thread *next = next_thread_to_run (); // 다음 스레드 선택
    struct thread *prev = NULL; // 이전 스레드 (switch_threads 반환값)

    ASSERT (intr_get_level () == INTR_OFF); // 인터럽트 비활성화 상태 확인
    ASSERT (cur->status != THREAD_RUNNING); // 현재 스레드는 RUNNING이 아니어야 함
    ASSERT (is_thread (next)); // 다음 스레드는 유효해야 함 (최소 idle_thread)

    if (cur != next) // 현재 스레드와 다음 스레드가 다르면
        prev = switch_threads (cur, next); // 컨텍스트 스위치 (cur -> next)
    
    /* * switch_threads는 'next' 스레드의 컨텍스트에서 반환됩니다.
     * 'prev'는 'cur' 스레드의 포인터를 갖게 됩니다.
     * (또는, 스레드가 처음 실행되는 경우 switch_entry에서 호출되며 prev는 NULL)
     */
    thread_schedule_tail (prev); // 스위치 후 마무리 작업
}

/* * allocate_tid: 새 스레드를 위한 유니크한 TID를 할당 (1부터 시작).
 * tid_lock을 사용하여 원자적으로 수행됩니다.
 */
static tid_t
allocate_tid (void)
{
    static tid_t next_tid = 1; // 정적 변수로 다음 TID 유지
    tid_t tid;

    lock_acquire (&tid_lock); // 락 획득
    tid = next_tid++;         // TID 할당 및 증가
    lock_release (&tid_lock); // 락 해제

    return tid;
}

uint32_t thread_stack_ofs = offsetof (struct thread, stack);