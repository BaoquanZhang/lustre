/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2007 Cluster File Systems, Inc.
 *   Author: Yury Umanets <umka@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

/* Idea of this code is rather simple. Each second, for each server namespace
 * we have SLV - server lock volume which is calculated on current number of
 * granted locks, grant speed for past period, etc - that is, locking load.
 * This SLV number may be thought as a flow definition for simplicity. It is
 * sent to clients with each occasion to let them know what is current load
 * situation on the server. By default, at the beginning, SLV on server is
 * set max value which is calculated as the following: allow to one client
 * have all locks of limit ->pl_limit for 10h.
 *
 * Next, on clients, number of cached locks is not limited artificially in any
 * way as it was before. Instead, client calculates CLV, that is, client lock
 * volume for each lock and compares it with last SLV from the server. CLV is
 * calculated as the number of locks in LRU * lock live time in seconds. If
 * CLV > SLV - lock is canceled.
 *
 * Client has LVF, that is, lock volume factor which regulates how much sensitive
 * client should be about last SLV from server. The higher LVF is the more locks
 * will be canceled on client. Default value for it is 1. Setting LVF to 2 means
 * that client will cancel locks 2 times faster.
 *
 * Locks on a client will be canceled more intensively in these cases:
 * (1) if SLV is smaller, that is, load is higher on the server;
 * (2) client has a lot of locks (the more locks are held by client, the bigger
 *     chances that some of them should be canceled);
 * (3) client has old locks (taken some time ago);
 *
 * Thus, according to flow paradigm that we use for better understanding SLV,
 * CLV is the volume of particle in flow described by SLV. According to this,
 * if flow is getting thinner, more and more particles become outside of it and
 * as particles are locks, they should be canceled.
 *
 * General idea of this belongs to Vitaly Fertman (vitaly@clusterfs.com). Andreas
 * Dilger (adilger@clusterfs.com) proposed few nice ideas like using LVF and many
 * cleanups. Flow definition to allow more easy understanding of the logic belongs
 * to Nikita Danilov (nikita@clusterfs.com) as well as many cleanups and fixes.
 * And design and implementation are done by Yury Umanets (umka@clusterfs.com).
 *
 * Glossary for terms used:
 *
 * pl_limit - Number of allowed locks in pool. Applies to server and client
 * side (tunable);
 *
 * pl_granted - Number of granted locks (calculated);
 * pl_grant_rate - Number of granted locks for last T (calculated);
 * pl_cancel_rate - Number of canceled locks for last T (calculated);
 * pl_grant_speed - Grant speed (GR - CR) for last T (calculated);
 * pl_grant_plan - Planned number of granted locks for next T (calculated);
 *
 * pl_grant_step - Grant plan step, that is how ->pl_grant_plan
 * will change in next T (tunable);
 *
 * pl_server_lock_volume - Current server lock volume (calculated);
 *
 * As it may be seen from list above, we have few possible tunables which may
 * affect behavior much. They all may be modified via proc. However, they also
 * give a possibility for constructing few pre-defined behavior policies. If
 * none of predefines is suitable for a working pattern being used, new one may
 * be "constructed" via proc tunables.
 */

#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
# include <lustre_dlm.h>
#else
# include <liblustre.h>
# include <libcfs/kp30.h>
#endif

#include <obd_class.h>
#include <obd_support.h>
#include "ldlm_internal.h"

#ifdef HAVE_LRU_RESIZE_SUPPORT

/* 50 ldlm locks for 1MB of RAM. */
#define LDLM_POOL_HOST_L ((num_physpages >> (20 - PAGE_SHIFT)) * 50)

/* Default step in % for grant plan. */
#define LDLM_POOL_GSP (5)

/* LDLM_POOL_GSP% of all locks is default GP. */
#define LDLM_POOL_GP(L)   ((L) * LDLM_POOL_GSP / 100)

/* Max age for locks on clients. */
#define LDLM_POOL_MAX_AGE (36000)

#ifdef __KERNEL__
extern cfs_proc_dir_entry_t *ldlm_ns_proc_dir;
#endif

