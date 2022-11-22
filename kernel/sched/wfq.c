#include "sched.h"

#include <linux/sched/wfq.h>
#include <linux/percpu.h>
#include <linux/timekeeping.h>

#define WFQ_BALANCE_SKIP 0
#define WFQ_BALANCE_IDLE 1
#define WFQ_BALANCE_PERIODIC 2

/* Records the last time we did periodic load balancing */
struct periodic_lb_info periodic_lb_info = {
	.last_bal_time = 0,
	.lock = __RW_LOCK_UNLOCKED(periodic_lb_info.lock),
};

void init_wfq_rq(struct wfq_rq *wfq_rq)
{
	wfq_rq->wfq_nr_running = 0;
	wfq_rq->total_weight = 0;
	wfq_rq->vruntime = 0;
	INIT_LIST_HEAD(&wfq_rq->wfq_rq_list);
}

/*
 * Some useful helper functions
 */
static inline struct task_struct *wfq_task_of(struct sched_wfq_entity *wfq_se)
{
	return container_of(wfq_se, struct task_struct, wfq);
}

static inline struct rq *wfq_rq_of(struct wfq_rq *wfq_rq)
{
	return container_of(wfq_rq, struct rq, wfq);
}

static u64 compute_vft(struct sched_wfq_entity *wfq_se)
{
	return wfq_se->vruntime + WFQ_VRUNTIME_PER_TICK / wfq_se->weight;
}

static u64 compute_task_vft(struct task_struct *p)
{
	struct sched_wfq_entity *wfq_se = &p->wfq;

	return compute_vft(wfq_se);
}

#ifdef CONFIG_SMP

/*
 * Determine if task p on the heaviest CPU's rq (@rq) can be moved to @cpu's rq
 */
static inline int pick_wfq_task(struct rq *rq, struct task_struct *p, int cpu, int balance_type)
{
	struct wfq_rq *src_wfq_rq = &rq->wfq;
	struct wfq_rq *dst_wfq_rq = &cpu_rq(cpu)->wfq;

	struct sched_wfq_entity *wfq_se = &p->wfq;
	int task_weight = wfq_se->weight;

	int dst_weight_after_bal = dst_wfq_rq->total_weight + task_weight;
	int src_weight_after_bal = src_wfq_rq->total_weight - task_weight;
	int imbalance_wont_reverse = dst_weight_after_bal <= src_weight_after_bal;

	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return 0;

	if (balance_type == WFQ_BALANCE_IDLE)
		return 1;

	if (imbalance_wont_reverse)
		return 1;
	else
		return 0;
}

/*
 * Determine which task to pull from the heaviest CPU's rq (@rq)
 */
static struct task_struct *pick_pushable_wfq_task(struct rq *rq, int cpu, int balance_type)
{
	struct sched_wfq_entity *wfq_se;

	/* Iterate through tasks on heaviest CPU's rq */
	list_for_each_entry(wfq_se, &rq->wfq.wfq_rq_list, entity_list) {
		struct task_struct *p = wfq_task_of(wfq_se);

		if (p != rq->curr && pick_wfq_task(rq, p, cpu, balance_type))
			return p;
	}

	return NULL;
}

/*
 * Add p to the dst_rq
 */
static void attach_task(struct rq *dst_rq, struct task_struct *p)
{
	struct rq_flags rf;

	rq_lock(dst_rq, &rf);

	update_rq_clock(dst_rq);
	activate_task(dst_rq, p, ENQUEUE_NOCLOCK);
	check_preempt_curr(dst_rq, p, 0);

	rq_unlock(dst_rq, &rf);
}

/*
 * Called if we must perform idle or periodic load balancing
 */
