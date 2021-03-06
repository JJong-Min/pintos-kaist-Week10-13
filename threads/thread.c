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
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of process in THREAD_BLOCK state */
static struct list sleep_list;
// sleep_list에서 awake되는 시점이 가장 빠른 thread의 awake_ticks 시점
static int64_t next_tick_to_awake; 

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* Donation. */
#define DONATE_MAX_DEPTH 8		/* maximum number of depth to donate */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

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
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
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
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
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
		thread_func *function, void *aux) {
	// function: thread가 생성된 뒤 그 thread의 context에서 수행할 업무(thread routine)
	// aux: function에 넘길 인자
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* parent-child 관계 
		- 현재 thread의 chilren list에 새로 생성된 thread 추가 (FIFO 방식)
	*/
	struct thread *parent = thread_current();
	list_push_back(&parent->children, &t->child_elem);

	/* file descriptor 관련
		- palloc을 활용해 kernel memory fool에 FDT 생성
		- next_fd를 2로 초기화: 0은 STDIN, 1은 STDOUT
	 */
	// printf("[thread_create]  %d, %d, %p \n", PAL_ZERO, FDT_PAGE_CNT, t->fdt);
	t->fdt = (struct file**) palloc_get_multiple(PAL_ZERO, FDT_PAGE_CNT);
	t->fdt[0] = 10; // dummy value 
	t->fdt[1] = 11; // dummy value
	t->next_fd = 2; // 다음에 들어갈 파일의 fd값
	t->max_fd = 1;  // 파일이 들어간 fd의 최대값

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);
	test_max_priority();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
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
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);

	// unblock될 때 우선순위를 정렬되어 ready_list에 스레드를 추가할 수 있게
	// list_push_back (&ready_list, &t->elem);
	list_insert_ordered (&ready_list, &t->elem, thread_compare_priority, 0);

	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
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
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
	// ready_list에 삽입되는 부분을 변경해줌
		list_insert_ordered (&ready_list, &curr->elem, thread_compare_priority, 0);
		// list_push_back (&ready_list, &curr->elem);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 가장 먼저 awake해야 하는 thread가 awake되어야 하는 시각을 업데이트 (setter) */
void update_next_tick_to_awake(int64_t ticks) { 
	// 질문: next_tick_to_awake를 초기 설정을 하지 않아도 되려나?
	next_tick_to_awake = (next_tick_to_awake > ticks) ? ticks : next_tick_to_awake;
}

/* 가장 먼저 awake해야 하는 thread가 awake되어야 하는 시각을 반환 (getter) */
int64_t get_next_tick_to_awake(void) {
	return next_tick_to_awake;
}

/* thread를 ticks까지 재우는 함수 */
void thread_sleep(int64_t ticks) {
	struct thread *cur;
	// interrupt를 막고, 이전 interrupt 상태를 저장함
	enum intr_level old_level;
	old_level = intr_disable();

	// 현재 thread를 잠재우기 위해 가져옴 (이 때, idle thread는 sleep되지 않아야 함)
	cur = thread_current();
	ASSERT(cur != idle_thread);
	// awake 함수가 실행될 시점 tick을 update
	update_next_tick_to_awake(cur->wakeup_tick = ticks);
	// 현재 thread를 sleep_list에 삽입함
	list_push_back(&sleep_list, &cur->elem);
	// 현재 thread의 상태를 block으로 변경 (scheduling까지 진행)
	thread_block();

	// interrupt 가능 여부를 이전 상태로 되돌림
	intr_set_level(old_level);
}

/* sleep_list에서 깨워야 하는 thread들을 모두 깨움 */
void thread_awake(int64_t curr_tick) {
	// next_tick_to_awake를 최대값으로 설정
	next_tick_to_awake = INT64_MAX;
	// sleep_list에 있는 첫 번째 thread로 e 초기 설정 (정확히는 첫 번째 thread의 elem)
	struct list_elem *e;
	e = list_begin(&sleep_list);
	// e가 thread의 끝(tail)에 닿을 때까지 순서대로 확인
	while (e != list_end(&sleep_list)) {
		// e로 thread의 주소값 확보
		struct thread *t = list_entry(e, struct thread, elem);
		// thread가 일어나야할 시점이 현재 시점보다 작거나 같은 경우,
		// 즉 thread가 일어나야할 시점에 이른 경우
		if (curr_tick >= t-> wakeup_tick) {
			// 해당 thread를 sleep_list에서 제거
			e = list_remove(&t->elem);
			// 해당 thread의 상태를 ready로 바꾸고 ready_list에 추가
			thread_unblock(t);
		} 
		// thread가 아직 일어나야할 시점이 아닌 경우
		else {
			// e를 다음 thread로 변경
			e = list_next(e);
			// 해당 thread의 일어나야할 시점으로 next_tick_to_awake 업데이트
			update_next_tick_to_awake(t->wakeup_tick);
		}
	}

}

