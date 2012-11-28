/*
 *  linux/kernel/time/tick-sched.c
 *
 *  Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  No idle tick implementation for low and high resolution timers
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  Distribute under GPLv2.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/cpuset.h>
#include <linux/posix-timers.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Per cpu nohz control structure
 */
static DEFINE_PER_CPU(struct tick_sched, tick_cpu_sched);

/*
 * The time, when the last jiffy update happened. Protected by xtime_lock.
 */
static ktime_t last_jiffies_update;

struct tick_sched *tick_get_tick_sched(int cpu)
{
	return &per_cpu(tick_cpu_sched, cpu);
}

/*
 * Must be called with interrupts disabled !
 */
static void tick_do_update_jiffies64(ktime_t now)
{
	unsigned long ticks = 0;
	ktime_t delta;

	/*
	 * Do a quick check without holding xtime_lock:
	 */
	delta = ktime_sub(now, last_jiffies_update);
	if (delta.tv64 < tick_period.tv64)
		return;

	/* Reevalute with xtime_lock held */
	write_seqlock(&xtime_lock);

	delta = ktime_sub(now, last_jiffies_update);
	if (delta.tv64 >= tick_period.tv64) {

		delta = ktime_sub(delta, tick_period);
		last_jiffies_update = ktime_add(last_jiffies_update,
						tick_period);

		/* Slow path for long timeouts */
		if (unlikely(delta.tv64 >= tick_period.tv64)) {
			s64 incr = ktime_to_ns(tick_period);

			ticks = ktime_divns(delta, incr);

			last_jiffies_update = ktime_add_ns(last_jiffies_update,
							   incr * ticks);
		}
		do_timer(++ticks);

		/* Keep the tick_next_period variable up to date */
		tick_next_period = ktime_add(last_jiffies_update, tick_period);
	}
	write_sequnlock(&xtime_lock);
}

/*
 * Initialize and return retrieve the jiffies update.
 */
static ktime_t tick_init_jiffy_update(void)
{
	ktime_t period;

	write_seqlock(&xtime_lock);
	/* Did we start the jiffies update yet ? */
	if (last_jiffies_update.tv64 == 0)
		last_jiffies_update = tick_next_period;
	period = last_jiffies_update;
	write_sequnlock(&xtime_lock);
	return period;
}

/*
 * NOHZ - aka dynamic tick functionality
 */
#ifdef CONFIG_NO_HZ
/*
 * NO HZ enabled ?
 */
static int tick_nohz_enabled __read_mostly  = 1;

/*
 * Enable / Disable tickless mode
 */
static int __init setup_tick_nohz(char *str)
{
	if (!strcmp(str, "off"))
		tick_nohz_enabled = 0;
	else if (!strcmp(str, "on"))
		tick_nohz_enabled = 1;
	else
		return 0;
	return 1;
}

__setup("nohz=", setup_tick_nohz);

/**
 * tick_nohz_update_jiffies - update jiffies when idle was interrupted
 *
 * Called from interrupt entry when the CPU was idle
 *
 * In case the sched_tick was stopped on this CPU, we have to check if jiffies
 * must be updated. Otherwise an interrupt handler could use a stale jiffy
 * value. We do this unconditionally on any cpu, as we don't know whether the
 * cpu, which has the update task assigned is in a long sleep.
 */
static void tick_nohz_update_jiffies(ktime_t now)
{
	int cpu = smp_processor_id();
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	unsigned long flags;

	ts->idle_waketime = now;

	local_irq_save(flags);
	tick_do_update_jiffies64(now);
	local_irq_restore(flags);

	touch_softlockup_watchdog();
}

/*
 * Updates the per cpu time idle statistics counters
 */
static void
update_ts_time_stats(int cpu, struct tick_sched *ts, ktime_t now, u64 *last_update_time)
{
	ktime_t delta;

	if (ts->idle_active) {
		delta = ktime_sub(now, ts->idle_entrytime);
		if (nr_iowait_cpu(cpu) > 0)
			ts->iowait_sleeptime = ktime_add(ts->iowait_sleeptime, delta);
		else
			ts->idle_sleeptime = ktime_add(ts->idle_sleeptime, delta);
		ts->idle_entrytime = now;
	}

	if (last_update_time)
		*last_update_time = ktime_to_us(now);

}

#ifdef CONFIG_CPUSETS_NO_HZ
/*
 * This defines the number of CPUs currently in (or wanting to
 * be in) adaptive nohz mode. Greater than 0 means at least
 * one CPU is ready to shut down its tick for non-idle purposes.
 */
static atomic_t __read_mostly nr_cpus_user_nohz = ATOMIC_INIT(0);

static inline int update_do_timer_cpu(int current_handler,
			int new_handler)
{
	return cmpxchg(&tick_do_timer_cpu, current_handler, new_handler);
}
#else
static inline int update_do_timer_cpu(int current_handler,
			int new_handler)
{
	int tmp = ACCESS_ONCE(tick_do_timer_cpu);
	tick_do_timer_cpu = new_handler;
	return tmp;
}
#endif

