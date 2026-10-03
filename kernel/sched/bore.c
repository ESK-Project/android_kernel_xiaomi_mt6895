/*
 * Burst-Oriented Response Enhancer (BORE) CPU Scheduler
 * Copyright (C) 2021-2025 Masahito Suzuki <firelzrd@gmail.com>
 */
#include <linux/sched/task.h>
#include <linux/sched/bore.h>
#include "sched.h"

DEFINE_STATIC_KEY_TRUE(sched_bore_key);
DEFINE_STATIC_KEY_TRUE(sched_burst_inherit_key);
DEFINE_STATIC_KEY_TRUE(sched_burst_ancestor_key);
DEFINE_STATIC_KEY_TRUE(sched_burst_protect_slice_cond_key);
DEFINE_STATIC_KEY_FALSE(sched_burst_protect_slice_prefer_key);

u8 __read_mostly sched_bore = 1;
#ifdef CONFIG_SYSCTL
static u8 __read_mostly sched_burst_inherit_type = 2;
static u8 __read_mostly sched_burst_protect_slice_lv = 1;
#endif
static u8 __read_mostly sched_burst_smoothness = 1;
static u8 __read_mostly sched_burst_penalty_offset = 24;
static unsigned int __read_mostly sched_burst_penalty_scale = 1536;
static unsigned int __read_mostly sched_burst_cache_lifetime = 75000000;

#define MAX_BURST_PENALTY ((40U << 8) - 1)
#define BURST_CACHE_SAMPLE_LIMIT 63
#define BURST_CACHE_SCAN_LIMIT (BURST_CACHE_SAMPLE_LIMIT * 2)

static u32 bore_reciprocal_lut[BURST_CACHE_SAMPLE_LIMIT + 1];

static u32 log2p1_u64_u32fp(u64 value, u8 fp)
{
	int clz, exponent;
	u32 mantissa;

	if (!value)
		return 0;
	clz = __builtin_clzll(value);
	exponent = 64 - clz;
	mantissa = (u32)((value << clz) << 1 >> (64 - fp));
	return exponent << fp | mantissa;
}

static u32 calc_burst_penalty(u64 burst_time)
{
	u32 greed = log2p1_u64_u32fp(burst_time, 8);
	u32 tolerance = sched_burst_penalty_offset << 8;
	u32 penalty, scaled_penalty;

	penalty = greed > tolerance ? greed - tolerance : 0;
	scaled_penalty = penalty * sched_burst_penalty_scale >> 10;
	return min(scaled_penalty, MAX_BURST_PENALTY);
}

static u64 rescale_slice(u64 delta, u8 old_prio, u8 new_prio)
{
	u64 unscaled;

	unscaled = mul_u64_u32_shr(delta, sched_prio_to_weight[old_prio], 10);
	return mul_u64_u32_shr(unscaled, sched_prio_to_wmult[new_prio], 22);
}

static u32 binary_smooth(u32 new, u32 old)
{
	u32 shift = sched_burst_smoothness;

	if (new <= old)
		return new;
	return old + ((new - old + (1U << shift) - 1) >> shift);
}

u8 effective_prio_bore(struct task_struct *p)
{
	int prio = p->static_prio - MAX_RT_PRIO;

	if (static_branch_likely(&sched_bore_key))
		prio += bore_score(p);
	return clamp(prio, 0, 39);
}

void reweight_task_bore(struct task_struct *p)
{
	struct load_weight lw;
	u8 prio;

	if (task_has_idle_policy(p))
		return;
	prio = effective_prio_bore(p);
	lw.weight = scale_load(sched_prio_to_weight[prio]);
	lw.inv_weight = sched_prio_to_wmult[prio];

	/* A new task has not attached its initial PELT load yet. */
	if (READ_ONCE(p->state) & TASK_NEW) {
		p->se.load = lw;
#ifdef CONFIG_SMP
		p->se.avg.load_avg = scale_load_down(lw.weight);
#endif
		return;
	}

	/* reweight_task() accounts PELT, lag and the Android reweight hook. */
	p->bore.stop_update = true;
	reweight_task(p, &lw);
	p->bore.stop_update = false;
}