/* 우선순위를 정렬하기 위해 비교함수 정의 */
bool 
thread_compare_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry (a, struct thread, elem)->priority 
			> list_entry (b, struct thread, elem)->priority;
}

/* donations를 우선순위 기준으로 정렬하기 위한 비교 함수 정의 */
bool
thread_compare_donate_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry (a, struct thread, donation_elem) -> priority 
			> list_entry (b, struct thread, donation_elem)-> priority;
}

/* ready_list에서 가장 높은 우선순위를 가진 스레드(head)가 현재 current_thread(CPU 점유중인)인 스레드보다 높으면
	CPU점유를 양보하는 함수 */
// test_max_priority 함수는 스레드가 새로 생성돼서 ready_list에 추가되거나 현재 실행중인 스레드의 우선순위가 재조정될 때 호출
// 즉, 스레드를 새로 생성하는 함수인 thread_create에서 현재 스레드의 우선순위를 재조정하는 thread_set_priority() 내부에 test_max_priority()를 추가
void
test_max_priority (void) {
	if (!intr_context() 
		&& !list_empty (&ready_list) 
		&& thread_current()->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority){
		thread_yield();
	}
}

/* current thread를 시작으로 필요한 범위까지 우선순위를 양도 */
void
donate_priority (void) {
	struct thread *curr = thread_current ();
	int depth;
	// 최대 DONATE_MAX_DEPTH 까지 우선순위를 양도 
	for (depth = 0; depth < DONATE_MAX_DEPTH; depth++) {
		// thread가 기다리는 lock이 있는지 확인
		if (!curr->wait_on_lock) break;
		// 기다리는 lock의 holder를 찾아, holder의 우선순위를 업데이트
		// - 이 때, holder의 우선순위 보다 현재 thread의 우선순위가 높은 경우에만 업데이트 필요
		struct thread *holder = curr->wait_on_lock->holder;
		if (holder->priority < curr->priority) {
			holder->priority = curr->priority;
		}
		// holder를 curr로 업데이트해 초점 이동
		curr = holder;
	}
}

/* current thread의 donations에서 lock이 반환되기를 기다리고 있던 thread를 제거
- 이 함수는 current thread가 lock을 release하는 시점에 실행됨 */
void
remove_with_lock (struct lock *lock) {
	struct list_elem *e;
	struct thread *curr = thread_current ();
	// 현재 thread의 donations들 중 반환할 lock을 기다리고 있던 thread를 삭제
	for (e = list_begin (&curr->donations); 
		e != list_end (&curr->donations); 
		e = list_next (e)) {
			struct thread *t = list_entry (e, struct thread, donation_elem);
			if (t->wait_on_lock == lock) {
				list_remove (&t->donation_elem);
			}
	}
}

/* current thread의 우선순위를 업데이트하는 함수 
 - 본래의 priority와 donations에 있는 우선순위 중 높은 값으로 업데이트 */
void
refresh_priority(void) {
	struct thread *curr = thread_current ();
	// 본래의 priority로 1차 업데이트
	curr->priority = curr->init_priority;
	// donations 중 우선순위가 가장 높은 thread의 우선순위와 비교하여 2차 업데이트
	if (!list_empty (&curr->donations)) {
		// donations을 우선순위 높은 순으로 정렬해 begin의 우선순위 확인 
		// - 질문: 이미 추가할 때마다 순서를 유지하고 있는데 꼭 해야할까? 
		//       혹은 어차피 여기서 정렬할거면, 그냥 넣을 때는 맨 뒤에 넣으면 되지 않을까?
		list_sort (&curr->donations, thread_compare_donate_priority, 0); 
		struct thread *front = list_entry(list_front (&curr->donations), struct thread, donation_elem);
		if (front->priority > curr->priority) {
			curr->priority = front->priority;
		}
	}
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	// current thread의 priority는 donation에 의해 수정된 상태일 수 있으므로
	// priority가 아닌 init_priority를 업데이트
	thread_current ()->init_priority = new_priority;
	// init_priority가 update된 상태에서 다시 refresh를 진행
	refresh_priority();
	test_max_priority();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
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
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
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
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	// donation 관련 멤버 초기 설정
	t->init_priority = priority;  // 변하지 않고, 변경된 priority를 되돌릴 때 사용됨
	t->wait_on_lock = NULL;	       
	list_init (&t->donations);

	/* parent child 관계 관련 */
	list_init(&t->children);		/* children list 생성 */
	sema_init(&t->fork_sema, 0);	/* parent가 down한 뒤 child(current thread)가 up */
	sema_init(&t->wait_sema, 0);	/* parent가 down한 뒤 child(current thread)가 up */
	sema_init(&t->free_sema, 0);	/* child(current thread)가 down한 뒤 parent가 up */

	/* 실행 중인 파일 관련 */
	t->running_file = NULL;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used bye the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