static int load_balance(int this_cpu, struct rq *this_rq, int balance_type)
{
	int heaviest_cpu_nr_running = 0;
	int heaviest_cpu = -1;
	int heaviest_cpu_weight = 0;
	int lightest_cpu = -1;
	int lightest_cpu_weight = INT_MAX;
	int cpu;

	struct rq *src_rq;
	struct rq *dst_rq;
	int dst_cpu;

	int ret = 0;

	struct rq_flags rf;

	struct task_struct *p;

	/* Determine CPUs with min and max total_weight */
	for_each_online_cpu(cpu) {
		unsigned long flags;
		struct rq *rq = cpu_rq(cpu);

		raw_spin_lock_irqsave(&rq->lock, flags);
		if (rq->wfq.total_weight > heaviest_cpu_weight) {
			heaviest_cpu_nr_running = rq->wfq.wfq_nr_running;
			heaviest_cpu_weight = rq->wfq.total_weight;
			heaviest_cpu = cpu;
		}

		if (rq->wfq.total_weight < lightest_cpu_weight) {
			lightest_cpu_weight = rq->wfq.total_weight;
			lightest_cpu = cpu;
		}
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}

	/*
	 * No need to balance if the heaviest and lightest CPUs are the same
	 * or if the heaviest CPU has 1 or 0 tasks
	 * or if a task sneaks on and makes our CPU the heaviest during idle LB
	 */
	if (heaviest_cpu == lightest_cpu ||
		heaviest_cpu_nr_running <= 1 ||
		(balance_type == WFQ_BALANCE_IDLE && heaviest_cpu == this_cpu))
		return ret;

	/* src_rq is the rq we will pull a task from */
	src_rq = cpu_rq(heaviest_cpu);

	/*
	 * dst_rq will receive a task. It is either:
	 * (1) lightest cpu if periodic balance
	 * (2) us if idle balance
	 */
	if (balance_type == WFQ_BALANCE_PERIODIC) {
		dst_rq = cpu_rq(lightest_cpu);
		dst_cpu = lightest_cpu;
	} else {
		dst_rq = this_rq;
		dst_cpu = this_cpu;
	}

	rcu_read_lock();

	/* Lock src_rq and find task that we should move */
	rq_lock_irqsave(src_rq, &rf);
	update_rq_clock(src_rq);

	p = pick_pushable_wfq_task(src_rq, dst_cpu, balance_type);

	/* Move the task from src_rq to dst_rq */
	if (p) {
		ret = 1;
		deactivate_task(src_rq, p, DEQUEUE_NOCLOCK);
		set_task_cpu(p, dst_cpu);
		rq_unlock(src_rq, &rf);

		// We did rcu_read_lock so p won't disappear after this line

		attach_task(dst_rq, p);

		local_irq_restore(rf.flags);
	} else
		rq_unlock_irqrestore(src_rq, &rf);
	rcu_read_unlock();

	return ret;
}

/*
 * idle_balance is called by schedule() if this_cpu is about to become
 * idle. Attempts to pull tasks from other CPUs.
 *
 * Returns:
 *   < 0 - we released the lock and there are !wfq tasks present
 *     0 - failed, no new tasks
 *   > 0 - success, new (wfq) tasks present
 */
static inline int newidle_balance(struct rq *this_rq, struct rq_flags *rf)
{
	int this_cpu = cpu_of(this_rq);
	int pulled_task = 0;

	rq_unlock(this_rq, rf);
	pulled_task = load_balance(this_cpu, this_rq, WFQ_BALANCE_IDLE);
	rq_lock(this_rq, rf);

	update_rq_clock(this_rq);

	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	if (this_rq->wfq.wfq_nr_running && !pulled_task)
		pulled_task = 1;

	/* Is there a task of a high priority class? */
	if (this_rq->nr_running != this_rq->wfq.wfq_nr_running)
		pulled_task = -1;

	return pulled_task;
}

/*
 * run_async_lb is triggered when needed from the scheduler tick for lb.
 */
static __latent_entropy void run_async_lb(struct softirq_action *h)
{
	struct rq *rq = this_rq();
	int cpu = cpu_of(rq);

	unsigned long flags;

	unsigned long next_bal_time;
	int do_periodic_lb = 0;

	write_lock_irqsave(&periodic_lb_info.lock, flags);
	next_bal_time = periodic_lb_info.last_bal_time +
		WFQ_PERIODIC_BALANCE_INTERVAL;

	if (time_after_eq(jiffies, next_bal_time)) {
		periodic_lb_info.last_bal_time = jiffies;
		do_periodic_lb = 1;
	}
	write_unlock_irqrestore(&periodic_lb_info.lock, flags);

	if (do_periodic_lb)
		load_balance(cpu, rq, WFQ_BALANCE_PERIODIC);
}

#else /* !CONFIG_SMP */

static inline int newidle_balance(struct rq *this_rq, struct rq_flags *rf)
{
	return 0;
}