extern atomic_t ldlm_srv_namespace_nr;
extern atomic_t ldlm_cli_namespace_nr;
extern struct list_head ldlm_namespace_list;
extern struct semaphore ldlm_namespace_lock;

#define avg(src, add) \
        ((src) = ((src) + (add)) / 2)

static inline __u64 dru(__u64 val, __u32 div)
{
        __u64 ret = val + (div - 1);
        do_div(ret, div);
        return ret;
}

static inline __u64 ldlm_pool_slv_max(__u32 L)
{
        /* Allow to have all locks for 1 client for 10 hrs.
         * Formula is the following: limit * 10h / 1 client. */
        __u64 lim = L *  LDLM_POOL_MAX_AGE / 1;
        return lim;
}

static inline __u64 ldlm_pool_slv_min(__u32 L)
{
        return 1;
}

enum {
        LDLM_POOL_GRANTED_STAT = 0,
        LDLM_POOL_GRANT_RATE_STAT,
        LDLM_POOL_CANCEL_RATE_STAT,
        LDLM_POOL_GRANT_PLAN_STAT,
        LDLM_POOL_SLV_STAT,
        LDLM_POOL_LAST_STAT
};

static inline struct ldlm_namespace *ldlm_pl2ns(struct ldlm_pool *pl)
{
        return container_of(pl, struct ldlm_namespace, ns_pool);
}

static int ldlm_srv_pool_recalc(struct ldlm_pool *pl)
{
        int slv_factor, limit, granted, grant_speed;
        int grant_rate, cancel_rate, grant_step;
        time_t recalc_interval_sec;
        __u32 grant_plan;
        __u64 slv;
        ENTRY;

        spin_lock(&pl->pl_lock);

        /* Get all values to local variables to avoid change some of them in
         * the middle of re-calc. */
        slv = ldlm_pool_get_slv(pl);
        limit = ldlm_pool_get_limit(pl);
        granted = atomic_read(&pl->pl_granted);
        grant_rate = atomic_read(&pl->pl_grant_rate);
        grant_plan = atomic_read(&pl->pl_grant_plan);
        grant_step = atomic_read(&pl->pl_grant_step);
        grant_speed = atomic_read(&pl->pl_grant_speed);
        cancel_rate = atomic_read(&pl->pl_cancel_rate);

        /* Zero out grant/cancel rates and speed for this T. */
        atomic_set(&pl->pl_grant_rate, 0);
        atomic_set(&pl->pl_cancel_rate, 0);
        atomic_set(&pl->pl_grant_speed, 0);

        /* Make sure that we use correct data for statistics. Pools thread may
         * be not scheduled long time due to big CPU contention. We need to
         * catch this. */
        recalc_interval_sec = cfs_duration_sec(cfs_time_current() -
                                               pl->pl_update_time);
        if (recalc_interval_sec == 0)
                recalc_interval_sec = 1;

        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_SLV_STAT, slv);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
                            granted);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
                            grant_rate / recalc_interval_sec);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_PLAN_STAT,
                            grant_plan / recalc_interval_sec);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
                            cancel_rate / recalc_interval_sec);

        /* Correcting old @grant_plan which may be obsolete in the case of big 
         * load on the server, when pools thread is not scheduled every 1s sharp
         * (curent period). All values used in calculation are updated from 
         * other threads and up-to-date. Only @grant_plan is calculated by pool 
         * thread and directly affects SLV. */
        grant_plan += grant_speed - (grant_speed / recalc_interval_sec);

        if ((slv_factor = limit - (granted - grant_plan)) <= 0)
                slv_factor = 1;

        grant_plan = granted + ((limit - granted) * grant_step) / 100;
        slv = (slv * ((slv_factor * 100) / limit));
        slv = dru(slv, 100);

        if (slv > ldlm_pool_slv_max(limit)) {
                CDEBUG(D_DLMTRACE, "Correcting SLV to allowed max "LPU64"\n",
                       ldlm_pool_slv_max(limit));
                slv = ldlm_pool_slv_max(limit);
        } else if (slv < ldlm_pool_slv_min(limit)) {
                CDEBUG(D_DLMTRACE, "Correcting SLV to allowed min "LPU64"\n",
                       ldlm_pool_slv_min(limit));
                slv = ldlm_pool_slv_min(limit);
        }

        ldlm_pool_set_slv(pl, slv);
        atomic_set(&pl->pl_grant_plan, grant_plan);
        pl->pl_update_time = cfs_time_current();
        spin_unlock(&pl->pl_lock);

        RETURN(0);
}

