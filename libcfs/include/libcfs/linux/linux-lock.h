/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
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
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * libcfs/include/libcfs/linux/linux-lock.h
 *
 * Basic library routines.
 */

#ifndef __LIBCFS_LINUX_CFS_LOCK_H__
#define __LIBCFS_LINUX_CFS_LOCK_H__

#ifndef __LIBCFS_LIBCFS_H__
#error Do not #include this file directly. #include <libcfs/libcfs.h> instead
#endif

#ifndef __KERNEL__
#error This include is only for kernel use.
#endif

#include <linux/smp_lock.h>
#include <linux/mutex.h>

/*
 * IMPORTANT !!!!!!!!
 *
 * All locks' declaration are not guaranteed to be initialized,
 * Althought some of they are initialized in Linux. All locks
 * declared by CFS_DECL_* should be initialized explicitly.
 */


/*
 * spin_lock (use Linux kernel's primitives)
 *
 * - spin_lock_init(x)
 * - spin_lock(x)
 * - spin_unlock(x)
 * - spin_trylock(x)
 *
 * - spin_lock_irqsave(x, f)
 * - spin_unlock_irqrestore(x, f)
 */

/*
 * rw_semaphore (use Linux kernel's primitives)
 *
 * - init_rwsem(x)
 * - down_read(x)
 * - up_read(x)
 * - down_write(x)
 * - up_write(x)
 */
#define fini_rwsem(s) do {} while(0)

/*
 * rwlock_t (use Linux kernel's primitives)
 *
 * - rwlock_init(x)
 * - read_lock(x)
 * - read_unlock(x)
 * - write_lock(x)
 * - write_unlock(x)
 */

/*
 * mutex:
 *
 * - init_mutex(x)
 * - init_mutex_locked(x)
 * - mutex_up(x)
 * - mutex_down(x)
 */
#define init_mutex(x)                   init_MUTEX(x)
#define init_mutex_locked(x)            init_MUTEX_LOCKED(x)
#define mutex_up(x)                     up(x)
#define mutex_down(x)                   down(x)
#define mutex_down_trylock(x)           down_trylock(x)

/*
 * completion (use Linux kernel's primitives)
 *
 * - init_complition(c)
 * - complete(c)
 * - wait_for_completion(c)
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)

/**************************************************************************
 *
 * Mutex interface from newer Linux kernels.
 *
 * this augments compatibility interface from include/linux/mutex.h
 *
 **************************************************************************/

struct mutex;

static inline void mutex_destroy(struct mutex *lock)
{
}

/*
 * This is for use in assertions _only_, i.e., this function should always
 * return 1.
 *
 * \retval 1 mutex is locked.
 *
 * \retval 0 mutex is not locked. This should never happen.
 */
static inline int mutex_is_locked(struct mutex *lock)
{
        return 1;
}
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16) */

#ifndef lockdep_set_class

/**************************************************************************
 *
 * Lockdep "implementation". Also see liblustre.h
 *
 **************************************************************************/

struct lock_class_key {
        ;
};

# define lockdep_set_class(lock, key) \
        do { (void)sizeof (lock);(void)sizeof (key); } while (0)
/* This has to be a macro, so that `subclass' can be undefined in kernels that
 * do not support lockdep. */


static inline void lockdep_off(void)
{
}

static inline void lockdep_on(void)
{
}

#endif /* lockdep_set_class */

#ifndef CONFIG_DEBUG_LOCK_ALLOC
#ifndef mutex_lock_nested
# define mutex_lock_nested(mutex, subclass) mutex_lock(mutex)
#endif

#ifndef spin_lock_nested
# define spin_lock_nested(lock, subclass) spin_lock(lock)
#endif

#ifndef down_read_nested
# define down_read_nested(lock, subclass) down_read(lock)
#endif

#ifndef down_write_nested
# define down_write_nested(lock, subclass) down_write(lock)
#endif
#endif /* CONFIG_DEBUG_LOCK_ALLOC */


/*
 * spinlock "implementation"
 */

typedef spinlock_t cfs_spinlock_t;

#define cfs_spin_lock_init(lock) spin_lock_init(lock)
#define cfs_spin_lock(lock)      spin_lock(lock)
#define cfs_spin_lock_bh(lock)   spin_lock_bh(lock)
#define cfs_spin_unlock(lock)    spin_unlock(lock)
#define cfs_spin_unlock_bh(lock) spin_unlock_bh(lock)

/*
 * rwlock "implementation"
 */

typedef rwlock_t cfs_rwlock_t;

#define cfs_rwlock_init(lock)      rwlock_init(lock)
#define cfs_read_lock(lock)        read_lock(lock)
#define cfs_read_unlock(lock)      read_unlock(lock)
#define cfs_write_lock_bh(lock)    write_lock_bh(lock)
#define cfs_write_unlock_bh(lock)  write_unlock_bh(lock)

#endif /* __LIBCFS_LINUX_CFS_LOCK_H__ */
