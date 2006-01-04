/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002 Cluster File Systems, Inc.
 * Author: Phil Schwan <phil@clusterfs.com>
 *
 * This file is part of Lustre, http://www.lustre.org.
 *
 * Lustre is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * Lustre is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Lustre; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Darwin porting library
 * Make things easy to port
 */
#define DEBUG_SUBSYSTEM S_LNET

#include <mach/mach_types.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/file.h>
#include <sys/conf.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/filedesc.h>
#include <sys/namei.h>
#include <miscfs/devfs/devfs.h>
#include <kern/kalloc.h>
#include <kern/zalloc.h>
#include <kern/thread.h>

#include <libcfs/libcfs.h>
#include <libcfs/kp30.h>

void    *darwin_current_journal_info = NULL;
int     darwin_current_cap_effective = -1;

/*
 * cfs pseudo device, actually pseudo char device in darwin
 */
#define KPORTAL_MAJOR  -1

kern_return_t  cfs_psdev_register(cfs_psdev_t *dev) {
	dev->index = cdevsw_add(KPORTAL_MAJOR, dev->devsw);
	if (dev->index < 0) {
		printf("libcfs_init: failed to allocate a major number!\n");
		return KERN_FAILURE;
	}
	dev->handle = devfs_make_node(makedev (dev->index, 0),
                                      DEVFS_CHAR, UID_ROOT,
                                      GID_WHEEL, 0666, (char *)dev->name, 0);
	return KERN_SUCCESS;
}

kern_return_t  cfs_psdev_deregister(cfs_psdev_t *dev) {
	devfs_remove(dev->handle);
	cdevsw_remove(dev->index, dev->devsw);
	return KERN_SUCCESS;
}

/*
 * KPortal symbol register / unregister support
 */
static struct rw_semaphore cfs_symbol_lock;
struct list_head           cfs_symbol_list;

void *
cfs_symbol_get(const char *name)
{
        struct list_head    *walker;
        struct cfs_symbol   *sym = NULL;

        down_read(&cfs_symbol_lock);
        list_for_each(walker, &cfs_symbol_list) {
                sym = list_entry (walker, struct cfs_symbol, sym_list);
                if (!strcmp(sym->name, name)) {
                        sym->ref ++;
                        break;
                }
        }
        up_read(&cfs_symbol_lock);
        if (sym != NULL)
                return sym->value;
        return NULL;
}

kern_return_t
cfs_symbol_put(const char *name)
{
        struct list_head    *walker;
        struct cfs_symbol   *sym = NULL;

        down_read(&cfs_symbol_lock);
        list_for_each(walker, &cfs_symbol_list) {
                sym = list_entry (walker, struct cfs_symbol, sym_list);
                if (!strcmp(sym->name, name)) {
                        sym->ref --;
                        LASSERT(sym->ref >= 0);
                        break;
                }
        }
        up_read(&cfs_symbol_lock);
        LASSERT(sym != NULL);

        return 0;
}

kern_return_t
cfs_symbol_register(const char *name, const void *value)
{
        struct list_head    *walker;
        struct cfs_symbol   *sym = NULL;
        struct cfs_symbol   *new = NULL;

        MALLOC(new, struct cfs_symbol *, sizeof(struct cfs_symbol), M_TEMP, M_WAITOK|M_ZERO);
        strncpy(new->name, name, CFS_SYMBOL_LEN);
        new->value = (void *)value;
        new->ref = 0;
        CFS_INIT_LIST_HEAD(&new->sym_list);

        down_write(&cfs_symbol_lock);
        list_for_each(walker, &cfs_symbol_list) {
                sym = list_entry (walker, struct cfs_symbol, sym_list);
                if (!strcmp(sym->name, name)) {
                        up_write(&cfs_symbol_lock);
                        FREE(new, M_TEMP);
                        return KERN_NAME_EXISTS;
                }

        }
        list_add_tail(&new->sym_list, &cfs_symbol_list);
        up_write(&cfs_symbol_lock);

        return KERN_SUCCESS;
}

kern_return_t
cfs_symbol_unregister(const char *name)
{
        struct list_head    *walker;
        struct list_head    *nxt;
        struct cfs_symbol   *sym = NULL;

        down_write(&cfs_symbol_lock);
        list_for_each_safe(walker, nxt, &cfs_symbol_list) {
                sym = list_entry (walker, struct cfs_symbol, sym_list);
                if (!strcmp(sym->name, name)) {
                        LASSERT(sym->ref == 0);
                        list_del (&sym->sym_list);
                        FREE(sym, M_TEMP);
                        break;
                }
        }
        up_write(&cfs_symbol_lock);

        return KERN_SUCCESS;
}