/*
 * check_drop_timer_duty: Check if this cpu can shut down
 * ticks without worrying about who is going to handle
 * timekeeping. The duty is dropped here as well if possible.
 * When there are adaptive nohz cpus in the system ready to
 * run user tasks without ticks, this function makes sure
 * that timekeeping is handled by a cpu. A non-adaptive-nohz
 * cpu, if any, will claim the duty as soon as it discovers
 * that some adaptive-nohz cpu is stuck with it.
 *
 * Returns the new value of tick_do_timer_cpu.
 */
static int check_drop_timer_duty(int cpu)
{
	int curr_handler, prev_handler, new_handler;
	int nrepeat = -1;
	bool drop_recheck;

repeat:
	WARN_ON_ONCE(++nrepeat > 1);
	drop_recheck = false;
	curr_handler = cpu;
	new_handler = TICK_DO_TIMER_NONE;

#ifdef CONFIG_CPUSETS_NO_HZ
	if (atomic_read(&nr_cpus_user_nohz) > 0) {
		curr_handler = ACCESS_ONCE(tick_do_timer_cpu);
		/*
		 * Keep the duty until someone takes it away.
		 * FIXME: Make nr_cpus_user_nohz an atomic cpumask
		 * to find an idle CPU to dump the duty at.
		 */
		if (curr_handler == cpu)
			return cpu;
		/*
		 * This cpu will try to take the duty if 1) there is
		 * no handler or 2) current handler seems to be an
		 * adaptive-nohz cpu. We take the duty from others
		 * only if the we are idle or not part of an
		 * adaptive-nohz cpuset.
		 * Once we take the duty, the check above ensures that
		 * we stick with it.
		 */
		if (unlikely(curr_handler == TICK_DO_TIMER_NONE)
			|| (per_cpu(tick_cpu_sched, curr_handler).user_nohz
				&& (is_idle_task(current)
					|| !cpuset_cpu_adaptive_nohz(cpu))))
			new_handler = cpu;
		else
			/*
			 * A regular CPU is updating the jiffies and we don't
			 * have to take it away from her.
			 */
			new_handler = curr_handler;
	} else {
		/*
		 * We might miss nr_cpus_user_nohz update and drop the duty
		 * whereas other CPUs think that we keep handling the
		 * timekeeping. To prevent this, we recheck its value after
		 * we update the timer_do_timer_cpu and start over if
		 * necessary.
		 */
		drop_recheck = true;
	}
#endif

	prev_handler = update_do_timer_cpu(curr_handler, new_handler);

	if (drop_recheck && atomic_read(&nr_cpus_user_nohz) > 0)
		goto repeat;

	if (likely(new_handler != TICK_DO_TIMER_NONE)) {
		if (prev_handler == curr_handler)
			return new_handler;
		/*
		 * Handler was probably changed under us. Whoever has
		 * the duty might just drop it and we wouldn't know.
		 * So, let's try again...
		 */
		goto repeat;
	} else {
		/* We either just dropped the duty or didn't have it. */
		return prev_handler == cpu ? TICK_DO_TIMER_NONE : prev_handler;
	}
}

static void tick_nohz_stop_idle(int cpu, ktime_t now)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);

	update_ts_time_stats(cpu, ts, now, NULL);
	ts->idle_active = 0;

	sched_clock_idle_wakeup_event(0);
}

static ktime_t tick_nohz_start_idle(int cpu, struct tick_sched *ts)
{
	ktime_t now = ktime_get();

	ts->idle_entrytime = now;

#ifdef CONFIG_CPUSETS_NO_HZ
	if (ts->user_nohz) {
		ts->user_nohz = 0;
		WARN_ON_ONCE(atomic_add_negative(-1, &nr_cpus_user_nohz));
	}
#endif

	ts->idle_active = 1;
	sched_clock_idle_sleep_event();
	return now;
}

/**
 * get_cpu_idle_time_us - get the total idle time of a cpu
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cummulative idle time (since boot) for a given
 * CPU, in microseconds.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * This function returns -1 if NOHZ is not enabled.
 */
u64 get_cpu_idle_time_us(int cpu, u64 *last_update_time)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now, idle;

	if (!tick_nohz_enabled)
		return -1;

	now = ktime_get();
	if (last_update_time) {
		update_ts_time_stats(cpu, ts, now, last_update_time);
		idle = ts->idle_sleeptime;
	} else {
		if (ts->idle_active && !nr_iowait_cpu(cpu)) {
			ktime_t delta = ktime_sub(now, ts->idle_entrytime);

			idle = ktime_add(ts->idle_sleeptime, delta);
		} else {
			idle = ts->idle_sleeptime;
		}
	}

	return ktime_to_us(idle);

}
EXPORT_SYMBOL_GPL(get_cpu_idle_time_us);

/**
 * get_cpu_iowait_time_us - get the total iowait time of a cpu
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cummulative iowait time (since boot) for a given
 * CPU, in microseconds.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * This function returns -1 if NOHZ is not enabled.
 */
