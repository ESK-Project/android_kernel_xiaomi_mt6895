#ifndef _KERNEL_SCHED_BORE_H
#define _KERNEL_SCHED_BORE_H

#include <linux/sched.h>
#include <linux/jump_label.h>
#include <linux/sysctl.h>

#define SCHED_BORE_AUTHOR		"Masahito Suzuki"
#define SCHED_BORE_PROGNAME	"BORE CPU Scheduler modification"
#define SCHED_BORE_VERSION	"6.8.0"

extern u8 sched_bore;
DECLARE_STATIC_KEY_TRUE(sched_bore_key);
DECLARE_STATIC_KEY_TRUE(sched_burst_protect_slice_cond_key);
DECLARE_STATIC_KEY_FALSE(sched_burst_protect_slice_prefer_key);

static inline u8 bore_score(struct task_struct *p)
{
	return p->bore.penalty >> 8;
}

u8 effective_prio_bore(struct task_struct *p);
void reweight_task_bore(struct task_struct *p);
void update_curr_bore(struct task_struct *p, u64 delta_exec);
void restart_burst_bore(struct task_struct *p);
void restart_burst_rescale_deadline_bore(struct task_struct *p);
void task_fork_bore(struct task_struct *p, struct task_struct *parent,
		    u64 clone_flags, u64 now);
void sched_post_fork_bore(struct task_struct *p);
void sched_init_bore(void);
void reset_task_bore(struct task_struct *p);

#endif /* _KERNEL_SCHED_BORE_H */
