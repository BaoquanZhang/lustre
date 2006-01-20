/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
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

#ifndef _OBD_SUPPORT
#define _OBD_SUPPORT

#ifdef __KERNEL__
#include <linux/config.h>
#include <linux/autoconf.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#endif
#include <libcfs/kp30.h>
#include <linux/lustre_compat25.h>

/* global variables */
extern atomic_t obd_memory;
extern int obd_memmax;
extern unsigned int obd_fail_loc;
extern unsigned int obd_dump_on_timeout;
extern unsigned int obd_timeout;          /* seconds */
#define PING_INTERVAL max(obd_timeout / 4, 1U)
extern unsigned int ldlm_timeout;
extern unsigned int obd_health_check_timeout;
extern char obd_lustre_upcall[128];
extern unsigned int obd_sync_filter;
extern wait_queue_head_t obd_race_waitq;

#define OBD_FAIL_MDS                     0x100
#define OBD_FAIL_MDS_HANDLE_UNPACK       0x101
#define OBD_FAIL_MDS_GETATTR_NET         0x102
#define OBD_FAIL_MDS_GETATTR_PACK        0x103
#define OBD_FAIL_MDS_READPAGE_NET        0x104
#define OBD_FAIL_MDS_READPAGE_PACK       0x105
#define OBD_FAIL_MDS_SENDPAGE            0x106
#define OBD_FAIL_MDS_REINT_NET           0x107
#define OBD_FAIL_MDS_REINT_UNPACK        0x108
#define OBD_FAIL_MDS_REINT_SETATTR       0x109
#define OBD_FAIL_MDS_REINT_SETATTR_WRITE 0x10a
#define OBD_FAIL_MDS_REINT_CREATE        0x10b
#define OBD_FAIL_MDS_REINT_CREATE_WRITE  0x10c
#define OBD_FAIL_MDS_REINT_UNLINK        0x10d
#define OBD_FAIL_MDS_REINT_UNLINK_WRITE  0x10e
#define OBD_FAIL_MDS_REINT_LINK          0x10f
#define OBD_FAIL_MDS_REINT_LINK_WRITE    0x110
#define OBD_FAIL_MDS_REINT_RENAME        0x111
#define OBD_FAIL_MDS_REINT_RENAME_WRITE  0x112
#define OBD_FAIL_MDS_OPEN_NET            0x113
#define OBD_FAIL_MDS_OPEN_PACK           0x114
#define OBD_FAIL_MDS_CLOSE_NET           0x115
#define OBD_FAIL_MDS_CLOSE_PACK          0x116
#define OBD_FAIL_MDS_CONNECT_NET         0x117
#define OBD_FAIL_MDS_CONNECT_PACK        0x118
#define OBD_FAIL_MDS_REINT_NET_REP       0x119
#define OBD_FAIL_MDS_DISCONNECT_NET      0x11a
#define OBD_FAIL_MDS_GETSTATUS_NET       0x11b
#define OBD_FAIL_MDS_GETSTATUS_PACK      0x11c
#define OBD_FAIL_MDS_STATFS_PACK         0x11d
#define OBD_FAIL_MDS_STATFS_NET          0x11e
#define OBD_FAIL_MDS_GETATTR_NAME_NET    0x11f
#define OBD_FAIL_MDS_PIN_NET             0x120
#define OBD_FAIL_MDS_UNPIN_NET           0x121
#define OBD_FAIL_MDS_ALL_REPLY_NET       0x122
#define OBD_FAIL_MDS_ALL_REQUEST_NET     0x123
#define OBD_FAIL_MDS_SYNC_NET            0x124
#define OBD_FAIL_MDS_SYNC_PACK           0x125
#define OBD_FAIL_MDS_DONE_WRITING_NET    0x126
#define OBD_FAIL_MDS_DONE_WRITING_PACK   0x127
#define OBD_FAIL_MDS_ALLOC_OBDO          0x128
#define OBD_FAIL_MDS_PAUSE_OPEN          0x129
#define OBD_FAIL_MDS_STATFS_LCW_SLEEP    0x12a
#define OBD_FAIL_MDS_OPEN_CREATE         0x12b
#define OBD_FAIL_MDS_OST_SETATTR         0x12c
#define OBD_FAIL_MDS_QUOTACHECK_NET      0x12d
#define OBD_FAIL_MDS_QUOTACTL_NET        0x12e
#define OBD_FAIL_MDS_CLIENT_ADD          0x12f
#define OBD_FAIL_MDS_GETXATTR_NET        0x130
#define OBD_FAIL_MDS_GETXATTR_PACK       0x131
#define OBD_FAIL_MDS_SETXATTR_NET        0x132
#define OBD_FAIL_MDS_SETXATTR            0x133
#define OBD_FAIL_MDS_SETXATTR_WRITE      0x134

