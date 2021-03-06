/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 *
 * Copyright (c) 2012, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Author: liang@whamcloud.com
 */

#define DEBUG_SUBSYSTEM S_LNET

#include <linux/cpu.h>
#include <linux/sched.h>
#include <libcfs/libcfs.h>

#ifdef CONFIG_SMP

/**
 * modparam for setting number of partitions
 *
 *  0 : estimate best value based on cores or NUMA nodes
 *  1 : disable multiple partitions
 * >1 : specify number of partitions
 */
static int	cpu_npartitions;
module_param(cpu_npartitions, int, 0444);
MODULE_PARM_DESC(cpu_npartitions, "# of CPU partitions");

/**
 * modparam for setting CPU partitions patterns:
 *
 * i.e: "0[0,1,2,3] 1[4,5,6,7]", number before bracket is CPU partition ID,
 *      number in bracket is processor ID (core or HT)
 *
 * i.e: "N 0[0,1] 1[2,3]" the first character 'N' means numbers in bracket
 *       are NUMA node ID, number before bracket is CPU partition ID.
 *
 * i.e: "N", shortcut expression to create CPT from NUMA & CPU topology
 *
 * NB: If user specified cpu_pattern, cpu_npartitions will be ignored
 */
static char	*cpu_pattern = "N";
module_param(cpu_pattern, charp, 0444);
MODULE_PARM_DESC(cpu_pattern, "CPU partitions pattern");

/* return number of HTs in the same core of \a cpu */
int
cfs_cpu_ht_nsiblings(int cpu)
{
	return cpumask_weight(topology_sibling_cpumask(cpu));
}
EXPORT_SYMBOL(cfs_cpu_ht_nsiblings);

void
cfs_cpt_table_free(struct cfs_cpt_table *cptab)
{
	int i;

	if (cptab->ctb_cpu2cpt != NULL) {
		LIBCFS_FREE(cptab->ctb_cpu2cpt,
			    nr_cpu_ids * sizeof(cptab->ctb_cpu2cpt[0]));
	}

	if (cptab->ctb_node2cpt != NULL) {
		LIBCFS_FREE(cptab->ctb_node2cpt,
			    nr_node_ids * sizeof(cptab->ctb_node2cpt[0]));
	}

	for (i = 0; cptab->ctb_parts != NULL && i < cptab->ctb_nparts; i++) {
		struct cfs_cpu_partition *part = &cptab->ctb_parts[i];

		if (part->cpt_nodemask != NULL) {
			LIBCFS_FREE(part->cpt_nodemask,
				    sizeof(*part->cpt_nodemask));
		}

		if (part->cpt_cpumask != NULL)
			LIBCFS_FREE(part->cpt_cpumask, cpumask_size());

		if (part->cpt_distance) {
			LIBCFS_FREE(part->cpt_distance,
				cptab->ctb_nparts *
					sizeof(part->cpt_distance[0]));
		}
	}

	if (cptab->ctb_parts != NULL) {
		LIBCFS_FREE(cptab->ctb_parts,
			    cptab->ctb_nparts * sizeof(cptab->ctb_parts[0]));
	}

	if (cptab->ctb_nodemask != NULL)
		LIBCFS_FREE(cptab->ctb_nodemask, sizeof(*cptab->ctb_nodemask));
	if (cptab->ctb_cpumask != NULL)
		LIBCFS_FREE(cptab->ctb_cpumask, cpumask_size());

	LIBCFS_FREE(cptab, sizeof(*cptab));
}
EXPORT_SYMBOL(cfs_cpt_table_free);

