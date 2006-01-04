#ifndef __LIBCFS_DARWIN_CFS_LOCK_H__
#define __LIBCFS_DARWIN_CFS_LOCK_H__

#ifndef __LIBCFS_LIBCFS_H__
#error Do not #include this file directly. #include <libcfs/libcfs.h> instead
#endif

#ifdef	__KERNEL__
#include <mach/sync_policy.h>
#include <mach/task.h>
#include <mach/semaphore.h>
#include <mach/mach_traps.h>

/* spin lock types and operations */
#include <kern/simple_lock.h>
#include <kern/assert.h>
#include <kern/thread.h>

#include <libcfs/darwin/darwin-types.h>
#include <libcfs/darwin/darwin-sync.h>

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
struct spin_lock {
	struct kspin spin;
};

typedef struct spin_lock spinlock_t;

static inline void spin_lock_init(spinlock_t *lock)
{
	kspin_init(&lock->spin);
}

static inline void spin_lock(spinlock_t *lock)
{
	kspin_lock(&lock->spin);
}

static inline void spin_unlock(spinlock_t *lock)
{
	kspin_unlock(&lock->spin);
}

static inline int spin_trylock(spinlock_t *lock)
{
	return kspin_trylock(&lock->spin);
}

#define spin_lock_bh(x)		spin_lock(x)
#define spin_unlock_bh(x)	spin_unlock(x)
#define spin_lock_bh_init(x)	spin_lock_init(x)

extern boolean_t ml_set_interrupts_enabled(boolean_t enable);
#define __disable_irq()         (spl_t) ml_set_interrupts_enabled(FALSE)
#define __enable_irq(x)         (void) ml_set_interrupts_enabled(x)

#define spin_lock_irqsave(s, f)		do{			\
					f = __disable_irq();	\
					spin_lock(s);	}while(0)

#define spin_unlock_irqrestore(s, f)	do{			\
					spin_unlock(s);		\
					__enable_irq(f);}while(0)

/* 
 * Semaphore
 *
 * - sema_init(x, v)
 * - __down(x)
 * - __up(x)
 */
struct semaphore {
	struct ksem sem;
};

static inline void sema_init(struct semaphore *s, int val)
{
	ksem_init(&s->sem, val);
}

static inline void __down(struct semaphore *s)
{
	ksem_down(&s->sem, 1);
}

static inline void __up(struct semaphore *s)
{
	ksem_up(&s->sem, 1);
}

/*
 * Mutex:
 *
 * - init_mutex(x)
 * - init_mutex_locked(x)
 * - mutex_up(x)
 * - mutex_down(x)
 */

#define mutex_up(s)			__up(s)
#define mutex_down(s)			__down(s)

#define init_mutex(x)			sema_init(x, 1)
#define init_mutex_locked(x)		sema_init(x, 0)

/*
 * Completion:
 *
 * - init_completion(c)
 * - complete(c)
 * - wait_for_completion(c)
 */
struct completion {
	/*
	 * Emulate completion by semaphore for now.
	 *
	 * XXX nikita: this is not safe if completion is used to synchronize
	 * exit from kernel daemon thread and kext unloading. In this case
	 * some core function (a la complete_and_exit()) is needed.
	 */
	struct ksem sem;
};

static inline void init_completion(struct completion *c)
{
	ksem_init(&c->sem, 0);
}

static inline void complete(struct completion *c)
{
	ksem_up(&c->sem, 1);
}

static inline void wait_for_completion(struct completion *c)
{
	ksem_down(&c->sem, 1);
}

/*
 * rw_semaphore:
 *
 * - DECLARE_RWSEM(x)
 * - init_rwsem(x)
 * - down_read(x)
 * - up_read(x)
 * - down_write(x)
 * - up_write(x)
 */
struct rw_semaphore {
	struct krw_sem s;
};

static inline void init_rwsem(struct rw_semaphore *s)
{
	krw_sem_init(&s->s);
}

static inline void down_read(struct rw_semaphore *s)
{
	krw_sem_down_r(&s->s);
}

static inline int down_read_trylock(struct rw_semaphore *s)
{
	int ret = krw_sem_down_r_try(&s->s);
	return ret == 0;
}

static inline void down_write(struct rw_semaphore *s)
{
	krw_sem_down_w(&s->s);
}

static inline int down_write_trylock(struct rw_semaphore *s)
{
	int ret = krw_sem_down_w_try(&s->s);
	return ret == 0;
}

static inline void up_read(struct rw_semaphore *s)
{
	krw_sem_up_r(&s->s);
}

static inline void up_write(struct rw_semaphore *s)
{
	krw_sem_up_w(&s->s);
}

/* 
 * read-write lock : Need to be investigated more!!
 *
 * - DECLARE_RWLOCK(l)
 * - rwlock_init(x)
 * - read_lock(x)
 * - read_unlock(x)
 * - write_lock(x)
 * - write_unlock(x)
 */
typedef struct krw_spin rwlock_t;

#define rwlock_init(pl)               krw_spin_init(pl)

#define read_lock(l)          krw_spin_down_r(l)
#define read_unlock(l)                krw_spin_up_r(l)
#define write_lock(l)         krw_spin_down_w(l)
#define write_unlock(l)               krw_spin_up_w(l)

#define write_lock_irqsave(l, f)	do{			\
					f = __disable_irq();	\
					write_lock(l);	}while(0)

#define write_unlock_irqrestore(l, f)	do{			\
					write_unlock(l);	\
					__enable_irq(f);}while(0)

#define read_lock_irqsave(l, f)		do{			\
					f = __disable_irq();	\
					read_lock(l);	}while(0)

#define read_unlock_irqrestore(l, f)	do{			\
					read_unlock(l);		\
					__enable_irq(f);}while(0)

/*
 * Funnel: 
 *
 * Safe funnel in/out
 */

#define CFS_DECL_FUNNEL_DATA			\
        boolean_t    __funnel_state = FALSE;	\
        funnel_t    *__funnel
#define CFS_DECL_CONE_DATA		CFS_DECL_FUNNEL_DATA
#define CFS_DECL_NET_DATA		CFS_DECL_FUNNEL_DATA

void lustre_cone_in(boolean_t *state, funnel_t **cone);
void lustre_cone_ex(boolean_t state, funnel_t *cone);

#define CFS_CONE_IN lustre_cone_in(&__funnel_state, &__funnel)
#define CFS_CONE_EX lustre_cone_ex(__funnel_state, __funnel)

void lustre_net_in(boolean_t *state, funnel_t **cone);
void lustre_net_ex(boolean_t state, funnel_t *cone);

#define CFS_NET_IN  lustre_net_in(&__funnel_state, &__funnel)
#define CFS_NET_EX  lustre_net_ex(__funnel_state, __funnel)

#else
#include <libcfs/user-lock.h>
#endif /* __KERNEL__ */

/* __XNU_CFS_LOCK_H */
#endif