#define OBD_FAIL_OST                     0x200
#define OBD_FAIL_OST_CONNECT_NET         0x201
#define OBD_FAIL_OST_DISCONNECT_NET      0x202
#define OBD_FAIL_OST_GET_INFO_NET        0x203
#define OBD_FAIL_OST_CREATE_NET          0x204
#define OBD_FAIL_OST_DESTROY_NET         0x205
#define OBD_FAIL_OST_GETATTR_NET         0x206
#define OBD_FAIL_OST_SETATTR_NET         0x207
#define OBD_FAIL_OST_OPEN_NET            0x208
#define OBD_FAIL_OST_CLOSE_NET           0x209
#define OBD_FAIL_OST_BRW_NET             0x20a
#define OBD_FAIL_OST_PUNCH_NET           0x20b
#define OBD_FAIL_OST_STATFS_NET          0x20c
#define OBD_FAIL_OST_HANDLE_UNPACK       0x20d
#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
#define OBD_FAIL_OST_BRW_READ_BULK       0x20f
#define OBD_FAIL_OST_SYNC_NET            0x210
#define OBD_FAIL_OST_ALL_REPLY_NET       0x211
#define OBD_FAIL_OST_ALL_REQUESTS_NET    0x212
#define OBD_FAIL_OST_LDLM_REPLY_NET      0x213
#define OBD_FAIL_OST_BRW_PAUSE_BULK      0x214
#define OBD_FAIL_OST_ENOSPC              0x215
#define OBD_FAIL_OST_EROFS               0x216
#define OBD_FAIL_OST_ENOENT              0x217
#define OBD_FAIL_OST_QUOTACHECK_NET      0x218
#define OBD_FAIL_OST_QUOTACTL_NET        0x219

#define OBD_FAIL_LDLM                    0x300
#define OBD_FAIL_LDLM_NAMESPACE_NEW      0x301
#define OBD_FAIL_LDLM_ENQUEUE            0x302
#define OBD_FAIL_LDLM_CONVERT            0x303
#define OBD_FAIL_LDLM_CANCEL             0x304
#define OBD_FAIL_LDLM_BL_CALLBACK        0x305
#define OBD_FAIL_LDLM_CP_CALLBACK        0x306
#define OBD_FAIL_LDLM_GL_CALLBACK        0x307
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
#define OBD_FAIL_LDLM_ENQUEUE_INTENT_ERR 0x309
#define OBD_FAIL_LDLM_CREATE_RESOURCE    0x30a
#define OBD_FAIL_LDLM_ENQUEUE_BLOCKED    0x30b
#define OBD_FAIL_LDLM_REPLY              0x30c
#define OBD_FAIL_LDLM_RECOV_CLIENTS      0x30d
#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e

#define OBD_FAIL_OSC                     0x400
#define OBD_FAIL_OSC_BRW_READ_BULK       0x401
#define OBD_FAIL_OSC_BRW_WRITE_BULK      0x402
#define OBD_FAIL_OSC_LOCK_BL_AST         0x403
#define OBD_FAIL_OSC_LOCK_CP_AST         0x404
#define OBD_FAIL_OSC_MATCH               0x405
#define OBD_FAIL_OSC_BRW_PREP_REQ        0x406
#define OBD_FAIL_OSC_SHUTDOWN            0x407

#define OBD_FAIL_PTLRPC                  0x500
#define OBD_FAIL_PTLRPC_ACK              0x501
#define OBD_FAIL_PTLRPC_RQBD             0x502
#define OBD_FAIL_PTLRPC_BULK_GET_NET     0x503
#define OBD_FAIL_PTLRPC_BULK_PUT_NET     0x504
#define OBD_FAIL_PTLRPC_DROP_RPC         0x505

#define OBD_FAIL_OBD_PING_NET            0x600
#define OBD_FAIL_OBD_LOG_CANCEL_NET      0x601
#define OBD_FAIL_OBD_LOGD_NET            0x602
#define OBD_FAIL_OBD_QC_CALLBACK_NET     0x603
#define OBD_FAIL_OBD_DQACQ               0x604

#define OBD_FAIL_TGT_REPLY_NET           0x700
#define OBD_FAIL_TGT_CONN_RACE           0x701