struct cfs_cpt_table *
cfs_cpt_table_alloc(unsigned int ncpt)
{
	struct cfs_cpt_table *cptab;
	int	i;

	LIBCFS_ALLOC(cptab, sizeof(*cptab));
	if (cptab == NULL)
		return NULL;

	cptab->ctb_nparts = ncpt;

	LIBCFS_ALLOC(cptab->ctb_cpumask, cpumask_size());
	LIBCFS_ALLOC(cptab->ctb_nodemask, sizeof(*cptab->ctb_nodemask));

	if (cptab->ctb_cpumask == NULL || cptab->ctb_nodemask == NULL)
		goto failed;

	LIBCFS_ALLOC(cptab->ctb_cpu2cpt,
		     nr_cpu_ids * sizeof(cptab->ctb_cpu2cpt[0]));
	if (cptab->ctb_cpu2cpt == NULL)
		goto failed;

	memset(cptab->ctb_cpu2cpt, -1,
	       nr_cpu_ids * sizeof(cptab->ctb_cpu2cpt[0]));

	LIBCFS_ALLOC(cptab->ctb_node2cpt,
		     nr_node_ids * sizeof(cptab->ctb_node2cpt[0]));
	if (cptab->ctb_node2cpt == NULL)
		goto failed;

	memset(cptab->ctb_node2cpt, -1,
	       nr_node_ids * sizeof(cptab->ctb_node2cpt[0]));

	LIBCFS_ALLOC(cptab->ctb_parts, ncpt * sizeof(cptab->ctb_parts[0]));
	if (cptab->ctb_parts == NULL)
		goto failed;

	for (i = 0; i < ncpt; i++) {
		struct cfs_cpu_partition *part = &cptab->ctb_parts[i];

		LIBCFS_ALLOC(part->cpt_cpumask, cpumask_size());
		if (!part->cpt_cpumask)
			goto failed;

		LIBCFS_ALLOC(part->cpt_nodemask, sizeof(*part->cpt_nodemask));
		if (!part->cpt_nodemask)
			goto failed;

		LIBCFS_ALLOC(part->cpt_distance,
			cptab->ctb_nparts * sizeof(part->cpt_distance[0]));
		if (!part->cpt_distance)
			goto failed;
	}

	return cptab;

failed:
	cfs_cpt_table_free(cptab);
	return NULL;
}
EXPORT_SYMBOL(cfs_cpt_table_alloc);

int
cfs_cpt_table_print(struct cfs_cpt_table *cptab, char *buf, int len)
{
	char	*tmp = buf;
	int	rc = -EFBIG;
	int	i;
	int	j;

	for (i = 0; i < cptab->ctb_nparts; i++) {
		if (len <= 0)
			goto out;

		rc = snprintf(tmp, len, "%d\t:", i);
		len -= rc;

		if (len <= 0)
			goto out;

		tmp += rc;
		for_each_cpu(j, cptab->ctb_parts[i].cpt_cpumask) {
			rc = snprintf(tmp, len, " %d", j);
			len -= rc;
			if (len <= 0)
				goto out;
			tmp += rc;
		}

		*tmp = '\n';
		tmp++;
		len--;
	}
	rc = 0;
 out:
	if (rc < 0)
		return rc;

	return tmp - buf;
}
EXPORT_SYMBOL(cfs_cpt_table_print);

int
cfs_cpt_distance_print(struct cfs_cpt_table *cptab, char *buf, int len)
{
	char	*tmp = buf;
	int	rc = -EFBIG;
	int	i;
	int	j;

	for (i = 0; i < cptab->ctb_nparts; i++) {
		if (len <= 0)
			goto out;

		rc = snprintf(tmp, len, "%d\t:", i);
		len -= rc;

		if (len <= 0)
			goto out;

		tmp += rc;
		for (j = 0; j < cptab->ctb_nparts; j++) {
			rc = snprintf(tmp, len, " %d:%d",
				j, cptab->ctb_parts[i].cpt_distance[j]);
			len -= rc;
			if (len <= 0)
				goto out;
			tmp += rc;
		}

		*tmp = '\n';
		tmp++;
		len--;
	}
	rc = 0;
 out:
	if (rc < 0)
		return rc;

	return tmp - buf;
}
EXPORT_SYMBOL(cfs_cpt_distance_print);

int
cfs_cpt_number(struct cfs_cpt_table *cptab)
{
	return cptab->ctb_nparts;
}
EXPORT_SYMBOL(cfs_cpt_number);

int
cfs_cpt_weight(struct cfs_cpt_table *cptab, int cpt)
{
	LASSERT(cpt == CFS_CPT_ANY || (cpt >= 0 && cpt < cptab->ctb_nparts));

	return cpt == CFS_CPT_ANY ?
	       cpumask_weight(cptab->ctb_cpumask) :
	       cpumask_weight(cptab->ctb_parts[cpt].cpt_cpumask);
}
EXPORT_SYMBOL(cfs_cpt_weight);

int
cfs_cpt_online(struct cfs_cpt_table *cptab, int cpt)
{
	LASSERT(cpt == CFS_CPT_ANY || (cpt >= 0 && cpt < cptab->ctb_nparts));

	return cpt == CFS_CPT_ANY ?
	       cpumask_any_and(cptab->ctb_cpumask,
			       cpu_online_mask) < nr_cpu_ids :
	       cpumask_any_and(cptab->ctb_parts[cpt].cpt_cpumask,
			       cpu_online_mask) < nr_cpu_ids;
}
EXPORT_SYMBOL(cfs_cpt_online);

cpumask_t *
cfs_cpt_cpumask(struct cfs_cpt_table *cptab, int cpt)
{
	LASSERT(cpt == CFS_CPT_ANY || (cpt >= 0 && cpt < cptab->ctb_nparts));

	return cpt == CFS_CPT_ANY ?
	       cptab->ctb_cpumask : cptab->ctb_parts[cpt].cpt_cpumask;
}
EXPORT_SYMBOL(cfs_cpt_cpumask);