u64 get_cpu_iowait_time_us(int cpu, u64 *last_update_time)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now, iowait;

	if (!tick_nohz_enabled)
		return -1;

	now = ktime_get();
	if (last_update_time) {
		update_ts_time_stats(cpu, ts, now, last_update_time);
		iowait = ts->iowait_sleeptime;
	} else {
		if (ts->idle_active && nr_iowait_cpu(cpu) > 0) {
			ktime_t delta = ktime_sub(now, ts->idle_entrytime);

			iowait = ktime_add(ts->iowait_sleeptime, delta);
		} else {
			iowait = ts->iowait_sleeptime;
		}
	}

	return ktime_to_us(iowait);
}
EXPORT_SYMBOL_GPL(get_cpu_iowait_time_us);

static ktime_t tick_nohz_stop_sched_tick(struct tick_sched *ts,
					 ktime_t now, int cpu)
{
	unsigned long seq, last_jiffies, next_jiffies, delta_jiffies;
	ktime_t last_update, expires, ret = { .tv64 = 0 };
	struct clock_event_device *dev = __get_cpu_var(tick_cpu_device).evtdev;
	u64 time_delta;
	int new_handler, prev_handler;


	/* Read jiffies and the time when jiffies were updated last */
	do {
		seq = read_seqbegin(&xtime_lock);
		last_update = last_jiffies_update;
		last_jiffies = jiffies;
		time_delta = timekeeping_max_deferment();
	} while (read_seqretry(&xtime_lock, seq));

	if (rcu_needs_cpu(cpu) || printk_needs_cpu(cpu) ||
	    arch_needs_cpu(cpu)) {
		next_jiffies = last_jiffies + 1;
		delta_jiffies = 1;
	} else {
		/* Get the next timer wheel timer */
		next_jiffies = get_next_timer_interrupt(last_jiffies);
		delta_jiffies = next_jiffies - last_jiffies;
	}
	/*
	 * Do not stop the tick, if we are only one off
	 * or if the cpu is required for rcu
	 */
	if (!ts->tick_stopped && delta_jiffies == 1)
		goto out;

	/* Schedule the tick, if we are at least one jiffie off */
	if ((long)delta_jiffies >= 1) {
		/*
		 * Check if adaptive nohz needs this CPU to take care
		 * of the jiffies update. We also drop the duty in this
		 * function if we can.
		 */
		prev_handler = ACCESS_ONCE(tick_do_timer_cpu);
		new_handler = check_drop_timer_duty(cpu);
		if (new_handler == cpu)
			goto out;

		/*
		 * If this cpu is the one which had the do_timer()
		 * duty last, we limit the sleep time to the
		 * timekeeping max_deferement value which we retrieved
		 * above. Otherwise we can sleep as long as we want.
		 */
		if (prev_handler == cpu) {
			ts->do_timer_last = 1;
		} else if (new_handler != TICK_DO_TIMER_NONE) {
			time_delta = KTIME_MAX;
			ts->do_timer_last = 0;
		} else if (!ts->do_timer_last) {
			time_delta = KTIME_MAX;
		}

		/*
		 * calculate the expiry time for the next timer wheel
		 * timer. delta_jiffies >= NEXT_TIMER_MAX_DELTA signals
		 * that there is no timer pending or at least extremely
		 * far into the future (12 days for HZ=1000). In this
		 * case we set the expiry to the end of time.
		 */
		if (likely(delta_jiffies < NEXT_TIMER_MAX_DELTA)) {
			/*
			 * Calculate the time delta for the next timer event.
			 * If the time delta exceeds the maximum time delta
			 * permitted by the current clocksource then adjust
			 * the time delta accordingly to ensure the
			 * clocksource does not wrap.
			 */
			time_delta = min_t(u64, time_delta,
					   tick_period.tv64 * delta_jiffies);
		}

		if (time_delta < KTIME_MAX)
			expires = ktime_add_ns(last_update, time_delta);
		else
			expires.tv64 = KTIME_MAX;

		/* Skip reprogram of event if its not changed */
		if (ts->tick_stopped && ktime_equal(expires, dev->next_event))
			goto out;

		ret = expires;

		/*
		 * nohz_stop_sched_tick can be called several times before
		 * the nohz_restart_sched_tick is called. This happens when
		 * interrupts arrive which do not cause a reschedule. In the
		 * first call we save the current tick time, so we can restart
		 * the scheduler tick in nohz_restart_sched_tick.
		 */
		if (!ts->tick_stopped) {
			ts->last_tick = hrtimer_get_expires(&ts->sched_timer);
			ts->tick_stopped = 1;
			trace_printk("Stop tick\n");
		}

		/*
		 * If the expiration time == KTIME_MAX, then
		 * in this case we simply stop the tick timer.
		 */
		 if (unlikely(expires.tv64 == KTIME_MAX)) {
			if (ts->nohz_mode == NOHZ_MODE_HIGHRES)
				hrtimer_cancel(&ts->sched_timer);
			goto out;
		}

		if (ts->nohz_mode == NOHZ_MODE_HIGHRES) {
			hrtimer_start(&ts->sched_timer, expires,
				      HRTIMER_MODE_ABS_PINNED);
			/* Check, if the timer was already in the past */
			if (hrtimer_active(&ts->sched_timer))
				goto out;
		} else if (!tick_program_event(expires, 0))
				goto out;
		/*
		 * We are past the event already. So we crossed a
		 * jiffie boundary. Update jiffies and raise the
		 * softirq.
		 */
		tick_do_update_jiffies64(ktime_get());
	}
	raise_softirq_irqoff(TIMER_SOFTIRQ);
out:
	ts->next_jiffies = next_jiffies;
	ts->last_jiffies = last_jiffies;
	ts->sleep_length = ktime_sub(dev->next_event, now);

	return ret;
}