#define OBD_FAIL_MDC_REVALIDATE_PAUSE    0x800


#define OBD_FAIL_MGMT                     0x900
#define OBD_FAIL_MGMT_FIRST_CONNECT       0x901
#define OBD_FAIL_MGMT_CONNECT_NET         0x117
#define OBD_FAIL_MGMT_DISCONNECT_NET      0x11a
#define OBD_FAIL_MGMT_ALL_REPLY_NET       0x122
#define OBD_FAIL_MGMT_ALL_REQUEST_NET     0x123

/* preparation for a more advanced failure testbed (not functional yet) */
#define OBD_FAIL_MASK_SYS    0x0000FF00
#define OBD_FAIL_MASK_LOC    (0x000000FF | OBD_FAIL_MASK_SYS)
#define OBD_FAIL_ONCE        0x80000000
#define OBD_FAILED           0x40000000
#define OBD_FAIL_MDS_ALL_NET 0x01000000
#define OBD_FAIL_OST_ALL_NET 0x02000000
#define OBD_FAIL_MGS_ALL_NET 0x01000000

#define OBD_FAIL_CHECK(id)   (((obd_fail_loc & OBD_FAIL_MASK_LOC) ==           \
                              ((id) & OBD_FAIL_MASK_LOC)) &&                   \
                              ((obd_fail_loc & (OBD_FAILED | OBD_FAIL_ONCE))!= \
                                (OBD_FAILED | OBD_FAIL_ONCE)))

#define OBD_FAIL_CHECK_ONCE(id)                                              \
({      int _ret_ = 0;                                                       \
        if (OBD_FAIL_CHECK(id)) {                                            \
                CERROR("*** obd_fail_loc=%x ***\n", id);                     \
                obd_fail_loc |= OBD_FAILED;                                  \
                if ((id) & OBD_FAIL_ONCE)                                    \
                        obd_fail_loc |= OBD_FAIL_ONCE;                       \
                _ret_ = 1;                                                   \
        }                                                                    \
        _ret_;                                                               \
})

#define OBD_FAIL_RETURN(id, ret)                                             \
do {                                                                         \
        if (OBD_FAIL_CHECK_ONCE(id)) {                                       \
                RETURN(ret);                                                 \
        }                                                                    \
} while(0)

#define OBD_FAIL_TIMEOUT(id, secs)                                           \
do {                                                                         \
        if (OBD_FAIL_CHECK_ONCE(id)) {                                      \
                CERROR("obd_fail_timeout id %x sleeping for %d secs\n",      \
                       (id), (secs));                                        \
                set_current_state(TASK_UNINTERRUPTIBLE);                     \
                schedule_timeout((secs) * HZ);                               \
                set_current_state(TASK_RUNNING);                             \
                CERROR("obd_fail_timeout id %x awake\n", (id));              \
       }                                                                     \
} while(0)

/* Prefer the kernel's version, if it exports it, because it might be
 * optimized for this CPU. */
#if defined(__KERNEL__) && (defined(CONFIG_CRC32) || defined(CONFIG_CRC32_MODULE))
# include <linux/crc32.h>
#else
/* crc32_le lifted from the Linux kernel, which had the following to say:
 *
 * This code is in the public domain; copyright abandoned.
 * Liability for non-performance of this code is limited to the amount
 * you paid for it.  Since it is distributed for free, your refund will
 * be very very small.  If it breaks, you get to keep both pieces.
 */
#define CRCPOLY_LE 0xedb88320
/**
 * crc32_le() - Calculate bitwise little-endian Ethernet AUTODIN II CRC32
 * @crc - seed value for computation.  ~0 for Ethernet, sometimes 0 for
 *        other uses, or the previous crc32 value if computing incrementally.
 * @p   - pointer to buffer over which CRC is run
 * @len - length of buffer @p
 */
static inline __u32 crc32_le(__u32 crc, unsigned char const *p, size_t len)
{
        int i;
        while (len--) {
                crc ^= *p++;
                for (i = 0; i < 8; i++)
                        crc = (crc >> 1) ^ ((crc & 1) ? CRCPOLY_LE : 0);
        }
        return crc;
}
#endif

#ifdef __KERNEL__
/* The idea here is to synchronise two threads to force a race. The
 * first thread that calls this with a matching fail_loc is put to
 * sleep. The next thread that calls with the same fail_loc wakes up
 * the first and continues. */
