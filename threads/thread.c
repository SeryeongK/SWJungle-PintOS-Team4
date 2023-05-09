#define USERPROG
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

#define THREAD_MAGIC 0xcd6abf4b

#define THREAD_BASIC 0xd42df210

static struct list ready_list;

static struct thread *idle_thread;

static struct thread *initial_thread;

static struct lock tid_lock;

static struct list destruction_req;

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);
void update_next_to_wake(int64_t local_ticks);

/*----------------추가 선언 함수-------------------*/
static struct list sleep_list;
static int64_t min_ticks; /*🤔*/

void thread_wakeup(int64_t ticks);
void thread_sleep(int64_t ticks);
int64_t get_next_to_wakeup(void);
void test_max_priority(void);
bool priority_less(const struct list_elem *a_, const struct list_elem *b_,
				   void *aux UNUSED);
/*----------------추가 함수 함수-------------------*/

#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&sleep_list);
	list_init(&destruction_req);

	min_ticks = INT64_MAX; /**/

	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
	/* file descriptor init */
}

void thread_start(void)
{
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	intr_enable();

	sema_down(&idle_started);
}

void thread_tick(void)
{
	struct thread *t = thread_current();

	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	struct thread *curr = thread_current();
	list_push_back(&curr->children_list, &t->child_elem);

	t->fdt = palloc_get_multiple(PAL_ZERO, FDT_PAGES);
	if (t->fdt == NULL)
		return TID_ERROR;

	t->next_fd = 2;
	/* 🤔 */
	t->fdt[0] = 1; // 의미가 있는 숫자는 아니다. 다만 해당 인덱스(식별자)를 사용하는 파일이 존재하므로 넣어준 것.
	t->fdt[1] = 2; // NULL 만들지 않으려고. 원래는 해당 파일을 가리키는 포인터가 들어가야함

	// count 초기화
	t->stdin_count = 1;
	t->stdout_count = 1;

	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* project2 프로세스 계층 구조 구현  */
	// if (curr != NULL)
	// {
	// 	t->parent_pd = curr;
	// 	sema_init(&curr->exit_sema, 0);
	// 	sema_init(&curr->load_sema, 0);
	// 	sema_init(&curr->wait_sema, 0);
	// }

	/*  94p
		😡 프로그램이 로드되지 않음
		😡 프로세스가 종료되지 않음
		😡자식리스트에 추가		*/

	thread_unblock(t); // t를 ready list에 추가함.

	test_max_priority(); // 준코 여기 비교, yield 다있으니까
						 // 여기는 5월 2일 준코 반갑다!

	return tid;
}

void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	/*-------------------------[project 1-2]-------------------------
	list_push_back(&ready_list, &t->elem);
	-------------------------[project 1-2]-------------------------*/
	list_insert_ordered(&ready_list, &t->elem, &priority_less, NULL);
	t->status = THREAD_READY;

	// 우선순위 정렬에 맞게 readu list에 넣는다.
	intr_set_level(old_level);
}

/* 현재 스레드와 priority를 비교하고, ready_list에 추가*/
const char *
thread_name(void)
{
	return thread_current()->name;
}

struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

tid_t thread_tid(void)
{
	return thread_current()->tid;
}

void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	intr_disable();

	// list_remove(&thread_current()->elem);
	// list_remove(&thread_current()->child_elem);
	// list_remove(&thread_current()->donation_elem);

	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
	{
		/*-------------------------[project 1-2]-------------------------
		list_push_back(&ready_list, &curr->elem);
		-------------------------[project 1-2]-------------------------*/
		list_insert_ordered(&ready_list, &curr->elem, priority_less, NULL);
	}
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

void thread_set_priority(int new_priority)
{
	thread_current()->init_priority = new_priority;

	refresh_priority();
	test_max_priority();
}

int thread_get_priority(void)
{
	return thread_current()->priority;
}

void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */
}

int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		intr_disable();
		thread_block();

		asm volatile("sti; hlt"
					 :
					 :
					 : "memory");
	}
}

static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable();
	function(aux);
	thread_exit();
}

