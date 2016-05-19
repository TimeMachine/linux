#include "sched.h"
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
//#define _debug
#define _sched_debug
#define kHZ 1000
#define odroid_xu

#ifdef odroid_xu
#define _1s_jiffies 24000000
static u32 freq_now;
#define jiffies_to_cycle(jiffies)  (jiffies / (_1s_jiffies / kHZ)) * freq_now
#define cycle_to_jiffies(cycle,freq)  (cycle / freq) * (_1s_jiffies / kHZ)
#endif

// lock by main_schedule
static DEFINE_SPINLOCK(mr_lock);
static DEFINE_SPINLOCK(queue_lock);
static int need_reschedule = 0;
static struct hrtimer hr_timer;
static int total_job = 0;

extern unsigned int get_stats_table(int cpu, unsigned int **freq);
//extern void change_governor_userspace(int cpu);
extern struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu);
static void main_schedule(int workload_predict);

static void get_cpu_frequency(int cpu)
{
	struct energy_rq *e_rq = &cpu_rq(cpu)->energy;

	//change_governor_userspace(cpu);

	e_rq->state_number = get_stats_table(cpu, &e_rq->freq);		
	e_rq->state_number = 9;
	/*int i = 0;	
	for (i = 0; i < e_rq->state_number; i++) {
		printk("cpu:%d  freq[%d] %d\n", cpu, i, e_rq->freq[i]);
	}*/
}

static void set_cpu_frequency(unsigned int c, unsigned int freq)
{
	int cpu = 0; //odroid-xu desgined.
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);		
	//struct rq* rq = cpu_rq(smp_processor_id());

	__cpufreq_driver_target(policy, freq, CPUFREQ_RELATION_L);
	//policy->governor->store_setspeed(policy, freq);
	cpufreq_cpu_put(policy);
}

static enum hrtimer_restart sched_period_timer(struct hrtimer *timer)
{
	ktime_t ktime;
	//get_cycles()
	printk("[period] cycle:%u\n",cpu_cycle());

	ktime = ktime_set(0, NSEC_PER_SEC);

	spin_lock(&mr_lock);
	need_reschedule = 0;
	main_schedule(true);
	spin_unlock(&mr_lock);
	
	hrtimer_forward_now(timer, ktime);
	
	return HRTIMER_RESTART;
}

static void update_credit(struct task_struct *curr)
{
	if (curr->ee.execute_start != 0) {
		int cpu = task_cpu(curr);
		int executionTime = cpu_cycle() - curr->ee.execute_start;
		curr->ee.total_execution += executionTime;
		if (curr->ee.credit[cpu] > executionTime)
 			curr->ee.credit[cpu] -= executionTime; 
		else
			curr->ee.credit[cpu] = 0;
	}		
	curr->ee.execute_start = cpu_cycle();
}