void
cfs_symbol_clean()
{
        struct list_head    *walker;
        struct cfs_symbol   *sym = NULL;

        down_write(&cfs_symbol_lock);
        list_for_each(walker, &cfs_symbol_list) {
                sym = list_entry (walker, struct cfs_symbol, sym_list);
                LASSERT(sym->ref == 0);
                list_del (&sym->sym_list);
                FREE(sym, M_TEMP);
        }
        up_write(&cfs_symbol_lock);
        return;
}

/*
 * Register sysctl table
 */
cfs_sysctl_table_header_t *
cfs_register_sysctl_table (cfs_sysctl_table_t *table, int arg)
{
	cfs_sysctl_table_t	item;
	int i = 0;

	while ((item = table[i++]) != NULL) {
		sysctl_register_oid(item);
	}
	return table;
}

/*
 * Unregister sysctl table
 */
void
cfs_unregister_sysctl_table (cfs_sysctl_table_header_t *table) {
	int i = 0;
	cfs_sysctl_table_t	item;

	while ((item = table[i++]) != NULL) {
		sysctl_unregister_oid(item);
	}
	return;
}

struct kernel_thread_arg cfs_thread_arg;

void
cfs_thread_agent_init()
{
        set_targ_stat(&cfs_thread_arg, THREAD_ARG_FREE);
        spin_lock_init(&cfs_thread_arg.lock);
        cfs_thread_arg.arg = NULL;
        cfs_thread_arg.func = NULL;
}

void
cfs_thread_agent (void)
{
        cfs_thread_t           func = NULL;
        void                   *arg = NULL;

        thread_arg_recv(&cfs_thread_arg, func, arg);
        /* printf("entry of thread agent (func: %08lx).\n", (void *)func); */
        assert(func != NULL);
        func(arg);
        /* printf("thread agent exit. (func: %08lx)\n", (void *)func); */
        (void) thread_terminate(current_act());
}

int
cfs_kernel_thread(cfs_thread_t  func, void *arg, int flag)
{
        int ret = 0;
        thread_t th = NULL;

        thread_arg_hold(&cfs_thread_arg, func, arg);
        th = kernel_thread(kernel_task, cfs_thread_agent);
        thread_arg_release(&cfs_thread_arg);
        if (th == THREAD_NULL)
                ret = -1;
        return ret;
}

void cfs_daemonize(char *str)
{
        snprintf(cfs_curproc_comm(), CFS_CURPROC_COMM_MAX, "%s", str);
        return;
}

extern int block_procsigmask(struct proc *p,  int bit);

cfs_sigset_t cfs_get_blocked_sigs()
{
        return cfs_current()->uu_sigmask;
}

void cfs_block_allsigs()
{
        block_procsigmask(current_proc(), -1);
}

void cfs_block_sigs(sigset_t bit)
{
        block_procsigmask(current_proc(), bit);
}

void lustre_cone_in(boolean_t *state, funnel_t **cone)
{
        *cone = thread_funnel_get();
        if (*cone == network_flock)
                thread_funnel_switch(NETWORK_FUNNEL, KERNEL_FUNNEL);
        else if (*cone == NULL)
                *state = thread_funnel_set(kernel_flock, TRUE);
}

void lustre_cone_ex(boolean_t state, funnel_t *cone)
{
        if (cone == network_flock)
                thread_funnel_switch(KERNEL_FUNNEL, NETWORK_FUNNEL);
        else if (cone == NULL)
                (void) thread_funnel_set(kernel_flock, state);
}

void lustre_net_in(boolean_t *state, funnel_t **cone)
{
        *cone = thread_funnel_get();
        if (*cone == kernel_flock)
                thread_funnel_switch(KERNEL_FUNNEL, NETWORK_FUNNEL);
        else if (*cone == NULL)
                *state = thread_funnel_set(network_flock, TRUE);
}

void lustre_net_ex(boolean_t state, funnel_t *cone)
{
        if (cone == kernel_flock)
                thread_funnel_switch(NETWORK_FUNNEL, KERNEL_FUNNEL);
        else if (cone == NULL)
                (void) thread_funnel_set(network_flock, state);
}


void cfs_waitq_init(struct cfs_waitq *waitq)
{
	ksleep_chan_init(&waitq->wq_ksleep_chan);
}

