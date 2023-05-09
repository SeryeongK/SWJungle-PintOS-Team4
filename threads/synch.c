#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
/* --------------------[project1]-----------------------*/
void donate_priority(void);
void remove_with_lock(struct lock *lock);
void refresh_priority(void);

bool sem_priority_less(const struct list_elem *a, const struct list_elem *b, void *aux);
bool donate_priority_less(struct list_elem *a, struct list_elem *b, void *aux);
/* --------------------[project1]-----------------------*/

void sema_init(struct semaphore *sema, unsigned value)
{
	ASSERT(sema != NULL);

	sema->value = value;
	list_init(&sema->waiters);
}

void sema_down(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);
	ASSERT(!intr_context());

	old_level = intr_disable();
	while (sema->value == 0) /* sema에 접근할 수 없을 때 */
	{
		list_insert_ordered(&sema->waiters, &thread_current()->elem, &priority_less, NULL); /* 접근 권한이 생기기를 기다리는 스레드를 waiters에 추가 */
		thread_block();																		   /* 해당 스레드 block */
	}
	sema->value--;
	intr_set_level(old_level);
}

bool sema_try_down(struct semaphore *sema)
{
	enum intr_level old_level;
	bool success;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level(old_level);

	return success;
}

void sema_up(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (!list_empty(&sema->waiters))
	{
		list_sort(&sema->waiters, &priority_less, NULL); /* 우선 순위가 변경됐을 경우 waiters 정렬 */
		thread_unblock(list_entry(list_pop_front(&sema->waiters),
								  struct thread, elem)); /* prioirty가 가장 높은 스레드를 unblock */
	}
	sema->value++;
	test_max_priority();
	intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

void sema_self_test(void)
{
	struct semaphore sema[2];
	int i;

	printf("Testing semaphores...");
	sema_init(&sema[0], 0);
	sema_init(&sema[1], 0);
	thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up(&sema[0]);
		sema_down(&sema[1]);
	}
	printf("done.\n");
}

static void
sema_test_helper(void *sema_)
{
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down(&sema[0]);
		sema_up(&sema[1]);
	}
}

void lock_init(struct lock *lock)
{
	ASSERT(lock != NULL);

	lock->holder = NULL;
	sema_init(&lock->semaphore, 1);
}

/* lock을 요구한 thread_current에 lock을 주는 함수 */
void lock_acquire(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	// ******************************LINE ADDED****************************** //
	// Project 1-2.3 : Priority Inversion Problem - Priority Donation
	// LOCK이 Holder를 가지고 있는지. 즉, LOCK이 점유되고 있는지 Check.
	// 다른 스레드가 LOCK을 점유하고 있으면 자신의 Priority를 Donation 하여
	// 현재 LOCK을 점유하는 스레드가 우선적으로 LOCK을 반환하도록 한다.
	// !CAUTION : lock_acquire 함수 내에서는 thread_current()는 LOCK을 얻고자 하는 스레드를 current로 취급한다.
	//            또한, lock_acquire 함수를 호출 할 수 있었다는 것은 지금 lock->holder 스레드 보다 priority가 높다는 것을 의미
	//            따라서 우선 순위의 대소 비교를 할 필요가 없다.

	/*if (lock -> holder != NULL){*/
	if (lock->holder)
	{
		thread_current()->wait_on_lock = lock; // 현재 스레드가 LOCK을 기다리고 있다고 알려준다.
		// donations 리스트에 넣어줄때는 FIFO(기부 순서)가 아닌 priority순으로 정렬하여 삽입
		// -> donor들이 나갈때도 priority 순으로 나가기 때문에.

		list_insert_ordered(&lock->holder->donations, &thread_current()->donation_elem, &donate_priority_less, NULL);
		/*list_push_back(&lock->holder->donations, &curr->donation_elem);*/
		donate_priority();
	}
	// *************************ADDED LINE ENDS HERE************************* //

	sema_down(&lock->semaphore);
	lock->holder = thread_current();

	// ******************************LINE ADDED****************************** //
	// Project 1-2.3 : Priority Inversion Problem - Priority Donation
	thread_current()->wait_on_lock = NULL;
	// *************************ADDED LINE ENDS HERE************************* //
}

bool lock_try_acquire(struct lock *lock)
{
	bool success;

	ASSERT(lock != NULL);
	ASSERT(!lock_held_by_current_thread(lock));

	success = sema_try_down(&lock->semaphore);
	if (success)
		lock->holder = thread_current();
	return success;
}