static bool can_stop_idle_tick(int cpu, struct tick_sched *ts)
{
	/*
	 * If this cpu is offline and it is the one which updates
	 * jiffies, then give up the assignment and let it be taken by
	 * the cpu which runs the tick timer next. If we don't drop
	 * this here the jiffies might be stale and do_timer() never
	 * invoked.
	 */
	if (unlikely(!cpu_online(cpu))) {
		/*
		 * FIXME: Might need some sort of protection
		 * against CPU hotunplug for adaptive nohz.
		 */
		if (cpu == tick_do_timer_cpu)
			tick_do_timer_cpu = TICK_DO_TIMER_NONE;
	}

	if (unlikely(ts->nohz_mode == NOHZ_MODE_INACTIVE))
		return false;

	if (need_resched())
		return false;

	if (unlikely(local_softirq_pending() && cpu_online(cpu))) {
		static int ratelimit;

		if (ratelimit < 10) {
			printk(KERN_ERR "NOHZ: local_softirq_pending %02x\n",
			       (unsigned int) local_softirq_pending());
			ratelimit++;
		}
		return false;
	}

	return true;
}

static void __tick_nohz_idle_enter(struct tick_sched *ts)
{
	ktime_t now, expires;
	int cpu = smp_processor_id();

	now = tick_nohz_start_idle(cpu, ts);

	if (can_stop_idle_tick(cpu, ts)) {
		int was_stopped = ts->tick_stopped;

		ts->idle_calls++;

		expires = tick_nohz_stop_sched_tick(ts, now, cpu);
		if (expires.tv64 > 0LL) {
			ts->idle_sleeps++;
			ts->idle_expires = expires;
		}

		if (!was_stopped && ts->tick_stopped) {
			ts->saved_jiffies = ts->last_jiffies;
			ts->saved_jiffies_whence = JIFFIES_SAVED_IDLE;
			select_nohz_load_balancer(1);
		}
	}
}

/**
 * tick_nohz_idle_enter - stop the idle tick from the idle task
 *
 * When the next event is more than a tick into the future, stop the idle tick
 * Called when we start the idle loop.
 *
 * The arch is responsible of calling:
 *
 * - rcu_idle_enter() after its last use of RCU before the CPU is put
 *  to sleep.
 * - rcu_idle_exit() before the first use of RCU after the CPU is woken up.
 */
void tick_nohz_idle_enter(void)
{
	struct tick_sched *ts;

	WARN_ON_ONCE(irqs_disabled());

	/*
 	 * Update the idle state in the scheduler domain hierarchy
 	 * when tick_nohz_stop_sched_tick() is called from the idle loop.
 	 * State will be updated to busy during the first busy tick after
 	 * exiting idle.
 	 */
	set_cpu_sd_state_idle();

	local_irq_disable();

	ts = &__get_cpu_var(tick_cpu_sched);
	/*
	 * set ts->inidle unconditionally. even if the system did not
	 * switch to nohz mode the cpu frequency governers rely on the
	 * update of the idle time accounting in tick_nohz_start_idle().
	 */
	ts->inidle = 1;
	__tick_nohz_idle_enter(ts);

	local_irq_enable();
}

#ifdef CONFIG_CPUSETS_NO_HZ
static bool can_stop_adaptive_tick(struct tick_sched *ts)
{
	int ret = true;

	if (!sched_can_stop_tick()
		|| posix_cpu_timers_running(current)
		|| rcu_pending(smp_processor_id()))
		ret = false;

	if (ret && !ts->user_nohz) {
		ts->user_nohz = 1;
		atomic_inc(&nr_cpus_user_nohz);
	} else if (!ret && ts->user_nohz) {
		ts->user_nohz = 0;
		WARN_ON_ONCE(atomic_add_negative(-1, &nr_cpus_user_nohz));
	}

	return ret;
}