/* Our goal here is to decrease SLV the way to make a client hold
 * @nr locks smaller in next 10h. */
static int ldlm_srv_pool_shrink(struct ldlm_pool *pl,
                                int nr, unsigned int gfp_mask)
{
        __u32 granted, limit;
        __u64 slv_delta;
        ENTRY;

        /* Client already canceled locks but server is already in shrinker and
         * can't cancel anything. Let's catch this race. */
        if ((granted = atomic_read(&pl->pl_granted)) == 0)
                RETURN(0);

        spin_lock(&pl->pl_lock);

        /* Simple proportion but it gives impression on how much should be
         * SLV changed for request @nr of locks to be canceled.*/
        slv_delta = nr * ldlm_pool_get_slv(pl);
        limit = ldlm_pool_get_limit(pl);
        do_div(slv_delta, granted);

        /* As SLV has some dependence on historical data, that is new value
         * is based on old one, this decreasing will make clients get some
         * locks back to the server and after some time it will stabilize.*/
        if (slv_delta < ldlm_pool_get_slv(pl))
                ldlm_pool_set_slv(pl, ldlm_pool_get_slv(pl) - slv_delta);
        else
                ldlm_pool_set_slv(pl, ldlm_pool_slv_min(limit));
        spin_unlock(&pl->pl_lock);

        /* We did not really free any memory here so far, it only will be
         * freed later may be, so that we return 0 to not confuse VM. */
        RETURN(0);
}

static int ldlm_cli_pool_recalc(struct ldlm_pool *pl)
{
        int grant_rate, cancel_rate;
        time_t recalc_interval_sec;
        ENTRY;

        spin_lock(&pl->pl_lock);
        grant_rate = atomic_read(&pl->pl_grant_rate);
        cancel_rate = atomic_read(&pl->pl_cancel_rate);

        recalc_interval_sec = cfs_duration_sec(cfs_time_current() -
                                               pl->pl_update_time);
        if (recalc_interval_sec == 0)
                recalc_interval_sec = 1;

        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_SLV_STAT,
                            ldlm_pool_get_slv(pl));
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
                            atomic_read(&pl->pl_granted));
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
                            grant_rate / recalc_interval_sec);
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
                            cancel_rate / recalc_interval_sec);

        spin_unlock(&pl->pl_lock);

        ldlm_cancel_lru(ldlm_pl2ns(pl), 0, LDLM_ASYNC);
        RETURN(0);
}

static int ldlm_cli_pool_shrink(struct ldlm_pool *pl,
                                int nr, unsigned int gfp_mask)
{
        ENTRY;
        RETURN(ldlm_cancel_lru(ldlm_pl2ns(pl), nr, LDLM_SYNC));
}

int ldlm_pool_recalc(struct ldlm_pool *pl)
{
        if (pl->pl_recalc != NULL && pool_recalc_enabled(pl))
                return pl->pl_recalc(pl);
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_recalc);

int ldlm_pool_shrink(struct ldlm_pool *pl, int nr,
                     unsigned int gfp_mask)
{
        if (pl->pl_shrink != NULL && pool_shrink_enabled(pl)) {
                CDEBUG(D_DLMTRACE, "%s: request to shrink %d locks\n",
                       pl->pl_name, nr);
                return pl->pl_shrink(pl, nr, gfp_mask);
        }
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_shrink);

/* The purpose of this function is to re-setup limit and maximal allowed
 * slv according to the passed limit. */
int ldlm_pool_setup(struct ldlm_pool *pl, __u32 limit)
{
        ENTRY;
        if (ldlm_pl2ns(pl)->ns_client == LDLM_NAMESPACE_SERVER) {
                spin_lock(&pl->pl_lock);
                ldlm_pool_set_limit(pl, limit);
                spin_unlock(&pl->pl_lock);
        }
        RETURN(0);
}
EXPORT_SYMBOL(ldlm_pool_setup);

