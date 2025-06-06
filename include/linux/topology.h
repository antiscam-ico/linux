/*
 * include/linux/topology.h
 *
 * Written by: Matthew Dobson, IBM Corporation
 *
 * Copyright (C) 2002, IBM Corp.
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Send feedback to <colpatch@us.ibm.com>
 */
#ifndef _LINUX_TOPOLOGY_H
#define _LINUX_TOPOLOGY_H

#include <linux/arch_topology.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/bitops.h>
#include <linux/mmzone.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <asm/topology.h>

#ifndef nr_cpus_node
#define nr_cpus_node(node) cpumask_weight(cpumask_of_node(node))
#endif

int arch_update_cpu_topology(void);

/* Conform to ACPI 2.0 SLIT distance definitions */
#define LOCAL_DISTANCE		10
#define REMOTE_DISTANCE		20
#define DISTANCE_BITS           8
#ifndef node_distance
#define node_distance(from,to)	((from) == (to) ? LOCAL_DISTANCE : REMOTE_DISTANCE)
#endif
#ifndef RECLAIM_DISTANCE
/*
 * If the distance between nodes in a system is larger than RECLAIM_DISTANCE
 * (in whatever arch specific measurement units returned by node_distance())
 * and node_reclaim_mode is enabled then the VM will only call node_reclaim()
 * on nodes within this distance.
 */
#define RECLAIM_DISTANCE 30
#endif

/*
 * The following tunable allows platforms to override the default node
 * reclaim distance (RECLAIM_DISTANCE) if remote memory accesses are
 * sufficiently fast that the default value actually hurts
 * performance.
 *
 * AMD EPYC machines use this because even though the 2-hop distance
 * is 32 (3.2x slower than a local memory access) performance actually
 * *improves* if allowed to reclaim memory and load balance tasks
 * between NUMA nodes 2-hops apart.
 */
extern int __read_mostly node_reclaim_distance;

#ifndef PENALTY_FOR_NODE_WITH_CPUS
#define PENALTY_FOR_NODE_WITH_CPUS	(1)
#endif

#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
DECLARE_PER_CPU(int, numa_node);

#ifndef numa_node_id
/* Returns the number of the current Node. */
static inline int numa_node_id(void)
{
	return raw_cpu_read(numa_node);
}
#endif

#ifndef cpu_to_node
static inline int cpu_to_node(int cpu)
{
	return per_cpu(numa_node, cpu);
}
#endif

#ifndef set_numa_node
static inline void set_numa_node(int node)
{
	this_cpu_write(numa_node, node);
}
#endif

#ifndef set_cpu_numa_node
static inline void set_cpu_numa_node(int cpu, int node)
{
	per_cpu(numa_node, cpu) = node;
}
#endif

#else	/* !CONFIG_USE_PERCPU_NUMA_NODE_ID */

/* Returns the number of the current Node. */
#ifndef numa_node_id
static inline int numa_node_id(void)
{
	return cpu_to_node(raw_smp_processor_id());
}
#endif

#endif	/* [!]CONFIG_USE_PERCPU_NUMA_NODE_ID */

#ifdef CONFIG_HAVE_MEMORYLESS_NODES

/*
 * N.B., Do NOT reference the '_numa_mem_' per cpu variable directly.
 * It will not be defined when CONFIG_HAVE_MEMORYLESS_NODES is not defined.
 * Use the accessor functions set_numa_mem(), numa_mem_id() and cpu_to_mem().
 */
DECLARE_PER_CPU(int, _numa_mem_);

#ifndef set_numa_mem
static inline void set_numa_mem(int node)
{
	this_cpu_write(_numa_mem_, node);
}
#endif

#ifndef numa_mem_id
/* Returns the number of the nearest Node with memory */
static inline int numa_mem_id(void)
{
	return raw_cpu_read(_numa_mem_);
}
#endif

#ifndef cpu_to_mem
static inline int cpu_to_mem(int cpu)
{
	return per_cpu(_numa_mem_, cpu);
}
#endif

#ifndef set_cpu_numa_mem
static inline void set_cpu_numa_mem(int cpu, int node)
{
	per_cpu(_numa_mem_, cpu) = node;
}
#endif

#else	/* !CONFIG_HAVE_MEMORYLESS_NODES */

#ifndef numa_mem_id
/* Returns the number of the nearest Node with memory */
static inline int numa_mem_id(void)
{
	return numa_node_id();
}
#endif

#ifndef cpu_to_mem
static inline int cpu_to_mem(int cpu)
{
	return cpu_to_node(cpu);
}
#endif

#endif	/* [!]CONFIG_HAVE_MEMORYLESS_NODES */

#if defined(topology_die_id) && defined(topology_die_cpumask)
#define TOPOLOGY_DIE_SYSFS
#endif
#if defined(topology_cluster_id) && defined(topology_cluster_cpumask)
#define TOPOLOGY_CLUSTER_SYSFS
#endif
#if defined(topology_book_id) && defined(topology_book_cpumask)
#define TOPOLOGY_BOOK_SYSFS
#endif
#if defined(topology_drawer_id) && defined(topology_drawer_cpumask)
#define TOPOLOGY_DRAWER_SYSFS
#endif