static void tick_nohz_cpuset_stop_tick(struct tick_sched *ts)
{
	struct pt_regs *regs = get_irq_regs();
	int cpu = smp_processor_id();
	int was_stopped;
	int user = 0;

	if (regs)
		user = user_mode(regs);

	if (!cpuset_adaptive_nohz() || is_idle_task(current))
		return;

	if (!ts->tick_stopped && ts->nohz_mode == NOHZ_MODE_INACTIVE)
		return;

	if (!can_stop_adaptive_tick(ts))
		return;

	/*
	 * If we stop the tick between the syscall exit hook and the actual
	 * return to userspace, we'll think we are in system space (due to
	 * user_mode() thinking so). And since we passed the syscall exit hook
	 * already we won't realize we are in userspace. So the time spent
	 * tickless would be spuriously accounted as belonging to system.
	 *
	 * To avoid this kind of problem, we only stop the tick from userspace
	 * (until we find a better solution).
	 * We can later enter the kernel and keep the tick stopped. But the place
	 * where we stop the tick must be userspace.
	 * We make an exception for kernel threads since they always execute in
	 * kernel space.
	 */
	if (!user && current->mm)
		return;

	was_stopped = ts->tick_stopped;
	tick_nohz_stop_sched_tick(ts, ktime_get(), cpu);

	if (!was_stopped && ts->tick_stopped) {
		WARN_ON_ONCE(ts->saved_jiffies_whence != JIFFIES_SAVED_NONE);
		if (user) {
			ts->saved_jiffies_whence = JIFFIES_SAVED_USER;
			__get_cpu_var(nohz_task_ext_qs) = 1;
			rcu_user_enter_irq();
		} else if (!current->mm) {
			ts->saved_jiffies_whence = JIFFIES_SAVED_SYS;
		}

		ts->saved_jiffies = jiffies;
		set_thread_flag(TIF_NOHZ);
		trace_printk("set TIF_NOHZ\n");
	}
}
#else
static void tick_nohz_cpuset_stop_tick(struct tick_sched *ts) { }
#endif

/**
 * tick_nohz_irq_exit - update next tick event from interrupt exit
 *
 * When an interrupt fires while we are idle and it doesn't cause
 * a reschedule, it may still add, modify or delete a timer, enqueue
 * an RCU callback, etc...
 * So we need to re-calculate and reprogram the next tick event.
 */
void tick_nohz_irq_exit(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	if (ts->inidle) {
		if (!need_resched())
			__tick_nohz_idle_enter(ts);
	} else {
		tick_nohz_cpuset_stop_tick(ts);
	}
}

/**
 * tick_nohz_get_sleep_length - return the length of the current sleep
 *
 * Called from power state control code with interrupts disabled
 */
ktime_t tick_nohz_get_sleep_length(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	return ts->sleep_length;
}

static void tick_nohz_restart(struct tick_sched *ts, ktime_t now)
{
	hrtimer_cancel(&ts->sched_timer);
	hrtimer_set_expires(&ts->sched_timer, ts->last_tick);

	while (1) {
		/* Forward the time to expire in the future */
		hrtimer_forward(&ts->sched_timer, now, tick_period);

		if (ts->nohz_mode == NOHZ_MODE_HIGHRES) {
			hrtimer_start_expires(&ts->sched_timer,
					      HRTIMER_MODE_ABS_PINNED);
			/* Check, if the timer was already in the past */
			if (hrtimer_active(&ts->sched_timer))
				break;
		} else {
			if (!tick_program_event(
				hrtimer_get_expires(&ts->sched_timer), 0))
				break;
		}
		/* Update jiffies and reread time */
		tick_do_update_jiffies64(now);
		now = ktime_get();
	}
}

static void __tick_nohz_restart_sched_tick(struct tick_sched *ts, ktime_t now)
{
	/* Update jiffies first */
	tick_do_update_jiffies64(now);

	touch_softlockup_watchdog();
	/*
	 * Cancel the scheduled timer and restore the tick
	 */
	ts->tick_stopped  = 0;
	ts->idle_exittime = now;

	tick_nohz_restart(ts, now);
	trace_printk("Restart sched tick\n");
}

/**
 * tick_nohz_restart_sched_tick - restart the tick for a tickless CPU
 *
 * Restart the tick when the CPU is in adaptive tickless mode.
 */
void tick_nohz_restart_sched_tick(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	unsigned long flags;
	ktime_t now;

	local_irq_save(flags);

	if (!ts->tick_stopped) {
		local_irq_restore(flags);
		return;
	}

	now = ktime_get();
	__tick_nohz_restart_sched_tick(ts, now);

	local_irq_restore(flags);
}


static void tick_nohz_account_ticks(struct tick_sched *ts)
{
	unsigned long ticks;
	/*
	 * We stopped the tick. Update process times would miss the
	 * time we ran tickless as update_process_times does only a 1 tick
	 * accounting. Enforce that this is accounted to nohz timeslices.
	 */
	ticks = jiffies - ts->saved_jiffies;
	/*
	 * We might be one off. Do not randomly account a huge number of ticks!
	 */
	if (ticks && ticks < LONG_MAX) {
		switch (ts->saved_jiffies_whence) {
		case JIFFIES_SAVED_IDLE:
			account_idle_ticks(ticks);
			break;
		case JIFFIES_SAVED_USER:
			account_user_ticks(current, ticks);
			break;
		case JIFFIES_SAVED_SYS:
			account_system_ticks(current, ticks);
			break;
		case JIFFIES_SAVED_NONE:
			break;
		default:
			WARN_ON_ONCE(1);
		}
	}
}