static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
	/*----------------[project1]-------------------*/
	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);
	/*----------------[project1]-------------------*/
	list_init(&t->children_list);

	sema_init(&t->wait_sema, 0);
	sema_init(&t->fork_sema, 0);
	sema_init(&t->free_sema, 0);
	t->running = NULL;
	t->exit_status = 0;
	/*---------------[준코]------------------------*/
}

static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

void do_iret(struct intr_frame *tf)
{
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
		:
		: "g"((uint64_t)tf)
		: "memory");
}

static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	__asm __volatile(
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
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
		"pop %%rbx\n"
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n"
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n"
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n"
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n"
		"movw %%cs, 8(%%rax)\n"
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n"
		"mov %%rsp, 24(%%rax)\n"
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		:
		: "g"(tf_cur), "g"(tf)
		: "memory");
}

/* running thread를 어떠한 상태 status로 바꾸고 싶을 때 사용하는 함수*/
static void do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

/* ready의 head의 status를 바꾸는 함수. schedule이전에 꼭꼭 running thread의 starus를 바꿔야한다.
curr_thread가 running이 아니고 ,ready_list의 head가 정상이고, */
static void schedule(void)
{
	struct thread *curr = running_thread(); // running Thread
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	next->status = THREAD_RUNNING;

	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		thread_launch(next);
	}
}

static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}

/*-------------------------[project 1]-------------------------*/
void thread_sleep(int64_t local_ticks) /* local_ticks: 깨울 시간 */
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());
	ASSERT(curr != idle_thread)

	old_level = intr_disable(); /* 인터럽트 방지 */

	curr->wake_up_tick = local_ticks;
	update_next_to_wake(local_ticks); /* sleep_list의 min_tick 업데이트 */
	list_push_back(&sleep_list, &curr->elem);
	thread_block();

	intr_set_level(old_level); /* 인터럽트 재개 */
}

void thread_wakeup(int64_t ticks) /* ticks: global ticks */
{
	struct list_elem *curr = list_begin(&sleep_list);
	/* ⚠️ list_front 사용 시 sleep_list가 비어 있을 경우, ASSERT 발생 => list_begin 사용 */
	while (curr != list_end(&sleep_list)) /* sleep_list 끝까지 탐색 */
	{
		struct thread *t = list_entry(curr, struct thread, elem);
		int64_t tmp_ticks = t->wake_up_tick;
		if (tmp_ticks <= ticks) /* 현재 탐색 중인 스레드가 깰 시간이 되었을 때 */
		{
			curr = list_remove(&t->elem); /* sleep_list에서 제거 */
			thread_unblock(t);
			/*
			⚠️ thread_unblock을 list_remove보다 먼저 사용 시 ready_list로 이동 => list_remove 시 ready_list에서 제거
				* 원래 의도: sleep_list에서 제거
			 */
		}
		else /* 깨울 스레드가 아니면 */
		{
			curr = list_next(curr);
			update_next_to_wake(t->wake_up_tick);
		}
	}
}

/* local_ticks와 min_ticks 비교 => 최솟값 업데이트 */
void update_next_to_wake(int64_t local_ticks)
{
	min_ticks = (local_ticks < min_ticks) ? local_ticks : min_ticks;
}

int64_t get_next_to_wakeup(void)
{
	return min_ticks;
	/* ⚠️ 이후 재새용성을 위한 함수 */
}

bool priority_less(const struct list_elem *a, const struct list_elem *b,
				   void *aux UNUSED)
{
	struct thread *t_a = list_entry(a, struct thread, elem);
	struct thread *t_b = list_entry(b, struct thread, elem);
	return (t_a->priority) > (t_b->priority);
}

void test_max_priority(void)
{
	if (list_empty(&ready_list) || intr_context())
	{
		return;
	}
	int run_priority = thread_current()->priority;
	struct list_elem *e = list_begin(&ready_list);
	struct thread *t = list_entry(e, struct thread, elem);
	if (t->priority > thread_get_priority())
	{
		thread_yield();
	}
}
/*-------------------------[project 1]-------------------------*/
