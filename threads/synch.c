/* 필요한 헤더 파일들을 포함합니다. */
#include "threads/synch.h"     // 현재 파일의 선언 (세마포어, 락, 조건 변수 구조체)
#include <stdio.h>               // printf (sema_self_test에서 사용)
#include <string.h>              // (특별히 사용되진 않음)
#include "threads/interrupt.h" // 인터럽트 제어 함수 (intr_disable, intr_set_level, intr_context)
#include "threads/thread.h"    // 스레드 제어 함수 (thread_current, thread_block, thread_unblock) 및 스케줄러 플래그

/* --- 세마포어 (Semaphore) --- */

/* * sema_init: 세마포어 SEMA를 초기값 VALUE로 초기화합니다.
 * 세마포어는 0 또는 양의 정수 값을 가지며, 'down'(P)과 'up'(V) 연산으로 조작됩니다.
 */
void
sema_init (struct semaphore *sema, unsigned value)
{
    ASSERT (sema != NULL); // 세마포어 포인터가 유효한지 확인

    sema->value = value; // 세마포어 카운터(자원 수)를 초기값으로 설정
    list_init (&sema->waiters); // 이 세마포어를 기다리는 스레드 리스트(waiters)를 초기화
}

/* * sema_down: 세마포어 'down'(P) 연산. SEMA의 값이 0보다 커질 때까지 기다린 후,
 * 원자적으로 값을 1 감소시킵니다. (자원 획득 시도)
 *
 * 이 함수는 잠들 수(sleep) 있으므로 인터럽트 핸들러 내에서 호출하면 안 됩니다.
 */
void
sema_down (struct semaphore *sema)
{
    enum intr_level old_level; // 이전 인터럽트 상태를 저장할 변수

    ASSERT (sema != NULL); // 세마포어 포인터 유효성 검사
    ASSERT (!intr_context ()); // 인터럽트 핸들러(컨텍스트)에서 호출되지 않았는지 확인

    old_level = intr_disable (); // 원자적 연산(값 확인 및 대기)을 위해 인터럽트 비활성화
    
    // sema->value가 0이면 (자원이 없으면) 0보다 커질 때까지 반복 대기
    while (sema->value == 0)
    {
        if (thread_mlfqs) // MLFQS 스케줄러가 활성화된 경우
        {
            /* [FIX] MLFQS: 현재 스레드를 대기열(waiters)의 '끝'에 추가 (FIFO) */
            list_push_back (&sema->waiters, &thread_current ()->elem);
        }
        else // Priority 스케줄러가 활성화된 경우
        {
            /* [Priority]: 현재 스레드를 우선순위에 따라 정렬된 위치에 추가 */
            list_insert_ordered (&sema->waiters, &thread_current ()->elem,
                                 thread_priority_compare_func, NULL);
        }
        thread_block (); // 스레드를 BLOCKED 상태로 만들고 스케줄러 호출 (잠들기)
    }
    
    // (루프를 빠져나오면) 자원이 있으므로 값을 1 감소시켜 자원 획득
    sema->value--;
    
    intr_set_level (old_level); // 이전 인터럽트 상태 복원
}


/* * sema_try_down: 'down' 연산을 시도하되, 
 * 세마포어 값이 0이 아닐 경우에만 성공합니다. (기다리지 않음)
 * 성공 시 true (값 1 감소), 실패 시 false (값이 0)를 반환합니다.
 *
 * 이 함수는 잠들지 않으므로 인터럽트 핸들러 내에서 호출할 수 있습니다.
 */
bool
sema_try_down (struct semaphore *sema)
{
    enum intr_level old_level;
    bool success; // 성공 여부 저장

    ASSERT (sema != NULL); // 세마포어 포인터 유효성 검사

    old_level = intr_disable (); // 원자적 확인 및 수정을 위해 인터럽트 비활성화
    
    if (sema->value > 0) // 자원이 있다면
        {
            sema->value--; // 자원 획득 (값 1 감소)
            success = true; // 성공
        }
    else // 자원이 없다면
        success = false; // 실패
        
    intr_set_level (old_level); // 이전 인터럽트 상태 복원

    return success; // 성공 또는 실패 반환
}