#endif

static void update_curr_wfq(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;
	u64 now;

	if (unlikely(curr->sched_class != &wfq_sched_class))
		return;

	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = now;
	cgroup_account_cputime(curr, delta_exec);
}

static void enqueue_task_wfq(struct rq *rq, struct task_struct *p, int flags)
{
	struct wfq_rq *wfq_rq = &rq->wfq;
	struct sched_wfq_entity *wfq_se = &p->wfq;

	/* Make sure weight is up to date */
	update_task_weight_wfq(p);

	++wfq_rq->wfq_nr_running;
	wfq_rq->total_weight += wfq_se->weight;

	/* Update task's vruntime to rq's vruntime */
	wfq_se->vruntime = wfq_rq->vruntime;

	list_add_tail(&wfq_se->entity_list, &wfq_rq->wfq_rq_list);
	add_nr_running(rq, 1);
}

static void dequeue_task_wfq(struct rq *rq, struct task_struct *p, int flags)
{
	struct wfq_rq *wfq_rq = &rq->wfq;
	struct sched_wfq_entity *wfq_se = &p->wfq;

	update_curr_wfq(rq);

	if (wfq_rq->wfq_nr_running) {
		--wfq_rq->wfq_nr_running;
		wfq_rq->total_weight -= wfq_se->weight;

		list_del(&wfq_se->entity_list);
		sub_nr_running(rq, 1);
	}
}

static void yield_task_wfq(struct rq *rq)
{
}

/* Check if p has lower VFT than rq_curr, in which case p preempts rq->curr */
static void check_preempt_curr_wfq(struct rq *rq, struct task_struct *p,
				   int flags)
{
	struct task_struct *curr = rq->curr;

	u64 p_vft = compute_task_vft(p);
	u64 curr_vft = compute_task_vft(curr);

	if (p_vft < curr_vft) {
		resched_curr(rq);
		return;
	}
}

/* Find sched_wfq_entity of task with lowest VFT */
static struct sched_wfq_entity *se_with_min_vft(struct wfq_rq *wfq_rq)
{
	struct sched_wfq_entity *min_se;
	u64 min_vft = ULONG_MAX;

	struct sched_wfq_entity *se;
	u64 vft;

	list_for_each_entry(se, &wfq_rq->wfq_rq_list, entity_list) {
		vft = compute_vft(se);

		if (vft < min_vft) {
			min_vft = vft;
			min_se = se;
		}
	}

	return min_se;
}

struct task_struct *pick_next_task_wfq(struct rq *rq,
	struct task_struct *prev, struct rq_flags *rf)
{
	struct sched_wfq_entity *min_se;
	struct wfq_rq *wfq_rq = &rq->wfq;
	struct task_struct *p;
	int new_tasks;

again:
	if (!sched_wfq_runnable(rq)) {
		if (!rf)
			return NULL;

		new_tasks = newidle_balance(rq, rf);

		/*
		 * Because newidle_balance() releases (and re-acquires) rq->lock, it is
		 * possible for any higher priority task to appear. In that case we
		 * must re-start the pick_next_entity() loop.
		 */
		if (new_tasks < 0)
			return RETRY_TASK;

		if (new_tasks > 0)
			goto again;

		return NULL;
	}

	if (prev)
		put_prev_task(rq, prev);

	/* Find and return task with minimum vft */
	min_se = se_with_min_vft(wfq_rq);
	p = wfq_task_of(min_se);

	p->se.exec_start = rq_clock_task(rq);

	return p;
}

static struct task_struct *__pick_next_task_wfq(struct rq *rq)
{
	return pick_next_task_wfq(rq, NULL, NULL);
}

static void put_prev_task_wfq(struct rq *rq, struct task_struct *p)
{
	update_curr_wfq(rq);
}

#ifdef CONFIG_SMP

/* Select rq to which p should be added. Should be rq of the lightest CPU. */
static int select_task_rq_wfq(struct task_struct *p, int task_cpu,
			      int sd_flag, int flags)
{
	int min_total_weight = INT_MAX;
	int cpu = 0;
	int lightest_cpu = -1;

	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		if (!cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;

		raw_spin_lock(&rq->lock);

		if (rq->wfq.total_weight < min_total_weight) {
			min_total_weight = rq->wfq.total_weight;
			lightest_cpu = cpu;
		}

		raw_spin_unlock(&rq->lock);
	}

	return lightest_cpu;
}

