/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
#ifndef _KP30_INCLUDED
#define _KP30_INCLUDED

#include <linux/libcfs.h>
#define PORTAL_DEBUG

#ifndef offsetof
# define offsetof(typ,memb)     ((unsigned long)((char *)&(((typ *)0)->memb)))
#endif

#define LOWEST_BIT_SET(x)       ((x) & ~((x) - 1))

#ifdef __KERNEL__
# include <linux/vmalloc.h>
# include <linux/time.h>
# include <linux/slab.h>
# include <linux/interrupt.h>
# include <linux/highmem.h>
# include <linux/module.h>
# include <linux/version.h>
# include <portals/p30.h>
# include <linux/smp_lock.h>
# include <asm/atomic.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
#define schedule_work schedule_task
#define prepare_work(wq,cb,cbdata)                                            \
do {                                                                          \
        INIT_TQUEUE((wq), 0, 0);                                              \
        PREPARE_TQUEUE((wq), (cb), (cbdata));                                 \
} while (0)

#define PageUptodate Page_Uptodate
#define our_recalc_sigpending(current) recalc_sigpending(current)
#define num_online_cpus() smp_num_cpus
static inline void our_cond_resched(void)
{
        if (current->need_resched)
               schedule ();
}
#define work_struct_t       struct tq_struct

#else

#define prepare_work(wq,cb,cbdata)                                            \
do {                                                                          \
        INIT_WORK((wq), (void *)(cb), (void *)(cbdata));                      \
} while (0)
#define wait_on_page wait_on_page_locked
#define our_recalc_sigpending(current) recalc_sigpending()
#define strtok(a,b) strpbrk(a, b)
static inline void our_cond_resched(void)
{
        cond_resched();
}
#define work_struct_t      struct work_struct

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0) */

#ifdef PORTAL_DEBUG
extern void kportal_assertion_failed(char *expr, char *file, const char *func,
                                     const int line);
#define LASSERT(e) ((e) ? 0 : kportal_assertion_failed( #e , __FILE__,  \
                                                        __FUNCTION__, __LINE__))
/* it would be great to dump_stack() here, but some kernels
 * export it as show_stack() and I can't be bothered to
 * proprely engage in that dance right now */
#define LASSERTF(cond, fmt...)                                                \
        do {                                                                  \
                if (unlikely(!(cond))) {                                      \
                        portals_debug_msg(0, D_EMERG,  __FILE__, __FUNCTION__,\
                                          __LINE__,  CDEBUG_STACK,            \
                                          "ASSERTION(" #cond ") failed:" fmt);\
                        LBUG();                                               \
                }                                                             \
        } while (0)

#else
#define LASSERT(e)
#define LASSERTF(cond, fmt...) do { } while (0)
#endif

#ifdef CONFIG_SMP
#define LASSERT_SPIN_LOCKED(lock) LASSERT(spin_is_locked(lock))
#else
#define LASSERT_SPIN_LOCKED(lock) do {} while(0)
#endif

#ifdef __arch_um__
#define LBUG_WITH_LOC(file, func, line)                                 \
do {                                                                    \
        CEMERG("LBUG - trying to dump log to /tmp/lustre-log\n");       \
        portals_debug_dumplog();                                        \
        portals_run_lbug_upcall(file, func, line);                      \
        panic("LBUG");                                                  \
} while (0)
#else
#define LBUG_WITH_LOC(file, func, line)                                 \
do {                                                                    \
        CEMERG("LBUG\n");                                               \
        portals_debug_dumplog();                                        \
        portals_run_lbug_upcall(file, func, line);                      \
        set_task_state(current, TASK_UNINTERRUPTIBLE);                  \
        schedule();                                                     \
} while (0)
#endif /* __arch_um__ */

#define LBUG() LBUG_WITH_LOC(__FILE__, __FUNCTION__, __LINE__)

/*
 * Memory
 */
#ifdef PORTAL_DEBUG
extern atomic_t portal_kmemory;

# define portal_kmem_inc(ptr, size)                                           \
do {                                                                          \
        atomic_add(size, &portal_kmemory);                                    \
} while (0)

# define portal_kmem_dec(ptr, size) do {                                      \
        atomic_sub(size, &portal_kmemory);                                    \
} while (0)

#else
# define portal_kmem_inc(ptr, size) do {} while (0)
# define portal_kmem_dec(ptr, size) do {} while (0)
#endif /* PORTAL_DEBUG */