#ifdef __KERNEL__
static int lprocfs_rd_pool_state(char *page, char **start, off_t off,
                                 int count, int *eof, void *data)
{
        int nr = 0, granted, grant_rate, cancel_rate;
        int grant_speed, grant_plan, grant_step;
        struct ldlm_pool *pl = data;
        __u32 limit;
        __u64 slv;

        spin_lock(&pl->pl_lock);
        slv = pl->pl_server_lock_volume;
        limit = ldlm_pool_get_limit(pl);
        granted = atomic_read(&pl->pl_granted);
        grant_rate = atomic_read(&pl->pl_grant_rate);
        cancel_rate = atomic_read(&pl->pl_cancel_rate);
        grant_speed = atomic_read(&pl->pl_grant_speed);
        grant_plan = atomic_read(&pl->pl_grant_plan);
        grant_step = atomic_read(&pl->pl_grant_step);
        spin_unlock(&pl->pl_lock);

        nr += snprintf(page + nr, count - nr, "LDLM pool state (%s):\n",
                       pl->pl_name);
        nr += snprintf(page + nr, count - nr, "  SLV: "LPU64"\n", slv);
        if (ldlm_pl2ns(pl)->ns_client == LDLM_NAMESPACE_SERVER) {
                nr += snprintf(page + nr, count - nr, "  GSP: %d%%\n",
                               grant_step);
                nr += snprintf(page + nr, count - nr, "  GP:  %d\n",
                               grant_plan);
        } else {
                nr += snprintf(page + nr, count - nr, "  LVF: %d\n",
                               atomic_read(&pl->pl_lock_volume_factor));
        }
        nr += snprintf(page + nr, count - nr, "  GR:  %d\n", grant_rate);
        nr += snprintf(page + nr, count - nr, "  CR:  %d\n", cancel_rate);
        nr += snprintf(page + nr, count - nr, "  GS:  %d\n", grant_speed);
        nr += snprintf(page + nr, count - nr, "  G:   %d\n", granted);
        nr += snprintf(page + nr, count - nr, "  L:   %d\n", limit);
        return nr;
}

static int ldlm_pool_proc_init(struct ldlm_pool *pl)
{
        struct ldlm_namespace *ns = ldlm_pl2ns(pl);
        struct proc_dir_entry *parent_ns_proc;
        struct lprocfs_vars pool_vars[2];
        char *var_name = NULL;
        int rc = 0;
        ENTRY;

        OBD_ALLOC(var_name, MAX_STRING_SIZE + 1);
        if (!var_name)
                RETURN(-ENOMEM);

        parent_ns_proc = lprocfs_srch(ldlm_ns_proc_dir, ns->ns_name);
        if (parent_ns_proc == NULL) {
                CERROR("%s: proc entry is not initialized\n",
                       ns->ns_name);
                GOTO(out_free_name, rc = -EINVAL);
        }
        pl->pl_proc_dir = lprocfs_register("pool", parent_ns_proc,
                                           NULL, NULL);
        if (IS_ERR(pl->pl_proc_dir)) {
                CERROR("LProcFS failed in ldlm-pool-init\n");
                rc = PTR_ERR(pl->pl_proc_dir);
                GOTO(out_free_name, rc);
        }

        var_name[MAX_STRING_SIZE] = '\0';
        memset(pool_vars, 0, sizeof(pool_vars));
        pool_vars[0].name = var_name;

        snprintf(var_name, MAX_STRING_SIZE, "server_lock_volume");
        pool_vars[0].data = &pl->pl_server_lock_volume;
        pool_vars[0].read_fptr = lprocfs_rd_u64;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "limit");
        pool_vars[0].data = &pl->pl_limit;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        pool_vars[0].write_fptr = lprocfs_wr_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "granted");
        pool_vars[0].data = &pl->pl_granted;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "control");
        pool_vars[0].data = &pl->pl_control;
        pool_vars[0].read_fptr = lprocfs_rd_uint;
        pool_vars[0].write_fptr = lprocfs_wr_uint;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "grant_speed");
        pool_vars[0].data = &pl->pl_grant_speed;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "cancel_rate");
        pool_vars[0].data = &pl->pl_cancel_rate;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        snprintf(var_name, MAX_STRING_SIZE, "grant_rate");
        pool_vars[0].data = &pl->pl_grant_rate;
        pool_vars[0].read_fptr = lprocfs_rd_atomic;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        if (ns->ns_client == LDLM_NAMESPACE_SERVER) {
                snprintf(var_name, MAX_STRING_SIZE, "grant_plan");
                pool_vars[0].data = &pl->pl_grant_plan;
                pool_vars[0].read_fptr = lprocfs_rd_atomic;
                lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

                snprintf(var_name, MAX_STRING_SIZE, "grant_step");
                pool_vars[0].data = &pl->pl_grant_step;
                pool_vars[0].read_fptr = lprocfs_rd_atomic;
                pool_vars[0].write_fptr = lprocfs_wr_atomic;
                lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);
        } else {
                snprintf(var_name, MAX_STRING_SIZE, "lock_volume_factor");
                pool_vars[0].data = &pl->pl_lock_volume_factor;
                pool_vars[0].read_fptr = lprocfs_rd_uint;
                pool_vars[0].write_fptr = lprocfs_wr_uint;
                lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);
        }

        snprintf(var_name, MAX_STRING_SIZE, "state");
        pool_vars[0].data = pl;
        pool_vars[0].read_fptr = lprocfs_rd_pool_state;
        lprocfs_add_vars(pl->pl_proc_dir, pool_vars, 0);

        pl->pl_stats = lprocfs_alloc_stats(LDLM_POOL_LAST_STAT -
                                           LDLM_POOL_GRANTED_STAT);
        if (!pl->pl_stats)
                GOTO(out_free_name, rc = -ENOMEM);

        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "granted", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant_rate", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "cancel_rate", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_PLAN_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant_plan", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SLV_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "slv", "slv");
        lprocfs_register_stats(pl->pl_proc_dir, "stats", pl->pl_stats);

        EXIT;