/*
 * Perform balance.
 * Called from put_prev_task_balance() in pick_next_task() in core.c.
 */
static int balance_wfq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	if (rq->wfq.wfq_nr_running)
		return 1;

	return newidle_balance(rq, rf) != 0;
}

#endif /* CONFIG_SMP */

static void set_next_task_wfq(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq_clock_task(rq);
}

/*
 * If wfq_se (sched_wfq_entity of currently running task) does not
 * belong to the task with the lowest VFT, reschedule.
 */
static void
check_preempt_tick(struct wfq_rq *wfq_rq, struct sched_wfq_entity *wfq_se)
{
	struct sched_wfq_entity *min_se = se_with_min_vft(wfq_rq);

	if (min_se != wfq_se)
		resched_curr(wfq_rq_of(wfq_rq));
}

/*
 * Every kernel tick, task_tick_wfq is called.
 */
static void task_tick_wfq(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_wfq_entity *wfq_se = &p->wfq;
	struct wfq_rq *wfq_rq = &rq->wfq;

	update_curr_wfq(rq);

	/* Make sure weight is up to date */
	wfq_rq->total_weight -= wfq_se->weight;
	update_task_weight_wfq(p);
	wfq_rq->total_weight += wfq_se->weight;

	/* Update task and rq vruntimes */
	wfq_se->vruntime += WFQ_VRUNTIME_PER_TICK / wfq_se->weight;
	wfq_rq->vruntime += WFQ_VRUNTIME_PER_TICK / wfq_rq->total_weight;

	/* Check if we should preempt p */
	if (wfq_rq->wfq_nr_running > 1)
		check_preempt_tick(wfq_rq, wfq_se);
}

static void prio_changed_wfq(struct rq *rq, struct task_struct *p, int oldprio)
{
}

/*
 * p just switched to WFQ.
 * Check if p should preempt rq->curr or if we should reschedule
 */
static void switched_to_wfq(struct rq *rq, struct task_struct *p)
{
	if (task_on_rq_queued(p) && rq->curr != p) {
		if (wfq_task(rq->curr))
			check_preempt_curr_wfq(rq, p, 0);
		else if (!dl_task(rq->curr) && !rt_task(rq->curr))
			resched_curr(rq);
	}
}

const struct sched_class wfq_sched_class
	__section("__wfq_sched_class") = {
	.enqueue_task		= enqueue_task_wfq,
	.dequeue_task		= dequeue_task_wfq,
	.yield_task		= yield_task_wfq,
	.check_preempt_curr	= check_preempt_curr_wfq,
	.pick_next_task		= __pick_next_task_wfq,
	.put_prev_task		= put_prev_task_wfq,
#ifdef CONFIG_SMP
	.balance		= balance_wfq,
	.select_task_rq		= select_task_rq_wfq,
	.set_cpus_allowed       = set_cpus_allowed_common,
#endif
	.set_next_task		= set_next_task_wfq,
	.task_tick		= task_tick_wfq,
	.prio_changed		= prio_changed_wfq,
	.switched_to		= switched_to_wfq,
	.update_curr		= update_curr_wfq,
};

/*
 * Trigger the SCHED_WFQ_SOFTIRQ if it is time to do periodic load balancing.
 */
void trigger_load_balance_wfq(struct rq *rq)
{
	unsigned long flags;
	unsigned long next_bal_time;

	/* Has it been 500 ms since the last periodic balance? */
	read_lock_irqsave(&periodic_lb_info.lock, flags);
	next_bal_time = periodic_lb_info.last_bal_time +
		WFQ_PERIODIC_BALANCE_INTERVAL;

	if (time_after_eq(jiffies, next_bal_time)) {
		read_unlock_irqrestore(&periodic_lb_info.lock, flags);

		/* Run run_async_lb (periodic load balance) */
		raise_softirq(SCHED_WFQ_SOFTIRQ);
	} else
		read_unlock_irqrestore(&periodic_lb_info.lock, flags);
}

__init void init_sched_wfq_class(void)
{
#ifdef CONFIG_SMP
	/* This creates a thread that will run periodic load balance */
	open_softirq(SCHED_WFQ_SOFTIRQ, run_async_lb);
#endif /* SMP */
}