/**
 * tick_nohz_idle_exit - restart the idle tick from the idle task
 *
 * Restart the idle tick when the CPU is woken up from idle
 * This also exit the RCU extended quiescent state. The CPU
 * can use RCU again after this function is called.
 */
void tick_nohz_idle_exit(void)

{
	int cpu = smp_processor_id();
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now;

	local_irq_disable();

	WARN_ON_ONCE(!ts->inidle);

	ts->inidle = 0;

	if (ts->idle_active || ts->tick_stopped)
		now = ktime_get();

	if (ts->idle_active)
		tick_nohz_stop_idle(cpu, now);

	if (ts->tick_stopped) {
		select_nohz_load_balancer(0);
		__tick_nohz_restart_sched_tick(ts, now);
#ifndef CONFIG_VIRT_CPU_ACCOUNTING
		tick_nohz_account_ticks(ts);
		ts->saved_jiffies_whence = JIFFIES_SAVED_NONE;
#endif
	}

	local_irq_enable();
}

static int tick_nohz_reprogram(struct tick_sched *ts, ktime_t now)
{
	hrtimer_forward(&ts->sched_timer, now, tick_period);
	return tick_program_event(hrtimer_get_expires(&ts->sched_timer), 0);
}

/*
 * The nohz low res interrupt handler
 */
static void tick_nohz_handler(struct clock_event_device *dev)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	struct pt_regs *regs = get_irq_regs();
	int cpu = smp_processor_id();
	ktime_t now = ktime_get();

	dev->next_event.tv64 = KTIME_MAX;

	/*
	 * Check if the do_timer duty was dropped. We don't care about
	 * concurrency: This happens only when the cpu in charge went
	 * into a long sleep. If two cpus happen to assign themself to
	 * this duty, then the jiffies update is still serialized by
	 * xtime_lock.
	 */
	if (unlikely(tick_do_timer_cpu == TICK_DO_TIMER_NONE))
		tick_do_timer_cpu = cpu;

	/* Check, if the jiffies need an update */
	if (tick_do_timer_cpu == cpu)
		tick_do_update_jiffies64(now);

	/*
	 * When we are idle and the tick is stopped, we have to touch
	 * the watchdog as we might not schedule for a really long
	 * time. This happens on complete idle SMP systems while
	 * waiting on the login prompt. We also increment the "start
	 * of idle" jiffy stamp so the idle accounting adjustment we
	 * do when we go busy again does not account too much ticks.
	 */
	if (ts->tick_stopped) {
		touch_softlockup_watchdog();
		ts->saved_jiffies++;
	}

	update_process_times(user_mode(regs));
	profile_tick(CPU_PROFILING);

	while (tick_nohz_reprogram(ts, now)) {
		now = ktime_get();
		tick_do_update_jiffies64(now);
	}
}

/**
 * tick_nohz_switch_to_nohz - switch to nohz mode
 */
static void tick_nohz_switch_to_nohz(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	ktime_t next;

	if (!tick_nohz_enabled)
		return;

	local_irq_disable();
	if (tick_switch_to_oneshot(tick_nohz_handler)) {
		local_irq_enable();
		return;
	}

	ts->nohz_mode = NOHZ_MODE_LOWRES;

	/*
	 * Recycle the hrtimer in ts, so we can share the
	 * hrtimer_forward with the highres code.
	 */
	hrtimer_init(&ts->sched_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	/* Get the next period */
	next = tick_init_jiffy_update();

	for (;;) {
		hrtimer_set_expires(&ts->sched_timer, next);
		if (!tick_program_event(next, 0))
			break;
		next = ktime_add(next, tick_period);
	}
	local_irq_enable();
}

/*
 * When NOHZ is enabled and the tick is stopped, we need to kick the
 * tick timer from irq_enter() so that the jiffies update is kept
 * alive during long running softirqs. That's ugly as hell, but
 * correctness is key even if we need to fix the offending softirq in
 * the first place.
 *
 * Note, this is different to tick_nohz_restart. We just kick the
 * timer and do not touch the other magic bits which need to be done
 * when idle is left.
 */
static void tick_nohz_kick_tick(int cpu, ktime_t now)
{
#if 0
	/* Switch back to 2.6.27 behaviour */

	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t delta;

	/*
	 * Do not touch the tick device, when the next expiry is either
	 * already reached or less/equal than the tick period.
	 */
	delta =	ktime_sub(hrtimer_get_expires(&ts->sched_timer), now);
	if (delta.tv64 <= tick_period.tv64)
		return;

	tick_nohz_restart(ts, now);
#endif
}

static inline void tick_check_nohz(int cpu)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	ktime_t now;

	if (!ts->idle_active && !ts->tick_stopped)
		return;
	now = ktime_get();
	if (ts->idle_active)
		tick_nohz_stop_idle(cpu, now);
	if (ts->tick_stopped) {
		tick_nohz_update_jiffies(now);
		tick_nohz_kick_tick(cpu, now);
	}
}

#else

static inline void tick_nohz_switch_to_nohz(void) { }
static inline void tick_check_nohz(int cpu) { }

#endif /* NO_HZ */