out_free_name:
        OBD_FREE(var_name, MAX_STRING_SIZE + 1);
        return rc;
}

static void ldlm_pool_proc_fini(struct ldlm_pool *pl)
{
        if (pl->pl_stats != NULL) {
                lprocfs_free_stats(&pl->pl_stats);
                pl->pl_stats = NULL;
        }
        if (pl->pl_proc_dir != NULL) {
                lprocfs_remove(&pl->pl_proc_dir);
                pl->pl_proc_dir = NULL;
        }
}
#else /* !__KERNEL__*/
#define ldlm_pool_proc_init(pl) (0)
#define ldlm_pool_proc_fini(pl) while (0) {}
#endif

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
                   int idx, ldlm_side_t client)
{
        int rc;
        ENTRY;

        spin_lock_init(&pl->pl_lock);
        atomic_set(&pl->pl_granted, 0);
        pl->pl_update_time = cfs_time_current();
        atomic_set(&pl->pl_lock_volume_factor, 1);

        atomic_set(&pl->pl_grant_rate, 0);
        atomic_set(&pl->pl_cancel_rate, 0);
        atomic_set(&pl->pl_grant_speed, 0);
        pl->pl_control = LDLM_POOL_CTL_FULL;
        atomic_set(&pl->pl_grant_step, LDLM_POOL_GSP);
        atomic_set(&pl->pl_grant_plan, LDLM_POOL_GP(LDLM_POOL_HOST_L));

        snprintf(pl->pl_name, sizeof(pl->pl_name), "ldlm-pool-%s-%d",
                 ns->ns_name, idx);

        if (client == LDLM_NAMESPACE_SERVER) {
                pl->pl_recalc = ldlm_srv_pool_recalc;
                pl->pl_shrink = ldlm_srv_pool_shrink;
                ldlm_pool_set_limit(pl, LDLM_POOL_HOST_L);
                ldlm_pool_set_slv(pl, ldlm_pool_slv_max(LDLM_POOL_HOST_L));
        } else {
                ldlm_pool_set_slv(pl, 1);
                ldlm_pool_set_limit(pl, 1);
                pl->pl_recalc = ldlm_cli_pool_recalc;
                pl->pl_shrink = ldlm_cli_pool_shrink;
        }

        rc = ldlm_pool_proc_init(pl);
        if (rc)
                RETURN(rc);

        CDEBUG(D_DLMTRACE, "Lock pool %s is initialized\n", pl->pl_name);

        RETURN(rc);
}
EXPORT_SYMBOL(ldlm_pool_init);