/* 다 쓴 lock을 해제하는 함수 */
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	remove_with_lock(lock);
	refresh_priority();
	lock->holder = NULL; /* lock의 holder 초기화 */

	sema_up(&lock->semaphore);

}

bool lock_held_by_current_thread(const struct lock *lock)
{
	ASSERT(lock != NULL);

	return lock->holder == thread_current();
}

/* --------------------[project1]-----------------------*/
/* 현재 스레드가 lock을 기다리고 있는 경우, lock을 보유하고 있는 다른 스레드의 우선순위를 현재 스레드의 우선순위로 업데이트(donation)하는 함수 */
void donate_priority(void)
{
	struct thread *cur = thread_current();
	int depth;
	for (depth = 0; depth < 8; depth++) /* nested donation의 제한 */
	{
		if (!cur->wait_on_lock) /* 현재 탐색 중인 스레드가 필요로 하는 락이 없으면(종료 지점) */
			break;

		struct thread *holder = cur->wait_on_lock->holder;
		holder->priority = cur->priority; /* priority donation */
		cur = holder;					  /* 필요한 lock의 holder를 탐색 스레드로 설정 */
	}
}

/* 인자 lock을 기다리고 있는 스레드를 donations에서 제거하는 함수 */
void remove_with_lock(struct lock *lock)
{
	struct list_elem *find; /* 탐색 포인터 */
	struct thread *curr = thread_current();

	for (find = list_begin(&curr->donations); find != list_end(&curr->donations); find = list_next(find))
	{
		struct thread *t = list_entry(find, struct thread, donation_elem);
		if (t->wait_on_lock == lock) /* 인자 lock을 기다리고 있는 스레드 */
		{
			list_remove(find); /* donations에서 삭제 */
		}
	}
}

/* 현재 스레드의 우선순위를 donations의 최댓값의 우선순위와 비교해서 큰 값으로 업데이트하는 함수 */
void refresh_priority(void)
{
	struct thread *curr = thread_current();
	curr->priority = curr->init_priority;

	if (!list_empty(&curr->donations))
	{
		list_sort(&curr->donations, &donate_priority_less, 0);

		struct thread *front = list_entry(list_front(&curr->donations), struct thread, donation_elem);
		if (front->priority > curr->priority)
			curr->priority = front->priority; /* 현재 스레드와 donations의 최댓값 중 큰 값으로 업데이트 */
	}
}
/* --------------------[project1]-----------------------*/

/* ===============================[condition variable]=============================== */

struct semaphore_elem
{
	struct list_elem elem;
	struct semaphore semaphore;
};

void cond_init(struct condition *cond)
{
	ASSERT(cond != NULL);

	list_init(&cond->waiters);
}

void cond_wait(struct condition *cond, struct lock *lock)
{
	struct semaphore_elem waiter;

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	sema_init(&waiter.semaphore, 0);
	waiter.semaphore.priority = thread_current()->priority;

	list_insert_ordered(&cond->waiters, &waiter.elem, &sem_priority_less, NULL);
	lock_release(lock);
	sema_down(&waiter.semaphore);
	lock_acquire(lock);
}

void cond_signal(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters))
	{
		// list_sort(&cond->waiters, sem_priority_less, NULL);
		sema_up(&list_entry(list_pop_front(&cond->waiters),
							struct semaphore_elem, elem)
					 ->semaphore);
	}
}

void cond_broadcast(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);

	while (!list_empty(&cond->waiters))
		cond_signal(cond, lock);
}

bool sem_priority_less(const struct list_elem *a, const struct list_elem *b, void *aux){
	struct semaphore_elem *a_sema = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *b_sema = list_entry(b, struct semaphore_elem, elem);

	// struct list *waiter_a_sema = &(a_sema->semaphore.waiters);
	// struct list *waiter_b_sema = &(b_sema->semaphore.waiters);

	// return list_entry(list_begin(waiter_a_sema), struct thread, elem)->priority > list_entry(list_begin(waiter_b_sema), struct thread, elem)->priority;
	return a_sema->semaphore.priority > b_sema->semaphore.priority;
}

bool donate_priority_less(struct list_elem *a, struct list_elem *b, void *aux){
	// struct thread *t_a = list_entry(a, struct thread, elem);
	// struct thread *t_b = list_entry(b, struct thread, elem);
	// return (t_a->priority) > (t_b->priority);
	return (list_entry(a, struct thread, donation_elem)->priority) > (list_entry(b, struct thread, donation_elem)->priority);
}
