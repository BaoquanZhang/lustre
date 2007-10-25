/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Permission Cache for Remote Client
 *   Author: Lai Siyao <lsy@clusterfs.com>
 *   Author: Fan Yong <fanyong@clusterfs.com>
 *
 *  Copyright (c) 2004-2006 Cluster File Systems, Inc.
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
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/module.h>
#include <linux/types.h>
#include <linux/random.h>
#include <linux/version.h>

#include <lustre_lite.h>
#include <lustre_ha.h>
#include <lustre_dlm.h>
#include <lprocfs_status.h>
#include <lustre_disk.h>
#include <lustre_param.h>
#include "llite_internal.h"

cfs_mem_cache_t *ll_remote_perm_cachep = NULL;
cfs_mem_cache_t *ll_rmtperm_hash_cachep = NULL;

static inline struct ll_remote_perm *alloc_ll_remote_perm(void)
{
        struct ll_remote_perm *lrp;

        OBD_SLAB_ALLOC(lrp, ll_remote_perm_cachep, SLAB_KERNEL, sizeof(*lrp));
        if (lrp)
                INIT_HLIST_NODE(&lrp->lrp_list);
        return lrp;
}

static inline void free_ll_remote_perm(struct ll_remote_perm *lrp)
{
        if (!lrp)
                return;

        if (!hlist_unhashed(&lrp->lrp_list))
                hlist_del(&lrp->lrp_list);
        OBD_SLAB_FREE(lrp, ll_remote_perm_cachep, sizeof(*lrp));
}

struct hlist_head *alloc_rmtperm_hash(void)
{
        struct hlist_head *hash;
        int i;

        OBD_SLAB_ALLOC(hash, ll_rmtperm_hash_cachep, SLAB_KERNEL,
                       REMOTE_PERM_HASHSIZE * sizeof(*hash));

        if (!hash)
                return NULL;

        for (i = 0; i < REMOTE_PERM_HASHSIZE; i++)
                INIT_HLIST_HEAD(hash + i);

        return hash;
}

void free_rmtperm_hash(struct hlist_head *hash)
{
        int i;
        struct ll_remote_perm *lrp;
        struct hlist_node *node, *next;

        if(!hash)
                return;

        for (i = 0; i < REMOTE_PERM_HASHSIZE; i++)
                hlist_for_each_entry_safe(lrp, node, next, hash + i, lrp_list)
                        free_ll_remote_perm(lrp);
        OBD_SLAB_FREE(hash, ll_rmtperm_hash_cachep,
                      REMOTE_PERM_HASHSIZE * sizeof(*hash));
}

static inline int remote_perm_hashfunc(uid_t uid)
{
        return uid & (REMOTE_PERM_HASHSIZE - 1);
}

/* NB: setxid permission is not checked here, instead it's done on
 * MDT when client get remote permission. (lookup/mdc_get_remote_perm). */
static int do_check_remote_perm(struct ll_inode_info *lli, int mask)
{
        struct hlist_head *head;
        struct ll_remote_perm *lrp;
        struct hlist_node *node;
        int found = 0, rc;
        ENTRY;

        if (!lli->lli_remote_perms)
                RETURN(-ENOENT);

        head = lli->lli_remote_perms + remote_perm_hashfunc(current->uid);

        spin_lock(&lli->lli_lock);
        hlist_for_each_entry(lrp, node, head, lrp_list) {
                if (lrp->lrp_uid != current->uid)
                        continue;
                if (lrp->lrp_gid != current->gid)
                        continue;
                if (lrp->lrp_fsuid != current->fsuid)
                        continue;
                if (lrp->lrp_fsgid != current->fsgid)
                        continue;
                found = 1;
                break;
        }

        if (!found)
                GOTO(out, rc = -ENOENT);

        CDEBUG(D_SEC, "found remote perm: %u/%u/%u/%u - %#x\n",
               lrp->lrp_uid, lrp->lrp_gid, lrp->lrp_fsuid, lrp->lrp_fsgid,
               lrp->lrp_access_perm);
        rc = ((lrp->lrp_access_perm & mask) == mask) ? 0 : -EACCES;

out:
        spin_unlock(&lli->lli_lock);
        return rc;
}

int ll_update_remote_perm(struct inode *inode, struct mdt_remote_perm *perm)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_remote_perm *lrp = NULL, *tmp = NULL;
        struct hlist_head *head, *perm_hash = NULL;
        struct hlist_node *node;
        ENTRY;

        LASSERT(ll_i2sbi(inode)->ll_flags & LL_SBI_RMT_CLIENT);

