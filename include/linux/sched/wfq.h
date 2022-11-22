#ifndef _SCHED_WFQ_H
#define _SCHED_WFQ_H

#define WFQ_PERIODIC_BALANCE_INTERVAL (500 * HZ / 1000)
#define WFQ_DEFAULT_WEIGHT 10
#define WFQ_VRUNTIME_PER_TICK (2 << 19)

struct periodic_lb_info {
	unsigned long last_bal_time;
	rwlock_t lock;
};

static inline int wfq_task(struct task_struct *p)
{
	if (likely(p->policy == SCHED_WFQ))
		return 1;
	return 0;
}

/*
 * set_task_weight_wfq is called from SYSCALL set_wfq_weight
 * to update p->wfq.new_weight to the weight that the user
 * passes in.
 */
static inline void set_task_weight_wfq(struct task_struct *p, int weight)
{
	atomic_set(&p->wfq.new_weight, weight);
}

/*
 * update_task_weight_wfq is called during enqueue and task_tick
 * in wfq.c to update p->wfq.weight to p->wfq.new_weight.
 */
static inline void update_task_weight_wfq(struct task_struct *p)
{
	p->wfq.weight = atomic_read(&p->wfq.new_weight);
}

#endif /* _SCHED_WFQ_H */