static void update_penalty(struct task_struct *p)
{
	struct bore_ctx *ctx = &p->bore;
	u8 old_prio = effective_prio_bore(p);

	ctx->penalty = p->flags & PF_KTHREAD ? 0 :
		max(ctx->curr_penalty, ctx->prev_penalty);
	if (effective_prio_bore(p) != old_prio)
		reweight_task_bore(p);
}

void update_curr_bore(struct task_struct *p, u64 delta_exec)
{
	struct bore_ctx *ctx = &p->bore;

	if (ctx->stop_update)
		return;
	ctx->burst_time += delta_exec;
	ctx->curr_penalty = calc_burst_penalty(ctx->burst_time);
	if (ctx->curr_penalty > ctx->prev_penalty)
		update_penalty(p);
}

void restart_burst_bore(struct task_struct *p)
{
	struct bore_ctx *ctx = &p->bore;

	ctx->prev_penalty = binary_smooth(ctx->curr_penalty, ctx->prev_penalty);
	ctx->curr_penalty = 0;
	ctx->burst_time = 0;
	update_penalty(p);
}

void restart_burst_rescale_deadline_bore(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	s64 vremain = se->deadline - se->vruntime;
	s64 vscaled;
	u8 old_prio = effective_prio_bore(p);
	u8 new_prio;

	restart_burst_bore(p);
	new_prio = effective_prio_bore(p);
	if (old_prio > new_prio) {
		vscaled = rescale_slice(abs(vremain), old_prio, new_prio);
		if (vremain < 0)
			vscaled = -vscaled;
		se->deadline = se->vruntime + vscaled;
	}
}

static bool task_is_bore_eligible(struct task_struct *p)
{
	return p && p->sched_class == &fair_sched_class && !p->exit_state;
}

static u32 count_children_upto2(struct task_struct *p)
{
	struct list_head *head = &p->children;
	struct list_head *first = head->next;
	struct list_head *second = first->next;

	return (first != head) + (second != head);
}

static bool burst_cache_expired(struct bore_bc *bc, u64 now)
{
	struct bore_bc value = { .value = READ_ONCE(bc->value) };
	u64 timestamp = (u64)value.timestamp << BORE_BC_TIMESTAMP_SHIFT;

	return now - timestamp > sched_burst_cache_lifetime;
}

static void update_burst_cache(struct bore_bc *bc, struct task_struct *p,
			       u32 count, u32 total, u64 now)
{
	u32 average = count == 1 ? total :
		(u32)(((u64)total * bore_reciprocal_lut[count]) >> 32);
	struct bore_bc value = {
		.penalty = max_t(u32, average, READ_ONCE(p->bore.penalty)),
		.timestamp = now >> BORE_BC_TIMESTAMP_SHIFT,
	};

	WRITE_ONCE(bc->value, value.value);
}

static u32 inherit_from_parent(struct task_struct *parent,
			       u64 clone_flags, u64 now)
{
	struct task_struct *child;
	struct bore_bc *bc;
	struct bore_bc value;
	u32 count = 0, total = 0, scanned = 0;

	if (clone_flags & CLONE_PARENT)
		parent = parent->real_parent;
	bc = &parent->bore.subtree;
	if (burst_cache_expired(bc, now)) {
		list_for_each_entry(child, &parent->children, sibling) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT ||
			    scanned++ >= BURST_CACHE_SCAN_LIMIT)
				break;
			if (!task_is_bore_eligible(child))
				continue;
			count++;
			total += READ_ONCE(child->bore.penalty);
		}
		update_burst_cache(bc, parent, count, total, now);
	}
	value.value = READ_ONCE(bc->value);
	return value.penalty;
}

static u32 inherit_from_ancestor_hub(struct task_struct *parent,
				     u64 clone_flags, u64 now)
{
	struct task_struct *ancestor = parent, *child, *descendant;
	struct bore_bc *bc;
	struct bore_bc value;
	u32 sole_child_count = 0, count = 0, total = 0, scanned = 0;

	if (clone_flags & CLONE_PARENT) {
		ancestor = ancestor->real_parent;
		sole_child_count = 1;
	}
	while (ancestor->real_parent != ancestor &&
	       count_children_upto2(ancestor) <= sole_child_count) {
		ancestor = ancestor->real_parent;
		sole_child_count = 1;
	}
	bc = &ancestor->bore.subtree;
	if (burst_cache_expired(bc, now)) {
		list_for_each_entry(child, &ancestor->children, sibling) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT ||
			    scanned++ >= BURST_CACHE_SCAN_LIMIT)
				break;
			descendant = child;
			while (count_children_upto2(descendant) == 1)
				descendant = list_first_entry(&descendant->children,
							      struct task_struct, sibling);
			if (!task_is_bore_eligible(descendant))
				continue;
			count++;
			total += READ_ONCE(descendant->bore.penalty);
		}
		update_burst_cache(bc, ancestor, count, total, now);
	}
	value.value = READ_ONCE(bc->value);
	return value.penalty;
}

