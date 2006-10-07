/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   This file is part of Lustre, http://www.lustre.org
 *
 * MDS data structures.
 * See also lustre_idl.h for wire formats of requests.
 */

#ifndef _LUSTRE_MDC_H
#define _LUSTRE_MDC_H

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/dcache.h>
# ifdef CONFIG_FS_POSIX_ACL
# include <linux/xattr_acl.h>
# endif
#endif
#include <lustre_handles.h>
#include <libcfs/kp30.h>
#include <lustre/lustre_idl.h>
#include <lustre_lib.h>
#include <lustre_dlm.h>
#include <lustre_log.h>
#include <lustre_export.h>

struct ptlrpc_client;
struct obd_export;
struct ptlrpc_request;
struct obd_device;

struct mdc_rpc_lock {
        struct semaphore rpcl_sem;
        struct lookup_intent *rpcl_it;
};

static inline void mdc_init_rpc_lock(struct mdc_rpc_lock *lck)
{
        sema_init(&lck->rpcl_sem, 1);
        lck->rpcl_it = NULL;
}

static inline void mdc_get_rpc_lock(struct mdc_rpc_lock *lck,
                                    struct lookup_intent *it)
{
        ENTRY;
        if (!it || (it->it_op != IT_GETATTR && it->it_op != IT_LOOKUP)) {
                down(&lck->rpcl_sem);
                LASSERT(lck->rpcl_it == NULL);
                lck->rpcl_it = it;
        }
}

static inline void mdc_put_rpc_lock(struct mdc_rpc_lock *lck,
                                    struct lookup_intent *it)
{
        if (!it || (it->it_op != IT_GETATTR && it->it_op != IT_LOOKUP)) {
                LASSERT(it == lck->rpcl_it);
                lck->rpcl_it = NULL;
                up(&lck->rpcl_sem);
        }
        EXIT;
}

/* mdc/mdc_locks.c */
int it_disposition(struct lookup_intent *it, int flag);
void it_clear_disposition(struct lookup_intent *it, int flag);
void it_set_disposition(struct lookup_intent *it, int flag);
int it_open_error(int phase, struct lookup_intent *it);
#ifdef HAVE_SPLIT_SUPPORT
int mdc_sendpage(struct obd_export *exp, const struct lu_fid *fid,
                 const struct page *page, int offset);
#endif
#endif
