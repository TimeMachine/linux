#include "sched.h"
#include <linux/cpufreq.h>
#define _debug


static void main_schedule(void);

static void update_curr_energy(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;
	u64 e_exec;
	
	//update the energy info.
	e_exec = rq->clock_task - curr->ee.execute_start;
	if (unlikely((s64)e_exec < 0))
		e_exec = 0;
	curr->ee.total_execution += e_exec;		
	curr->ee.execute_start = rq->clock_task;

	//update the curr info.		
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
	next->ee.execute_start = rq->clock_task;
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
	main_schedule();
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
	main_schedule();
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
	main_schedule();
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

extern unsigned int get_stats_table(int cpu, unsigned int **freq);
static void get_cpu_frequency(int cpu)
{
	struct energy_rq *e_rq = &cpu_rq(cpu)->energy;
	int i = 0;
	e_rq->state_number = get_stats_table(0, &e_rq->freq);		
	for (i = 0; i < e_rq->state_number; i++) {
		printk("cpu:%d  freq[%d] %d\n", cpu, i, e_rq->freq[i]);
	}
		
}

static void set_cpu_frequency(void)
{
	
}

static void main_schedule(void)
{
	u64 slice_start =  cpu_rq(0)->clock_task;
	struct rq *i_rq;
	int i = 0;
	printk("%s\n",__PRETTY_FUNCTION__);
	// update all job data and then use them to predict.
	for (i = 0 ;i < NR_CPUS; i++) {
		i_rq = cpu_rq(i);
		if (i_rq->energy.energy_nr_running != 0)
			update_curr_energy(i_rq);
		i_rq->energy.timeslice_start = slice_start;
		
		// for the first time (init cpu freq)
		if (unlikely(i_rq->energy.freq == NULL))
			get_cpu_frequency(i);
	}	
	workload_prediction();
	algo();
	set_cpu_frequency();
}

static void task_tick_energy(struct rq *rq, struct task_struct *curr, int queued)
{
	int cpu = smp_processor_id();
	if (cpu == 0) {
		if (rq->clock_task - rq->energy.timeslice_start >= NSEC_PER_SEC) {
#ifdef _debug
			printk("%s begin\n",__PRETTY_FUNCTION__);
#endif
			printk("clock: %llu |timeslice:%llu |HZ:%u\n",rq->clock_task, rq->energy.timeslice_start ,HZ);
			// reschedule because of the time slice 
			main_schedule();
#ifdef _debug
			printk("%s end\n",__PRETTY_FUNCTION__);
#endif
		}
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