/*
 * Called from irq_enter to notify about the possible interruption of idle()
 */
void tick_check_idle(int cpu)
{
	tick_check_oneshot_broadcast(cpu);
	tick_check_nohz(cpu);
}

#ifdef CONFIG_CPUSETS_NO_HZ
DEFINE_PER_CPU(int, nohz_task_ext_qs);

void tick_nohz_exit_kernel(void)
{
	unsigned long flags;
	struct tick_sched *ts;
	unsigned long delta_jiffies;

	local_irq_save(flags);

	ts = &__get_cpu_var(tick_cpu_sched);

	if (!ts->tick_stopped) {
		local_irq_restore(flags);
		return;
	}

	WARN_ON_ONCE(ts->saved_jiffies_whence != JIFFIES_SAVED_SYS);

	delta_jiffies = jiffies - ts->saved_jiffies;
	account_system_ticks(current, delta_jiffies);

	ts->saved_jiffies = jiffies;
	ts->saved_jiffies_whence = JIFFIES_SAVED_USER;

	__get_cpu_var(nohz_task_ext_qs) = 1;
	rcu_user_enter();

	local_irq_restore(flags);
}

void tick_nohz_enter_kernel(void)
{
	unsigned long flags;
	struct tick_sched *ts;
	unsigned long delta_jiffies;

	local_irq_save(flags);

	ts = &__get_cpu_var(tick_cpu_sched);

	if (!ts->tick_stopped) {
		local_irq_restore(flags);
		return;
	}

	if (__get_cpu_var(nohz_task_ext_qs) == 1) {
		__get_cpu_var(nohz_task_ext_qs) = 0;
		rcu_user_exit();
	}

	WARN_ON_ONCE(ts->saved_jiffies_whence != JIFFIES_SAVED_USER);

	delta_jiffies = jiffies - ts->saved_jiffies;
	account_user_ticks(current, delta_jiffies);

	ts->saved_jiffies = jiffies;
	ts->saved_jiffies_whence = JIFFIES_SAVED_SYS;

	local_irq_restore(flags);
}

void tick_nohz_cpu_exit_qs(bool irq)
{
	if (__get_cpu_var(nohz_task_ext_qs)) {
		if (irq)
			rcu_user_exit_irq();
		else
			rcu_user_exit();
		__get_cpu_var(nohz_task_ext_qs) = 0;
	}
}

void tick_nohz_enter_exception(struct pt_regs *regs)
{
	if (user_mode(regs))
		tick_nohz_enter_kernel();
}

void tick_nohz_exit_exception(struct pt_regs *regs)
{
	if (user_mode(regs))
		tick_nohz_exit_kernel();
}

static void tick_nohz_restart_adaptive(struct tick_sched *ts)
{
	tick_nohz_flush_current_times(true);

	if (ts->user_nohz) {
		ts->user_nohz = 0;
		WARN_ON_ONCE(atomic_add_negative(-1, &nr_cpus_user_nohz));
	}
	tick_nohz_restart_sched_tick();
	clear_thread_flag(TIF_NOHZ);
	trace_printk("clear TIF_NOHZ\n");
	tick_nohz_cpu_exit_qs(true);
}

void tick_nohz_check_adaptive(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	if (ts->tick_stopped && !is_idle_task(current)) {
		if (!can_stop_adaptive_tick(ts))
			tick_nohz_restart_adaptive(ts);
	}
}

void cpuset_exit_nohz_interrupt(void *unused)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	trace_printk("IPI: Nohz exit\n");
	if (ts->tick_stopped && !is_idle_task(current))
		tick_nohz_restart_adaptive(ts);
}

/*
 * Flush cputime and clear hooks before context switch in case we
 * haven't yet received the IPI that should take care of that.
 */
void tick_nohz_pre_schedule(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	/*
	 * We are holding the rq lock and if we restart the tick now
	 * we could deadlock by acquiring the lock twice. Instead
	 * we do that on post schedule time. For now do the cleanups
	 * on the prev task.
	 */
	if (ts->tick_stopped) {
		tick_nohz_flush_current_times(true);
		clear_thread_flag(TIF_NOHZ);
		trace_printk("clear TIF_NOHZ\n");
		/* FIXME: warn if we are in RCU idle mode */
	}
}

void tick_nohz_post_schedule(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	unsigned long flags;

	local_irq_save(flags);
	if (ts->tick_stopped) {
		if (is_idle_task(current)) {
			ts->saved_jiffies = jiffies;
			ts->saved_jiffies_whence = JIFFIES_SAVED_IDLE;
		} else {
			tick_nohz_restart_sched_tick();
		}
	}
	local_irq_restore(flags);
}

void tick_nohz_flush_current_times(bool restart_tick)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	if (ts->tick_stopped) {
		tick_nohz_account_ticks(ts);
		if (restart_tick)
			ts->saved_jiffies_whence = JIFFIES_SAVED_NONE;
		else
			ts->saved_jiffies = jiffies;
	}
}
#else

