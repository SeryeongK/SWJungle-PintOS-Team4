#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "lib/kernel/hash.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define FDT_PAGES 3
#define FDCOUNT_LIMIT FDT_PAGES * (1 << 9)

/* States in a thread's life cycle. */
enum thread_status
{
	THREAD_RUNNING, /* Running thread. */
	THREAD_READY,	/* Not running but ready to run. */
	THREAD_BLOCKED, /* Waiting for an event to trigger. */
	THREAD_DYING	/* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0	   /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63	   /* Highest priority. */

/*----------------[project1]-------------------*/
void thread_sleep(int64_t ticks);
static struct list ready_list;

static struct list sleep_list;
void update_next_to_wake(int64_t local_ticks);
int64_t get_next_to_wakeup(void);

/*----------------[project1]-------------------*/

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread
{
	/* Owned by thread.c. */
	tid_t tid;				   /* Thread identifier. */
	enum thread_status status; /* Thread state. */
	char name[16];			   /* Name (for debugging purposes). */
	int priority;			   /* Priority. */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem; /* List element. */

	/* local tick */
	int64_t wake_up_tick;

	/*----------------[project1]-------------------*/
	/* priority donaion 관련 element 추가 */
	int init_priority;				// donation이후 우선순위를 초기화하기 위해 초기값 저장
	struct lock *wait_on_lock;		// 해당 스레드가 대기하고 있는 lock자료구조 주소 저장
	struct list donations;			// multiple donation 을 고려하기 위해사용
	struct list_elem donation_elem; // multiple donation 을 고려하기 위해사용
	/*----------------[project1]-------------------*/

	/*----------------[project2]-------------------*/
	/* parent-children hierachy */
	struct thread *parent_pd; // 부모 프로세스의 디스크립터
	struct list_elem child_elem;
	struct list children_list;

	bool isload;
	bool isexit;

	struct semaphore wait_sema;
	struct semaphore fork_sema;
	struct semaphore free_sema;
	int exit_status;

	struct intr_frame parent_if;
	struct semaphore exit_sema;
	struct semaphore load_sema;

	struct file **fdt;
	int next_fd;
	
	struct file *running; // 현재 스레드가 사용 중인 파일(load하고 있는 파일)
	int stdin_count;
	int stdout_count;
	/*----------------[project2]-------------------*/
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
	void *user_rsp;
	void *user_stack_bottom;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* Information for switching */
	unsigned magic;		  /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

// 준코 : 정의
void thread_sleep(int64_t);
void thread_wakeup(int64_t);
void update_next_to_wake(int64_t local_ticks);
int64_t get_next_to_wakeup(void);
void test_max_priority(void);
bool priority_less(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED);

#endif /* threads/thread.h */