void ldlm_pool_fini(struct ldlm_pool *pl)
{
        ENTRY;
        ldlm_pool_proc_fini(pl);
        pl->pl_recalc = NULL;
        pl->pl_shrink = NULL;
        EXIT;
}
EXPORT_SYMBOL(ldlm_pool_fini);

void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        ENTRY;
        atomic_inc(&pl->pl_granted);
        atomic_inc(&pl->pl_grant_rate);
        atomic_inc(&pl->pl_grant_speed);
        EXIT;
}
EXPORT_SYMBOL(ldlm_pool_add);

void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        ENTRY;
        LASSERT(atomic_read(&pl->pl_granted) > 0);
        atomic_dec(&pl->pl_granted);
        atomic_inc(&pl->pl_cancel_rate);
        atomic_dec(&pl->pl_grant_speed);
        EXIT;
}
EXPORT_SYMBOL(ldlm_pool_del);

/* ->pl_lock should be taken. */
__u64 ldlm_pool_get_slv(struct ldlm_pool *pl)
{
        return pl->pl_server_lock_volume;
}
EXPORT_SYMBOL(ldlm_pool_get_slv);

/* ->pl_lock should be taken. */
void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv)
{
        pl->pl_server_lock_volume = slv;
}
EXPORT_SYMBOL(ldlm_pool_set_slv);

__u32 ldlm_pool_get_limit(struct ldlm_pool *pl)
{
        return atomic_read(&pl->pl_limit);
}
EXPORT_SYMBOL(ldlm_pool_get_limit);

void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit)
{
        atomic_set(&pl->pl_limit, limit);
}
EXPORT_SYMBOL(ldlm_pool_set_limit);

/* Server side is only enabled for kernel space for now. */
#ifdef __KERNEL__
static int ldlm_pool_granted(struct ldlm_pool *pl)
{
        return atomic_read(&pl->pl_granted);
}

static struct ptlrpc_thread *ldlm_pools_thread;
static struct shrinker *ldlm_pools_shrinker;
static struct completion ldlm_pools_comp;