static u32 inherit_from_thread_group(struct task_struct *p, u64 now)
{
	struct task_struct *leader = p->group_leader, *thread;
	struct bore_bc *bc = &leader->bore.group;
	struct bore_bc value;
	u32 count = 0, total = 0, scanned = 0;

	if (burst_cache_expired(bc, now)) {
		for_each_thread(leader, thread) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT ||
			    scanned++ >= BURST_CACHE_SCAN_LIMIT)
				break;
			if (!task_is_bore_eligible(thread))
				continue;
			count++;
			total += READ_ONCE(thread->bore.penalty);
		}
		update_burst_cache(bc, leader, count, total, now);
	}
	value.value = READ_ONCE(bc->value);
	return value.penalty;
}

void task_fork_bore(struct task_struct *p, struct task_struct *parent,
		    u64 clone_flags, u64 now)
{
	struct bore_ctx *ctx = &p->bore;
	unsigned long flags;
	u32 inherited_penalty;

	if (!task_is_bore_eligible(p))
		return;
	if (!static_branch_likely(&sched_bore_key))
		goto reweight;

	/* 5.10 reparenting splices children without RCU list primitives. */
	read_lock_irqsave(&tasklist_lock, flags);
	if (clone_flags & CLONE_THREAD)
		inherited_penalty = inherit_from_thread_group(parent, now);
	else if (static_branch_likely(&sched_burst_inherit_key))
		inherited_penalty = static_branch_likely(&sched_burst_ancestor_key) ?
			inherit_from_ancestor_hub(parent, clone_flags, now) :
			inherit_from_parent(parent, clone_flags, now);
	else
		inherited_penalty = 0;
	ctx->prev_penalty = max_t(u16, ctx->prev_penalty, inherited_penalty);
	read_unlock_irqrestore(&tasklist_lock, flags);
	ctx->penalty = p->flags & PF_KTHREAD ? 0 : ctx->prev_penalty;
reweight:
	/* Reset copied weights even if inheritance adds no penalty. */
	reweight_task_bore(p);
}

void sched_post_fork_bore(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;

	/* A toggle can miss a child between inheritance and publication. */
	rq = task_rq_lock(p, &rf);
	if (task_is_bore_eligible(p)) {
		update_rq_clock(rq);
		reweight_task_bore(p);
	}
	task_rq_unlock(rq, p, &rf);
}

void reset_task_bore(struct task_struct *p)
{
	memset(&p->bore, 0, sizeof(p->bore));
}

void __init sched_init_bore(void)
{
	int i;

	pr_info("%s %s by %s\n", SCHED_BORE_PROGNAME,
		SCHED_BORE_VERSION, SCHED_BORE_AUTHOR);
	for (i = 1; i <= BURST_CACHE_SAMPLE_LIMIT; i++)
		bore_reciprocal_lut[i] = div64_u64(0xffffffffULL + i, i);
	reset_task_bore(&init_task);
}

#ifdef CONFIG_SYSCTL
static DEFINE_MUTEX(sched_bore_mutex);
static int maxval_3 = 3;
static int maxval_6_bits = 63;
static int maxval_12_bits = 4095;

static void update_inherit_type(void)
{
	if (sched_burst_inherit_type)
		static_branch_enable(&sched_burst_inherit_key);
	else
		static_branch_disable(&sched_burst_inherit_key);
	if (sched_burst_inherit_type == 2)
		static_branch_enable(&sched_burst_ancestor_key);
	else
		static_branch_disable(&sched_burst_ancestor_key);
}