/* * sema_up: 세마포어 'up'(V) 연산. SEMA의 값을 1 증가시킵니다. (자원 반납)
 * 만약 대기 중인 스레드가 있다면, 그중 하나를 깨웁니다.
 *
 * 이 함수는 인터럽트 핸들러 내에서 호출될 수 있습니다.
 */
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;
    struct thread *t = NULL; // 깨어날 스레드를 가리킬 포인터

    ASSERT (sema != NULL); // 세마포어 포인터 유효성 검사

    old_level = intr_disable (); // 원자적 연산을 위해 인터럽트 비활성화
    
    if (!list_empty (&sema->waiters)) // 대기 중인 스레드가 있다면
    {
        /*
         * [Priority]: waiters 리스트는 우선순위로 정렬되어 있으므로, 
         * list_pop_front()는 가장 우선순위가 높은 스레드를 반환합니다.
         * [MLFQS]: waiters 리스트는 FIFO이므로, 가장 오래 기다린 스레드를 반환합니다.
         */
        t = list_entry (list_pop_front (&sema->waiters), // 리스트의 '앞'에서 스레드를 꺼냄
                        struct thread, elem);
        thread_unblock (t); // 해당 스레드를 READY 상태로 만듦 (ready_list에 추가)
    }
    
    sema->value++; // 세마포어 값 1 증가 (자원 반납 또는 신호)

    /* [FIX 10] MLFQS와 Priority 스케줄링 모두 지원하도록 선점 로직 수정 */
    
    if (t != NULL) // 만약 스레드 't'를 방금 깨웠다면
    {
        bool preempt = false; // 선점(preemption) 여부 플래그
        
        if (thread_mlfqs) { // MLFQS 모드일 경우
            /* MLFQS: 깨어난 스레드(t)의 큐 레벨이 현재 스레드보다 높으면(숫자가 낮으면) 선점 */
            preempt = (t->q_level < thread_current()->q_level);
        } else { // Priority 모드일 경우
            /* Priority: 깨어난 스레드(t)의 우선순위가 현재 스레드보다 높으면(숫자가 크면) 선점 */
            preempt = (t->priority > thread_current()->priority);
        }

        // 선점이 필요하고, 현재 코드가 인터럽트 핸들러가 아니라면 (일반 스레드)
        if (preempt && !intr_context()) {
             thread_yield(); // 즉시 CPU 양보
        } 
        // 선점이 필요하고, 현재 코드가 인터럽트 핸들러 내부라면
        else if (preempt && intr_context()) {
             intr_yield_on_return(); // 인터럽트 핸들러 종료 시 CPU 양보
        }
    }
    
    intr_set_level (old_level); // 이전 인터럽트 상태 복원
}

/* 세마포어 테스트용 헬퍼 함수 (선언) */
static void sema_test_helper (void *sema_);

/* 세마포어 자체 테스트 함수 */
void
sema_self_test (void)
{
    struct semaphore sema[2];
    int i;

    printf ("Testing semaphores...");
    sema_init (&sema[0], 0); // sema[0] = 0
    sema_init (&sema[1], 0); // sema[1] = 0
    // "sema-test" 스레드 생성, sema_test_helper 함수 실행
    thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
    
    // 메인 스레드와 "sema-test" 스레드가 핑퐁(ping-pong)처럼 신호를 주고받음
    for (i = 0; i < 10; i++)
        {
            sema_up (&sema[0]);   // 헬퍼 스레드를 깨움
            sema_down (&sema[1]); // 헬퍼 스레드가 깨워줄 때까지 대기
        }
    printf ("done.\n");
}

/* sema_self_test에 사용되는 헬퍼 스레드 함수 */
static void
sema_test_helper (void *sema_)
{
    struct semaphore *sema = sema_; // 인자로 받은 세마포어 배열
    int i;

    for (i = 0; i < 10; i++)
        {
            sema_down (&sema[0]); // 메인 스레드가 깨워줄 때까지 대기
            sema_up (&sema[1]);   // 메인 스레드를 깨움
        }
}


/* --- 락 (Lock) --- */
/* 락은 세마포어를 이용해 구현되며, 초기값이 1인 세마포어와 유사합니다.
 * 차이점: 1. 락은 오직 한 스레드만 보유 가능 (value가 1을 초과하지 않음)
 * 2. 락은 '소유자(holder)' 개념이 있어, 획득한 스레드만 반납할 수 있음.
 */