nodemask_t *
cfs_cpt_nodemask(struct cfs_cpt_table *cptab, int cpt)
{
	LASSERT(cpt == CFS_CPT_ANY || (cpt >= 0 && cpt < cptab->ctb_nparts));

	return cpt == CFS_CPT_ANY ?
	       cptab->ctb_nodemask : cptab->ctb_parts[cpt].cpt_nodemask;
}
EXPORT_SYMBOL(cfs_cpt_nodemask);

unsigned
cfs_cpt_distance(struct cfs_cpt_table *cptab, int cpt1, int cpt2)
{
	LASSERT(cpt1 == CFS_CPT_ANY || (cpt1 >= 0 && cpt1 < cptab->ctb_nparts));
	LASSERT(cpt2 == CFS_CPT_ANY || (cpt2 >= 0 && cpt2 < cptab->ctb_nparts));

	if (cpt1 == CFS_CPT_ANY || cpt2 == CFS_CPT_ANY)
		return cptab->ctb_distance;

	return cptab->ctb_parts[cpt1].cpt_distance[cpt2];
}
EXPORT_SYMBOL(cfs_cpt_distance);

/*
 * Calculate the maximum NUMA distance between all nodes in the
 * from_mask and all nodes in the to_mask.
 */
static unsigned
cfs_cpt_distance_calculate(nodemask_t *from_mask, nodemask_t *to_mask)
{
	unsigned maximum;
	unsigned distance;
	int to;
	int from;

	maximum = 0;
	for_each_node_mask(from, *from_mask) {
		for_each_node_mask(to, *to_mask) {
			distance = node_distance(from, to);
			if (maximum < distance)
				maximum = distance;
		}
	}
	return maximum;
}

static void cfs_cpt_add_cpu(struct cfs_cpt_table *cptab, int cpt, int cpu)
{
	cptab->ctb_cpu2cpt[cpu] = cpt;

	cpumask_set_cpu(cpu, cptab->ctb_cpumask);
	cpumask_set_cpu(cpu, cptab->ctb_parts[cpt].cpt_cpumask);
}

static void cfs_cpt_del_cpu(struct cfs_cpt_table *cptab, int cpt, int cpu)
{
	cpumask_clear_cpu(cpu, cptab->ctb_parts[cpt].cpt_cpumask);
	cpumask_clear_cpu(cpu, cptab->ctb_cpumask);

	cptab->ctb_cpu2cpt[cpu] = -1;
}

static void cfs_cpt_add_node(struct cfs_cpt_table *cptab, int cpt, int node)
{
	int cpt2;
	struct cfs_cpu_partition *part;
	struct cfs_cpu_partition *part2;

	if (!node_isset(node, *cptab->ctb_nodemask)) {
		/* first time node is added to the CPT table */
		node_set(node, *cptab->ctb_nodemask);
		cptab->ctb_node2cpt[node] = cpt;
		cptab->ctb_distance = cfs_cpt_distance_calculate(
							cptab->ctb_nodemask,
							cptab->ctb_nodemask);
	}

	part = &cptab->ctb_parts[cpt];
	if (!node_isset(node, *part->cpt_nodemask)) {
		/* first time node is added to this CPT */
		node_set(node, *part->cpt_nodemask);
		for (cpt2 = 0; cpt2 < cptab->ctb_nparts; cpt2++) {
			part2 = &cptab->ctb_parts[cpt2];
			part->cpt_distance[cpt2] = cfs_cpt_distance_calculate(
						part->cpt_nodemask,
						part2->cpt_nodemask);
			part2->cpt_distance[cpt] = cfs_cpt_distance_calculate(
						part2->cpt_nodemask,
						part->cpt_nodemask);
		}
	}
}