#ifndef topology_physical_package_id
#define topology_physical_package_id(cpu)	((void)(cpu), -1)
#endif
#ifndef topology_die_id
#define topology_die_id(cpu)			((void)(cpu), -1)
#endif
#ifndef topology_cluster_id
#define topology_cluster_id(cpu)		((void)(cpu), -1)
#endif
#ifndef topology_core_id
#define topology_core_id(cpu)			((void)(cpu), 0)
#endif
#ifndef topology_book_id
#define topology_book_id(cpu)			((void)(cpu), -1)
#endif
#ifndef topology_drawer_id
#define topology_drawer_id(cpu)			((void)(cpu), -1)
#endif
#ifndef topology_ppin
#define topology_ppin(cpu)			((void)(cpu), 0ull)
#endif
#ifndef topology_sibling_cpumask
#define topology_sibling_cpumask(cpu)		cpumask_of(cpu)
#endif
#ifndef topology_core_cpumask
#define topology_core_cpumask(cpu)		cpumask_of(cpu)
#endif
#ifndef topology_cluster_cpumask
#define topology_cluster_cpumask(cpu)		cpumask_of(cpu)
#endif
#ifndef topology_die_cpumask
#define topology_die_cpumask(cpu)		cpumask_of(cpu)
#endif
#ifndef topology_book_cpumask
#define topology_book_cpumask(cpu)		cpumask_of(cpu)
#endif
#ifndef topology_drawer_cpumask
#define topology_drawer_cpumask(cpu)		cpumask_of(cpu)
#endif

#if defined(CONFIG_SCHED_SMT) && !defined(cpu_smt_mask)
static inline const struct cpumask *cpu_smt_mask(int cpu)
{
	return topology_sibling_cpumask(cpu);
}
#endif

#ifndef topology_is_primary_thread

static inline bool topology_is_primary_thread(unsigned int cpu)
{
	/*
	 * When disabling SMT, the primary thread of the SMT will remain
	 * enabled/active. Architectures that have a special primary thread
	 * (e.g. x86) need to override this function. Otherwise the first
	 * thread in the SMT can be made the primary thread.
	 *
	 * The sibling cpumask of an offline CPU always contains the CPU
	 * itself on architectures using the implementation of
	 * CONFIG_GENERIC_ARCH_TOPOLOGY for building their topology.
	 * Other architectures not using CONFIG_GENERIC_ARCH_TOPOLOGY for
	 * building their topology have to check whether to use this default
	 * implementation or to override it.
	 */
	return cpu == cpumask_first(topology_sibling_cpumask(cpu));
}
#define topology_is_primary_thread topology_is_primary_thread

#endif

static inline const struct cpumask *cpu_cpu_mask(int cpu)
{
	return cpumask_of_node(cpu_to_node(cpu));
}

#ifdef CONFIG_NUMA
int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node);
extern const struct cpumask *sched_numa_hop_mask(unsigned int node, unsigned int hops);
#else
static __always_inline int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node)
{
	return cpumask_nth_and(cpu, cpus, cpu_online_mask);
}

static inline const struct cpumask *
sched_numa_hop_mask(unsigned int node, unsigned int hops)
{
	return ERR_PTR(-EOPNOTSUPP);
}
#endif	/* CONFIG_NUMA */

/**
 * for_each_node_numadist() - iterate over nodes in increasing distance
 *			      order, starting from a given node
 * @node: the iteration variable and the starting node.
 * @unvisited: a nodemask to keep track of the unvisited nodes.
 *
 * This macro iterates over NUMA node IDs in increasing distance from the
 * starting @node and yields MAX_NUMNODES when all the nodes have been
 * visited.
 *
 * Note that by the time the loop completes, the @unvisited nodemask will
 * be fully cleared, unless the loop exits early.
 *
 * The difference between for_each_node() and for_each_node_numadist() is
 * that the former allows to iterate over nodes in numerical order, whereas
 * the latter iterates over nodes in increasing order of distance.
 *
 * This complexity of this iterator is O(N^2), where N represents the
 * number of nodes, as each iteration involves scanning all nodes to
 * find the one with the shortest distance.
 *
 * Requires rcu_lock to be held.
 */
#define for_each_node_numadist(node, unvisited)					\
	for (int __start = (node),						\
	     (node) = nearest_node_nodemask((__start), &(unvisited));		\
	     (node) < MAX_NUMNODES;						\
	     node_clear((node), (unvisited)),					\
	     (node) = nearest_node_nodemask((__start), &(unvisited)))

/**
 * for_each_numa_hop_mask - iterate over cpumasks of increasing NUMA distance
 *                          from a given node.
 * @mask: the iteration variable.
 * @node: the NUMA node to start the search from.
 *
 * Requires rcu_lock to be held.
 *
 * Yields cpu_online_mask for @node == NUMA_NO_NODE.
 */
#define for_each_numa_hop_mask(mask, node)				       \
	for (unsigned int __hops = 0;					       \
	     mask = (node != NUMA_NO_NODE || __hops) ?			       \
		     sched_numa_hop_mask(node, __hops) :		       \
		     cpu_online_mask,					       \
	     !IS_ERR_OR_NULL(mask);					       \
	     __hops++)

DECLARE_PER_CPU(unsigned long, cpu_scale);

static inline unsigned long topology_get_cpu_scale(int cpu)
{
	return per_cpu(cpu_scale, cpu);
}

void topology_set_cpu_scale(unsigned int cpu, unsigned long capacity);

#endif /* _LINUX_TOPOLOGY_H */