static void readjust_all_task_weights(void)
{
	struct task_struct *group, *task;
	struct rq_flags rf;
	struct rq *rq;
	unsigned long flags;

	read_lock_irqsave(&tasklist_lock, flags);
	/* Pair the toggle with task publication and sched_post_fork_bore(). */
	smp_mb__after_spinlock();
	for_each_process_thread(group, task) {
		rq = task_rq_lock(task, &rf);
		if (task_is_bore_eligible(task)) {
			update_rq_clock(rq);
			reweight_task_bore(task);
		}
		task_rq_unlock(rq, task, &rf);
	}
	read_unlock_irqrestore(&tasklist_lock, flags);
}

static int sched_bore_update_handler(struct ctl_table *table, int write,
				     void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&sched_bore_mutex);
	ret = proc_dou8vec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;
	if (sched_bore)
		static_branch_enable(&sched_bore_key);
	else
		static_branch_disable(&sched_bore_key);
	readjust_all_task_weights();
out:
	mutex_unlock(&sched_bore_mutex);
	return ret;
}

static int
sched_burst_inherit_type_update_handler(struct ctl_table *table, int write,
					void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&sched_bore_mutex);
	ret = proc_dou8vec_minmax(table, write, buffer, lenp, ppos);
	if (!ret && write)
		update_inherit_type();
	mutex_unlock(&sched_bore_mutex);
	return ret;
}

static int
sched_burst_protect_slice_lv_update_handler(struct ctl_table *table, int write,
					    void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&sched_bore_mutex);
	ret = proc_dou8vec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;
	if (sched_burst_protect_slice_lv == 1 || sched_burst_protect_slice_lv == 2)
		static_branch_enable(&sched_burst_protect_slice_cond_key);
	else
		static_branch_disable(&sched_burst_protect_slice_cond_key);
	if (sched_burst_protect_slice_lv >= 2)
		static_branch_enable(&sched_burst_protect_slice_prefer_key);
	else
		static_branch_disable(&sched_burst_protect_slice_prefer_key);
out:
	mutex_unlock(&sched_bore_mutex);
	return ret;
}

static struct ctl_table sched_bore_sysctls[] = {
	{
		.procname = "sched_bore",
		.data = &sched_bore,
		.maxlen = sizeof(sched_bore),
		.mode = 0644,
		.proc_handler = sched_bore_update_handler,
		.extra1 = SYSCTL_ZERO,
		.extra2 = SYSCTL_ONE,
	},
	{
		.procname = "sched_burst_inherit_type",
		.data = &sched_burst_inherit_type,
		.maxlen = sizeof(sched_burst_inherit_type),
		.mode = 0644,
		.proc_handler = sched_burst_inherit_type_update_handler,
		.extra1 = SYSCTL_ZERO,
		.extra2 = SYSCTL_TWO,
	},
	{
		.procname = "sched_burst_smoothness",
		.data = &sched_burst_smoothness,
		.maxlen = sizeof(sched_burst_smoothness),
		.mode = 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1 = SYSCTL_ZERO,
		.extra2 = &maxval_3,
	},
	{
		.procname = "sched_burst_penalty_offset",
		.data = &sched_burst_penalty_offset,
		.maxlen = sizeof(sched_burst_penalty_offset),
		.mode = 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1 = SYSCTL_ZERO,
		.extra2 = &maxval_6_bits,
	},
	{
		.procname = "sched_burst_penalty_scale",
		.data = &sched_burst_penalty_scale,
		.maxlen = sizeof(sched_burst_penalty_scale),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = SYSCTL_ZERO,
		.extra2 = &maxval_12_bits,
	},
	{
		.procname = "sched_burst_cache_lifetime",
		.data = &sched_burst_cache_lifetime,
		.maxlen = sizeof(sched_burst_cache_lifetime),
		.mode = 0644,
		.proc_handler = proc_douintvec,
	},
	{
		.procname = "sched_burst_protect_slice_lv",
		.data = &sched_burst_protect_slice_lv,
		.maxlen = sizeof(sched_burst_protect_slice_lv),
		.mode = 0644,
		.proc_handler = sched_burst_protect_slice_lv_update_handler,
		.extra1 = SYSCTL_ZERO,
		.extra2 = &maxval_3,
	},
	{ }
};

static int __init sched_bore_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_bore_sysctls);
	return 0;
}
late_initcall(sched_bore_sysctl_init);
#endif /* CONFIG_SYSCTL */