static void cfs_cpt_del_node(struct cfs_cpt_table *cptab, int cpt, int node)
{
	int cpu;
	int cpt2;
	struct cfs_cpu_partition *part;
	struct cfs_cpu_partition *part2;

	part = &cptab->ctb_parts[cpt];

	for_each_cpu(cpu, part->cpt_cpumask) {
		/* this CPT has other CPU belonging to this node? */
		if (cpu_to_node(cpu) == node)
			break;
	}

	if (cpu >= nr_cpu_ids && node_isset(node,  *part->cpt_nodemask)) {
		/* No more CPUs in the node for this CPT. */
		node_clear(node, *part->cpt_nodemask);
		for (cpt2 = 0; cpt2 < cptab->ctb_nparts; cpt2++) {
			part2 = &cptab->ctb_parts[cpt2];
			if (node_isset(node, *part2->cpt_nodemask))
				cptab->ctb_node2cpt[node] = cpt2;
			part->cpt_distance[cpt2] = cfs_cpt_distance_calculate(
						part->cpt_nodemask,
						part2->cpt_nodemask);
			part2->cpt_distance[cpt] = cfs_cpt_distance_calculate(
						part2->cpt_nodemask,
						part->cpt_nodemask);
		}
	}

	for_each_cpu(cpu, cptab->ctb_cpumask) {
		/* this CPT-table has other CPUs belonging to this node? */
		if (cpu_to_node(cpu) == node)
			break;
	}

	if (cpu >= nr_cpu_ids && node_isset(node, *cptab->ctb_nodemask)) {
		/* No more CPUs in the table for this node. */
		node_clear(node, *cptab->ctb_nodemask);
		cptab->ctb_node2cpt[node] = -1;
		cptab->ctb_distance =
			cfs_cpt_distance_calculate(cptab->ctb_nodemask,
					cptab->ctb_nodemask);
	}
}

int
cfs_cpt_set_cpu(struct cfs_cpt_table *cptab, int cpt, int cpu)
{
	LASSERT(cpt >= 0 && cpt < cptab->ctb_nparts);

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		CDEBUG(D_INFO, "CPU %d is invalid or it's offline\n", cpu);
		return 0;
	}

	if (cptab->ctb_cpu2cpt[cpu] != -1) {
		CDEBUG(D_INFO, "CPU %d is already in partition %d\n",
		       cpu, cptab->ctb_cpu2cpt[cpu]);
		return 0;
	}

	LASSERT(!cpumask_test_cpu(cpu, cptab->ctb_cpumask));
	LASSERT(!cpumask_test_cpu(cpu, cptab->ctb_parts[cpt].cpt_cpumask));

	cfs_cpt_add_cpu(cptab, cpt, cpu);
	cfs_cpt_add_node(cptab, cpt, cpu_to_node(cpu));

	return 1;
}
EXPORT_SYMBOL(cfs_cpt_set_cpu);

void
cfs_cpt_unset_cpu(struct cfs_cpt_table *cptab, int cpt, int cpu)
{
	LASSERT(cpt == CFS_CPT_ANY || (cpt >= 0 && cpt < cptab->ctb_nparts));

	if (cpu < 0 || cpu >= nr_cpu_ids) {
		CDEBUG(D_INFO, "Invalid CPU id %d\n", cpu);
		return;
	}

	if (cpt == CFS_CPT_ANY) {
		/* caller doesn't know the partition ID */
		cpt = cptab->ctb_cpu2cpt[cpu];
		if (cpt < 0) { /* not set in this CPT-table */
			CDEBUG(D_INFO, "Try to unset cpu %d which is "
				       "not in CPT-table %p\n", cpt, cptab);
			return;
		}

	} else if (cpt != cptab->ctb_cpu2cpt[cpu]) {
		CDEBUG(D_INFO,
		       "CPU %d is not in cpu-partition %d\n", cpu, cpt);
		return;
	}

	LASSERT(cpumask_test_cpu(cpu, cptab->ctb_parts[cpt].cpt_cpumask));
	LASSERT(cpumask_test_cpu(cpu, cptab->ctb_cpumask));

	cfs_cpt_del_cpu(cptab, cpt, cpu);
	cfs_cpt_del_node(cptab, cpt, cpu_to_node(cpu));
}
EXPORT_SYMBOL(cfs_cpt_unset_cpu);

int
cfs_cpt_set_cpumask(struct cfs_cpt_table *cptab, int cpt, const cpumask_t *mask)
{
	int cpu;

	if (cpumask_weight(mask) == 0 ||
	    cpumask_any_and(mask, cpu_online_mask) >= nr_cpu_ids) {
		CDEBUG(D_INFO, "No online CPU is found in the CPU mask "
			       "for CPU partition %d\n", cpt);
		return 0;
	}

	for_each_cpu(cpu, mask) {
		cfs_cpt_add_cpu(cptab, cpt, cpu);
		cfs_cpt_add_node(cptab, cpt, cpu_to_node(cpu));
	}

	return 1;
}
EXPORT_SYMBOL(cfs_cpt_set_cpumask);

void
cfs_cpt_unset_cpumask(struct cfs_cpt_table *cptab, int cpt,
		      const cpumask_t *mask)
{
	int cpu;

	for_each_cpu(cpu, mask)
		cfs_cpt_unset_cpu(cptab, cpt, cpu);
}
EXPORT_SYMBOL(cfs_cpt_unset_cpumask);