static int ldlm_pools_thread_main(void *arg)
{
        struct ptlrpc_thread *thread = (struct ptlrpc_thread *)arg;
        char *t_name = "ldlm_poold";
        ENTRY;

        cfs_daemonize(t_name);
        thread->t_flags = SVC_RUNNING;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        CDEBUG(D_DLMTRACE, "%s: pool thread starting, process %d\n",
               t_name, cfs_curproc_pid());

        while (1) {
                __u32 nr_l = 0, nr_p = 0, l;
                struct ldlm_namespace *ns;
                struct l_wait_info lwi;
                int rc, equal = 0;

                /* Check all namespaces. */
                mutex_down(&ldlm_namespace_lock);
                list_for_each_entry(ns, &ldlm_namespace_list, ns_list_chain) {
                        if (ns->ns_appetite != LDLM_NAMESPACE_MODEST)
                                continue;

                        if (ns->ns_client == LDLM_NAMESPACE_SERVER) {
                                l = ldlm_pool_granted(&ns->ns_pool);
                                if (l == 0)
                                        l = 1;

                                /* Set the modest pools limit equal to
                                 * their avg granted locks + 5%. */
                                l += dru(l * LDLM_POOLS_MODEST_MARGIN, 100);
                                ldlm_pool_setup(&ns->ns_pool, l);
                                nr_l += l;
                                nr_p++;
                        }

                        /* After setup is done - recalc the pool. */
                        rc = ldlm_pool_recalc(&ns->ns_pool);
                        if (rc)
                                CERROR("%s: pool recalculation error "
                                       "%d\n", ns->ns_pool.pl_name, rc);
                }

                if (nr_l >= 2 * (LDLM_POOL_HOST_L / 3)) {
                        CWARN("Modest pools eat out 2/3 of locks limit. %d of %lu. "
                              "Upgrade server!\n", nr_l, LDLM_POOL_HOST_L);
                        equal = 1;
                }

                list_for_each_entry(ns, &ldlm_namespace_list, ns_list_chain) {
                        if (!equal && ns->ns_appetite != LDLM_NAMESPACE_GREEDY)
                                continue;

                        if (ns->ns_client == LDLM_NAMESPACE_SERVER) {
                                if (equal) {
                                        /* In the case 2/3 locks are eaten out by
                                         * modest pools, we re-setup equal limit
                                         * for _all_ pools. */
                                        l = LDLM_POOL_HOST_L /
                                                atomic_read(&ldlm_srv_namespace_nr);
                                } else {
                                        /* All the rest of greedy pools will have
                                         * all locks in equal parts.*/
                                        l = (LDLM_POOL_HOST_L - nr_l) /
                                                (atomic_read(&ldlm_srv_namespace_nr) -
                                                 nr_p);
                                }
                                ldlm_pool_setup(&ns->ns_pool, l);
                        }

                        /* After setup is done - recalc the pool. */
                        rc = ldlm_pool_recalc(&ns->ns_pool);
                        if (rc)
                                CERROR("%s: pool recalculation error "
                                       "%d\n", ns->ns_pool.pl_name, rc);
                }
                mutex_up(&ldlm_namespace_lock);

                /* Wait until the next check time, or until we're
                 * stopped. */
                lwi = LWI_TIMEOUT(cfs_time_seconds(LDLM_POOLS_THREAD_PERIOD),
                                  NULL, NULL);
                l_wait_event(thread->t_ctl_waitq, (thread->t_flags &
                                                   (SVC_STOPPING|SVC_EVENT)),
                             &lwi);

                if (thread->t_flags & SVC_STOPPING) {
                        thread->t_flags &= ~SVC_STOPPING;
                        break;
                } else if (thread->t_flags & SVC_EVENT) {
                        thread->t_flags &= ~SVC_EVENT;
                }
        }

        thread->t_flags = SVC_STOPPED;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        CDEBUG(D_DLMTRACE, "%s: pool thread exiting, process %d\n",
               t_name, cfs_curproc_pid());

        complete_and_exit(&ldlm_pools_comp, 0);
}

static int ldlm_pools_thread_start(ldlm_side_t client)
{
        struct l_wait_info lwi = { 0 };
        int rc;
        ENTRY;

        if (ldlm_pools_thread != NULL)
                RETURN(-EALREADY);

        OBD_ALLOC_PTR(ldlm_pools_thread);
        if (ldlm_pools_thread == NULL)
                RETURN(-ENOMEM);

        ldlm_pools_thread->t_id = client;
        init_completion(&ldlm_pools_comp);
        cfs_waitq_init(&ldlm_pools_thread->t_ctl_waitq);

        /* CLONE_VM and CLONE_FILES just avoid a needless copy, because we
         * just drop the VM and FILES in ptlrpc_daemonize() right away. */
        rc = cfs_kernel_thread(ldlm_pools_thread_main, ldlm_pools_thread,
                               CLONE_VM | CLONE_FILES);
        if (rc < 0) {
                CERROR("Can't start pool thread, error %d\n",
                       rc);
                OBD_FREE(ldlm_pools_thread, sizeof(*ldlm_pools_thread));
                ldlm_pools_thread = NULL;
                RETURN(rc);
        }
        l_wait_event(ldlm_pools_thread->t_ctl_waitq,
                     (ldlm_pools_thread->t_flags & SVC_RUNNING), &lwi);
        RETURN(0);
}

static void ldlm_pools_thread_stop(void)
{
        ENTRY;

        if (ldlm_pools_thread == NULL) {
                EXIT;
                return;
        }

        ldlm_pools_thread->t_flags = SVC_STOPPING;
        cfs_waitq_signal(&ldlm_pools_thread->t_ctl_waitq);

        /* Make sure that pools thread is finished before freeing @thread.
         * This fixes possible race and oops due to accessing freed memory
         * in pools thread. */
        wait_for_completion(&ldlm_pools_comp);
        OBD_FREE_PTR(ldlm_pools_thread);
        ldlm_pools_thread = NULL;
        EXIT;
}

