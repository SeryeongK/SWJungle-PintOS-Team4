#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

static int64_t ticks;

static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

void timer_init(void)
{
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb(0x43, 0x34);
	outb(0x40, count & 0xff);
	outb(0x40, count >> 8);

	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	ASSERT(intr_get_level() == INTR_ON);
	printf("Calibrating timer...  ");

	loops_per_tick = 1u << 10;
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;
		ASSERT(loops_per_tick != 0);
	}

	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

int64_t
timer_ticks(void)
{
	enum intr_level old_level = intr_disable();
	int64_t t = ticks;
	intr_set_level(old_level);
	barrier();
	return t;
}

int64_t
timer_elapsed(int64_t then)
{
	return timer_ticks() - then;
}

/*-------------------------[project 1]-------------------------*/
void timer_sleep(int64_t local_ticks) /* local_ticks: 재우고 싶은 시간*/
{
	int64_t start = timer_ticks();

	ASSERT(intr_get_level() == INTR_ON); /* 인터럽트 방지 */

	if (timer_elapsed(start) < local_ticks) /* 깨울 시간이 안 됐을 경우 */
	{
		thread_sleep(start + local_ticks);
	}
}
/*-------------------------[project 1]-------------------------*/

void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

static void
timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++;
	thread_tick();

	/*-------------------------[project 1]-------------------------*/
	/* 깨울 스레드가 있으면 깨우기 */
	if (get_next_to_wakeup() <= ticks) /* get_next_to_wakeup(): 가장 작은 wakeup_ticks를 가진 스레드를 반환 */
	{
		thread_wakeup(ticks);
	}
	/*-------------------------[project 1]-------------------------*/
}

static bool
too_many_loops(unsigned loops)
{
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	start = ticks;
	busy_wait(loops);

	barrier();
	return start != ticks;
}

static void NO_INLINE
busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

static void
real_time_sleep(int64_t num, int32_t denom)
{

	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		timer_sleep(ticks);
	}
	else
	{
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
