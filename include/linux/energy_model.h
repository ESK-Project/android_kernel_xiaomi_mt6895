/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ENERGY_MODEL_H
#define _LINUX_ENERGY_MODEL_H
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/jump_label.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/types.h>

/**
 * em_perf_state - Performance state of a performance domain
 * @frequency:	The frequency in KHz, for consistency with CPUFreq
 * @power:	The power consumed at this level, in milli-watts (by 1 CPU or
		by a registered device). It can be a total power: static and
		dynamic.
 * @cost:	The cost coefficient associated with this level, used during
 *		energy calculation. Equal to: power * max_frequency / frequency
 */
struct em_perf_state {
	unsigned long frequency;
	unsigned long power;
	unsigned long cost;
};

/**
 * em_perf_domain - Performance domain
 * @table:		List of performance states, in ascending order
 * @nr_perf_states:	Number of performance states
 * @milliwatts:		Flag indicating the power values are in milli-Watts
 *			or some other scale.
 * @cpus:		Cpumask covering the CPUs of the domain. It's here
 *			for performance reasons to avoid potential cache
 *			misses during energy calculations in the scheduler
 *			and simplifies allocating/freeing that memory region.
 *
 * In case of CPU device, a "performance domain" represents a group of CPUs
 * whose performance is scaled together. All CPUs of a performance domain
 * must have the same micro-architecture. Performance domains often have
 * a 1-to-1 mapping with CPUFreq policies. In case of other devices the @cpus
 * field is unused.
 */
struct em_perf_domain {
	struct em_perf_state *table;
	int nr_perf_states;
	int milliwatts;
	unsigned long cpus[];
};

#define em_span_cpus(em) (to_cpumask((em)->cpus))

#ifdef CONFIG_ENERGY_MODEL
#define EM_MAX_POWER 0xFFFF

/*
 * Increase resolution of energy estimation calculations for 64-bit
 * architectures. The extra resolution improves decision made by EAS for the
 * task placement when two Performance Domains might provide similar energy
 * estimation values (w/o better resolution the values could be equal).
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 */
#ifdef CONFIG_64BIT
#define em_scale_power(p) ((p) * 1000)
#else
#define em_scale_power(p) (p)
#endif

struct em_data_callback {
	/**
	 * active_power() - Provide power at the next performance state of
	 *		a device
	 * @power	: Active power at the performance state in mW
	 *		(modified)
	 * @freq	: Frequency at the performance state in kHz
	 *		(modified)
	 * @dev		: Device for which we do this operation (can be a CPU)
	 *
	 * active_power() must find the lowest performance state of 'dev' above
	 * 'freq' and update 'power' and 'freq' to the matching active power
	 * and frequency.
	 *
	 * In case of CPUs, the power is the one of a single CPU in the domain,
	 * expressed in milli-watts. It is expected to fit in the
	 * [0, EM_MAX_POWER] range.
	 *
	 * Return 0 on success.
	 */
	int (*active_power)(unsigned long *power, unsigned long *freq,
			    struct device *dev);
};
#define EM_DATA_CB(_active_power_cb) { .active_power = &_active_power_cb }

struct em_perf_domain *em_cpu_get(int cpu);
struct em_perf_domain *em_pd_get(struct device *dev);
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *span,
				bool milliwatts);
void em_dev_unregister_perf_domain(struct device *dev);

static inline int em_pd_get_previous_state(struct em_perf_domain *pd, int idx)
{
	return idx > 0 && idx < pd->nr_perf_states ? idx - 1 : -1;
}

/**
 * em_pd_nr_perf_states() - Get the number of performance states of a perf.
 *				domain
 * @pd		: performance domain for which this must be done
 *
 * Return: the number of performance states in the performance domain table
 */
static inline int em_pd_nr_perf_states(struct em_perf_domain *pd)
{
	return pd->nr_perf_states;
}

#else
struct em_data_callback {};
#define EM_DATA_CB(_active_power_cb) { }

static inline
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *span,
				bool milliwatts)
{
	return -EINVAL;
}
static inline void em_dev_unregister_perf_domain(struct device *dev)
{
}
static inline struct em_perf_domain *em_cpu_get(int cpu)
{
	return NULL;
}
static inline struct em_perf_domain *em_pd_get(struct device *dev)
{
	return NULL;
}
static inline int em_pd_get_previous_state(struct em_perf_domain *pd, int idx)
{
	return -1;
}
static inline int em_pd_nr_perf_states(struct em_perf_domain *pd)
{
	return 0;
}
#endif

#endif