static void tick_do_timer_check_handler(int cpu)
{
#ifdef CONFIG_NO_HZ
	/*
	 * Check if the do_timer duty was dropped. We don't care about
	 * concurrency: This happens only when the cpu in charge went
	 * into a long sleep. If two cpus happen to assign themself to
	 * this duty, then the jiffies update is still serialized by
	 * xtime_lock.
	 */
	if (unlikely(tick_do_timer_cpu == TICK_DO_TIMER_NONE))
		tick_do_timer_cpu = cpu;
#endif
}

#endif /* CONFIG_CPUSETS_NO_HZ */

/*
 * High resolution timer specific code
 */
#ifdef CONFIG_HIGH_RES_TIMERS
/*
 * We rearm the timer until we get disabled by the idle code.
 * Called with interrupts disabled and timer->base->cpu_base->lock held.
 */
static enum hrtimer_restart tick_sched_timer(struct hrtimer *timer)
{
	struct tick_sched *ts =
		container_of(timer, struct tick_sched, sched_timer);
	struct pt_regs *regs = get_irq_regs();
	ktime_t now = ktime_get();
	int cpu = smp_processor_id();

#ifdef CONFIG_NO_HZ
	/*
	 * Check if the do_timer duty was dropped. We don't care about
	 * concurrency: This happens only when the cpu in charge went
	 * into a long sleep. If two cpus happen to assign themself to
	 * this duty, then the jiffies update is still serialized by
	 * xtime_lock.
	 */
	if (unlikely(tick_do_timer_cpu == TICK_DO_TIMER_NONE))
		tick_do_timer_cpu = cpu;
#endif

	/* Check, if the jiffies need an update */
	if (tick_do_timer_cpu == cpu)
		tick_do_update_jiffies64(now);

	/*
	 * Do not call, when we are not in irq context and have
	 * no valid regs pointer
	 */
	if (regs) {
		int user = user_mode(regs);
		/*
		 * When the tick is stopped, we have to touch the watchdog
		 * as we might not schedule for a really long time. This
		 * happens on complete idle SMP systems while waiting on
		 * the login prompt. We also increment the last jiffy stamp
		 * recorded when we stopped the tick so the cpu time accounting
		 * adjustment does not account too much ticks when we flush them.
		 */
		if (ts->tick_stopped) {
			/* CHECKME: may be this is only needed in idle */
			touch_softlockup_watchdog();
			ts->saved_jiffies++;
		}
		update_process_times(user);
		profile_tick(CPU_PROFILING);
		trace_printk("tick\n");
	}

	hrtimer_forward(timer, now, tick_period);

	return HRTIMER_RESTART;
}

/**
 * tick_setup_sched_timer - setup the tick emulation timer
 */
void tick_setup_sched_timer(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);
	ktime_t now = ktime_get();

	/*
	 * Emulate tick processing via per-CPU hrtimers:
	 */
	hrtimer_init(&ts->sched_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	ts->sched_timer.function = tick_sched_timer;

	/* Get the next period (per cpu) */
	hrtimer_set_expires(&ts->sched_timer, tick_init_jiffy_update());

	for (;;) {
		hrtimer_forward(&ts->sched_timer, now, tick_period);
		hrtimer_start_expires(&ts->sched_timer,
				      HRTIMER_MODE_ABS_PINNED);
		/* Check, if the timer was already in the past */
		if (hrtimer_active(&ts->sched_timer))
			break;
		now = ktime_get();
	}

#ifdef CONFIG_NO_HZ
	if (tick_nohz_enabled)
		ts->nohz_mode = NOHZ_MODE_HIGHRES;
#endif
}
#endif /* HIGH_RES_TIMERS */

#if defined CONFIG_NO_HZ || defined CONFIG_HIGH_RES_TIMERS
void tick_cancel_sched_timer(int cpu)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);

# ifdef CONFIG_HIGH_RES_TIMERS
	if (ts->sched_timer.base)
		hrtimer_cancel(&ts->sched_timer);
# endif

	ts->nohz_mode = NOHZ_MODE_INACTIVE;
}
#endif

/**
 * Async notification about clocksource changes
 */
void tick_clock_notify(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		set_bit(0, &per_cpu(tick_cpu_sched, cpu).check_clocks);
}

/*
 * Async notification about clock event changes
 */
void tick_oneshot_notify(void)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	set_bit(0, &ts->check_clocks);
}

/**
 * Check, if a change happened, which makes oneshot possible.
 *
 * Called cyclic from the hrtimer softirq (driven by the timer
 * softirq) allow_nohz signals, that we can switch into low-res nohz
 * mode, because high resolution timers are disabled (either compile
 * or runtime).
 */
int tick_check_oneshot_change(int allow_nohz)
{
	struct tick_sched *ts = &__get_cpu_var(tick_cpu_sched);

	if (!test_and_clear_bit(0, &ts->check_clocks))
		return 0;

	if (ts->nohz_mode != NOHZ_MODE_INACTIVE)
		return 0;

	if (!timekeeping_valid_for_hres() || !tick_is_oneshot_available())
		return 0;

	if (!allow_nohz)
		return 1;

	tick_nohz_switch_to_nohz();
	return 0;
}