int
cfs_cpt_set_node(struct cfs_cpt_table *cptab, int cpt, int node)
{
	const cpumask_t *mask;
	int		cpu;

	if (node < 0 || node >= nr_node_ids) {
		CDEBUG(D_INFO,
		       "Invalid NUMA id %d for CPU partition %d\n", node, cpt);
		return 0;
	}

	mask = cpumask_of_node(node);

	for_each_cpu(cpu, mask)
		cfs_cpt_add_cpu(cptab, cpt, cpu);

	cfs_cpt_add_node(cptab, cpt, node);

	return 1;
}
EXPORT_SYMBOL(cfs_cpt_set_node);

void
cfs_cpt_unset_node(struct cfs_cpt_table *cptab, int cpt, int node)
{
	const cpumask_t *mask;
	int cpu;

	if (node < 0 || node >= nr_node_ids) {
		CDEBUG(D_INFO,
		       "Invalid NUMA id %d for CPU partition %d\n", node, cpt);
		return;
	}

	mask = cpumask_of_node(node);

	for_each_cpu(cpu, mask)
		cfs_cpt_del_cpu(cptab, cpt, cpu);

	cfs_cpt_del_node(cptab, cpt, node);
}
EXPORT_SYMBOL(cfs_cpt_unset_node);

int
cfs_cpt_set_nodemask(struct cfs_cpt_table *cptab, int cpt, nodemask_t *mask)
{
	int	i;

	for_each_node_mask(i, *mask) {
		if (!cfs_cpt_set_node(cptab, cpt, i))
			return 0;
	}

	return 1;
}
EXPORT_SYMBOL(cfs_cpt_set_nodemask);

void
cfs_cpt_unset_nodemask(struct cfs_cpt_table *cptab, int cpt, nodemask_t *mask)
{
	int	i;

	for_each_node_mask(i, *mask)
		cfs_cpt_unset_node(cptab, cpt, i);
}
EXPORT_SYMBOL(cfs_cpt_unset_nodemask);

int cfs_cpt_spread_node(struct cfs_cpt_table *cptab, int cpt)
{
	nodemask_t	*mask;
	int		weight;
	int		rotor;
	int		node;

	/* convert CPU partition ID to HW node id */

	if (cpt < 0 || cpt >= cptab->ctb_nparts) {
		mask = cptab->ctb_nodemask;
		rotor = cptab->ctb_spread_rotor++;
	} else {
		mask = cptab->ctb_parts[cpt].cpt_nodemask;
		rotor = cptab->ctb_parts[cpt].cpt_spread_rotor++;
	}

	weight = nodes_weight(*mask);
	LASSERT(weight > 0);

	rotor %= weight;

	for_each_node_mask(node, *mask) {
		if (rotor-- == 0)
			return node;
	}

	LBUG();
	return 0;
}
EXPORT_SYMBOL(cfs_cpt_spread_node);

int
cfs_cpt_current(struct cfs_cpt_table *cptab, int remap)
{
	int	cpu = smp_processor_id();
	int	cpt = cptab->ctb_cpu2cpt[cpu];

	if (cpt < 0) {
		if (!remap)
			return cpt;

		/* don't return negative value for safety of upper layer,
		 * instead we shadow the unknown cpu to a valid partition ID */
		cpt = cpu % cptab->ctb_nparts;
	}

	return cpt;
}
EXPORT_SYMBOL(cfs_cpt_current);

int
cfs_cpt_of_cpu(struct cfs_cpt_table *cptab, int cpu)
{
	LASSERT(cpu >= 0 && cpu < nr_cpu_ids);

	return cptab->ctb_cpu2cpt[cpu];
}
EXPORT_SYMBOL(cfs_cpt_of_cpu);

int
cfs_cpt_of_node(struct cfs_cpt_table *cptab, int node)
{
	if (node < 0 || node > nr_node_ids)
		return CFS_CPT_ANY;

	return cptab->ctb_node2cpt[node];
}
EXPORT_SYMBOL(cfs_cpt_of_node);

int
cfs_cpt_bind(struct cfs_cpt_table *cptab, int cpt)
{
	cpumask_t	*cpumask;
	nodemask_t	*nodemask;
	int		rc;
	int		i;

	LASSERT(cpt == CFS_CPT_ANY || (cpt >= 0 && cpt < cptab->ctb_nparts));

	if (cpt == CFS_CPT_ANY) {
		cpumask = cptab->ctb_cpumask;
		nodemask = cptab->ctb_nodemask;
	} else {
		cpumask = cptab->ctb_parts[cpt].cpt_cpumask;
		nodemask = cptab->ctb_parts[cpt].cpt_nodemask;
	}

	if (cpumask_any_and(cpumask, cpu_online_mask) >= nr_cpu_ids) {
		CERROR("No online CPU found in CPU partition %d, did someone "
		       "do CPU hotplug on system? You might need to reload "
		       "Lustre modules to keep system working well.\n", cpt);
		return -EINVAL;
	}

	for_each_online_cpu(i) {
		if (cpumask_test_cpu(i, cpumask))
			continue;

		rc = set_cpus_allowed_ptr(current, cpumask);
		set_mems_allowed(*nodemask);
		if (rc == 0)
			schedule(); /* switch to allowed CPU */

		return rc;
	}

	/* don't need to set affinity because all online CPUs are covered */
	return 0;
}
EXPORT_SYMBOL(cfs_cpt_bind);