#define PORTAL_VMALLOC_SIZE        16384

#define PORTAL_ALLOC_GFP(ptr, size, mask)                                 \
do {                                                                      \
        LASSERT(!in_interrupt() ||                                        \
               (size <= PORTAL_VMALLOC_SIZE && mask == GFP_ATOMIC));      \
        if ((size) > PORTAL_VMALLOC_SIZE)                                 \
                (ptr) = vmalloc(size);                                    \
        else                                                              \
                (ptr) = kmalloc((size), (mask));                          \
        if ((ptr) == NULL) {                                              \
                CERROR("PORTALS: out of memory at %s:%d (tried to alloc '"\
                       #ptr "' = %d)\n", __FILE__, __LINE__, (int)(size));\
                CERROR("PORTALS: %d total bytes allocated by portals\n",  \
                       atomic_read(&portal_kmemory));                     \
        } else {                                                          \
                portal_kmem_inc((ptr), (size));                           \
                memset((ptr), 0, (size));                                 \
        }                                                                 \
        CDEBUG(D_MALLOC, "kmalloced '" #ptr "': %d at %p (tot %d).\n",    \
               (int)(size), (ptr), atomic_read (&portal_kmemory));        \
} while (0)

#define PORTAL_ALLOC(ptr, size) \
        PORTAL_ALLOC_GFP(ptr, size, GFP_NOFS)

#define PORTAL_ALLOC_ATOMIC(ptr, size) \
        PORTAL_ALLOC_GFP(ptr, size, GFP_ATOMIC)

#define PORTAL_FREE(ptr, size)                                          \
do {                                                                    \
        int s = (size);                                                 \
        if ((ptr) == NULL) {                                            \
                CERROR("PORTALS: free NULL '" #ptr "' (%d bytes) at "   \
                       "%s:%d\n", s, __FILE__, __LINE__);               \
                break;                                                  \
        }                                                               \
        if (s > PORTAL_VMALLOC_SIZE)                                    \
                vfree(ptr);                                             \
        else                                                            \
                kfree(ptr);                                             \
        portal_kmem_dec((ptr), s);                                      \
        CDEBUG(D_MALLOC, "kfreed '" #ptr "': %d at %p (tot %d).\n",     \
               s, (ptr), atomic_read(&portal_kmemory));                 \
} while (0)

/* ------------------------------------------------------------------- */

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))

#define PORTAL_SYMBOL_REGISTER(x) inter_module_register(#x, THIS_MODULE, &x)
#define PORTAL_SYMBOL_UNREGISTER(x) inter_module_unregister(#x)

#define PORTAL_SYMBOL_GET(x) ((typeof(&x))inter_module_get(#x))
#define PORTAL_SYMBOL_PUT(x) inter_module_put(#x)

#define PORTAL_MODULE_USE       MOD_INC_USE_COUNT
#define PORTAL_MODULE_UNUSE     MOD_DEC_USE_COUNT
#else

#define PORTAL_SYMBOL_REGISTER(x)
#define PORTAL_SYMBOL_UNREGISTER(x)

#define PORTAL_SYMBOL_GET(x) symbol_get(x)
#define PORTAL_SYMBOL_PUT(x) symbol_put(x)

#define PORTAL_MODULE_USE       try_module_get(THIS_MODULE)
#define PORTAL_MODULE_UNUSE     module_put(THIS_MODULE)

#endif

/******************************************************************************/

#ifdef PORTALS_PROFILING
#define prof_enum(FOO) PROF__##FOO
enum {
        prof_enum(our_recvmsg),
        prof_enum(our_sendmsg),
        prof_enum(socknal_recv),
        prof_enum(lib_parse),
        prof_enum(conn_list_walk),
        prof_enum(memcpy),
        prof_enum(lib_finalize),
        prof_enum(pingcli_time),
        prof_enum(gmnal_send),
        prof_enum(gmnal_recv),
        MAX_PROFS
};

struct prof_ent {
        char *str;
        /* hrmph.  wrap-tastic. */
        u32       starts;
        u32       finishes;
        cycles_t  total_cycles;
        cycles_t  start;
        cycles_t  end;
};

extern struct prof_ent prof_ents[MAX_PROFS];