int ldlm_pools_init(ldlm_side_t client)
{
        int rc;
        ENTRY;

        rc = ldlm_pools_thread_start(client);
        if (rc == 0)
                ldlm_pools_shrinker = set_shrinker(DEFAULT_SEEKS,
                                                   ldlm_pools_shrink);
        RETURN(rc);
}
EXPORT_SYMBOL(ldlm_pools_init);

void ldlm_pools_fini(void)
{
        if (ldlm_pools_shrinker != NULL) {
                remove_shrinker(ldlm_pools_shrinker);
                ldlm_pools_shrinker = NULL;
        }
        ldlm_pools_thread_stop();
}
EXPORT_SYMBOL(ldlm_pools_fini);

void ldlm_pools_wakeup(void)
{
        ENTRY;
        if (ldlm_pools_thread == NULL)
                return;
        ldlm_pools_thread->t_flags |= SVC_EVENT;
        cfs_waitq_signal(&ldlm_pools_thread->t_ctl_waitq);
        EXIT;
}
EXPORT_SYMBOL(ldlm_pools_wakeup);

/* Cancel @nr locks from all namespaces (if possible). Returns number of
 * cached locks after shrink is finished. All namespaces are asked to
 * cancel approximately equal amount of locks. */
int ldlm_pools_shrink(int nr, unsigned int gfp_mask)
{
        struct ldlm_namespace *ns;
        int total = 0, cached = 0;

        if (nr != 0 && !(gfp_mask & __GFP_FS))
                return -1;

        CDEBUG(D_DLMTRACE, "request to shrink %d locks from all pools\n",
               nr);
        mutex_down(&ldlm_namespace_lock);
        list_for_each_entry(ns, &ldlm_namespace_list, ns_list_chain)
                total += ldlm_pool_granted(&ns->ns_pool);

        if (nr == 0) {
                mutex_up(&ldlm_namespace_lock);
                return total;
        }

        /* Check all namespaces. */
        list_for_each_entry(ns, &ldlm_namespace_list, ns_list_chain) {
                struct ldlm_pool *pl = &ns->ns_pool;
                int cancel, nr_locks;

                nr_locks = ldlm_pool_granted(&ns->ns_pool);
                cancel = 1 + nr_locks * nr / total;
                cancel = ldlm_pool_shrink(pl, cancel, gfp_mask);
                cached += ldlm_pool_granted(&ns->ns_pool);
        }
        mutex_up(&ldlm_namespace_lock);
        return cached;
}
EXPORT_SYMBOL(ldlm_pools_shrink);
#endif /* __KERNEL__ */

#else /* !HAVE_LRU_RESIZE_SUPPORT */
int ldlm_pool_setup(struct ldlm_pool *pl, __u32 limit)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_setup);

int ldlm_pool_recalc(struct ldlm_pool *pl)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_recalc);

int ldlm_pool_shrink(struct ldlm_pool *pl,
                     int nr, unsigned int gfp_mask)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_shrink);

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
                   int idx, ldlm_side_t client)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_init);

void ldlm_pool_fini(struct ldlm_pool *pl)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_fini);

void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_add);

void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_del);

__u64 ldlm_pool_get_slv(struct ldlm_pool *pl)
{
        return 1;
}
EXPORT_SYMBOL(ldlm_pool_get_slv);

void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_set_slv);

__u32 ldlm_pool_get_limit(struct ldlm_pool *pl)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pool_get_limit);

void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit)
{
        return;
}
EXPORT_SYMBOL(ldlm_pool_set_limit);

int ldlm_pools_init(ldlm_side_t client)
{
        return 0;
}
EXPORT_SYMBOL(ldlm_pools_init);

void ldlm_pools_fini(void)
{
        return;
}
EXPORT_SYMBOL(ldlm_pools_fini);

void ldlm_pools_wakeup(void)
{
        return;
}
EXPORT_SYMBOL(ldlm_pools_wakeup);
#endif /* HAVE_LRU_RESIZE_SUPPORT */