/* lock_init: 락(LOCK)을 초기화합니다. */
void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL); // 락 포인터 유효성 검사

    lock->holder = NULL; // 락 소유자(holder)를 NULL로 초기화
    sema_init (&lock->semaphore, 1); // 내부 세마포어 값을 1 (사용 가능)로 초기화
}

/* * lock_acquire: 락(LOCK)을 획득합니다.
 * 락이 사용 가능해질 때까지 (필요하면) 잠들며 기다립니다.
 * 이미 락을 보유한 스레드가 다시 획득하려 하면 안 됩니다 (Non-recursive).
 */
void
lock_acquire (struct lock *lock)
{
    ASSERT (lock != NULL); // 락 포인터 유효성 검사
    ASSERT (!intr_context ()); // 인터럽트 핸들러에서 호출 금지 (잠들 수 있음)
    // 현재 스레드가 이미 락을 보유하고 있는지 확인 (재귀적 획득 방지)
    ASSERT (!lock_held_by_current_thread (lock)); 

    sema_down (&lock->semaphore); // 내부 세마포어를 'down' (값이 1->0이 됨, 획득)
    lock->holder = thread_current (); // 락의 소유자를 현재 스레드로 설정
}

/* * lock_try_acquire: 락(LOCK) 획득을 시도합니다. (기다리지 않음)
 * 성공 시 true, 실패(다른 스레드가 보유 중) 시 false를 반환합니다.
 * 잠들지 않으므로 인터럽트 핸들러에서도 호출 가능합니다.
 */
bool
lock_try_acquire (struct lock *lock)
{
    bool success; // 성공 여부

    ASSERT (lock != NULL); // 락 포인터 유효성 검사
    ASSERT (!lock_held_by_current_thread (lock)); // 재귀적 획득 방지

    success = sema_try_down (&lock->semaphore); // 내부 세마포어 'try_down' 시도
    
    if (success) // 성공했다면
        lock->holder = thread_current (); // 락 소유자를 현재 스레드로 설정
        
    return success; // 결과 반환
}

/* * lock_release: 락(LOCK)을 반납합니다.
 * 락은 반드시 현재 스레드에 의해 소유되어 있어야 합니다.
 */
void
lock_release (struct lock *lock)
{
    ASSERT (lock != NULL); // 락 포인터 유효성 검사
    // 현재 스레드가 락의 소유자인지 확인
    ASSERT (lock_held_by_current_thread (lock)); 

    lock->holder = NULL; // 락 소유자 정보 제거
    sema_up (&lock->semaphore); // 내부 세마포어를 'up' (값이 0->1이 됨, 반납)
}

/* * lock_held_by_current_thread: 현재 스레드가 락(LOCK)을 보유하고 있는지 확인.
 */
bool
lock_held_by_current_thread (const struct lock *lock)
{
    ASSERT (lock != NULL); // 락 포인터 유효성 검사

    // 락의 소유자가 현재 스레드인지 비교하여 반환
    return lock->holder == thread_current (); 
}


struct semaphore_elem

{

    struct list_elem elem;      /* List element. */

    struct semaphore semaphore; /* This semaphore. */

    int priority;               /* [FIX 2] 스레드의 우선순위를 저장 */

};

/* --- 조건 변수 (Condition Variable) --- */
/* 조건 변수는 특정 조건이 충족되기를 기다리는 스레드들을 관리합니다.
 * 항상 락과 함께 사용되어야 합니다.
 */

/* cond_init: 조건 변수(COND)를 초기화합니다. */
void
cond_init (struct condition *cond)
{
    ASSERT (cond != NULL); // 조건 변수 포인터 유효성 검사

    // 대기자(waiter) 리스트를 초기화합니다.
    // 이 리스트에는 'struct semaphore_elem'이 저장됩니다.
    list_init (&cond->waiters);
}

/* * [FIX 3] [Priority 스케줄러용]
 * 조건 변수 대기 리스트(cond->waiters)에 사용될 비교 함수.
 * 'struct semaphore_elem'에 저장된 'priority'를 비교합니다.
 */