#define PROF_START(FOO)                                         \
        do {                                                    \
                struct prof_ent *pe = &prof_ents[PROF__##FOO];  \
                pe->starts++;                                   \
                pe->start = get_cycles();                       \
        } while (0)

#define PROF_FINISH(FOO)                                        \
        do {                                                    \
                struct prof_ent *pe = &prof_ents[PROF__##FOO];  \
                pe->finishes++;                                 \
                pe->end = get_cycles();                         \
                pe->total_cycles += (pe->end - pe->start);      \
        } while (0)
#else /* !PORTALS_PROFILING */
#define PROF_START(FOO) do {} while(0)
#define PROF_FINISH(FOO) do {} while(0)
#endif /* PORTALS_PROFILING */

/* debug.c */
extern spinlock_t stack_backtrace_lock;

char *portals_debug_dumpstack(void);
void portals_run_upcall(char **argv);
void portals_run_lbug_upcall(char * file, const char *fn, const int line);
void portals_debug_dumplog(void);
int portals_debug_init(unsigned long bufsize);
int portals_debug_cleanup(void);
int portals_debug_clear_buffer(void);
int portals_debug_mark_buffer(char *text);
int portals_debug_set_daemon(unsigned int cmd, unsigned int length,
                             char *file, unsigned int size);
__s32 portals_debug_copy_to_user(char *buf, unsigned long len);
#if (__GNUC__)
/* Use the special GNU C __attribute__ hack to have the compiler check the
 * printf style argument string against the actual argument count and
 * types.
 */
#ifdef printf
# warning printf has been defined as a macro...
# undef printf
#endif
void portals_debug_msg(int subsys, int mask, char *file, const char *fn,
                       const int line, unsigned long stack,
                       char *format, ...)
        __attribute__ ((format (printf, 7, 8)));
#else
void portals_debug_msg(int subsys, int mask, char *file, const char *fn,
                       const int line, unsigned long stack,
                       const char *format, ...);
#endif /* __GNUC__ */
void portals_debug_set_level(unsigned int debug_level);

# define fprintf(a, format, b...) CDEBUG(D_OTHER, format , ## b)
# define printf(format, b...) CDEBUG(D_OTHER, format , ## b)
# define time(a) CURRENT_TIME

extern void kportal_daemonize (char *name);
extern void kportal_blockallsigs (void);

#else  /* !__KERNEL__ */
# include <stdio.h>
# include <stdlib.h>
#ifndef __CYGWIN__
# include <stdint.h>
#else
# include <cygwin-ioctl.h>
#endif
# include <unistd.h>
# include <time.h>
# include <limits.h>
# include <asm/types.h>
# ifndef DEBUG_SUBSYSTEM
#  define DEBUG_SUBSYSTEM S_UNDEFINED
# endif
# ifdef PORTAL_DEBUG
#  undef NDEBUG
#  include <assert.h>
#  define LASSERT(e)     assert(e)
#  define LASSERTF(cond, args...)     assert(cond)
# else
#  define LASSERT(e)
#  define LASSERTF(cond, args...) do { } while (0)
# endif
# define printk(format, args...) printf (format, ## args)
# define PORTAL_ALLOC(ptr, size) do { (ptr) = malloc(size); } while (0);
# define PORTAL_FREE(a, b) do { free(a); } while (0);
void portals_debug_dumplog(void);
# define portals_debug_msg(subsys, mask, file, fn, line, stack, format, a...) \
    printf("%02x:%06x (@%lu %s:%s,l. %d %d %lu): " format,                    \
           (subsys), (mask), (long)time(0), file, fn, line,                   \
           getpid() , stack, ## a);
#endif

/* support decl needed both by kernel and liblustre */
char *portals_nid2str(int nal, ptl_nid_t nid, char *str);

#ifndef CURRENT_TIME
# define CURRENT_TIME time(0)
#endif

/******************************************************************************/
/* Light-weight trace
 * Support for temporary event tracing with minimal Heisenberg effect. */
#define LWT_SUPPORT  0

#define LWT_MEMORY   (64<<20)
#define LWT_MAX_CPUS 4

typedef struct {
        cycles_t    lwte_when;
        char       *lwte_where;
        void       *lwte_task;
        long        lwte_p1;
        long        lwte_p2;
        long        lwte_p3;
        long        lwte_p4;
#if BITS_PER_LONG > 32
        long        lwte_pad;
#endif
} lwt_event_t;

#if LWT_SUPPORT
#ifdef __KERNEL__
#define LWT_EVENTS_PER_PAGE (PAGE_SIZE / sizeof (lwt_event_t))

typedef struct _lwt_page {
        struct list_head     lwtp_list;
        struct page         *lwtp_page;
        lwt_event_t         *lwtp_events;
} lwt_page_t;

typedef struct {
        int                lwtc_current_index;
        lwt_page_t        *lwtc_current_page;
} lwt_cpu_t;

extern int       lwt_enabled;
extern lwt_cpu_t lwt_cpus[];

extern int  lwt_init (void);
extern void lwt_fini (void);
extern int  lwt_lookup_string (int *size, char *knlptr,
                               char *usrptr, int usrsize);
extern int  lwt_control (int enable, int clear);
extern int  lwt_snapshot (cycles_t *now, int *ncpu, int *total_size,
                          void *user_ptr, int user_size);

/* Note that we _don't_ define LWT_EVENT at all if LWT_SUPPORT isn't set.
 * This stuff is meant for finding specific problems; it never stays in
 * production code... */

#define LWTSTR(n)       #n
#define LWTWHERE(f,l)   f ":" LWTSTR(l)

#define LWT_EVENT(p1, p2, p3, p4)                                       \
do {                                                                    \
        unsigned long    flags;                                         \
        lwt_cpu_t       *cpu;                                           \
        lwt_page_t      *p;                                             \
        lwt_event_t     *e;                                             \
                                                                        \
        local_irq_save (flags);                                         \
                                                                        \
        if (lwt_enabled) {                                              \
                cpu = &lwt_cpus[smp_processor_id()];                    \
                p = cpu->lwtc_current_page;                             \
                e = &p->lwtp_events[cpu->lwtc_current_index++];         \
                                                                        \
                if (cpu->lwtc_current_index >= LWT_EVENTS_PER_PAGE) {   \
                        cpu->lwtc_current_page =                        \
                                list_entry (p->lwtp_list.next,          \
                                            lwt_page_t, lwtp_list);     \
                        cpu->lwtc_current_index = 0;                    \
                }                                                       \
                                                                        \
                e->lwte_when  = get_cycles();                           \
                e->lwte_where = LWTWHERE(__FILE__,__LINE__);            \
                e->lwte_task  = current;                                \
                e->lwte_p1    = (long)(p1);                             \
                e->lwte_p2    = (long)(p2);                             \
                e->lwte_p3    = (long)(p3);                             \
                e->lwte_p4    = (long)(p4);                             \
        }                                                               \
                                                                        \
        local_irq_restore (flags);                                      \
} while (0)
#else  /* __KERNEL__ */
#define LWT_EVENT(p1,p2,p3,p4)     /* no userland implementation yet */
#endif /* __KERNEL__ */
#endif /* LWT_SUPPORT */

struct portals_device_userstate
{
        int          pdu_memhog_pages;
        struct page *pdu_memhog_root_page;
};

#include <linux/portals_lib.h>

/*
 * USER LEVEL STUFF BELOW
 */

#define PORTAL_IOCTL_VERSION 0x00010007
#define PING_SYNC       0
#define PING_ASYNC      1

struct portal_ioctl_hdr {
        __u32 ioc_len;
        __u32 ioc_version;
};

struct portals_debug_ioctl_data
{
        struct portal_ioctl_hdr hdr;
        unsigned int subs;
        unsigned int debug;
};

#define PORTAL_IOC_INIT(data)                           \
do {                                                    \
        memset(&data, 0, sizeof(data));                 \
        data.ioc_version = PORTAL_IOCTL_VERSION;        \
        data.ioc_len = sizeof(data);                    \
} while (0)

/* FIXME check conflict with lustre_lib.h */
#define PTL_IOC_DEBUG_MASK             _IOWR('f', 250, long)

static inline int portal_ioctl_packlen(struct portal_ioctl_data *data)
{
        int len = sizeof(*data);
        len += size_round(data->ioc_inllen1);
        len += size_round(data->ioc_inllen2);
        return len;
}

static inline int portal_ioctl_is_invalid(struct portal_ioctl_data *data)
{
        if (data->ioc_len > (1<<30)) {
                CERROR ("PORTALS ioctl: ioc_len larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inllen1 > (1<<30)) {
                CERROR ("PORTALS ioctl: ioc_inllen1 larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inllen2 > (1<<30)) {
                CERROR ("PORTALS ioctl: ioc_inllen2 larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inlbuf1 && !data->ioc_inllen1) {
                CERROR ("PORTALS ioctl: inlbuf1 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_inlbuf2 && !data->ioc_inllen2) {
                CERROR ("PORTALS ioctl: inlbuf2 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_pbuf1 && !data->ioc_plen1) {
                CERROR ("PORTALS ioctl: pbuf1 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_pbuf2 && !data->ioc_plen2) {
                CERROR ("PORTALS ioctl: pbuf2 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_plen1 && !data->ioc_pbuf1) {
                CERROR ("PORTALS ioctl: plen1 nonzero but no pbuf1 pointer\n");
                return 1;
        }
        if (data->ioc_plen2 && !data->ioc_pbuf2) {
                CERROR ("PORTALS ioctl: plen2 nonzero but no pbuf2 pointer\n");
                return 1;
        }
        if (portal_ioctl_packlen(data) != data->ioc_len ) {
                CERROR ("PORTALS ioctl: packlen != ioc_len\n");
                return 1;
        }
        if (data->ioc_inllen1 &&
            data->ioc_bulk[data->ioc_inllen1 - 1] != '\0') {
                CERROR ("PORTALS ioctl: inlbuf1 not 0 terminated\n");
                return 1;
        }
        if (data->ioc_inllen2 &&
            data->ioc_bulk[size_round(data->ioc_inllen1) +
                           data->ioc_inllen2 - 1] != '\0') {
                CERROR ("PORTALS ioctl: inlbuf2 not 0 terminated\n");
                return 1;
        }
        return 0;
}

#ifndef __KERNEL__
static inline int portal_ioctl_pack(struct portal_ioctl_data *data, char **pbuf,
                                    int max)
{
        char *ptr;
        struct portal_ioctl_data *overlay;
        data->ioc_len = portal_ioctl_packlen(data);
        data->ioc_version = PORTAL_IOCTL_VERSION;

        if (*pbuf && portal_ioctl_packlen(data) > max)
                return 1;
        if (*pbuf == NULL) {
                *pbuf = malloc(data->ioc_len);
        }
        if (!*pbuf)
                return 1;
        overlay = (struct portal_ioctl_data *)*pbuf;
        memcpy(*pbuf, data, sizeof(*data));

        ptr = overlay->ioc_bulk;
        if (data->ioc_inlbuf1)
                LOGL(data->ioc_inlbuf1, data->ioc_inllen1, ptr);
        if (data->ioc_inlbuf2)
                LOGL(data->ioc_inlbuf2, data->ioc_inllen2, ptr);
        if (portal_ioctl_is_invalid(overlay))
                return 1;

        return 0;
}
#else
#include <asm/uaccess.h>

/* buffer MUST be at least the size of portal_ioctl_hdr */
static inline int portal_ioctl_getdata(char *buf, char *end, void *arg)
{
        struct portal_ioctl_hdr *hdr;
        struct portal_ioctl_data *data;
        int err;
        ENTRY;

        hdr = (struct portal_ioctl_hdr *)buf;
        data = (struct portal_ioctl_data *)buf;

        err = copy_from_user(buf, (void *)arg, sizeof(*hdr));
        if ( err ) {
                EXIT;
                return err;
        }

        if (hdr->ioc_version != PORTAL_IOCTL_VERSION) {
                CERROR ("PORTALS: version mismatch kernel vs application\n");
                return -EINVAL;
        }

        if (hdr->ioc_len + buf >= end) {
                CERROR ("PORTALS: user buffer exceeds kernel buffer\n");
                return -EINVAL;
        }


        if (hdr->ioc_len < sizeof(struct portal_ioctl_data)) {
                CERROR ("PORTALS: user buffer too small for ioctl\n");
                return -EINVAL;
        }

        err = copy_from_user(buf, (void *)arg, hdr->ioc_len);
        if ( err ) {
                EXIT;
                return err;
        }

        if (portal_ioctl_is_invalid(data)) {
                CERROR ("PORTALS: ioctl not correctly formatted\n");
                return -EINVAL;
        }

        if (data->ioc_inllen1) {
                data->ioc_inlbuf1 = &data->ioc_bulk[0];
        }

        if (data->ioc_inllen2) {
                data->ioc_inlbuf2 = &data->ioc_bulk[0] +
                        size_round(data->ioc_inllen1);
        }

        EXIT;
        return 0;
}
#endif

/* ioctls for manipulating snapshots 30- */
#define IOC_PORTAL_TYPE                   'e'
#define IOC_PORTAL_MIN_NR                 30

#define IOC_PORTAL_PING                    _IOWR('e', 30, long)
#define IOC_PORTAL_GET_DEBUG               _IOWR('e', 31, long)
#define IOC_PORTAL_CLEAR_DEBUG             _IOWR('e', 32, long)
#define IOC_PORTAL_MARK_DEBUG              _IOWR('e', 33, long)
#define IOC_PORTAL_PANIC                   _IOWR('e', 34, long)
#define IOC_PORTAL_NAL_CMD                 _IOWR('e', 35, long)
#define IOC_PORTAL_GET_NID                 _IOWR('e', 36, long)
#define IOC_PORTAL_FAIL_NID                _IOWR('e', 37, long)
#define IOC_PORTAL_SET_DAEMON              _IOWR('e', 38, long)
#define IOC_PORTAL_LWT_CONTROL             _IOWR('e', 39, long)
#define IOC_PORTAL_LWT_SNAPSHOT            _IOWR('e', 40, long)
#define IOC_PORTAL_LWT_LOOKUP_STRING       _IOWR('e', 41, long)
#define IOC_PORTAL_MEMHOG                  _IOWR('e', 42, long)
#define IOC_PORTAL_MAX_NR                             42

enum {
        QSWNAL    = 1,
        SOCKNAL   = 2,
        GMNAL     = 3,
        /*          4 unused */
        TCPNAL    = 5,
        ROUTER    = 6,
        IBNAL     = 7,
        CRAY_KB_ERNAL = 8,
        NAL_ENUM_END_MARKER
};

#define PTL_NALFMT_SIZE               26 /* %u:%u.%u.%u.%u (10+4+4+4+3+1) */

#define NAL_MAX_NR (NAL_ENUM_END_MARKER - 1)

#define NAL_CMD_REGISTER_PEER_FD     100
#define NAL_CMD_CLOSE_CONNECTION     101
#define NAL_CMD_REGISTER_MYNID       102
#define NAL_CMD_PUSH_CONNECTION      103
#define NAL_CMD_GET_CONN             104
#define NAL_CMD_DEL_AUTOCONN         105
#define NAL_CMD_ADD_AUTOCONN         106
#define NAL_CMD_GET_AUTOCONN         107
#define NAL_CMD_GET_TXDESC           108
#define NAL_CMD_ADD_ROUTE            109
#define NAL_CMD_DEL_ROUTE            110
#define NAL_CMD_GET_ROUTE            111
#define NAL_CMD_NOTIFY_ROUTER        112

enum {
        DEBUG_DAEMON_START       =  1,
        DEBUG_DAEMON_STOP        =  2,
        DEBUG_DAEMON_PAUSE       =  3,
        DEBUG_DAEMON_CONTINUE    =  4,
};


enum cfg_record_type {
        PORTALS_CFG_TYPE = 1,
        LUSTRE_CFG_TYPE = 123,
};

typedef int (*cfg_record_cb_t)(enum cfg_record_type, int len, void *data);

#ifdef __CYGWIN__
# ifndef BITS_PER_LONG
#  if (~0UL) == 0xffffffffUL
#   define BITS_PER_LONG 32
#  else
#   define BITS_PER_LONG 64
#  endif
# endif
#endif

#if defined(__x86_64__)
# define LPU64 "%Lu"
# define LPD64 "%Ld"
# define LPX64 "%#Lx"
# define LPSZ  "%lu"
# define LPSSZ "%ld"
# define LP_POISON ((void *)0x5a5a5a5a5a5a5a5a)
#elif (BITS_PER_LONG == 32 || __WORDSIZE == 32)
# define LPU64 "%Lu"
# define LPD64 "%Ld"
# define LPX64 "%#Lx"
# define LPSZ  "%u"
# define LPSSZ "%d"
# define LP_POISON ((void *)0x5a5a5a5a)
#elif (BITS_PER_LONG == 64 || __WORDSIZE == 64)
# define LPU64 "%lu"
# define LPD64 "%ld"
# define LPX64 "%#lx"
# define LPSZ  "%lu"
# define LPSSZ "%ld"
# define LP_POISON ((void *)0x5a5a5a5a5a5a5a5a)
#endif
#ifndef LPU64
# error "No word size defined"
#endif

#endif
