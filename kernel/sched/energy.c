#include "sched.h"
#define _debug

static void update_curr_energy(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;
	u64 e_exec;
	
	//update energy info.
	e_exec = rq->clock_task - curr->ee.timeslice_start;
	if (unlikely((s64)e_exec < 0))
		e_exec = 0;
	curr->ee.timeslice_execution[rq->cpu] += e_exec;
	curr->ee.total_execution += e_exec;		
	
	//update curr info.
	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
			max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq->clock_task;
	cpuacct_charge(curr, delta_exec);
}

static inline int on_energy_rq(struct sched_energy_entity *ee)
{
    return !list_empty(&ee->list_item);
}


#ifdef CONFIG_SMP
static int
select_task_rq_energy(struct task_struct *p, int sd_flag, int flags)
{
	return task_cpu(p); 
}
#endif /* CONFIG_SMP */

static void
check_preempt_curr_energy(struct rq *rq, struct task_struct *p, int flags)
{
	/* we're never preempted */
}

static struct task_struct *pick_next_task_energy(struct rq *rq)
{
	struct task_struct *next;
	struct sched_energy_entity *next_ee;
	struct energy_rq *e_rq = &(rq->energy);
	if(e_rq->energy_nr_running == 0)
		return NULL;
	next_ee = list_entry(e_rq->queue.next, struct sched_energy_entity, list_item);
	next = container_of(next_ee, struct task_struct, ee);
	next->se.exec_start = rq->clock_task;
#ifdef _debug
	printk("%s end\n",__PRETTY_FUNCTION__);
#endif
	return next;
}

static void
enqueue_task_energy(struct rq *rq, struct task_struct *p, int flags)
{
#ifdef _debug
	printk("%s begin\n",__PRETTY_FUNCTION__);
#endif
	list_add_tail(&(p->ee.list_item),&(rq->energy.queue));
	p->ee.rq_e = &rq->energy;
	rq->energy.energy_nr_running++;
	inc_nr_running(rq);
#ifdef _debug
	printk("%s end\n",__PRETTY_FUNCTION__);
#endif
}

static void
dequeue_task_energy(struct rq *rq, struct task_struct *p, int flags)
{
#ifdef _debug
	printk("%s begin\n",__PRETTY_FUNCTION__);
#endif
	update_curr_energy(rq);
	list_del(&(p->ee.list_item));
	rq->energy.energy_nr_running--;
	dec_nr_running(rq);
#ifdef _debug
	printk("%s end\n",__PRETTY_FUNCTION__);
#endif
}

static void requeue_task_energy(struct rq *rq, struct task_struct *p)
{
	if (on_energy_rq(&p->ee)) {
		list_move_tail(&p->ee.list_item, &rq->energy.queue);
	}
}

static void yield_task_energy(struct rq *rq)
{
	requeue_task_energy(rq,rq->curr);
}

static void put_prev_task_energy(struct rq *rq, struct task_struct *prev)
{
#ifdef _debug
	printk("%s begin\n",__PRETTY_FUNCTION__);
#endif
	update_curr_energy(rq);
	//prev->se.exec_start = 0;
#ifdef _debug
	printk("%s end\n",__PRETTY_FUNCTION__);
#endif
}

static void workload_prediction(void)
{
	
}

static void algo(void)
{
	
}

static void get_cpu_frequency(void)
{
	
}

static void set_cpu_frequency(void)
{
	
}

static void task_tick_energy(struct rq *rq, struct task_struct *curr, int queued)
{
	int cpu = smp_processor_id();
	if (cpu == 0) {
		struct energy_rq *e_rq;
		struct task_struct *i_curr;
		struct list_head *head;
		struct list_head *pos;
		struct sched_energy_entity *data;
		u64 delta_exec;
		int i = 0;
		
		for (i = 0 ;i < NR_CPUS; i++) {
			e_rq = &cpu_rq(i)->energy;
			if (e_rq->energy_nr_running != 0) {
				// update per CPU's current execution job info.
				i_curr = cpu_rq(i)->curr;
				delta_exec = rq->clock_task - i_curr->ee.timeslice_start;
				i_curr->ee.timeslice_execution[i] += delta_exec;
				i_curr->ee.total_execution += delta_exec;		
				
				// update all job start time.	
				head = &e_rq->queue;
				for (pos = head->next; pos != head; pos = pos->next) {
					data = list_entry(pos, struct sched_energy_entity, list_item);
					data->timeslice_start = rq->clock_task;
				}
			}
		}	
		get_cpu_frequency();
		workload_prediction();
		algo();
		set_cpu_frequency();
	}
}

static void set_curr_task_energy(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq->clock_task;
}

static void switched_to_energy(struct rq *rq, struct task_struct *p)
{
	if (rq->curr == p)
		resched_task(rq->curr);	
}

static void
prio_changed_energy(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static unsigned int
get_rr_interval_energy(struct rq *rq, struct task_struct *task)
{
	return 0;
}

const struct sched_class energy_sched_class = {
	.next			= &rt_sched_class,

	.enqueue_task		= enqueue_task_energy,
	.dequeue_task		= dequeue_task_energy,
	.yield_task		= yield_task_energy,

	.check_preempt_curr	= check_preempt_curr_energy,

	.pick_next_task		= pick_next_task_energy,
	.put_prev_task		= put_prev_task_energy,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_energy,
#endif

	.set_curr_task          = set_curr_task_energy,
	.task_tick		= task_tick_energy,

	.get_rr_interval	= get_rr_interval_energy,

	.prio_changed		= prio_changed_energy,
	.switched_to		= switched_to_energy,
};