static void update_curr_energy(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;
	
	//update the energy info.
	update_credit(curr);
	rq->energy.time_sharing = rq->clock_task;

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

static void reschedule(int cpu)
{
	struct rq *i_rq = cpu_rq(cpu);
	//if (cpu == smp_processor_id()) 
	if (cpu == smp_processor_id() && raw_spin_is_locked(&i_rq->lock))
		resched_task(i_rq->curr);
	//else if(i_rq->curr == i_rq->idle) 
	//	wake_up_idle_cpu(cpu);
	else
		resched_cpu(cpu);
}

static void move_task_to_rq(struct rq *rq, struct sched_energy_entity *task ,int set)
{
	if (task_contributes_to_load(task->instance))
		task->rq_e->rq->nr_uninterruptible++;
	list_del(&task->list_item);
	task->rq_e->energy_nr_running--;
	dec_nr_running(task->rq_e->rq);
	
	//printk("before move pid:%d energy_nr:%lu nr:%lu task_contributes_to_load:%d\n",task->instance->pid, task->rq_e->energy_nr_running, task->rq_e->rq->nr_running, task_contributes_to_load(task->instance));
	//deactivate_task(task->rq_e->rq, task->instance, 1);
	if (set)
		set_task_cpu(task->instance, rq->cpu);
	//activate_task(rq, task->instance, 1);
	
	if (task_contributes_to_load(task->instance))
		rq->nr_uninterruptible--;
	list_add_tail(&task->list_item,&rq->energy.queue);
	task->rq_e = &rq->energy;
	task->rq_e->energy_nr_running++;
	inc_nr_running(rq);
	
	//printk("after move pid:%d energy_nr:%lu nr:%lu task_contributes_to_load:%d\n",task->instance->pid, task->rq_e->energy_nr_running, task->rq_e->rq->nr_running, task_contributes_to_load(task->instance));
}

#ifdef CONFIG_SMP
static int
select_task_rq_energy(struct task_struct *p, int sd_flag, int flags)
{
	return task_cpu(p); 
}

static void post_schedule_energy(struct rq *rq)
{
	rq->curr->ee.select = 0;
}

static void pre_schedule_energy(struct rq *rq, struct task_struct *prev)
{
#ifdef _debug
	printk("cpu:%d, %s ,pid:%d ,need_move:%d\n",smp_processor_id(),__PRETTY_FUNCTION__,prev->pid,prev->ee.need_move);
#endif
	if (prev->ee.need_move >= 0) {
		if (prev->on_rq) {
#ifdef _debug
			printk("[need move] :%d\n",prev->ee.need_move);
#endif
			spin_lock(&queue_lock);	
			move_task_to_rq(cpu_rq(prev->ee.need_move) ,&prev->ee ,0);
			prev->ee.need_move = -1;
			reschedule(prev->ee.need_move);
			spin_unlock(&queue_lock);	
		}
		else
			prev->ee.need_move = -1;
	}

	if (unlikely(rq->energy.set_freq != -1)) {	
		set_cpu_frequency(rq->cpu, rq->energy.set_freq);	
#ifdef odroid_xu
		freq_now = rq->energy.set_freq;
#endif
		rq->energy.set_freq = -1;
	}
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
	struct energy_rq *e_rq;
	struct list_head *pos;
	int find = 0, i = 0, j = 0;
	int try_cpu[4] = {0};
	/*
	 * Priority : split task -> non-split task -> other CPU run queue
	 * try_cpu  :		0				1			    2 , 3
	 */
	try_cpu[0] = rq->cpu; 
	try_cpu[1] = rq->cpu;
	// try to steal other run queue
	try_cpu[2] = (rq->cpu - 1 + NR_CPUS) % NR_CPUS; 
	try_cpu[3] = (rq->cpu + 1) % NR_CPUS; 

	for (i = 0; i < 4; i++) {
		e_rq = &cpu_rq(try_cpu[i])->energy;
		spin_lock(&queue_lock);
		if(e_rq->energy_nr_running == 0){
			spin_unlock(&queue_lock);	
			continue;
		}
		pos = e_rq->queue.next;
		for (j = 0; pos != &e_rq->queue; pos = pos->next) {
			next_ee = list_entry(pos, struct sched_energy_entity, list_item);
			if (task_cpu(next_ee->instance) != try_cpu[i] && 
			   (cpu_rq(task_cpu(next_ee->instance))->curr->pid == next_ee->instance->pid || 
			    raw_spin_is_locked(&cpu_rq(task_cpu(next_ee->instance))->lock)))
				continue;
			if (!next_ee->credit[rq->cpu])
				continue;
			if (i <= 1) {
				if (next_ee->need_move != -1) {
					pos = pos->next;
					move_task_to_rq(cpu_rq(next_ee->need_move) ,next_ee ,0);
					next_ee->need_move = -1;
					reschedule(next_ee->need_move);
					pos = pos->prev;
					continue;
				}
				if ((i == 0 && next_ee->split) || i == 1) {
					find = 1;
					break;
				}
			}
			else if (!raw_spin_is_locked(&cpu_rq(try_cpu[i])->lock) && 
					next_ee->instance->pid != cpu_rq(try_cpu[i])->curr->pid &&
					next_ee->select == 0 && next_ee->instance->state == TASK_RUNNING &&
					next_ee->need_move == -1) {
				printk("steal | i:%d, next-pid:%d, curr-pid:%d, j:%d\n",try_cpu[i],next_ee->instance->pid, cpu_rq(try_cpu[i])->curr->pid,j);
				//steal other cpu run queue.
				move_task_to_rq(rq ,next_ee ,1);
				find = 1;
				break;
			}
			//printk("[pick] cpu:%d try:%d pid:%d loop:%d need_move:%d state:%ld\n",rq->cpu,try_cpu[i],next_ee->instance->pid,j++,next_ee->need_move ,next_ee->instance->state);
		}
		if(find == 1) {
			// To be executed task has put first entry.
			next_ee->select = 1;
			printk("[find] pid:%d tsak_cpu:%d cpu:%d credit:%u\n",next_ee->instance->pid ,task_cpu(next_ee->instance), rq->cpu, next_ee->credit[rq->cpu]);
			if (task_cpu(next_ee->instance) != try_cpu[i]){
				set_task_cpu(next_ee->instance, rq->cpu);
			}
			spin_unlock(&queue_lock);	
			break;
		}
		spin_unlock(&queue_lock);	
	}
	if (find == 0) {
#ifdef _debug
		if (rq->curr->policy == 6)
			printk("[pick NULL] cpu:%d \n",smp_processor_id());
#endif
		return NULL;
	}
	next = container_of(next_ee, struct task_struct, ee);
	next->se.exec_start = rq->clock_task;
	next->ee.execute_start = cpu_cycle();
	rq->post_schedule = 1;
#ifdef _debug
	printk("cpu:%d, %s ,pid:%d,end\n",smp_processor_id(),__PRETTY_FUNCTION__,next->pid);
#endif
	return next;
}

static void
enqueue_task_energy(struct rq *rq, struct task_struct *p, int flags)
{
	int i = 0;
#ifdef _debug
	printk("cpu:%d, %s ,pid:%d begin\n",smp_processor_id(),__PRETTY_FUNCTION__,p->pid);
#endif
	// for the first time (init cpu freq)
	if ( unlikely(rq->energy.freq == NULL) ) {
		for (i = 0; i < NR_CPUS; i++)  
			get_cpu_frequency(i);
#ifdef odroid_xu
		freq_now = rq->energy.freq[0];
#endif
	}
	spin_lock(&queue_lock);
	list_add_tail(&(p->ee.list_item),&(rq->energy.queue));
	p->ee.rq_e = &rq->energy;
	rq->energy.energy_nr_running++;
	inc_nr_running(rq);

	if(flags == 0 && p->ee.first == 1) {
	//update the new task info.
		p->ee.workload = rq->energy.freq[rq->energy.state_number / 2] * kHZ;
		p->ee.credit[rq->cpu] = cycle_to_jiffies(p->ee.workload, p->ee.workload);
		printk("[enqueue] new job pid:%d, total_job:%d\n",p->pid, total_job);
		total_job++;
	
		if (total_job == 1) {
			ktime_t ktime;
			ktime = ktime_set(0, NSEC_PER_SEC);
			hrtimer_start( &hr_timer, ktime, HRTIMER_MODE_REL);
		}
		need_reschedule = 1;
		p->ee.first = 0;
	}
	spin_unlock(&queue_lock);	
#ifdef _debug
	printk("[debug en] pid:%d prev->state:%ld flag:%d\n", p->pid, p->state, flags);
#endif
}

static void
dequeue_task_energy(struct rq *rq, struct task_struct *p, int flags)
{
#ifdef _debug
	printk("cpu:%d, %s ,pid:%d begin\n",smp_processor_id(),__PRETTY_FUNCTION__,p->pid);
#endif
	update_curr_energy(rq);

	spin_lock(&queue_lock);	
	list_del(&(p->ee.list_item));
	rq->energy.energy_nr_running--;
	dec_nr_running(rq);
	spin_unlock(&queue_lock);	

	if(flags) {
		if (p->state == TASK_DEAD) {
			total_job--;
			if (total_job == 0) 
				hrtimer_cancel(&hr_timer);
		}
		need_reschedule = 1;
	}
#ifdef _debug
	printk("[debug de] pid:%d prev->state:%ld flag:%d\n", p->pid, p->state, flags);
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
	spin_lock(&queue_lock);	
	requeue_task_energy(rq,rq->curr);
	spin_unlock(&queue_lock);	
	need_reschedule = 1;
}

static void put_prev_task_energy(struct rq *rq, struct task_struct *prev)
{
#ifdef _debug
	//printk("cpu:%d, %s\n",smp_processor_id() ,__PRETTY_FUNCTION__);
#endif
	update_curr_energy(rq);
}

static void workload_prediction(void)
{
	struct rq *i_rq;
	int i = 0;
	struct list_head *head;
	struct sched_energy_entity *data;
	struct list_head *pos;

	spin_lock(&queue_lock);	
	for (i = 0 ;i < NR_CPUS; i++) {
		i_rq = cpu_rq(i);
		if (i_rq->energy.energy_nr_running != 0) {
			head = &i_rq->energy.queue;
			for(pos = head->next; pos != head; pos = pos->next) {
				data = list_entry(pos ,struct sched_energy_entity, list_item);
				// predict workload from the statistics.
				printk("pid:%d, cpu:%d, exeute_start:%u, total_execution:%u, workload:%u, workload(jiffies):%u\n",data->instance->pid , i, data->execute_start ,data->total_execution, data->workload, cycle_to_jiffies(data->workload, freq_now));
				// for the newly job
				if (data->total_execution == 0) {
					data->workload = i_rq->energy.freq[i_rq->energy.state_number / 2] * kHZ; // kHz -> Hz
					data->over_predict = 0;
				}
				else if (jiffies_to_cycle(data->total_execution) >= data->workload) {
					if (data->over_predict) 
						data->workload = (i_rq->energy.freq[0] + data->workload / kHZ) / 2 * kHZ;
					else
						data->workload += 100000 * kHZ;
					data->over_predict++;
				}
				else {
#ifdef odroid_xu
					data->workload = jiffies_to_cycle(data->total_execution);
#else
					data->workload = data->total_execution;
#endif
					data->over_predict = 0;
				}
				// reset the statistics.
				data->total_execution = 0;
			}
		}
	}

	spin_unlock(&queue_lock);	
}

static int compare(const void *a, const void *b)
{
	struct sched_energy_entity * const *ippa = a;	
	struct sched_energy_entity * const *ippb = b;	
	const struct sched_energy_entity *ipa = *ippa;	
	const struct sched_energy_entity *ipb = *ippb;	
	if (ipa->dummy_workload > ipb->dummy_workload)
		return -1;
	if (ipa->dummy_workload < ipb->dummy_workload)
		return 1;
	return 0;
}

static void algo(int workload_predict)
{
	u64 total_workload = 0;
	unsigned int o_freq[NR_CPUS] = {0};
	#define Max_jobs 1000
	struct sched_energy_entity **data = (struct sched_energy_entity **)kmalloc(sizeof(struct sched_energy_entity *)*Max_jobs, __GFP_IO | __GFP_FS);
	int job_count = 0;
	struct rq *i_rq;
	struct list_head *head;
	struct list_head *pos;
	int i = 0 ,j = 0, k = 0;
	#define soft_float 1000000
	int ptr = 0;
	int ptr_max = 0;
	unsigned int pre_load = 0;
	int o_cpu = 0;
	int max_freq = 0;

	spin_lock(&queue_lock);	
	for (i = 0 ;i < NR_CPUS; i++) {
		i_rq = cpu_rq(i);
		if (i_rq->energy.energy_nr_running != 0) {
			head = &i_rq->energy.queue;
			for(pos = head->next; pos != head; pos = pos->next) {
				data[job_count] = list_entry(pos ,struct sched_energy_entity, list_item);
				//if (data[job_count]->instance->state == TASK_RUNNING) {
				if (data[job_count]->instance->state == TASK_RUNNING && 
					data[job_count]->workload > jiffies_to_cycle(data[job_count]->total_execution)) {
					if (workload_predict)
						data[job_count]->dummy_workload = data[job_count]->workload;
					else
						data[job_count]->dummy_workload = data[job_count]->workload - jiffies_to_cycle( data[job_count]->total_execution);
					//data[job_count]->dummy_workload = data[job_count]->workload;
					total_workload += data[job_count]->dummy_workload;
					for (k = 0 ;k < NR_CPUS; k++) 
						data[job_count]->credit[k] = 0;
					data[job_count]->split = 0;
					job_count++;
				}
			}
		}
	}
	if (job_count == 0) {
		spin_unlock(&queue_lock);	
		return;
	}
	sort(data, job_count, sizeof(struct sched_energy_entity*), compare, NULL);
#ifdef _sched_debug
	printk("========= input===========\n");
	for(i=0;i<NR_CPUS;i++){
		printk("CPU %d:", i);
		for(j=0;j<cpu_rq(i)->energy.state_number;j++)
			printk(" %4d", cpu_rq(i)->energy.freq[j]);
		printk("\n");
	}
	printk("job workload:");
	for(j=0;j<job_count;j++)
		printk("  (%d) %4d", data[j]->instance->pid, data[j]->dummy_workload);
	printk("\n");
#endif
	for (i = 0; i < NR_CPUS; i++) {
		int f_total = 0;
		int a_jp = 0;
		i_rq = cpu_rq(i);
		if (total_workload == 0) {
			o_freq[i] = 0;
			continue;
		}
		for (j = 0; j < i_rq->energy.state_number; j++) {
			BUG_ON(ptr_max >= job_count);
			if (((u64) i_rq->energy.freq[j] * kHZ < data[ptr_max]->dummy_workload) ||
				((u64) i_rq->energy.freq[j] * kHZ * (NR_CPUS - i) < total_workload) ||
				((1 * soft_float - pre_load) < (data[ptr]->dummy_workload) / (i_rq->energy.freq[j] * kHZ / soft_float) ))
				break;
		}
		//printk("__debug cpu_rq(i)->energy.freq[j]:%d,data[ptr_max]->workload:%u,total_workload:%llu,pre_load:%u\n",cpu_rq(i)->energy.freq[j],data[ptr_max]->dummy_workload,total_workload,pre_load);
		o_freq[i] = j == 0 ? i_rq->energy.freq[j] : i_rq->energy.freq[j-1];  	
		f_total = o_freq[i] * kHZ;
		a_jp = 0;
		while ( ptr < job_count ) {
			int state = 0;
			if (f_total >= data[ptr]->dummy_workload)
				state = 1;
			a_jp = state ? data[ptr]->dummy_workload : f_total;
			total_workload -= a_jp;
			f_total -= a_jp;
			data[ptr]->credit[i] = cycle_to_jiffies(a_jp, o_freq[i]);
			if (state) {
				if (data[ptr]->rq_e->rq->cpu != i && data[ptr]->instance->on_rq) {
					// if task is scheduled to other cpu, task have to move to other queue.
					printk("[algo] pid:%d ,from:%d ,to:%d\n",data[ptr]->instance->pid,data[ptr]->rq_e->rq->cpu,i_rq->cpu);
					if (data[ptr]->select == 0 && data[ptr]->instance->pid != data[ptr]->rq_e->rq->curr->pid) {
						if (data[ptr]->instance->pid != cpu_rq(task_cpu(data[ptr]->instance))->curr->pid)
							move_task_to_rq(i_rq ,data[ptr] ,1);
						else
							data[ptr]->need_move = i;
							//move_task_to_rq(i_rq ,data[ptr] ,0);
					}
					else {
						data[ptr]->need_move = i;
					}
					printk("[algo] select:%d from_pid:%d true_pid:%d\n",data[ptr]->select,data[ptr]->rq_e->rq->curr->pid,cpu_rq(task_cpu(data[ptr]->instance))->curr->pid);
				}
				ptr++;
				ptr_max = ptr;
				pre_load = 1 * soft_float;
			}
			else {
				data[ptr]->dummy_workload -= a_jp;
				data[ptr]->split = 1;
				pre_load = a_jp / (o_freq[i] * kHZ / soft_float);
				if (ptr + 1 < job_count && 
					data[ptr]->dummy_workload < data[ptr+1]->dummy_workload)
					ptr_max++;
				break;
			}
		}
	}
	spin_unlock(&queue_lock);	
#ifdef _sched_debug
	printk("=========output===========\n");
	for(i=0;i<NR_CPUS;i++)
		printk("CPU:%d freq:%d\n",i , o_freq[i]);
	for(i=0;i<job_count;i++){
		printk("job:%d, credit:", i);
		for(j=0;j<NR_CPUS;j++)
			printk(" %4d", data[i]->credit[j]);
		printk("\n");
	}
#endif
	kfree(data);

	// symmetric freq.
	for(i = 0; i < NR_CPUS; i++) {
		if (max_freq < o_freq[i]) { 
			o_cpu = i;
			max_freq = o_freq[i];
		}
	}
#ifdef _sched_debug
	printk("o_cpu:%u, max_freq:%u\n", o_cpu, max_freq);
#endif
	cpu_rq(0)->energy.set_freq = max_freq;	
	//set_cpu_frequency(o_cpu, max_freq);	

	for (i = 0 ;i < NR_CPUS; i++) {
		i_rq = cpu_rq(i);
#ifdef _sched_debug
		printk("resched %d, pid:%d\n",i,i_rq->curr->pid);
#endif
		if (i_rq->curr->ee.credit[i] == 0){
			reschedule(i);
		}
	}
	
}

static void main_schedule(int workload_predict)
{
	struct rq *i_rq;
	int i = 0;
	//printk("clock: %llu |timeslice:%llu |HZ:%u |workload_predict:%d\n",cpu_rq(smp_processor_id())->clock_task, cpu_rq(smp_processor_id())->energy.timeslice_start ,HZ, workload_predict);
	//preempt_disable();
	// update all job data and then use them to predict.
	for (i = 0 ;i < NR_CPUS; i++) {
		i_rq = cpu_rq(i);
		if (i_rq->energy.energy_nr_running != 0)
			update_curr_energy(i_rq);
	}	
	if (total_job) {
		if (workload_predict == true) 
			workload_prediction();
		//printk("[main_schedule] algo start\n");
		algo(workload_predict);
	}
}

static void task_tick_energy(struct rq *rq, struct task_struct *curr, int queued)
{
	if (spin_trylock(&mr_lock)) {
#ifdef _debug
		//printk("cpu:%d, %s, pid:%d\n",smp_processor_id() ,__PRETTY_FUNCTION__, curr->pid);
#endif
		//over scheduling time slice:
		/*if (rq->clock_task - timeslice_start >= NSEC_PER_SEC) {
			//printk("[tick] over 1s.\n");
			// reschedule because of the time slice 
			need_reschedule = 0;
			main_schedule(true);
			timeslice_start = rq->clock_task;
		}*/
			//printk("[tick] need_reschedule\n");
		if (need_reschedule) {
			need_reschedule = 0;
			main_schedule(false);
			spin_unlock(&mr_lock);
			return;
		}
		spin_unlock(&mr_lock);
	}
	update_credit(curr);
	if (!spin_is_locked(&mr_lock) && 
		((rq->clock_task - rq->energy.time_sharing >= 30 * USEC_PER_SEC)||
		curr->ee.credit[rq->cpu] == 0)) {
		// time sharing
		rq->energy.time_sharing = rq->clock_task;
		spin_lock(&queue_lock);	
		requeue_task_energy(rq,curr);
		spin_unlock(&queue_lock);	
		reschedule(rq->cpu);
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
	.pre_schedule       = pre_schedule_energy,
	.post_schedule      = post_schedule_energy,
#endif

	.set_curr_task          = set_curr_task_energy,
	.task_tick		= task_tick_energy,

	.get_rr_interval	= get_rr_interval_energy,

	.prio_changed		= prio_changed_energy,
	.switched_to		= switched_to_energy,
};

__init void init_sched_energy_class(void)
{
	hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hr_timer.function = sched_period_timer;
}