/**
 * Choose max to \a number CPUs from \a node and set them in \a cpt.
 * We always prefer to choose CPU in the same core/socket.
 */
static int
cfs_cpt_choose_ncpus(struct cfs_cpt_table *cptab, int cpt,
		     cpumask_t *node, int number)
{
	cpumask_t	*socket = NULL;
	cpumask_t	*core = NULL;
	int		rc = 0;
	int		cpu;

	LASSERT(number > 0);

	if (number >= cpumask_weight(node)) {
		while (!cpumask_empty(node)) {
			cpu = cpumask_first(node);

			rc = cfs_cpt_set_cpu(cptab, cpt, cpu);
			if (!rc)
				return -EINVAL;
			cpumask_clear_cpu(cpu, node);
		}
		return 0;
	}

	/* allocate scratch buffer */
	LIBCFS_ALLOC(socket, cpumask_size());
	LIBCFS_ALLOC(core, cpumask_size());
	if (socket == NULL || core == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	while (!cpumask_empty(node)) {
		cpu = cpumask_first(node);

		/* get cpumask for cores in the same socket */
		cpumask_copy(socket, topology_core_cpumask(cpu));
		cpumask_and(socket, socket, node);

		LASSERT(!cpumask_empty(socket));

		while (!cpumask_empty(socket)) {
			int     i;

			/* get cpumask for hts in the same core */
			cpumask_copy(core, topology_sibling_cpumask(cpu));
			cpumask_and(core, core, node);

			LASSERT(!cpumask_empty(core));

			for_each_cpu(i, core) {
				cpumask_clear_cpu(i, socket);
				cpumask_clear_cpu(i, node);

				rc = cfs_cpt_set_cpu(cptab, cpt, i);
				if (!rc) {
					rc = -EINVAL;
					goto out;
				}

				if (--number == 0)
					goto out;
			}
			cpu = cpumask_first(socket);
		}
	}

out:
	if (socket != NULL)
		LIBCFS_FREE(socket, cpumask_size());
	if (core != NULL)
		LIBCFS_FREE(core, cpumask_size());
	return rc;
}

#define CPT_WEIGHT_MIN  4u

static unsigned int
cfs_cpt_num_estimate(void)
{
	unsigned nnode = num_online_nodes();
	unsigned ncpu  = num_online_cpus();
	unsigned ncpt;

	if (ncpu <= CPT_WEIGHT_MIN) {
		ncpt = 1;
		goto out;
	}

	/* generate reasonable number of CPU partitions based on total number
	 * of CPUs, Preferred N should be power2 and match this condition:
	 * 2 * (N - 1)^2 < NCPUS <= 2 * N^2 */
	for (ncpt = 2; ncpu > 2 * ncpt * ncpt; ncpt <<= 1) {}

	if (ncpt <= nnode) { /* fat numa system */
		while (nnode > ncpt)
			nnode >>= 1;

	} else { /* ncpt > nnode */
		while ((nnode << 1) <= ncpt)
			nnode <<= 1;
	}

	ncpt = nnode;

out:
#if (BITS_PER_LONG == 32)
	/* config many CPU partitions on 32-bit system could consume
	 * too much memory */
	ncpt = min(2U, ncpt);
#endif
	while (ncpu % ncpt != 0)
		ncpt--; /* worst case is 1 */

	return ncpt;
}

static struct cfs_cpt_table *
cfs_cpt_table_create(int ncpt)
{
	struct cfs_cpt_table *cptab = NULL;
	cpumask_t	*mask = NULL;
	int		cpt = 0;
	int		num;
	int		rc;
	int		i;

	rc = cfs_cpt_num_estimate();
	if (ncpt <= 0)
		ncpt = rc;

	if (ncpt > num_online_cpus() || ncpt > 4 * rc) {
		CWARN("CPU partition number %d is larger than suggested "
		      "value (%d), your system may have performance"
		      "issue or run out of memory while under pressure\n",
		      ncpt, rc);
	}

	if (num_online_cpus() % ncpt != 0) {
		CERROR("CPU number %d is not multiple of cpu_npartition %d, "
		       "please try different cpu_npartitions value or"
		       "set pattern string by cpu_pattern=STRING\n",
		       (int)num_online_cpus(), ncpt);
		goto failed;
	}

	cptab = cfs_cpt_table_alloc(ncpt);
	if (cptab == NULL) {
		CERROR("Failed to allocate CPU map(%d)\n", ncpt);
		goto failed;
	}

	num = num_online_cpus() / ncpt;
	if (num == 0) {
		CERROR("CPU changed while setting CPU partition\n");
		goto failed;
	}

	LIBCFS_ALLOC(mask, cpumask_size());
	if (mask == NULL) {
		CERROR("Failed to allocate scratch cpumask\n");
		goto failed;
	}

	for_each_online_node(i) {
		cpumask_copy(mask, cpumask_of_node(i));

		while (!cpumask_empty(mask)) {
			struct cfs_cpu_partition *part;
			int    n;

			/* Each emulated NUMA node has all allowed CPUs in
			 * the mask.
			 * End loop when all partitions have assigned CPUs.
			 */
			if (cpt == ncpt)
				break;

			part = &cptab->ctb_parts[cpt];

			n = num - cpumask_weight(part->cpt_cpumask);
			LASSERT(n > 0);

			rc = cfs_cpt_choose_ncpus(cptab, cpt, mask, n);
			if (rc < 0)
				goto failed;

			LASSERT(num >= cpumask_weight(part->cpt_cpumask));
			if (num == cpumask_weight(part->cpt_cpumask))
				cpt++;
		}
	}

	if (cpt != ncpt ||
	    num != cpumask_weight(cptab->ctb_parts[ncpt - 1].cpt_cpumask)) {
		CERROR("Expect %d(%d) CPU partitions but got %d(%d), "
		       "CPU hotplug/unplug while setting?\n",
		       cptab->ctb_nparts, num, cpt,
		       cpumask_weight(cptab->ctb_parts[ncpt - 1].cpt_cpumask));
		goto failed;
	}

	LIBCFS_FREE(mask, cpumask_size());

	return cptab;

 failed:
	CERROR("Failed to setup CPU-partition-table with %d "
	       "CPU-partitions, online HW nodes: %d, HW cpus: %d.\n",
	       ncpt, num_online_nodes(), num_online_cpus());

	if (mask != NULL)
		LIBCFS_FREE(mask, cpumask_size());

	if (cptab != NULL)
		cfs_cpt_table_free(cptab);

	return NULL;
}

static struct cfs_cpt_table *
cfs_cpt_table_create_pattern(char *pattern)
{
	struct cfs_cpt_table	*cptab;
	char			*str;
	int			node	= 0;
	int			ncpt	= 0;
	int			high;
	int			cpt;
	int			rc;
	int			c;
	int			i;

	str = cfs_trimwhite(pattern);
	if (*str == 'n' || *str == 'N') {
		pattern = str + 1;
		if (*pattern != '\0') {
			node = 1; /* numa pattern */

		} else { /* shortcut to create CPT from NUMA & CPU topology */
			node = -1;
			ncpt = num_online_nodes();
		}
	}

	if (ncpt == 0) { /* scanning bracket which is mark of partition */
		for (str = pattern;; str++, ncpt++) {
			str = strchr(str, '[');
			if (str == NULL)
				break;
		}
	}

	if (ncpt == 0 ||
	    (node && ncpt > num_online_nodes()) ||
	    (!node && ncpt > num_online_cpus())) {
		CERROR("Invalid pattern %s, or too many partitions %d\n",
		       pattern, ncpt);
		return NULL;
	}

	cptab = cfs_cpt_table_alloc(ncpt);
	if (cptab == NULL) {
		CERROR("Failed to allocate cpu partition table\n");
		return NULL;
	}

	if (node < 0) { /* shortcut to create CPT from NUMA & CPU topology */
		cpt = 0;
		for_each_online_node(i) {
			if (cpt >= ncpt) {
				CERROR("CPU changed while setting CPU "
				       "partition table, %d/%d\n", cpt, ncpt);
				goto failed;
			}

			rc = cfs_cpt_set_node(cptab, cpt++, i);
			if (!rc)
				goto failed;
		}
		return cptab;
	}

	high = node ? nr_node_ids - 1 : nr_cpu_ids - 1;

	for (str = cfs_trimwhite(pattern), c = 0;; c++) {
		struct cfs_range_expr	*range;
		struct cfs_expr_list	*el;
		char			*bracket = strchr(str, '[');
		int			n;

		if (bracket == NULL) {
			if (*str != 0) {
				CERROR("Invalid pattern %s\n", str);
				goto failed;
			} else if (c != ncpt) {
				CERROR("expect %d partitions but found %d\n",
				       ncpt, c);
				goto failed;
			}
			break;
		}

		if (sscanf(str, "%d%n", &cpt, &n) < 1) {
			CERROR("Invalid cpu pattern %s\n", str);
			goto failed;
		}

		if (cpt < 0 || cpt >= ncpt) {
			CERROR("Invalid partition id %d, total partitions %d\n",
			       cpt, ncpt);
			goto failed;
		}

		if (cfs_cpt_weight(cptab, cpt) != 0) {
			CERROR("Partition %d has already been set.\n", cpt);
			goto failed;
		}

		str = cfs_trimwhite(str + n);
		if (str != bracket) {
			CERROR("Invalid pattern %s\n", str);
			goto failed;
		}

		bracket = strchr(str, ']');
		if (bracket == NULL) {
			CERROR("missing right bracket for cpt %d, %s\n",
			       cpt, str);
			goto failed;
		}

		if (cfs_expr_list_parse(str, (bracket - str) + 1,
					0, high, &el) != 0) {
			CERROR("Can't parse number range: %s\n", str);
			goto failed;
		}

		list_for_each_entry(range, &el->el_exprs, re_link) {
			for (i = range->re_lo; i <= range->re_hi; i++) {
				if ((i - range->re_lo) % range->re_stride != 0)
					continue;

				rc = node ? cfs_cpt_set_node(cptab, cpt, i) :
					    cfs_cpt_set_cpu(cptab, cpt, i);
				if (!rc) {
					cfs_expr_list_free(el);
					goto failed;
				}
			}
		}

		cfs_expr_list_free(el);

		if (!cfs_cpt_online(cptab, cpt)) {
			CERROR("No online CPU is found on partition %d\n", cpt);
			goto failed;
		}

		str = cfs_trimwhite(bracket + 1);
	}

	return cptab;

 failed:
	cfs_cpt_table_free(cptab);
	return NULL;
}

#ifdef CONFIG_HOTPLUG_CPU
static int
cfs_cpu_notify(struct notifier_block *self, unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	bool	     warn;

	switch (action) {
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
	default:
		if (action != CPU_DEAD && action != CPU_DEAD_FROZEN) {
			CDEBUG(D_INFO, "CPU changed [cpu %u action %lx]\n",
			       cpu, action);
			break;
		}

		/* if all HTs in a core are offline, it may break affinity */
		warn = cpumask_any_and(topology_sibling_cpumask(cpu),
				       cpu_online_mask) >= nr_cpu_ids;
		CDEBUG(warn ? D_WARNING : D_INFO,
		       "Lustre: can't support CPU plug-out well now, "
		       "performance and stability could be impacted"
		       "[CPU %u action: %lx]\n", cpu, action);
	}

	return NOTIFY_OK;
}

static struct notifier_block cfs_cpu_notifier = {
	.notifier_call	= cfs_cpu_notify,
	.priority	= 0
};

#endif

void
cfs_cpu_fini(void)
{
	if (cfs_cpt_table != NULL)
		cfs_cpt_table_free(cfs_cpt_table);

#ifdef CONFIG_HOTPLUG_CPU
	unregister_hotcpu_notifier(&cfs_cpu_notifier);
#endif
}

int
cfs_cpu_init(void)
{
	LASSERT(cfs_cpt_table == NULL);

#ifdef CONFIG_HOTPLUG_CPU
	register_hotcpu_notifier(&cfs_cpu_notifier);
#endif
	get_online_cpus();
	if (*cpu_pattern != 0) {
		char *cpu_pattern_dup = kstrdup(cpu_pattern, GFP_KERNEL);

		if (cpu_pattern_dup == NULL) {
			CERROR("Failed to duplicate cpu_pattern\n");
			goto failed;
		}

		cfs_cpt_table = cfs_cpt_table_create_pattern(cpu_pattern_dup);
		kfree(cpu_pattern_dup);
		if (cfs_cpt_table == NULL) {
			CERROR("Failed to create cptab from pattern %s\n",
			       cpu_pattern);
			goto failed;
		}

	} else {
		cfs_cpt_table = cfs_cpt_table_create(cpu_npartitions);
		if (cfs_cpt_table == NULL) {
			CERROR("Failed to create ptable with npartitions %d\n",
			       cpu_npartitions);
			goto failed;
		}
	}
	put_online_cpus();

	LCONSOLE(0, "HW nodes: %d, HW CPU cores: %d, npartitions: %d\n",
		     num_online_nodes(), num_online_cpus(),
		     cfs_cpt_number(cfs_cpt_table));
	return 0;

failed:
	put_online_cpus();
	cfs_cpu_fini();
	return -1;
}

#endif