#define OBD_RACE(id)                                            \
do {                                                            \
        if  (OBD_FAIL_CHECK_ONCE(id)) {                         \
                CERROR("obd_race id %x sleeping\n", (id));      \
                interruptible_sleep_on(&obd_race_waitq);                      \
                CERROR("obd_fail_race id %x awake\n", (id));    \
        } else if ((obd_fail_loc & OBD_FAIL_MASK_LOC) ==        \
                    ((id) & OBD_FAIL_MASK_LOC)) {               \
                wake_up(&obd_race_waitq);                       \
        }                                                       \
} while(0)
#else
/* sigh.  an expedient fix until OBD_RACE is fixed up */
#define OBD_RACE(foo) do {} while(0)
#endif

#define fixme() CDEBUG(D_OTHER, "FIXME\n");

#ifdef __KERNEL__
# include <linux/types.h>
# include <linux/blkdev.h>
# include <linux/lvfs.h>

static inline void OBD_FAIL_WRITE(int id, struct super_block *sb)
{
        if (OBD_FAIL_CHECK(id)) {
                BDEVNAME_DECLARE_STORAGE(tmp);
                CERROR("obd_fail_loc=%x, fail write operation on %s\n",
                       id, ll_bdevname(sb, tmp));
                lvfs_set_rdonly(lvfs_sbdev(sb));
                /* We set FAIL_ONCE because we never "un-fail" a device */
                obd_fail_loc |= OBD_FAILED | OBD_FAIL_ONCE;
        }
}
#else /* !__KERNEL__ */
# define LTIME_S(time) (time)
/* for obd_class.h */
# ifndef ERR_PTR
#  define ERR_PTR(a) ((void *)(a))
# endif
#endif  /* __KERNEL__ */

extern atomic_t libcfs_kmemory;

#if defined(LUSTRE_UTILS) /* this version is for utils only */
#define OBD_ALLOC_GFP(ptr, size, gfp_mask)                                    \
do {                                                                          \
        (ptr) = kmalloc(size, (gfp_mask));                                    \
        if ((ptr) == NULL) {                                                  \
                CERROR("kmalloc of '" #ptr "' (%d bytes) failed at %s:%d\n",  \
                       (int)(size), __FILE__, __LINE__);                      \
        } else {                                                              \
                memset(ptr, 0, size);                                         \
                CDEBUG(D_MALLOC, "kmalloced '" #ptr "': %d at %p\n",          \
                       (int)(size), ptr);                                     \
        }                                                                     \
} while (0)
#else /* this version is for the kernel and liblustre */
#define OBD_ALLOC_GFP(ptr, size, gfp_mask)                                    \
do {                                                                          \
        (ptr) = kmalloc(size, (gfp_mask));                                    \
        if ((ptr) == NULL) {                                                  \
                CERROR("kmalloc of '" #ptr "' (%d bytes) failed at %s:%d\n",  \
                       (int)(size), __FILE__, __LINE__);                      \
                CERROR("%d total bytes allocated by Lustre, %d by Portals\n", \
                       atomic_read(&obd_memory), atomic_read(&libcfs_kmemory));\
        } else {                                                              \
                memset(ptr, 0, size);                                         \
                atomic_add(size, &obd_memory);                                \
                if (atomic_read(&obd_memory) > obd_memmax)                    \
                        obd_memmax = atomic_read(&obd_memory);                \
                CDEBUG(D_MALLOC, "kmalloced '" #ptr "': %d at %p (tot %d)\n", \
                       (int)(size), ptr, atomic_read(&obd_memory));           \
        }                                                                     \
} while (0)
#endif

#ifndef OBD_GFP_MASK
# define OBD_GFP_MASK GFP_NOFS
#endif

#define OBD_ALLOC(ptr, size) OBD_ALLOC_GFP(ptr, size, OBD_GFP_MASK)
#define OBD_ALLOC_WAIT(ptr, size) OBD_ALLOC_GFP(ptr, size, GFP_KERNEL)
#define OBD_ALLOC_PTR(ptr) OBD_ALLOC(ptr, sizeof *(ptr))
#define OBD_ALLOC_PTR_WAIT(ptr) OBD_ALLOC_WAIT(ptr, sizeof *(ptr))