#if 0
        if (perm->rp_uid != current->uid ||
            perm->rp_gid != current->gid ||
            perm->rp_fsuid != current->fsuid ||
            perm->rp_fsgid != current->fsgid) {
                /* user might setxid in this small period */
                CDEBUG(D_SEC,
                       "remote perm user %u/%u/%u/%u != current %u/%u/%u/%u\n",
                       perm->rp_uid, perm->rp_gid, perm->rp_fsuid,
                       perm->rp_fsgid, current->uid, current->gid,
                       current->fsuid, current->fsgid);
                RETURN(-EAGAIN);
        }
#endif

        if (!lli->lli_remote_perms) {
                perm_hash = alloc_rmtperm_hash();
                if (perm_hash == NULL) {
                        CERROR("alloc lli_remote_perms failed!\n");
                        RETURN(-ENOMEM);
                }
        }

        spin_lock(&lli->lli_lock);

        if (!lli->lli_remote_perms)
                lli->lli_remote_perms = perm_hash;
        else if (perm_hash)
                free_rmtperm_hash(perm_hash);

        head = lli->lli_remote_perms + remote_perm_hashfunc(perm->rp_uid);

again:
        hlist_for_each_entry(tmp, node, head, lrp_list) {
                if (tmp->lrp_uid != perm->rp_uid)
                        continue;
                if (tmp->lrp_gid != perm->rp_gid)
                        continue;
                if (tmp->lrp_fsuid != perm->rp_fsuid)
                        continue;
                if (tmp->lrp_fsgid != perm->rp_fsgid)
                        continue;
                if (lrp)
                        free_ll_remote_perm(lrp);
                lrp = tmp;
                break;
        }

        if (!lrp) {
                spin_unlock(&lli->lli_lock);
                lrp = alloc_ll_remote_perm();
                if (!lrp) {
                        CERROR("alloc memory for ll_remote_perm failed!\n");
                        RETURN(-ENOMEM);
                }
                spin_lock(&lli->lli_lock);
                goto again;
        }

        lrp->lrp_access_perm = perm->rp_access_perm;
        if (lrp != tmp) {
                lrp->lrp_uid         = perm->rp_uid;
                lrp->lrp_gid         = perm->rp_gid;
                lrp->lrp_fsuid       = perm->rp_fsuid;
                lrp->lrp_fsgid       = perm->rp_fsgid;
                hlist_add_head(&lrp->lrp_list, head);
        }
        lli->lli_rmtperm_utime = jiffies;
        spin_unlock(&lli->lli_lock);

        CDEBUG(D_SEC, "new remote perm@%p: %u/%u/%u/%u - %#x\n",
               lrp, lrp->lrp_uid, lrp->lrp_gid, lrp->lrp_fsuid, lrp->lrp_fsgid,
               lrp->lrp_access_perm);

        RETURN(0);
}

int lustre_check_remote_perm(struct inode *inode, int mask)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req = NULL;
        struct mdt_remote_perm *perm;
        struct obd_capa *oc;
        unsigned long utime;
        int i = 0, rc;
        ENTRY;

check:
        utime = lli->lli_rmtperm_utime;
        rc = do_check_remote_perm(lli, mask);
        if (!rc || ((rc != -ENOENT) && i))
                RETURN(rc);

        might_sleep();

        down(&lli->lli_rmtperm_sem);
        /* check again */
        if (utime != lli->lli_rmtperm_utime) {
                rc = do_check_remote_perm(lli, mask);
                if (!rc || ((rc != -ENOENT) && i)) {
                        up(&lli->lli_rmtperm_sem);
                        RETURN(rc);
                }
        }

        if (i++ > 5) {
                CERROR("check remote perm falls in dead loop!\n");
                LBUG();
        }

        oc = ll_mdscapa_get(inode);
        rc = md_get_remote_perm(sbi->ll_md_exp, ll_inode2fid(inode), oc, &req);
        capa_put(oc);
        if (rc) {
                up(&lli->lli_rmtperm_sem);
                RETURN(rc);
        }

        perm = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF + 1, sizeof(*perm));
        LASSERT(perm);
        LASSERT(lustre_rep_swabbed(req, REPLY_REC_OFF + 1));

        rc = ll_update_remote_perm(inode, perm);
        up(&lli->lli_rmtperm_sem);

        ptlrpc_req_finished(req);

        if (rc == -ENOMEM)
                RETURN(rc);

        goto check;
}

#if 0  /* NB: remote perms can't be freed in ll_mdc_blocking_ast of UPDATE lock,
        * because it will fail sanity test 48.
        */
void ll_free_remote_perms(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct hlist_head *hash = lli->lli_remote_perms;
        struct ll_remote_perm *lrp;
        struct hlist_node *node, *next;
        int i;

        LASSERT(hash);

        spin_lock(&lli->lli_lock);

        for (i = 0; i < REMOTE_PERM_HASHSIZE; i++) {
                hlist_for_each_entry_safe(lrp, node, next, hash + i, lrp_list)
                        free_ll_remote_perm(lrp);
        }

        spin_unlock(&lli->lli_lock);
}
#endif