bool
sema_elem_priority_compare_func (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED)
{
    // list_elem을 포함하는 semaphore_elem 구조체 포인터 획득
    struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
    struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);
    
    // 우선순위가 높은(숫자가 큰) 스레드가 리스트의 '앞'에 오도록 정렬
    return sa->priority > sb->priority; 
}
   
/* * cond_wait: 조건 변수(COND)를 기다립니다.
 * 이 함수는 원자적으로 락(LOCK)을 반납하고, 신호(signal)를 받을 때까지 잠듭니다.
 * 신호를 받아 깨어나면, 락을 다시 획득한 후 반환합니다.
 *
 * 락은 이 함수 호출 전에 반드시 획득되어 있어야 합니다.
 */
void
cond_wait (struct condition *cond, struct lock *lock)
{
    /*
     * 각 스레드는 자신만의 대기용 세마포어를 가집니다.
     * 이 'waiter'는 현재 스레드의 스택에 생성됩니다.
     */
    struct semaphore_elem waiter; 

    ASSERT (cond != NULL); // 조건 변수 포인터 유효성 검사
    ASSERT (lock != NULL); // 락 포인터 유효성 검사
    ASSERT (!intr_context ()); // 인터럽트 핸들러에서 호출 금지 (잠들 수 있음)
    ASSERT (lock_held_by_current_thread (lock)); // 락을 보유하고 있는지 확인

    sema_init (&waiter.semaphore, 0); // 대기용 세마포어를 0으로 초기화 (sema_down 시 잠들도록)
    
    /* * [Priority] 우선순위 스케줄러일 때만 이 값이 정렬에 사용됩니다.
     * 현재 스레드의 우선순위를 waiter에 저장합니다.
     */
    waiter.priority = thread_current ()->priority; 
    
    if (thread_mlfqs) // MLFQS 모드일 경우
    {
        /* [FIX] MLFQS: 대기열(cond->waiters)의 '끝'에 추가 (FIFO) */
        list_push_back (&cond->waiters, &waiter.elem);
    }
    else // Priority 모드일 경우
    {
        /* [Priority]: 우선순위에 따라 정렬된 위치에 추가 */
        list_insert_ordered (&cond->waiters, &waiter.elem, 
                             sema_elem_priority_compare_func, NULL);
    }
    
    lock_release (lock); // (중요) 잠들기 전에 락을 반납합니다.
    sema_down (&waiter.semaphore); // 자신만의 세마포어를 'down'하며 잠듭니다. (신호 대기)
    lock_acquire (lock); // (깨어난 후) 락을 다시 획득하고 반환합니다.
}

/* * cond_signal: 조건 변수(COND)를 기다리는 스레드 중 하나에게 신호를 보냅니다.
 * (락(LOCK)에 의해 보호됨)
 * 락은 이 함수 호출 전에 반드시 획득되어 있어야 합니다.
 */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL); // 조건 변수 포인터 유효성 검사
    ASSERT (lock != NULL); // 락 포인터 유효성 검사
    ASSERT (!intr_context ()); // 인터럽트 핸들러에서 호출 금지
    ASSERT (lock_held_by_current_thread (lock)); // 락을 보유하고 있는지 확인

    /*
     * cond_wait에서 리스트가 이미 정렬(Priority)되었거나 FIFO(MLFQS)이므로,
     * list_pop_front()는 항상 올바른 다음 스레드
     * (가장 우선순위가 높거나, 가장 오래 기다린)의 waiter를 반환합니다.
     */
    if (!list_empty (&cond->waiters)) // 기다리는 스레드가 있다면
    {
        // 리스트의 '앞'에서 waiter를 꺼내고, 그 waiter의 세마포어를 'up'하여 스레드를 깨움
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                 ->semaphore);
        // (sema_up 내부에 선점 로직이 포함되어 있음)
    }
}

/* * cond_broadcast: 조건 변수(COND)를 기다리는 *모든* 스레드에게 신호를 보냅니다.
 * (락(LOCK)에 의해 보호됨)
 * 락은 이 함수 호출 전에 반드시 획득되어 있어야 합니다.
 */

void
cond_broadcast (struct condition *cond, struct lock *lock)

{

    ASSERT (cond != NULL);

    ASSERT (lock != NULL);



    while (!list_empty (&cond->waiters))

        cond_signal (cond, lock);

}


