/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.sf.net/projects/lustre/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DEBUG_SUBSYSTEM S_LDLM
#ifdef __KERNEL__
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/version.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <asm/segment.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#else 
#include <liblustre.h>
#endif

#include <linux/lustre_dlm.h>
#include <linux/lustre_lib.h>

/* invariants:
 - only the owner of the lock changes l_owner/l_depth
 - if a non-owner changes or checks the variables a spin lock is taken
*/

void l_lock_init(struct lustre_lock *lock)
{
        sema_init(&lock->l_sem, 1);
        spin_lock_init(&lock->l_spin);
}

void l_lock(struct lustre_lock *lock)
{
        int owner = 0;

        spin_lock(&lock->l_spin);
        if (lock->l_owner == current)
                owner = 1;
        spin_unlock(&lock->l_spin);

        /* This is safe to increment outside the spinlock because we
         * can only have 1 CPU running on the current task
         * (i.e. l_owner == current), regardless of the number of CPUs.
         */
        if (owner) {
                ++lock->l_depth;
        } else {
                down(&lock->l_sem);
                spin_lock(&lock->l_spin);
                lock->l_owner = current;
                lock->l_depth = 0;
                spin_unlock(&lock->l_spin);
        }
}

void l_unlock(struct lustre_lock *lock)
{
        LASSERT(lock->l_owner == current);
        LASSERT(lock->l_depth >= 0);

        spin_lock(&lock->l_spin);
        if (--lock->l_depth < 0) {
                lock->l_owner = NULL;
                spin_unlock(&lock->l_spin);
                up(&lock->l_sem);
                return;
        }
        spin_unlock(&lock->l_spin);
}

int l_has_lock(struct lustre_lock *lock)
{
        int depth = -1, owner = 0;

        spin_lock(&lock->l_spin);
        if (lock->l_owner == current) {
                depth = lock->l_depth;
                owner = 1;
        }
        spin_unlock(&lock->l_spin);

        if (depth >= 0)
                CDEBUG(D_INFO, "lock_depth: %d\n", depth);
        return owner;
}

#ifdef __KERNEL__
#include <linux/lustre_version.h>
void l_check_no_ns_lock(struct ldlm_namespace *ns)
{
        static unsigned long next_msg;

        if (l_has_lock(&ns->ns_lock) && time_after(jiffies, next_msg)) {
                CERROR("namespace %s lock held illegally; tell phil\n",
                       ns->ns_name);
#if (LUSTRE_KERNEL_VERSION >= 30)
                CERROR(portals_debug_dumpstack());
#endif
                next_msg = jiffies + 60 * HZ;
        }
}

#else
void l_check_no_ns_lock(struct ldlm_namespace *ns)
{
        if (l_has_lock(&ns->ns_lock)) {
                CERROR("namespace %s lock held illegally; tell phil\n",
                       ns->ns_name);
}
#endif /* __KERNEL__ */