#ifdef __arch_um__
# define OBD_VMALLOC(ptr, size) OBD_ALLOC(ptr, size)
#else
# define OBD_VMALLOC(ptr, size)                                               \
do {                                                                          \
        (ptr) = vmalloc(size);                                                \
        if ((ptr) == NULL) {                                                  \
                CERROR("vmalloc of '" #ptr "' (%d bytes) failed at %s:%d\n",  \
                       (int)(size), __FILE__, __LINE__);                      \
                CERROR("%d total bytes allocated by Lustre, %d by Portals\n", \
                       atomic_read(&obd_memory), atomic_read(&libcfs_kmemory));\
        } else {                                                              \
                memset(ptr, 0, size);                                         \
                atomic_add(size, &obd_memory);                                \
                if (atomic_read(&obd_memory) > obd_memmax)                    \
                        obd_memmax = atomic_read(&obd_memory);                \
                CDEBUG(D_MALLOC, "vmalloced '" #ptr "': %d at %p (tot %d)\n", \
                       (int)(size), ptr, atomic_read(&obd_memory));           \
        }                                                                     \
} while (0)
#endif

#ifdef CONFIG_DEBUG_SLAB
#define POISON(ptr, c, s) do {} while (0)
#else
#define POISON(ptr, c, s) memset(ptr, c, s)
#endif

#if POISON_BULK
#define POISON_PAGE(page, val) do { memset(kmap(page), val, PAGE_SIZE);       \
                                    kunmap(page); } while (0)
#else
#define POISON_PAGE(page, val) do { } while (0)
#endif

#ifdef __KERNEL__
#define OBD_FREE(ptr, size)                                                   \
do {                                                                          \
        LASSERT(ptr);                                                         \
        atomic_sub(size, &obd_memory);                                        \
        CDEBUG(D_MALLOC, "kfreed '" #ptr "': %d at %p (tot %d).\n",           \
               (int)(size), ptr, atomic_read(&obd_memory));                   \
        POISON(ptr, 0x5a, size);                                              \
        kfree(ptr);                                                           \
        (ptr) = (void *)0xdeadbeef;                                           \
} while (0)
#else
#define OBD_FREE(ptr, size) ((void)(size), free((ptr)))
#endif

#ifdef __arch_um__
# define OBD_VFREE(ptr, size) OBD_FREE(ptr, size)
#else
# define OBD_VFREE(ptr, size)                                                 \
do {                                                                          \
        LASSERT(ptr);                                                         \
        atomic_sub(size, &obd_memory);                                        \
        CDEBUG(D_MALLOC, "vfreed '" #ptr "': %d at %p (tot %d).\n",           \
               (int)(size), ptr, atomic_read(&obd_memory));                   \
        POISON(ptr, 0x5a, size);                                              \
        vfree(ptr);                                                           \
        (ptr) = (void *)0xdeadbeef;                                           \
} while (0)
#endif

/* we memset() the slab object to 0 when allocation succeeds, so DO NOT
 * HAVE A CTOR THAT DOES ANYTHING.  its work will be cleared here.  we'd
 * love to assert on that, but slab.c keeps kmem_cache_s all to itself. */
#define OBD_SLAB_ALLOC(ptr, slab, type, size)                                 \
do {                                                                          \
        LASSERT(!in_interrupt());                                             \
        (ptr) = kmem_cache_alloc(slab, (type));                               \
        if ((ptr) == NULL) {                                                  \
                CERROR("slab-alloc of '"#ptr"' (%d bytes) failed at %s:%d\n", \
                       (int)(size), __FILE__, __LINE__);                      \
                CERROR("%d total bytes allocated by Lustre, %d by Portals\n", \
                       atomic_read(&obd_memory), atomic_read(&libcfs_kmemory));\
        } else {                                                              \
                memset(ptr, 0, size);                                         \
                atomic_add(size, &obd_memory);                                \
                if (atomic_read(&obd_memory) > obd_memmax)                    \
                        obd_memmax = atomic_read(&obd_memory);                \
                CDEBUG(D_MALLOC, "slab-alloced '"#ptr"': %d at %p (tot %d)\n",\
                       (int)(size), ptr, atomic_read(&obd_memory));           \
        }                                                                     \
} while (0)

#define OBD_FREE_PTR(ptr) OBD_FREE(ptr, sizeof *(ptr))

#define OBD_SLAB_FREE(ptr, slab, size)                                        \
do {                                                                          \
        LASSERT(ptr);                                                         \
        CDEBUG(D_MALLOC, "slab-freed '" #ptr "': %d at %p (tot %d).\n",       \
               (int)(size), ptr, atomic_read(&obd_memory));                   \
        atomic_sub(size, &obd_memory);                                        \
        POISON(ptr, 0x5a, size);                                              \
        kmem_cache_free(slab, ptr);                                           \
        (ptr) = (void *)0xdeadbeef;                                           \
} while (0)

#define KEY_IS(str) \
        (keylen == strlen(str) && memcmp(key, str, keylen) == 0)

#endif