void cfs_waitlink_init(struct cfs_waitlink *link)
{
	ksleep_link_init(&link->wl_ksleep_link);
}

void cfs_waitq_add(struct cfs_waitq *waitq, struct cfs_waitlink *link)
{
        link->wl_waitq = waitq;
	ksleep_add(&waitq->wq_ksleep_chan, &link->wl_ksleep_link);
}

void cfs_waitq_add_exclusive(struct cfs_waitq *waitq,
                             struct cfs_waitlink *link)
{
        link->wl_waitq = waitq;
	link->wl_ksleep_link.flags |= KSLEEP_EXCLUSIVE;
	ksleep_add(&waitq->wq_ksleep_chan, &link->wl_ksleep_link);
}

void cfs_waitq_forward(struct cfs_waitlink *link,
                       struct cfs_waitq *waitq)
{
	link->wl_ksleep_link.forward = &waitq->wq_ksleep_chan;
}

void cfs_waitq_del(struct cfs_waitq *waitq,
                   struct cfs_waitlink *link)
{
	ksleep_del(&waitq->wq_ksleep_chan, &link->wl_ksleep_link);
}

int cfs_waitq_active(struct cfs_waitq *waitq)
{
	return (1);
}

void cfs_waitq_signal(struct cfs_waitq *waitq)
{
	/*
	 * XXX nikita: do NOT call libcfs_debug_msg() (CDEBUG/ENTRY/EXIT)
	 * from here: this will lead to infinite recursion.
	 */
	ksleep_wake(&waitq->wq_ksleep_chan);
}

void cfs_waitq_signal_nr(struct cfs_waitq *waitq, int nr)
{
	ksleep_wake_nr(&waitq->wq_ksleep_chan, nr);
}

void cfs_waitq_broadcast(struct cfs_waitq *waitq)
{
	ksleep_wake_all(&waitq->wq_ksleep_chan);
}

void cfs_waitq_wait(struct cfs_waitlink *link, cfs_task_state_t state)
{
        ksleep_wait(&link->wl_waitq->wq_ksleep_chan, state);
}

cfs_duration_t  cfs_waitq_timedwait(struct cfs_waitlink *link,
                                    cfs_task_state_t state,
                                    cfs_duration_t timeout)
{
        CDEBUG(D_TRACE, "timeout: %llu\n", (long long unsigned)timeout);
        return ksleep_timedwait(&link->wl_waitq->wq_ksleep_chan, 
                                state, timeout);
}

typedef  void (*ktimer_func_t)(void *);
void cfs_timer_init(cfs_timer_t *t, void (* func)(unsigned long), void *arg)
{
        ktimer_init(&t->t, (ktimer_func_t)func, arg);
}

void cfs_timer_done(struct cfs_timer *t)
{
        ktimer_done(&t->t);
}

void cfs_timer_arm(struct cfs_timer *t, cfs_time_t deadline)
{
        ktimer_arm(&t->t, deadline);
}

void cfs_timer_disarm(struct cfs_timer *t)
{
        ktimer_disarm(&t->t);
}

int  cfs_timer_is_armed(struct cfs_timer *t)
{
        return ktimer_is_armed(&t->t);
}

cfs_time_t cfs_timer_deadline(struct cfs_timer *t)
{
        return ktimer_deadline(&t->t);
}

void cfs_enter_debugger(void)
{
        extern void PE_enter_debugger(char *cause);
        PE_enter_debugger("CFS");
}

int cfs_online_cpus()
{
        host_basic_info_data_t hinfo;
        kern_return_t kret;
        int count = HOST_BASIC_INFO_COUNT;
#define BSD_HOST 1
        kret = host_info(BSD_HOST, HOST_BASIC_INFO, &hinfo, &count);
        if (kret == KERN_SUCCESS) 
                return (hinfo.avail_cpus);
        return(-EINVAL);
}

extern spinlock_t trace_cpu_serializer;
extern struct list_head page_death_row;
extern spinlock_t page_death_row_phylax;

void raw_page_death_row_clean(void);

int libcfs_arch_init(void)
{
	init_rwsem(&cfs_symbol_lock);
        CFS_INIT_LIST_HEAD(&cfs_symbol_list);
        cfs_thread_agent_init();
        spin_lock_init(&trace_cpu_serializer);
        CFS_INIT_LIST_HEAD(&page_death_row);
        spin_lock_init(&page_death_row_phylax);
	return 0;
}

void libcfs_arch_cleanup(void)
{
	cfs_symbol_clean();
        raw_page_death_row_clean();
}

