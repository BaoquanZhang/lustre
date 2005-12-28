#define DEBUG_PORTAL_ALLOC
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifndef __WINNT_SOCKNAL_LIB_H__
#define __WINNT_SOCKNAL_LIB_H__

#include <libcfs/libcfs.h>
#include <libcfs/kp30.h>

#define SOCKNAL_ARCH_EAGER_ACK	0

#ifndef CONFIG_SMP

static inline
int ksocknal_nsched(void)
{
        return 1;
}

#else

static inline int
ksocknal_nsched(void)
{
        return num_online_cpus();
}

static inline int
ksocknal_sched2cpu(int i)
{
        return i;
}

static inline int
ksocknal_irqsched2cpu(int i)
{
        return i;
}

#endif

#endif
