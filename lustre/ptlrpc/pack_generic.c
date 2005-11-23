/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Eric Barton <eeb@clusterfs.com>
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
 *
 * (Un)packing of OST requests
 *
 */

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_net.h>


#define HDR_SIZE(count) \
    size_round(offsetof (struct lustre_msg, buflens[(count)]))

int lustre_msg_swabbed(struct lustre_msg *msg)
{
        return (msg->magic == __swab32(PTLRPC_MSG_MAGIC));
}

static void
lustre_init_msg (struct lustre_msg *msg, int count, int *lens, char **bufs)
{
        char *ptr;
        int   i;

        msg->magic = PTLRPC_MSG_MAGIC;
        msg->version = PTLRPC_MSG_VERSION;
        msg->bufcount = count;
        for (i = 0; i < count; i++)
                msg->buflens[i] = lens[i];

        if (bufs == NULL)
                return;

        ptr = (char *)msg + HDR_SIZE(count);
        for (i = 0; i < count; i++) {
                char *tmp = bufs[i];
                LOGL(tmp, lens[i], ptr);
        }
}

int lustre_pack_request (struct ptlrpc_request *req,
                         int count, int *lens, char **bufs)
{
        int reqlen;
        ENTRY;

        reqlen = lustre_msg_size (count, lens);
        /* See if we got it from prealloc pool */
        if (req->rq_reqmsg) {
                /* Cannot return error here, that would create
                   infinite loop in ptlrpc_prep_req_pool */
                /* In this case ptlrpc_prep_req_from_pool sets req->rq_reqlen
                   to maximum size that would fit into this preallocated
                   request */
                LASSERTF(req->rq_reqlen >= reqlen, "req->rq_reqlen %d, "
                                                   "reqlen %d\n",req->rq_reqlen,
                                                    reqlen);
                memset(req->rq_reqmsg, 0, reqlen);
        } else {
                OBD_ALLOC(req->rq_reqmsg, reqlen);
                if (req->rq_reqmsg == NULL)
                        RETURN(-ENOMEM);
        }
        req->rq_reqlen = reqlen;

        lustre_init_msg (req->rq_reqmsg, count, lens, bufs);
        RETURN (0);
}

#if RS_DEBUG
LIST_HEAD(ptlrpc_rs_debug_lru);
spinlock_t ptlrpc_rs_debug_lock = SPIN_LOCK_UNLOCKED;

#define PTLRPC_RS_DEBUG_LRU_ADD(rs)                                     \
do {                                                                    \
        unsigned long __flags;                                          \
                                                                        \
        spin_lock_irqsave(&ptlrpc_rs_debug_lock, __flags);              \
        list_add_tail(&(rs)->rs_debug_list, &ptlrpc_rs_debug_lru);      \
        spin_unlock_irqrestore(&ptlrpc_rs_debug_lock, __flags);         \
} while (0)

#define PTLRPC_RS_DEBUG_LRU_DEL(rs)                                     \
do {                                                                    \
        unsigned long __flags;                                          \
                                                                        \
        spin_lock_irqsave(&ptlrpc_rs_debug_lock, __flags);              \
        list_del(&(rs)->rs_debug_list);                                 \
        spin_unlock_irqrestore(&ptlrpc_rs_debug_lock, __flags);         \
} while (0)
#else
# define PTLRPC_RS_DEBUG_LRU_ADD(rs) do {} while(0)
# define PTLRPC_RS_DEBUG_LRU_DEL(rs) do {} while(0)
#endif

static struct ptlrpc_reply_state *lustre_get_emerg_rs(struct ptlrpc_service *svc,
                                                      int size)
{
        unsigned long flags;
        struct ptlrpc_reply_state *rs = NULL;

        spin_lock_irqsave(&svc->srv_lock, flags);
        /* See if we have anything in a pool, and wait if nothing */
        while (list_empty(&svc->srv_free_rs_list)) {
                struct l_wait_info lwi;
                int rc;
                spin_unlock_irqrestore(&svc->srv_lock, flags);
                /* If we cannot get anything for some long time, we better
                   bail out instead of waiting infinitely */
                lwi = LWI_TIMEOUT(10 * HZ, NULL, NULL);
                rc = l_wait_event(svc->srv_free_rs_waitq,
                                  !list_empty(&svc->srv_free_rs_list), &lwi);
                if (rc)
                        goto out;
                spin_lock_irqsave(&svc->srv_lock, flags);
        }
        
        rs = list_entry(svc->srv_free_rs_list.next, struct ptlrpc_reply_state,
                        rs_list);
        list_del(&rs->rs_list);
        spin_unlock_irqrestore(&svc->srv_lock, flags);
        LASSERT(rs);
        LASSERTF(svc->srv_max_reply_size > size, "Want %d, prealloc %d\n", size,
                 svc->srv_max_reply_size);
        memset(rs, 0, size);
        rs->rs_prealloc = 1;
out:
        return rs;
}


int lustre_pack_reply (struct ptlrpc_request *req,
                       int count, int *lens, char **bufs)
{
        struct ptlrpc_reply_state *rs;
        int                        msg_len;
        int                        size;
        ENTRY;

        LASSERT (req->rq_reply_state == NULL);

        msg_len = lustre_msg_size (count, lens);
        size = offsetof (struct ptlrpc_reply_state, rs_msg) + msg_len;
        OBD_ALLOC (rs, size);
        if (unlikely(rs == NULL)) {
                rs = lustre_get_emerg_rs(req->rq_rqbd->rqbd_service, size);
                if (!rs)
                        RETURN (-ENOMEM);
        }
        atomic_set(&rs->rs_refcount, 1);        /* 1 ref for rq_reply_state */
        rs->rs_cb_id.cbid_fn = reply_out_callback;
        rs->rs_cb_id.cbid_arg = rs;
        rs->rs_service = req->rq_rqbd->rqbd_service;
        rs->rs_size = size;
        INIT_LIST_HEAD(&rs->rs_exp_list);
        INIT_LIST_HEAD(&rs->rs_obd_list);

        req->rq_replen = msg_len;
        req->rq_reply_state = rs;
        req->rq_repmsg = &rs->rs_msg;
        lustre_init_msg (&rs->rs_msg, count, lens, bufs);

        PTLRPC_RS_DEBUG_LRU_ADD(rs);

        RETURN (0);
}

void lustre_free_reply_state (struct ptlrpc_reply_state *rs)
{
        PTLRPC_RS_DEBUG_LRU_DEL(rs);

        LASSERT (atomic_read(&rs->rs_refcount) == 0);
        LASSERT (!rs->rs_difficult || rs->rs_handled);
        LASSERT (!rs->rs_on_net);
        LASSERT (!rs->rs_scheduled);
        LASSERT (rs->rs_export == NULL);
        LASSERT (rs->rs_nlocks == 0);
        LASSERT (list_empty(&rs->rs_exp_list));
        LASSERT (list_empty(&rs->rs_obd_list));

        if (unlikely(rs->rs_prealloc)) {
                unsigned long flags;
                struct ptlrpc_service *svc = rs->rs_service;

                spin_lock_irqsave(&svc->srv_lock, flags);
                list_add(&rs->rs_list,
                         &svc->srv_free_rs_list);
                spin_unlock_irqrestore(&svc->srv_lock, flags);
                wake_up(&svc->srv_free_rs_waitq);
        } else {
                OBD_FREE(rs, rs->rs_size);
        }
}

/* This returns the size of the buffer that is required to hold a lustre_msg
 * with the given sub-buffer lengths. */
int lustre_msg_size(int count, int *lengths)
{
        int size;
        int i;

        size = HDR_SIZE (count);
        for (i = 0; i < count; i++)
                size += size_round(lengths[i]);

        return size;
}

int lustre_unpack_msg(struct lustre_msg *m, int len)
{
        int   flipped;
        int   required_len;
        int   i;
        ENTRY;

        /* We can provide a slightly better error log, if we check the
         * message magic and version first.  In the future, struct
         * lustre_msg may grow, and we'd like to log a version mismatch,
         * rather than a short message.
         *
         */
        required_len = MAX (offsetof (struct lustre_msg, version) +
                            sizeof (m->version),
                            offsetof (struct lustre_msg, magic) +
                            sizeof (m->magic));
        if (len < required_len) {
                /* can't even look inside the message */
                CERROR ("message length %d too small for magic/version check\n",
                        len);
                RETURN (-EINVAL);
        }

        flipped = lustre_msg_swabbed(m);
        if (flipped)
                __swab32s (&m->version);
        else if (m->magic != PTLRPC_MSG_MAGIC) {
                CERROR("wrong lustre_msg magic %#08x\n", m->magic);
                RETURN (-EINVAL);
        }

        if (m->version != PTLRPC_MSG_VERSION) {
                CERROR("wrong lustre_msg version %#08x\n", m->version);
                RETURN (-EINVAL);
        }

        /* Now we know the sender speaks my language (but possibly flipped)...*/
        required_len = HDR_SIZE(0);
        if (len < required_len) {
                /* can't even look inside the message */
                CERROR ("message length %d too small for lustre_msg\n", len);
                RETURN (-EINVAL);
        }

        if (flipped) {
                __swab32s (&m->type);
                __swab32s (&m->opc);
                __swab64s (&m->last_xid);
                __swab64s (&m->last_committed);
                __swab64s (&m->transno);
                __swab32s (&m->status);
                __swab32s (&m->flags);
                __swab32s (&m->conn_cnt);
                __swab32s (&m->bufcount);
        }

        required_len = HDR_SIZE(m->bufcount);

        if (len < required_len) {
                /* didn't receive all the buffer lengths */
                CERROR ("message length %d too small for %d buflens\n",
                        len, m->bufcount);
                RETURN(-EINVAL);
        }

        for (i = 0; i < m->bufcount; i++) {
                if (flipped)
                        __swab32s (&m->buflens[i]);
                required_len += size_round(m->buflens[i]);
        }

        if (len < required_len) {
                CERROR("len: %d, required_len %d\n", len, required_len);
                CERROR("bufcount: %d\n", m->bufcount);
                for (i = 0; i < m->bufcount; i++)
                        CERROR("buffer %d length %d\n", i, m->buflens[i]);
                RETURN(-EINVAL);
        }

        RETURN(0);
}

/**
 * lustre_msg_buflen - return the length of buffer @n in message @m
 * @m - lustre_msg (request or reply) to look at
 * @n - message index (base 0)
 *
 * returns zero for non-existent message indices
 */
int lustre_msg_buflen(struct lustre_msg *m, int n)
{
        if (n >= m->bufcount)
                return 0;

        return m->buflens[n];
}
EXPORT_SYMBOL(lustre_msg_buflen);

void *lustre_msg_buf(struct lustre_msg *m, int n, int min_size)
{
        int i;
        int offset;
        int buflen;
        int bufcount;

        LASSERT (m != NULL);
        LASSERT (n >= 0);

        bufcount = m->bufcount;
        if (n >= bufcount) {
                CDEBUG(D_INFO, "msg %p buffer[%d] not present (count %d)\n",
                       m, n, bufcount);
                return NULL;
        }

        buflen = m->buflens[n];
        if (buflen < min_size) {
                CERROR("msg %p buffer[%d] size %d too small (required %d)\n",
                       m, n, buflen, min_size);
                return NULL;
        }

        offset = HDR_SIZE(bufcount);
        for (i = 0; i < n; i++)
                offset += size_round(m->buflens[i]);

        return (char *)m + offset;
}

char *lustre_msg_string (struct lustre_msg *m, int index, int max_len)
{
        /* max_len == 0 means the string should fill the buffer */
        char *str = lustre_msg_buf (m, index, 0);
        int   slen;
        int   blen;

        if (str == NULL) {
                CERROR ("can't unpack string in msg %p buffer[%d]\n", m, index);
                return (NULL);
        }

        blen = m->buflens[index];
        slen = strnlen (str, blen);

        if (slen == blen) {                     /* not NULL terminated */
                CERROR ("can't unpack non-NULL terminated string in "
                        "msg %p buffer[%d] len %d\n", m, index, blen);
                return (NULL);
        }

        if (max_len == 0) {
                if (slen != blen - 1) {
                        CERROR ("can't unpack short string in msg %p "
                                "buffer[%d] len %d: strlen %d\n",
                                m, index, blen, slen);
                        return (NULL);
                }
        } else if (slen > max_len) {
                CERROR ("can't unpack oversized string in msg %p "
                        "buffer[%d] len %d strlen %d: max %d expected\n",
                        m, index, blen, slen, max_len);
                return (NULL);
        }

        return (str);
}

/* Wrap up the normal fixed length cases */
void *lustre_swab_buf(struct lustre_msg *msg, int index, int min_size,
                      void *swabber)
{
        void *ptr;

        ptr = lustre_msg_buf(msg, index, min_size);
        if (ptr == NULL)
                return NULL;

        if (swabber != NULL && lustre_msg_swabbed(msg))
                ((void (*)(void *))swabber)(ptr);

        return ptr;
}

void *lustre_swab_reqbuf(struct ptlrpc_request *req, int index, int min_size,
                         void *swabber)
{
        LASSERT_REQSWAB(req, index);
        return lustre_swab_buf(req->rq_reqmsg, index, min_size, swabber);
}

void *lustre_swab_repbuf(struct ptlrpc_request *req, int index, int min_size,
                         void *swabber)
{
        LASSERT_REPSWAB(req, index);
        return lustre_swab_buf(req->rq_repmsg, index, min_size, swabber);
}

/* byte flipping routines for all wire types declared in
 * lustre_idl.h implemented here.
 */

void lustre_swab_connect(struct obd_connect_data *ocd)
{
        __swab64s (&ocd->ocd_connect_flags);
        __swab32s (&ocd->ocd_version);
        __swab32s (&ocd->ocd_grant);
        __swab32s (&ocd->ocd_index);
        __swab32s (&ocd->ocd_unused);
        CLASSERT(offsetof(typeof(*ocd), padding1) != 0);
        CLASSERT(offsetof(typeof(*ocd), padding2) != 0);
        CLASSERT(offsetof(typeof(*ocd), padding3) != 0);
        CLASSERT(offsetof(typeof(*ocd), padding4) != 0);
        CLASSERT(offsetof(typeof(*ocd), padding5) != 0);
        CLASSERT(offsetof(typeof(*ocd), padding6) != 0);
}

void lustre_swab_obdo (struct obdo  *o)
{
        __swab64s (&o->o_valid);
        __swab64s (&o->o_id);
        __swab64s (&o->o_gr);
        __swab64s (&o->o_fid);
        __swab64s (&o->o_size);
        __swab64s (&o->o_mtime);
        __swab64s (&o->o_atime);
        __swab64s (&o->o_ctime);
        __swab64s (&o->o_blocks);
        __swab64s (&o->o_grant);
        __swab32s (&o->o_blksize);
        __swab32s (&o->o_mode);
        __swab32s (&o->o_uid);
        __swab32s (&o->o_gid);
        __swab32s (&o->o_flags);
        __swab32s (&o->o_nlink);
        __swab32s (&o->o_generation);
        __swab32s (&o->o_misc);
        __swab32s (&o->o_easize);
        __swab32s (&o->o_mds);
        CLASSERT(offsetof(typeof(*o), o_padding_1) != 0);
        CLASSERT(offsetof(typeof(*o), o_padding_2) != 0);
        /* o_inline is opaque */
}

void lustre_swab_obd_statfs (struct obd_statfs *os)
{
        __swab64s (&os->os_type);
        __swab64s (&os->os_blocks);
        __swab64s (&os->os_bfree);
        __swab64s (&os->os_bavail);
        __swab64s (&os->os_files);
        __swab64s (&os->os_ffree);
        /* no need to swab os_fsid */
        __swab32s (&os->os_bsize);
        __swab32s (&os->os_namelen);
        __swab64s (&os->os_maxbytes);
        CLASSERT(offsetof(typeof(*os), os_spare) != 0);
}

void lustre_swab_obd_ioobj (struct obd_ioobj *ioo)
{
        __swab64s (&ioo->ioo_id);
        __swab64s (&ioo->ioo_gr);
        __swab32s (&ioo->ioo_type);
        __swab32s (&ioo->ioo_bufcnt);
}

void lustre_swab_niobuf_remote (struct niobuf_remote *nbr)
{
        __swab64s (&nbr->offset);
        __swab32s (&nbr->len);
        __swab32s (&nbr->flags);
}

void lustre_swab_ost_body (struct ost_body *b)
{
        lustre_swab_obdo (&b->oa);
}

void lustre_swab_ost_last_id(obd_id *id)
{
        __swab64s(id);
}

void lustre_swab_ost_lvb(struct ost_lvb *lvb)
{
        __swab64s(&lvb->lvb_size);
        __swab64s(&lvb->lvb_mtime);
        __swab64s(&lvb->lvb_atime);
        __swab64s(&lvb->lvb_ctime);
        __swab64s(&lvb->lvb_blocks);
}

void lustre_swab_mds_status_req (struct mds_status_req *r)
{
        __swab32s (&r->flags);
        __swab32s (&r->repbuf);
}

void lustre_swab_mds_body (struct mds_body *b)
{
        lustre_swab_ll_fid (&b->fid1);
        lustre_swab_ll_fid (&b->fid2);
        /* handle is opaque */
        __swab64s (&b->valid);
        __swab64s (&b->size);
        __swab64s (&b->mtime);
        __swab64s (&b->atime);
        __swab64s (&b->ctime);
        __swab64s (&b->blocks);
        __swab64s (&b->io_epoch);
        __swab64s (&b->ino);
        __swab32s (&b->fsuid);
        __swab32s (&b->fsgid);
        __swab32s (&b->capability);
        __swab32s (&b->mode);
        __swab32s (&b->uid);
        __swab32s (&b->gid);
        __swab32s (&b->flags);
        __swab32s (&b->rdev);
        __swab32s (&b->nlink);
        __swab32s (&b->generation);
        __swab32s (&b->suppgid);
        __swab32s (&b->eadatasize);
        CLASSERT(offsetof(typeof(*b), padding_1) != 0);
        CLASSERT(offsetof(typeof(*b), padding_2) != 0);
        CLASSERT(offsetof(typeof(*b), padding_3) != 0);
        CLASSERT(offsetof(typeof(*b), padding_4) != 0);
}

void lustre_swab_mgmt_ost_info(struct mgmt_ost_info *oinfo)
{
        __swab64s(&oinfo->moi_nid);
        __swab32s(&oinfo->moi_stripe_index);
}

void lustre_swab_mgmt_mds_info(struct mgmt_mds_info *minfo)
{
        __swab64s(&minfo->mmi_nid);
        __swab32s(&minfo->mmi_index);
        __swab32s(&minfo->mmi_pattern);
        __swab64s(&minfo->mmi_stripe_size);
        __swab64s(&minfo->mmi_stripe_offset);
}

static void lustre_swab_obd_dqinfo (struct obd_dqinfo *i)
{
        __swab64s (&i->dqi_bgrace);
        __swab64s (&i->dqi_igrace);
        __swab32s (&i->dqi_flags);
        __swab32s (&i->dqi_valid);
}

static void lustre_swab_obd_dqblk (struct obd_dqblk *b)
{
        __swab64s (&b->dqb_ihardlimit);
        __swab64s (&b->dqb_isoftlimit);
        __swab64s (&b->dqb_curinodes);
        __swab64s (&b->dqb_bhardlimit);
        __swab64s (&b->dqb_bsoftlimit);
        __swab64s (&b->dqb_curspace);
        __swab64s (&b->dqb_btime);
        __swab64s (&b->dqb_itime);
        __swab32s (&b->dqb_valid);
        CLASSERT(offsetof(typeof(*b), padding) != 0);
}

void lustre_swab_obd_quotactl (struct obd_quotactl *q)
{
        __swab32s (&q->qc_cmd);
        __swab32s (&q->qc_type);
        __swab32s (&q->qc_id);
        __swab32s (&q->qc_stat);
        lustre_swab_obd_dqinfo (&q->qc_dqinfo);
        lustre_swab_obd_dqblk (&q->qc_dqblk);
}

void lustre_swab_mds_rec_setattr (struct mds_rec_setattr *sa)
{
        __swab32s (&sa->sa_opcode);
        __swab32s (&sa->sa_fsuid);
        __swab32s (&sa->sa_fsgid);
        __swab32s (&sa->sa_cap);
        __swab32s (&sa->sa_suppgid);
        __swab32s (&sa->sa_mode);
        lustre_swab_ll_fid (&sa->sa_fid);
        __swab64s (&sa->sa_valid);
        __swab64s (&sa->sa_size);
        __swab64s (&sa->sa_mtime);
        __swab64s (&sa->sa_atime);
        __swab64s (&sa->sa_ctime);
        __swab32s (&sa->sa_uid);
        __swab32s (&sa->sa_gid);
        __swab32s (&sa->sa_attr_flags);
        CLASSERT(offsetof(typeof(*sa), sa_padding) != 0);
}

void lustre_swab_mds_rec_create (struct mds_rec_create *cr)
{
        __swab32s (&cr->cr_opcode);
        __swab32s (&cr->cr_fsuid);
        __swab32s (&cr->cr_fsgid);
        __swab32s (&cr->cr_cap);
        __swab32s (&cr->cr_flags); /* for use with open */
        __swab32s (&cr->cr_mode);
        lustre_swab_ll_fid (&cr->cr_fid);
        lustre_swab_ll_fid (&cr->cr_replayfid);
        __swab64s (&cr->cr_time);
        __swab64s (&cr->cr_rdev);
        __swab32s (&cr->cr_suppgid);
        CLASSERT(offsetof(typeof(*cr), cr_padding_1) != 0);
        CLASSERT(offsetof(typeof(*cr), cr_padding_2) != 0);
        CLASSERT(offsetof(typeof(*cr), cr_padding_3) != 0);
        CLASSERT(offsetof(typeof(*cr), cr_padding_4) != 0);
        CLASSERT(offsetof(typeof(*cr), cr_padding_5) != 0);
}

void lustre_swab_mds_rec_link (struct mds_rec_link *lk)
{
        __swab32s (&lk->lk_opcode);
        __swab32s (&lk->lk_fsuid);
        __swab32s (&lk->lk_fsgid);
        __swab32s (&lk->lk_cap);
        __swab32s (&lk->lk_suppgid1);
        __swab32s (&lk->lk_suppgid2);
        lustre_swab_ll_fid (&lk->lk_fid1);
        lustre_swab_ll_fid (&lk->lk_fid2);
        __swab64s (&lk->lk_time);
        CLASSERT(offsetof(typeof(*lk), lk_padding_1) != 0);
        CLASSERT(offsetof(typeof(*lk), lk_padding_2) != 0);
        CLASSERT(offsetof(typeof(*lk), lk_padding_3) != 0);
        CLASSERT(offsetof(typeof(*lk), lk_padding_4) != 0);
}

void lustre_swab_mds_rec_unlink (struct mds_rec_unlink *ul)
{
        __swab32s (&ul->ul_opcode);
        __swab32s (&ul->ul_fsuid);
        __swab32s (&ul->ul_fsgid);
        __swab32s (&ul->ul_cap);
        __swab32s (&ul->ul_suppgid);
        __swab32s (&ul->ul_mode);
        lustre_swab_ll_fid (&ul->ul_fid1);
        lustre_swab_ll_fid (&ul->ul_fid2);
        __swab64s (&ul->ul_time);
        CLASSERT(offsetof(typeof(*ul), ul_padding_1) != 0);
        CLASSERT(offsetof(typeof(*ul), ul_padding_2) != 0);
        CLASSERT(offsetof(typeof(*ul), ul_padding_3) != 0);
        CLASSERT(offsetof(typeof(*ul), ul_padding_4) != 0);
}

void lustre_swab_mds_rec_rename (struct mds_rec_rename *rn)
{
        __swab32s (&rn->rn_opcode);
        __swab32s (&rn->rn_fsuid);
        __swab32s (&rn->rn_fsgid);
        __swab32s (&rn->rn_cap);
        __swab32s (&rn->rn_suppgid1);
        __swab32s (&rn->rn_suppgid2);
        lustre_swab_ll_fid (&rn->rn_fid1);
        lustre_swab_ll_fid (&rn->rn_fid2);
        __swab64s (&rn->rn_time);
        CLASSERT(offsetof(typeof(*rn), rn_padding_1) != 0);
        CLASSERT(offsetof(typeof(*rn), rn_padding_2) != 0);
        CLASSERT(offsetof(typeof(*rn), rn_padding_3) != 0);
        CLASSERT(offsetof(typeof(*rn), rn_padding_4) != 0);
}

void lustre_swab_lov_desc (struct lov_desc *ld)
{
        __swab32s (&ld->ld_tgt_count);
        __swab32s (&ld->ld_active_tgt_count);
        __swab32s (&ld->ld_default_stripe_count);
        __swab64s (&ld->ld_default_stripe_size);
        __swab64s (&ld->ld_default_stripe_offset);
        __swab32s (&ld->ld_pattern);
        /* uuid endian insensitive */
}

static void print_lum (struct lov_user_md *lum)
{
        CDEBUG(D_OTHER, "lov_user_md %p:\n", lum);
        CDEBUG(D_OTHER, "\tlmm_magic: %#x\n", lum->lmm_magic);
        CDEBUG(D_OTHER, "\tlmm_pattern: %#x\n", lum->lmm_pattern);
        CDEBUG(D_OTHER, "\tlmm_object_id: "LPU64"\n", lum->lmm_object_id);
        CDEBUG(D_OTHER, "\tlmm_object_gr: "LPU64"\n", lum->lmm_object_gr);
        CDEBUG(D_OTHER, "\tlmm_stripe_size: %#x\n", lum->lmm_stripe_size);
        CDEBUG(D_OTHER, "\tlmm_stripe_count: %#x\n", lum->lmm_stripe_count);
        CDEBUG(D_OTHER, "\tlmm_stripe_offset: %#x\n", lum->lmm_stripe_offset);
}

void lustre_swab_lov_user_md(struct lov_user_md *lum)
{
        ENTRY;
        CDEBUG(D_IOCTL, "swabbing lov_user_md\n");
        __swab32s(&lum->lmm_magic);
        __swab32s(&lum->lmm_pattern);
        __swab64s(&lum->lmm_object_id);
        __swab64s(&lum->lmm_object_gr);
        __swab32s(&lum->lmm_stripe_size);
        __swab16s(&lum->lmm_stripe_count);
        __swab16s(&lum->lmm_stripe_offset);
        print_lum(lum);
        EXIT;
}

static void print_lum_objs(struct lov_user_md *lum)
{
        struct lov_user_ost_data *lod;
        int i;
        ENTRY;
        if (!(libcfs_debug & D_OTHER)) /* don't loop on nothing */
                return;
        CDEBUG(D_OTHER, "lov_user_md_objects: %p\n", lum);
        for (i = 0; i < lum->lmm_stripe_count; i++) {
                lod = &lum->lmm_objects[i];
                CDEBUG(D_OTHER, "(%i) lod->l_object_id: "LPX64"\n", i, lod->l_object_id);
                CDEBUG(D_OTHER, "(%i) lod->l_object_gr: "LPX64"\n", i, lod->l_object_gr);
                CDEBUG(D_OTHER, "(%i) lod->l_ost_gen: %#x\n", i, lod->l_ost_gen);
                CDEBUG(D_OTHER, "(%i) lod->l_ost_idx: %#x\n", i, lod->l_ost_idx);
        }
        EXIT;
}

void lustre_swab_lov_user_md_objects(struct lov_user_md *lum)
{
        struct lov_user_ost_data *lod;
        int i;
        ENTRY;
        for (i = 0; i < lum->lmm_stripe_count; i++) {
                lod = &lum->lmm_objects[i];
                __swab64s(&lod->l_object_id);
                __swab64s(&lod->l_object_gr);
                __swab32s(&lod->l_ost_gen);
                __swab32s(&lod->l_ost_idx);
        }
        print_lum_objs(lum);
        EXIT;
}

void lustre_swab_ldlm_res_id (struct ldlm_res_id *id)
{
        int  i;

        for (i = 0; i < RES_NAME_SIZE; i++)
                __swab64s (&id->name[i]);
}

void lustre_swab_ldlm_policy_data (ldlm_policy_data_t *d)
{
        /* the lock data is a union and the first two fields are always an
         * extent so it's ok to process an LDLM_EXTENT and LDLM_FLOCK lock
         * data the same way. */
        __swab64s(&d->l_extent.start);
        __swab64s(&d->l_extent.end);
        __swab64s(&d->l_extent.gid);
        __swab32s(&d->l_flock.pid);
}

void lustre_swab_ldlm_intent (struct ldlm_intent *i)
{
        __swab64s (&i->opc);
}

void lustre_swab_ldlm_resource_desc (struct ldlm_resource_desc *r)
{
        __swab32s (&r->lr_type);
        CLASSERT(offsetof(typeof(*r), lr_padding) != 0);
        lustre_swab_ldlm_res_id (&r->lr_name);
}

void lustre_swab_ldlm_lock_desc (struct ldlm_lock_desc *l)
{
        lustre_swab_ldlm_resource_desc (&l->l_resource);
        __swab32s (&l->l_req_mode);
        __swab32s (&l->l_granted_mode);
        lustre_swab_ldlm_policy_data (&l->l_policy_data);
}

void lustre_swab_ldlm_request (struct ldlm_request *rq)
{
        __swab32s (&rq->lock_flags);
        CLASSERT(offsetof(typeof(*rq), lock_padding) != 0);
        lustre_swab_ldlm_lock_desc (&rq->lock_desc);
        /* lock_handle1 opaque */
        /* lock_handle2 opaque */
}

void lustre_swab_ldlm_reply (struct ldlm_reply *r)
{
        __swab32s (&r->lock_flags);
        CLASSERT(offsetof(typeof(*r), lock_padding) != 0);
        lustre_swab_ldlm_lock_desc (&r->lock_desc);
        /* lock_handle opaque */
        __swab64s (&r->lock_policy_res1);
        __swab64s (&r->lock_policy_res2);
}

/* no one calls this */
int llog_log_swabbed(struct llog_log_hdr *hdr)
{
        if (hdr->llh_hdr.lrh_type == __swab32(LLOG_HDR_MAGIC))
                return 1;
        if (hdr->llh_hdr.lrh_type == LLOG_HDR_MAGIC)
                return 0;
        return -1;
}

void lustre_swab_qdata(struct qunit_data *d)
{
        __swab32s (&d->qd_id);
        __swab32s (&d->qd_type);
        __swab32s (&d->qd_count);
        __swab32s (&d->qd_isblk);
}

void lustre_assert_wire_constants(void)
{
        /* Wire protocol assertions generated by 'wirecheck'
         * running on Linux schatzie.adilger.int 2.6.12-1.1378_FC3 #1 Wed Sep 14 04:24:31 EDT 2005 i6
         * with gcc version 3.3.4 20040817 (Red Hat Linux 3.3.4-2) */


        /* Constants... */
        LASSERTF(PTLRPC_MSG_MAGIC == 0x0BD00BD0," found %lld\n",
                 (long long)PTLRPC_MSG_MAGIC);
        LASSERTF(PTLRPC_MSG_VERSION == 0x00000003," found %lld\n",
                 (long long)PTLRPC_MSG_VERSION);
        LASSERTF(PTL_RPC_MSG_REQUEST == 4711, " found %lld\n",
                 (long long)PTL_RPC_MSG_REQUEST);
        LASSERTF(PTL_RPC_MSG_ERR == 4712, " found %lld\n",
                 (long long)PTL_RPC_MSG_ERR);
        LASSERTF(PTL_RPC_MSG_REPLY == 4713, " found %lld\n",
                 (long long)PTL_RPC_MSG_REPLY);
        LASSERTF(MSG_LAST_REPLAY == 1, " found %lld\n",
                 (long long)MSG_LAST_REPLAY);
        LASSERTF(MSG_RESENT == 2, " found %lld\n",
                 (long long)MSG_RESENT);
        LASSERTF(MSG_REPLAY == 4, " found %lld\n",
                 (long long)MSG_REPLAY);
        LASSERTF(MSG_CONNECT_RECOVERING == 1, " found %lld\n",
                 (long long)MSG_CONNECT_RECOVERING);
        LASSERTF(MSG_CONNECT_RECONNECT == 2, " found %lld\n",
                 (long long)MSG_CONNECT_RECONNECT);
        LASSERTF(MSG_CONNECT_REPLAYABLE == 4, " found %lld\n",
                 (long long)MSG_CONNECT_REPLAYABLE);
        LASSERTF(OST_REPLY == 0, " found %lld\n",
                 (long long)OST_REPLY);
        LASSERTF(OST_GETATTR == 1, " found %lld\n",
                 (long long)OST_GETATTR);
        LASSERTF(OST_SETATTR == 2, " found %lld\n",
                 (long long)OST_SETATTR);
        LASSERTF(OST_READ == 3, " found %lld\n",
                 (long long)OST_READ);
        LASSERTF(OST_WRITE == 4, " found %lld\n",
                 (long long)OST_WRITE);
        LASSERTF(OST_CREATE == 5, " found %lld\n",
                 (long long)OST_CREATE);
        LASSERTF(OST_DESTROY == 6, " found %lld\n",
                 (long long)OST_DESTROY);
        LASSERTF(OST_GET_INFO == 7, " found %lld\n",
                 (long long)OST_GET_INFO);
        LASSERTF(OST_CONNECT == 8, " found %lld\n",
                 (long long)OST_CONNECT);
        LASSERTF(OST_DISCONNECT == 9, " found %lld\n",
                 (long long)OST_DISCONNECT);
        LASSERTF(OST_PUNCH == 10, " found %lld\n",
                 (long long)OST_PUNCH);
        LASSERTF(OST_OPEN == 11, " found %lld\n",
                 (long long)OST_OPEN);
        LASSERTF(OST_CLOSE == 12, " found %lld\n",
                 (long long)OST_CLOSE);
        LASSERTF(OST_STATFS == 13, " found %lld\n",
                 (long long)OST_STATFS);
        LASSERTF(OST_SAN_READ == 14, " found %lld\n",
                 (long long)OST_SAN_READ);
        LASSERTF(OST_SAN_WRITE == 15, " found %lld\n",
                 (long long)OST_SAN_WRITE);
        LASSERTF(OST_SYNC == 16, " found %lld\n",
                 (long long)OST_SYNC);
        LASSERTF(OST_QUOTACHECK == 18, " found %lld\n",
                 (long long)OST_QUOTACHECK);
        LASSERTF(OST_QUOTACTL == 19, " found %lld\n",
                 (long long)OST_QUOTACTL);
        LASSERTF(OST_LAST_OPC == 20, " found %lld\n",
                 (long long)OST_LAST_OPC);
        LASSERTF(OBD_OBJECT_EOF == 0xffffffffffffffffULL," found %lld\n",
                 (long long)OBD_OBJECT_EOF);
        LASSERTF(MDS_GETATTR == 33, " found %lld\n",
                 (long long)MDS_GETATTR);
        LASSERTF(MDS_GETATTR_NAME == 34, " found %lld\n",
                 (long long)MDS_GETATTR_NAME);
        LASSERTF(MDS_CLOSE == 35, " found %lld\n",
                 (long long)MDS_CLOSE);
        LASSERTF(MDS_REINT == 36, " found %lld\n",
                 (long long)MDS_REINT);
        LASSERTF(MDS_READPAGE == 37, " found %lld\n",
                 (long long)MDS_READPAGE);
        LASSERTF(MDS_CONNECT == 38, " found %lld\n",
                 (long long)MDS_CONNECT);
        LASSERTF(MDS_DISCONNECT == 39, " found %lld\n",
                 (long long)MDS_DISCONNECT);
        LASSERTF(MDS_GETSTATUS == 40, " found %lld\n",
                 (long long)MDS_GETSTATUS);
        LASSERTF(MDS_STATFS == 41, " found %lld\n",
                 (long long)MDS_STATFS);
        LASSERTF(MDS_PIN == 42, " found %lld\n",
                 (long long)MDS_PIN);
        LASSERTF(MDS_UNPIN == 43, " found %lld\n",
                 (long long)MDS_UNPIN);
        LASSERTF(MDS_SYNC == 44, " found %lld\n",
                 (long long)MDS_SYNC);
        LASSERTF(MDS_DONE_WRITING == 45, " found %lld\n",
                 (long long)MDS_DONE_WRITING);
        LASSERTF(MDS_SET_INFO == 46, " found %lld\n",
                 (long long)MDS_SET_INFO);
        LASSERTF(MDS_QUOTACHECK == 47, " found %lld\n",
                 (long long)MDS_QUOTACHECK);
        LASSERTF(MDS_QUOTACTL == 48, " found %lld\n",
                 (long long)MDS_QUOTACTL);
        LASSERTF(MDS_LAST_OPC == 51, " found %lld\n",
                 (long long)MDS_LAST_OPC);
        LASSERTF(REINT_SETATTR == 1, " found %lld\n",
                 (long long)REINT_SETATTR);
        LASSERTF(REINT_CREATE == 2, " found %lld\n",
                 (long long)REINT_CREATE);
        LASSERTF(REINT_LINK == 3, " found %lld\n",
                 (long long)REINT_LINK);
        LASSERTF(REINT_UNLINK == 4, " found %lld\n",
                 (long long)REINT_UNLINK);
        LASSERTF(REINT_RENAME == 5, " found %lld\n",
                 (long long)REINT_RENAME);
        LASSERTF(REINT_OPEN == 6, " found %lld\n",
                 (long long)REINT_OPEN);
        LASSERTF(REINT_MAX == 7, " found %lld\n",
                 (long long)REINT_MAX);
        LASSERTF(DISP_IT_EXECD == 1, " found %lld\n",
                 (long long)DISP_IT_EXECD);
        LASSERTF(DISP_LOOKUP_EXECD == 2, " found %lld\n",
                 (long long)DISP_LOOKUP_EXECD);
        LASSERTF(DISP_LOOKUP_NEG == 4, " found %lld\n",
                 (long long)DISP_LOOKUP_NEG);
        LASSERTF(DISP_LOOKUP_POS == 8, " found %lld\n",
                 (long long)DISP_LOOKUP_POS);
        LASSERTF(DISP_OPEN_CREATE == 16, " found %lld\n",
                 (long long)DISP_OPEN_CREATE);
        LASSERTF(DISP_OPEN_OPEN == 32, " found %lld\n",
                 (long long)DISP_OPEN_OPEN);
        LASSERTF(MDS_STATUS_CONN == 1, " found %lld\n",
                 (long long)MDS_STATUS_CONN);
        LASSERTF(MDS_STATUS_LOV == 2, " found %lld\n",
                 (long long)MDS_STATUS_LOV);
        LASSERTF(MDS_OPEN_HAS_EA == 1073741824, " found %lld\n",
                 (long long)MDS_OPEN_HAS_EA);
        LASSERTF(LDLM_ENQUEUE == 101, " found %lld\n",
                 (long long)LDLM_ENQUEUE);
        LASSERTF(LDLM_CONVERT == 102, " found %lld\n",
                 (long long)LDLM_CONVERT);
        LASSERTF(LDLM_CANCEL == 103, " found %lld\n",
                 (long long)LDLM_CANCEL);
        LASSERTF(LDLM_BL_CALLBACK == 104, " found %lld\n",
                 (long long)LDLM_BL_CALLBACK);
        LASSERTF(LDLM_CP_CALLBACK == 105, " found %lld\n",
                 (long long)LDLM_CP_CALLBACK);
        LASSERTF(LDLM_GL_CALLBACK == 106, " found %lld\n",
                 (long long)LDLM_GL_CALLBACK);
        LASSERTF(LDLM_LAST_OPC == 107, " found %lld\n",
                 (long long)LDLM_LAST_OPC);
        LASSERTF(LCK_EX == 1, " found %lld\n",
                 (long long)LCK_EX);
        LASSERTF(LCK_PW == 2, " found %lld\n",
                 (long long)LCK_PW);
        LASSERTF(LCK_PR == 4, " found %lld\n",
                 (long long)LCK_PR);
        LASSERTF(LCK_CW == 8, " found %lld\n",
                 (long long)LCK_CW);
        LASSERTF(LCK_CR == 16, " found %lld\n",
                 (long long)LCK_CR);
        LASSERTF(LCK_NL == 32, " found %lld\n",
                 (long long)LCK_NL);
        LASSERTF(LCK_GROUP == 64, " found %lld\n",
                 (long long)LCK_GROUP);
        LASSERTF(LCK_MAXMODE == 65, " found %lld\n",
                 (long long)LCK_MAXMODE);
        LASSERTF(MGMT_CONNECT == 250, " found %lld\n",
                 (long long)MGMT_CONNECT);
        LASSERTF(MGMT_DISCONNECT == 251, " found %lld\n",
                 (long long)MGMT_DISCONNECT);
        LASSERTF(MGMT_EXCEPTION == 252, " found %lld\n",
                 (long long)MGMT_EXCEPTION);
        LASSERTF(MGMT_REGISTER == 253, " found %lld\n",
                 (long long)MGMT_REGISTER);
        LASSERTF(MGMT_OST_ADD == 254, " found %lld\n",
                 (long long)MGMT_OST_ADD);
        LASSERTF(MGMT_OST_DEL == 255, " found %lld\n",
                 (long long)MGMT_OST_DEL);
        LASSERTF(OBD_PING == 400, " found %lld\n",
                 (long long)OBD_PING);
        LASSERTF(OBD_LOG_CANCEL == 401, " found %lld\n",
                 (long long)OBD_LOG_CANCEL);
        LASSERTF(OBD_QC_CALLBACK == 402, " found %lld\n",
                 (long long)OBD_QC_CALLBACK);
        LASSERTF(OBD_LAST_OPC == 403, " found %lld\n",
                 (long long)OBD_LAST_OPC);
        LASSERTF(QUOTA_DQACQ == 601, " found %lld\n",
                 (long long)QUOTA_DQACQ);
        LASSERTF(QUOTA_DQREL == 602, " found %lld\n",
                 (long long)QUOTA_DQREL);
        LASSERTF(OBD_CONNECT_RDONLY == 1, " found %lld\n",
                 (long long)OBD_CONNECT_RDONLY);
        LASSERTF(OBD_CONNECT_INDEX == 2, " found %lld\n",
                 (long long)OBD_CONNECT_INDEX);
        LASSERTF(OBD_CONNECT_GRANT == 8, " found %lld\n",
                 (long long)OBD_CONNECT_GRANT);
        LASSERTF(OBD_CONNECT_SRVLOCK == 16, " found %lld\n",
                 (long long)OBD_CONNECT_SRVLOCK);
        LASSERTF(OBD_CONNECT_ACL == 128, " found %lld\n",
                 (long long)OBD_CONNECT_ACL);
        LASSERTF(OBD_CONNECT_XATTR == 256, " found %lld\n",
                 (long long)OBD_CONNECT_XATTR);
        LASSERTF(OBD_CONNECT_CROW == 512, " found %lld\n",
                 (long long)OBD_CONNECT_CROW);
        /* Sizes and Offsets */


        /* Checks for struct lustre_handle */
        LASSERTF((int)sizeof(struct lustre_handle) == 8, " found %lld\n",
                 (long long)(int)sizeof(struct lustre_handle));
        LASSERTF((int)offsetof(struct lustre_handle, cookie) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_handle, cookie));
        LASSERTF((int)sizeof(((struct lustre_handle *)0)->cookie) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_handle *)0)->cookie));

        /* Checks for struct lustre_msg */
        LASSERTF((int)sizeof(struct lustre_msg) == 64, " found %lld\n",
                 (long long)(int)sizeof(struct lustre_msg));
        LASSERTF((int)offsetof(struct lustre_msg, handle) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, handle));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->handle) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->handle));
        LASSERTF((int)offsetof(struct lustre_msg, magic) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, magic));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->magic) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->magic));
        LASSERTF((int)offsetof(struct lustre_msg, type) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, type));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->type));
        LASSERTF((int)offsetof(struct lustre_msg, version) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, version));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->version) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->version));
        LASSERTF((int)offsetof(struct lustre_msg, opc) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, opc));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->opc) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->opc));
        LASSERTF((int)offsetof(struct lustre_msg, last_xid) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, last_xid));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->last_xid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->last_xid));
        LASSERTF((int)offsetof(struct lustre_msg, last_committed) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, last_committed));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->last_committed) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->last_committed));
        LASSERTF((int)offsetof(struct lustre_msg, transno) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, transno));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->transno) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->transno));
        LASSERTF((int)offsetof(struct lustre_msg, status) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, status));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->status) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->status));
        LASSERTF((int)offsetof(struct lustre_msg, flags) == 52, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, flags));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->flags));
        LASSERTF((int)offsetof(struct lustre_msg, bufcount) == 60, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, bufcount));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->bufcount) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->bufcount));
        LASSERTF((int)offsetof(struct lustre_msg, buflens[7]) == 92, " found %lld\n",
                 (long long)(int)offsetof(struct lustre_msg, buflens[7]));
        LASSERTF((int)sizeof(((struct lustre_msg *)0)->buflens[7]) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lustre_msg *)0)->buflens[7]));

        /* Checks for struct obdo */
        LASSERTF((int)sizeof(struct obdo) == 208, " found %lld\n",
                 (long long)(int)sizeof(struct obdo));
        LASSERTF((int)offsetof(struct obdo, o_valid) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_valid));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_valid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_valid));
        LASSERTF((int)offsetof(struct obdo, o_id) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_id));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_id) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_id));
        LASSERTF((int)offsetof(struct obdo, o_gr) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_gr));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_gr) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_gr));
        LASSERTF((int)offsetof(struct obdo, o_fid) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_fid));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_fid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_fid));
        LASSERTF((int)offsetof(struct obdo, o_size) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_size));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_size) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_size));
        LASSERTF((int)offsetof(struct obdo, o_mtime) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_mtime));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_mtime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_mtime));
        LASSERTF((int)offsetof(struct obdo, o_atime) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_atime));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_atime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_atime));
        LASSERTF((int)offsetof(struct obdo, o_ctime) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_ctime));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_ctime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_ctime));
        LASSERTF((int)offsetof(struct obdo, o_blocks) == 64, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_blocks));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_blocks) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_blocks));
        LASSERTF((int)offsetof(struct obdo, o_grant) == 72, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_grant));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_grant) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_grant));
        LASSERTF((int)offsetof(struct obdo, o_blksize) == 80, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_blksize));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_blksize) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_blksize));
        LASSERTF((int)offsetof(struct obdo, o_mode) == 84, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_mode));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_mode));
        LASSERTF((int)offsetof(struct obdo, o_uid) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_uid));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_uid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_uid));
        LASSERTF((int)offsetof(struct obdo, o_gid) == 92, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_gid));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_gid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_gid));
        LASSERTF((int)offsetof(struct obdo, o_flags) == 96, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_flags));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_flags));
        LASSERTF((int)offsetof(struct obdo, o_nlink) == 100, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_nlink));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_nlink) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_nlink));
        LASSERTF((int)offsetof(struct obdo, o_generation) == 104, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_generation));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_generation) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_generation));
        LASSERTF((int)offsetof(struct obdo, o_misc) == 108, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_misc));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_misc) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_misc));
        LASSERTF((int)offsetof(struct obdo, o_easize) == 112, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_easize));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_easize) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_easize));
        LASSERTF((int)offsetof(struct obdo, o_mds) == 116, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_mds));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_mds) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_mds));
        LASSERTF((int)offsetof(struct obdo, o_padding_1) == 120, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_padding_1));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_padding_1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_padding_1));
        LASSERTF((int)offsetof(struct obdo, o_padding_2) == 124, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_padding_2));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_padding_2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_padding_2));
        LASSERTF((int)offsetof(struct obdo, o_inline) == 128, " found %lld\n",
                 (long long)(int)offsetof(struct obdo, o_inline));
        LASSERTF((int)sizeof(((struct obdo *)0)->o_inline) == 80, " found %lld\n",
                 (long long)(int)sizeof(((struct obdo *)0)->o_inline));
        LASSERTF(OBD_INLINESZ == 80, " found %lld\n",
                 (long long)OBD_INLINESZ);
        LASSERTF(OBD_MD_FLID == 1, " found %lld\n",
                 (long long)OBD_MD_FLID);
        LASSERTF(OBD_MD_FLATIME == 2, " found %lld\n",
                 (long long)OBD_MD_FLATIME);
        LASSERTF(OBD_MD_FLMTIME == 4, " found %lld\n",
                 (long long)OBD_MD_FLMTIME);
        LASSERTF(OBD_MD_FLCTIME == 8, " found %lld\n",
                 (long long)OBD_MD_FLCTIME);
        LASSERTF(OBD_MD_FLSIZE == 16, " found %lld\n",
                 (long long)OBD_MD_FLSIZE);
        LASSERTF(OBD_MD_FLBLOCKS == 32, " found %lld\n",
                 (long long)OBD_MD_FLBLOCKS);
        LASSERTF(OBD_MD_FLBLKSZ == 64, " found %lld\n",
                 (long long)OBD_MD_FLBLKSZ);
        LASSERTF(OBD_MD_FLMODE == 128, " found %lld\n",
                 (long long)OBD_MD_FLMODE);
        LASSERTF(OBD_MD_FLTYPE == 256, " found %lld\n",
                 (long long)OBD_MD_FLTYPE);
        LASSERTF(OBD_MD_FLUID == 512, " found %lld\n",
                 (long long)OBD_MD_FLUID);
        LASSERTF(OBD_MD_FLGID == 1024, " found %lld\n",
                 (long long)OBD_MD_FLGID);
        LASSERTF(OBD_MD_FLFLAGS == 2048, " found %lld\n",
                 (long long)OBD_MD_FLFLAGS);
        LASSERTF(OBD_MD_FLNLINK == 8192, " found %lld\n",
                 (long long)OBD_MD_FLNLINK);
        LASSERTF(OBD_MD_FLGENER == 16384, " found %lld\n",
                 (long long)OBD_MD_FLGENER);
        LASSERTF(OBD_MD_FLINLINE == 32768, " found %lld\n",
                 (long long)OBD_MD_FLINLINE);
        LASSERTF(OBD_MD_FLRDEV == 65536, " found %lld\n",
                 (long long)OBD_MD_FLRDEV);
        LASSERTF(OBD_MD_FLEASIZE == 131072, " found %lld\n",
                 (long long)OBD_MD_FLEASIZE);
        LASSERTF(OBD_MD_LINKNAME == 262144, " found %lld\n",
                 (long long)OBD_MD_LINKNAME);
        LASSERTF(OBD_MD_FLHANDLE == 524288, " found %lld\n",
                 (long long)OBD_MD_FLHANDLE);
        LASSERTF(OBD_MD_FLCKSUM == 1048576, " found %lld\n",
                 (long long)OBD_MD_FLCKSUM);
        LASSERTF(OBD_MD_FLQOS == 2097152, " found %lld\n",
                 (long long)OBD_MD_FLQOS);
        LASSERTF(OBD_MD_FLCOOKIE == 8388608, " found %lld\n",
                 (long long)OBD_MD_FLCOOKIE);
        LASSERTF(OBD_MD_FLGROUP == 16777216, " found %lld\n",
                 (long long)OBD_MD_FLGROUP);
        LASSERTF(OBD_MD_FLIFID == 33554432, " found %lld\n",
                 (long long)OBD_MD_FLIFID);
        LASSERTF(OBD_MD_FLEPOCH == 67108864, " found %lld\n",
                 (long long)OBD_MD_FLEPOCH);
        LASSERTF(OBD_MD_FLGRANT == 134217728, " found %lld\n",
                 (long long)OBD_MD_FLGRANT);
        LASSERTF(OBD_MD_FLDIREA == 268435456, " found %lld\n",
                 (long long)OBD_MD_FLDIREA);
        LASSERTF(OBD_MD_FLUSRQUOTA == 536870912, " found %lld\n",
                 (long long)OBD_MD_FLUSRQUOTA);
        LASSERTF(OBD_MD_FLGRPQUOTA == 1073741824, " found %lld\n",
                 (long long)OBD_MD_FLGRPQUOTA);
        LASSERTF(OBD_MD_MDS == 4294967296ULL, " found %lld\n",
                 (long long)OBD_MD_MDS);
        LASSERTF(OBD_MD_REINT == 8589934592ULL, " found %lld\n",
                 (long long)OBD_MD_REINT);
        LASSERTF(OBD_FL_INLINEDATA == 1, " found %lld\n",
                 (long long)OBD_FL_INLINEDATA);
        LASSERTF(OBD_FL_OBDMDEXISTS == 2, " found %lld\n",
                 (long long)OBD_FL_OBDMDEXISTS);
        LASSERTF(OBD_FL_DELORPHAN == 4, " found %lld\n",
                 (long long)OBD_FL_DELORPHAN);
        LASSERTF(OBD_FL_NORPC == 8, " found %lld\n",
                 (long long)OBD_FL_NORPC);
        LASSERTF(OBD_FL_IDONLY == 16, " found %lld\n",
                 (long long)OBD_FL_IDONLY);
        LASSERTF(OBD_FL_RECREATE_OBJS == 32, " found %lld\n",
                 (long long)OBD_FL_RECREATE_OBJS);
        LASSERTF(OBD_FL_DEBUG_CHECK == 64, " found %lld\n",
                 (long long)OBD_FL_DEBUG_CHECK);
        LASSERTF(OBD_FL_NO_USRQUOTA == 256, " found %lld\n",
                 (long long)OBD_FL_NO_USRQUOTA);
        LASSERTF(OBD_FL_NO_GRPQUOTA == 512, " found %lld\n",
                 (long long)OBD_FL_NO_GRPQUOTA);

        /* Checks for struct lov_mds_md_v1 */
        LASSERTF((int)sizeof(struct lov_mds_md_v1) == 32, " found %lld\n",
                 (long long)(int)sizeof(struct lov_mds_md_v1));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_magic) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_magic));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_magic) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_magic));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_pattern) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_pattern));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_pattern) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_pattern));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_object_id) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_object_id));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_object_id) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_object_id));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_object_gr) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_object_gr));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_object_gr) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_object_gr));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_stripe_size) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_stripe_size));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_stripe_size) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_stripe_size));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_stripe_count) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_stripe_count));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_stripe_count) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_stripe_count));
        LASSERTF((int)offsetof(struct lov_mds_md_v1, lmm_objects) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct lov_mds_md_v1, lmm_objects));
        LASSERTF((int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_objects) == 0, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_mds_md_v1 *)0)->lmm_objects));

        /* Checks for struct lov_ost_data_v1 */
        LASSERTF((int)sizeof(struct lov_ost_data_v1) == 24, " found %lld\n",
                 (long long)(int)sizeof(struct lov_ost_data_v1));
        LASSERTF((int)offsetof(struct lov_ost_data_v1, l_object_id) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct lov_ost_data_v1, l_object_id));
        LASSERTF((int)sizeof(((struct lov_ost_data_v1 *)0)->l_object_id) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_ost_data_v1 *)0)->l_object_id));
        LASSERTF((int)offsetof(struct lov_ost_data_v1, l_object_gr) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct lov_ost_data_v1, l_object_gr));
        LASSERTF((int)sizeof(((struct lov_ost_data_v1 *)0)->l_object_gr) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_ost_data_v1 *)0)->l_object_gr));
        LASSERTF((int)offsetof(struct lov_ost_data_v1, l_ost_gen) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct lov_ost_data_v1, l_ost_gen));
        LASSERTF((int)sizeof(((struct lov_ost_data_v1 *)0)->l_ost_gen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_ost_data_v1 *)0)->l_ost_gen));
        LASSERTF((int)offsetof(struct lov_ost_data_v1, l_ost_idx) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct lov_ost_data_v1, l_ost_idx));
        LASSERTF((int)sizeof(((struct lov_ost_data_v1 *)0)->l_ost_idx) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_ost_data_v1 *)0)->l_ost_idx));
        LASSERTF(LOV_MAGIC_V1 == 198249424, " found %lld\n",
                 (long long)LOV_MAGIC_V1);
        LASSERTF(LOV_PATTERN_RAID0 == 1, " found %lld\n",
                 (long long)LOV_PATTERN_RAID0);
        LASSERTF(LOV_PATTERN_RAID1 == 2, " found %lld\n",
                 (long long)LOV_PATTERN_RAID1);

        /* Checks for struct obd_statfs */
        LASSERTF((int)sizeof(struct obd_statfs) == 144, " found %lld\n",
                 (long long)(int)sizeof(struct obd_statfs));
        LASSERTF((int)offsetof(struct obd_statfs, os_type) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_type));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_type) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_type));
        LASSERTF((int)offsetof(struct obd_statfs, os_blocks) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_blocks));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_blocks) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_blocks));
        LASSERTF((int)offsetof(struct obd_statfs, os_bfree) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_bfree));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_bfree) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_bfree));
        LASSERTF((int)offsetof(struct obd_statfs, os_bavail) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_bavail));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_bavail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_bavail));
        LASSERTF((int)offsetof(struct obd_statfs, os_ffree) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_ffree));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_ffree) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_ffree));
        LASSERTF((int)offsetof(struct obd_statfs, os_fsid) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_fsid));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_fsid) == 40, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_fsid));
        LASSERTF((int)offsetof(struct obd_statfs, os_bsize) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_bsize));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_bsize) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_bsize));
        LASSERTF((int)offsetof(struct obd_statfs, os_namelen) == 92, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_namelen));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_namelen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_namelen));
        LASSERTF((int)offsetof(struct obd_statfs, os_spare) == 104, " found %lld\n",
                 (long long)(int)offsetof(struct obd_statfs, os_spare));
        LASSERTF((int)sizeof(((struct obd_statfs *)0)->os_spare) == 40, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_statfs *)0)->os_spare));

        /* Checks for struct obd_ioobj */
        LASSERTF((int)sizeof(struct obd_ioobj) == 24, " found %lld\n",
                 (long long)(int)sizeof(struct obd_ioobj));
        LASSERTF((int)offsetof(struct obd_ioobj, ioo_id) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct obd_ioobj, ioo_id));
        LASSERTF((int)sizeof(((struct obd_ioobj *)0)->ioo_id) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_ioobj *)0)->ioo_id));
        LASSERTF((int)offsetof(struct obd_ioobj, ioo_gr) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct obd_ioobj, ioo_gr));
        LASSERTF((int)sizeof(((struct obd_ioobj *)0)->ioo_gr) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_ioobj *)0)->ioo_gr));
        LASSERTF((int)offsetof(struct obd_ioobj, ioo_type) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct obd_ioobj, ioo_type));
        LASSERTF((int)sizeof(((struct obd_ioobj *)0)->ioo_type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_ioobj *)0)->ioo_type));
        LASSERTF((int)offsetof(struct obd_ioobj, ioo_bufcnt) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct obd_ioobj, ioo_bufcnt));
        LASSERTF((int)sizeof(((struct obd_ioobj *)0)->ioo_bufcnt) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_ioobj *)0)->ioo_bufcnt));

        /* Checks for struct obd_quotactl */
        LASSERTF((int)sizeof(struct obd_quotactl) == 112, " found %lld\n",
                 (long long)(int)sizeof(struct obd_quotactl));
        LASSERTF((int)offsetof(struct obd_quotactl, qc_cmd) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct obd_quotactl, qc_cmd));
        LASSERTF((int)sizeof(((struct obd_quotactl *)0)->qc_cmd) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_quotactl *)0)->qc_cmd));
        LASSERTF((int)offsetof(struct obd_quotactl, qc_type) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct obd_quotactl, qc_type));
        LASSERTF((int)sizeof(((struct obd_quotactl *)0)->qc_type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_quotactl *)0)->qc_type));
        LASSERTF((int)offsetof(struct obd_quotactl, qc_id) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct obd_quotactl, qc_id));
        LASSERTF((int)sizeof(((struct obd_quotactl *)0)->qc_id) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_quotactl *)0)->qc_id));
        LASSERTF((int)offsetof(struct obd_quotactl, qc_stat) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct obd_quotactl, qc_stat));
        LASSERTF((int)sizeof(((struct obd_quotactl *)0)->qc_stat) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_quotactl *)0)->qc_stat));
        LASSERTF((int)offsetof(struct obd_quotactl, qc_dqinfo) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct obd_quotactl, qc_dqinfo));
        LASSERTF((int)sizeof(((struct obd_quotactl *)0)->qc_dqinfo) == 24, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_quotactl *)0)->qc_dqinfo));
        LASSERTF((int)offsetof(struct obd_quotactl, qc_dqblk) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct obd_quotactl, qc_dqblk));
        LASSERTF((int)sizeof(((struct obd_quotactl *)0)->qc_dqblk) == 72, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_quotactl *)0)->qc_dqblk));

        /* Checks for struct obd_dqinfo */
        LASSERTF((int)sizeof(struct obd_dqinfo) == 24, " found %lld\n",
                 (long long)(int)sizeof(struct obd_dqinfo));
        LASSERTF((int)offsetof(struct obd_dqinfo, dqi_bgrace) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqinfo, dqi_bgrace));
        LASSERTF((int)sizeof(((struct obd_dqinfo *)0)->dqi_bgrace) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqinfo *)0)->dqi_bgrace));
        LASSERTF((int)offsetof(struct obd_dqinfo, dqi_igrace) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqinfo, dqi_igrace));
        LASSERTF((int)sizeof(((struct obd_dqinfo *)0)->dqi_igrace) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqinfo *)0)->dqi_igrace));
        LASSERTF((int)offsetof(struct obd_dqinfo, dqi_flags) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqinfo, dqi_flags));
        LASSERTF((int)sizeof(((struct obd_dqinfo *)0)->dqi_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqinfo *)0)->dqi_flags));
        LASSERTF((int)offsetof(struct obd_dqinfo, dqi_valid) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqinfo, dqi_valid));
        LASSERTF((int)sizeof(((struct obd_dqinfo *)0)->dqi_valid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqinfo *)0)->dqi_valid));

        /* Checks for struct obd_dqblk */
        LASSERTF((int)sizeof(struct obd_dqblk) == 72, " found %lld\n",
                 (long long)(int)sizeof(struct obd_dqblk));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_bhardlimit) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_bhardlimit));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_bhardlimit) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_bhardlimit));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_bsoftlimit) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_bsoftlimit));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_bsoftlimit) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_bsoftlimit));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_curspace) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_curspace));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_curspace) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_curspace));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_ihardlimit) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_ihardlimit));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_ihardlimit) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_ihardlimit));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_isoftlimit) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_isoftlimit));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_isoftlimit) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_isoftlimit));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_curinodes) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_curinodes));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_curinodes) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_curinodes));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_btime) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_btime));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_btime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_btime));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_itime) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_itime));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_itime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_itime));
        LASSERTF((int)offsetof(struct obd_dqblk, dqb_valid) == 64, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, dqb_valid));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->dqb_valid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->dqb_valid));
        LASSERTF((int)offsetof(struct obd_dqblk, padding) == 68, " found %lld\n",
                 (long long)(int)offsetof(struct obd_dqblk, padding));
        LASSERTF((int)sizeof(((struct obd_dqblk *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct obd_dqblk *)0)->padding));

        /* Checks for struct niobuf_remote */
        LASSERTF((int)sizeof(struct niobuf_remote) == 16, " found %lld\n",
                 (long long)(int)sizeof(struct niobuf_remote));
        LASSERTF((int)offsetof(struct niobuf_remote, offset) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct niobuf_remote, offset));
        LASSERTF((int)sizeof(((struct niobuf_remote *)0)->offset) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct niobuf_remote *)0)->offset));
        LASSERTF((int)offsetof(struct niobuf_remote, len) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct niobuf_remote, len));
        LASSERTF((int)sizeof(((struct niobuf_remote *)0)->len) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct niobuf_remote *)0)->len));
        LASSERTF((int)offsetof(struct niobuf_remote, flags) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct niobuf_remote, flags));
        LASSERTF((int)sizeof(((struct niobuf_remote *)0)->flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct niobuf_remote *)0)->flags));
        LASSERTF(OBD_BRW_READ == 1, " found %lld\n",
                 (long long)OBD_BRW_READ);
        LASSERTF(OBD_BRW_WRITE == 2, " found %lld\n",
                 (long long)OBD_BRW_WRITE);
        LASSERTF(OBD_BRW_SYNC == 8, " found %lld\n",
                 (long long)OBD_BRW_SYNC);
        LASSERTF(OBD_BRW_FROM_GRANT == 32, " found %lld\n",
                 (long long)OBD_BRW_FROM_GRANT);
        LASSERTF(OBD_BRW_NOQUOTA == 256, " found %lld\n",
                 (long long)OBD_BRW_NOQUOTA);

        /* Checks for struct ost_body */
        LASSERTF((int)sizeof(struct ost_body) == 208, " found %lld\n",
                 (long long)(int)sizeof(struct ost_body));
        LASSERTF((int)offsetof(struct ost_body, oa) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ost_body, oa));
        LASSERTF((int)sizeof(((struct ost_body *)0)->oa) == 208, " found %lld\n",
                 (long long)(int)sizeof(((struct ost_body *)0)->oa));

        /* Checks for struct ll_fid */
        LASSERTF((int)sizeof(struct ll_fid) == 16, " found %lld\n",
                 (long long)(int)sizeof(struct ll_fid));
        LASSERTF((int)offsetof(struct ll_fid, id) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ll_fid, id));
        LASSERTF((int)sizeof(((struct ll_fid *)0)->id) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ll_fid *)0)->id));
        LASSERTF((int)offsetof(struct ll_fid, generation) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ll_fid, generation));
        LASSERTF((int)sizeof(((struct ll_fid *)0)->generation) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ll_fid *)0)->generation));
        LASSERTF((int)offsetof(struct ll_fid, f_type) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct ll_fid, f_type));
        LASSERTF((int)sizeof(((struct ll_fid *)0)->f_type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ll_fid *)0)->f_type));

        /* Checks for struct mds_status_req */
        LASSERTF((int)sizeof(struct mds_status_req) == 8, " found %lld\n",
                 (long long)(int)sizeof(struct mds_status_req));
        LASSERTF((int)offsetof(struct mds_status_req, flags) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_status_req, flags));
        LASSERTF((int)sizeof(((struct mds_status_req *)0)->flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_status_req *)0)->flags));
        LASSERTF((int)offsetof(struct mds_status_req, repbuf) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct mds_status_req, repbuf));
        LASSERTF((int)sizeof(((struct mds_status_req *)0)->repbuf) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_status_req *)0)->repbuf));

        /* Checks for struct mds_body */
        LASSERTF((int)sizeof(struct mds_body) == 168, " found %lld\n",
                 (long long)(int)sizeof(struct mds_body));
        LASSERTF((int)offsetof(struct mds_body, fid1) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, fid1));
        LASSERTF((int)sizeof(((struct mds_body *)0)->fid1) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->fid1));
        LASSERTF((int)offsetof(struct mds_body, fid2) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, fid2));
        LASSERTF((int)sizeof(((struct mds_body *)0)->fid2) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->fid2));
        LASSERTF((int)offsetof(struct mds_body, handle) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, handle));
        LASSERTF((int)sizeof(((struct mds_body *)0)->handle) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->handle));
        LASSERTF((int)offsetof(struct mds_body, size) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, size));
        LASSERTF((int)sizeof(((struct mds_body *)0)->size) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->size));
        LASSERTF((int)offsetof(struct mds_body, blocks) == 80, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, blocks));
        LASSERTF((int)sizeof(((struct mds_body *)0)->blocks) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->blocks));
        LASSERTF((int)offsetof(struct mds_body, io_epoch) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, io_epoch));
        LASSERTF((int)sizeof(((struct mds_body *)0)->io_epoch) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->io_epoch));
        LASSERTF((int)offsetof(struct mds_body, ino) == 96, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, ino));
        LASSERTF((int)sizeof(((struct mds_body *)0)->ino) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->ino));
        LASSERTF((int)offsetof(struct mds_body, valid) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, valid));
        LASSERTF((int)sizeof(((struct mds_body *)0)->valid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->valid));
        LASSERTF((int)offsetof(struct mds_body, fsuid) == 104, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, fsuid));
        LASSERTF((int)sizeof(((struct mds_body *)0)->fsuid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->fsuid));
        LASSERTF((int)offsetof(struct mds_body, fsgid) == 108, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, fsgid));
        LASSERTF((int)sizeof(((struct mds_body *)0)->fsgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->fsgid));
        LASSERTF((int)offsetof(struct mds_body, capability) == 112, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, capability));
        LASSERTF((int)sizeof(((struct mds_body *)0)->capability) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->capability));
        LASSERTF((int)offsetof(struct mds_body, mode) == 116, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, mode));
        LASSERTF((int)sizeof(((struct mds_body *)0)->mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->mode));
        LASSERTF((int)offsetof(struct mds_body, uid) == 120, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, uid));
        LASSERTF((int)sizeof(((struct mds_body *)0)->uid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->uid));
        LASSERTF((int)offsetof(struct mds_body, gid) == 124, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, gid));
        LASSERTF((int)sizeof(((struct mds_body *)0)->gid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->gid));
        LASSERTF((int)offsetof(struct mds_body, mtime) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, mtime));
        LASSERTF((int)sizeof(((struct mds_body *)0)->mtime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->mtime));
        LASSERTF((int)offsetof(struct mds_body, ctime) == 72, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, ctime));
        LASSERTF((int)sizeof(((struct mds_body *)0)->ctime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->ctime));
        LASSERTF((int)offsetof(struct mds_body, atime) == 64, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, atime));
        LASSERTF((int)sizeof(((struct mds_body *)0)->atime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->atime));
        LASSERTF((int)offsetof(struct mds_body, flags) == 128, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, flags));
        LASSERTF((int)sizeof(((struct mds_body *)0)->flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->flags));
        LASSERTF((int)offsetof(struct mds_body, rdev) == 132, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, rdev));
        LASSERTF((int)sizeof(((struct mds_body *)0)->rdev) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->rdev));
        LASSERTF((int)offsetof(struct mds_body, nlink) == 136, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, nlink));
        LASSERTF((int)sizeof(((struct mds_body *)0)->nlink) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->nlink));
        LASSERTF((int)offsetof(struct mds_body, generation) == 140, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, generation));
        LASSERTF((int)sizeof(((struct mds_body *)0)->generation) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->generation));
        LASSERTF((int)offsetof(struct mds_body, suppgid) == 144, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, suppgid));
        LASSERTF((int)sizeof(((struct mds_body *)0)->suppgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->suppgid));
        LASSERTF((int)offsetof(struct mds_body, eadatasize) == 148, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, eadatasize));
        LASSERTF((int)sizeof(((struct mds_body *)0)->eadatasize) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->eadatasize));
        LASSERTF((int)offsetof(struct mds_body, padding_1) == 152, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, padding_1));
        LASSERTF((int)sizeof(((struct mds_body *)0)->padding_1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->padding_1));
        LASSERTF((int)offsetof(struct mds_body, padding_2) == 156, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, padding_2));
        LASSERTF((int)sizeof(((struct mds_body *)0)->padding_2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->padding_2));
        LASSERTF((int)offsetof(struct mds_body, padding_3) == 160, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, padding_3));
        LASSERTF((int)sizeof(((struct mds_body *)0)->padding_3) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->padding_3));
        LASSERTF((int)offsetof(struct mds_body, padding_4) == 164, " found %lld\n",
                 (long long)(int)offsetof(struct mds_body, padding_4));
        LASSERTF((int)sizeof(((struct mds_body *)0)->padding_4) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_body *)0)->padding_4));
        LASSERTF(FMODE_READ == 1, " found %lld\n",
                 (long long)FMODE_READ);
        LASSERTF(FMODE_WRITE == 2, " found %lld\n",
                 (long long)FMODE_WRITE);
        LASSERTF(FMODE_EXEC == 4, " found %lld\n",
                 (long long)FMODE_EXEC);
        LASSERTF(MDS_OPEN_CREAT == 64, " found %lld\n",
                 (long long)MDS_OPEN_CREAT);
        LASSERTF(MDS_OPEN_EXCL == 128, " found %lld\n",
                 (long long)MDS_OPEN_EXCL);
        LASSERTF(MDS_OPEN_TRUNC == 512, " found %lld\n",
                 (long long)MDS_OPEN_TRUNC);
        LASSERTF(MDS_OPEN_APPEND == 1024, " found %lld\n",
                 (long long)MDS_OPEN_APPEND);
        LASSERTF(MDS_OPEN_SYNC == 4096, " found %lld\n",
                 (long long)MDS_OPEN_SYNC);
        LASSERTF(MDS_OPEN_DIRECTORY == 65536, " found %lld\n",
                 (long long)MDS_OPEN_DIRECTORY);
        LASSERTF(MDS_OPEN_DELAY_CREATE == 16777216, " found %lld\n",
                 (long long)MDS_OPEN_DELAY_CREATE);
        LASSERTF(MDS_OPEN_HAS_EA == 1073741824, " found %lld\n",
                 (long long)MDS_OPEN_HAS_EA);

        /* Checks for struct mds_rec_setattr */
        LASSERTF((int)sizeof(struct mds_rec_setattr) == 96, " found %lld\n",
                 (long long)(int)sizeof(struct mds_rec_setattr));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_opcode) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_opcode));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_opcode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_opcode));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_fsuid) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_fsuid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_fsuid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_fsuid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_fsgid) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_fsgid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_fsgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_fsgid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_cap) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_cap));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_cap) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_cap));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_suppgid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_suppgid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_suppgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_suppgid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_mode) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_mode));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_mode));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_fid) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_fid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_fid) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_fid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_valid) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_valid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_valid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_valid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_size) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_size));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_size) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_size));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_mtime) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_mtime));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_mtime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_mtime));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_atime) == 64, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_atime));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_atime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_atime));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_ctime) == 72, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_ctime));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_ctime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_ctime));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_uid) == 80, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_uid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_uid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_uid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_gid) == 84, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_gid));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_gid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_gid));
        LASSERTF((int)offsetof(struct mds_rec_setattr, sa_attr_flags) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_setattr, sa_attr_flags));
        LASSERTF((int)sizeof(((struct mds_rec_setattr *)0)->sa_attr_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_setattr *)0)->sa_attr_flags));

        /* Checks for struct mds_rec_create */
        LASSERTF((int)sizeof(struct mds_rec_create) == 96, " found %lld\n",
                 (long long)(int)sizeof(struct mds_rec_create));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_opcode) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_opcode));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_opcode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_opcode));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_fsuid) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_fsuid));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_fsuid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_fsuid));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_fsgid) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_fsgid));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_fsgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_fsgid));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_cap) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_cap));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_cap) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_cap));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_flags) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_flags));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_flags));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_mode) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_mode));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_mode));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_fid) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_fid));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_fid) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_fid));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_replayfid) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_replayfid));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_replayfid) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_replayfid));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_time) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_time));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_time) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_time));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_rdev) == 64, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_rdev));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_rdev) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_rdev));
        LASSERTF((int)offsetof(struct mds_rec_create, cr_suppgid) == 72, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_create, cr_suppgid));
        LASSERTF((int)sizeof(((struct mds_rec_create *)0)->cr_suppgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_create *)0)->cr_suppgid));

        /* Checks for struct mds_rec_link */
        LASSERTF((int)sizeof(struct mds_rec_link) == 80, " found %lld\n",
                 (long long)(int)sizeof(struct mds_rec_link));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_opcode) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_opcode));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_opcode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_opcode));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_fsuid) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_fsuid));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_fsuid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_fsuid));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_fsgid) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_fsgid));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_fsgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_fsgid));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_cap) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_cap));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_cap) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_cap));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_suppgid1) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_suppgid1));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_suppgid1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_suppgid1));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_suppgid2) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_suppgid2));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_suppgid2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_suppgid2));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_fid1) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_fid1));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_fid1) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_fid1));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_fid2) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_fid2));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_fid2) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_fid2));
        LASSERTF((int)offsetof(struct mds_rec_link, lk_time) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_link, lk_time));
        LASSERTF((int)sizeof(((struct mds_rec_link *)0)->lk_time) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_link *)0)->lk_time));

        /* Checks for struct mds_rec_unlink */
        LASSERTF((int)sizeof(struct mds_rec_unlink) == 80, " found %lld\n",
                 (long long)(int)sizeof(struct mds_rec_unlink));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_opcode) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_opcode));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_opcode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_opcode));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_fsuid) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_fsuid));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_fsuid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_fsuid));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_fsgid) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_fsgid));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_fsgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_fsgid));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_cap) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_cap));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_cap) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_cap));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_suppgid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_suppgid));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_suppgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_suppgid));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_mode) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_mode));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_mode));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_fid1) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_fid1));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_fid1) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_fid1));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_fid2) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_fid2));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_fid2) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_fid2));
        LASSERTF((int)offsetof(struct mds_rec_unlink, ul_time) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_unlink, ul_time));
        LASSERTF((int)sizeof(((struct mds_rec_unlink *)0)->ul_time) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_unlink *)0)->ul_time));

        /* Checks for struct mds_rec_rename */
        LASSERTF((int)sizeof(struct mds_rec_rename) == 80, " found %lld\n",
                 (long long)(int)sizeof(struct mds_rec_rename));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_opcode) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_opcode));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_opcode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_opcode));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_fsuid) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_fsuid));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_fsuid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_fsuid));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_fsgid) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_fsgid));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_fsgid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_fsgid));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_cap) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_cap));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_cap) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_cap));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_suppgid1) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_suppgid1));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_suppgid1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_suppgid1));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_suppgid2) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_suppgid2));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_suppgid2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_suppgid2));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_fid1) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_fid1));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_fid1) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_fid1));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_fid2) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_fid2));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_fid2) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_fid2));
        LASSERTF((int)offsetof(struct mds_rec_rename, rn_time) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct mds_rec_rename, rn_time));
        LASSERTF((int)sizeof(((struct mds_rec_rename *)0)->rn_time) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct mds_rec_rename *)0)->rn_time));

        /* Checks for struct lov_desc */
        LASSERTF((int)sizeof(struct lov_desc) == 88, " found %lld\n",
                 (long long)(int)sizeof(struct lov_desc));
        LASSERTF((int)offsetof(struct lov_desc, ld_tgt_count) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_tgt_count));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_tgt_count) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_tgt_count));
        LASSERTF((int)offsetof(struct lov_desc, ld_active_tgt_count) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_active_tgt_count));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_active_tgt_count) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_active_tgt_count));
        LASSERTF((int)offsetof(struct lov_desc, ld_default_stripe_count) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_default_stripe_count));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_default_stripe_count) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_default_stripe_count));
        LASSERTF((int)offsetof(struct lov_desc, ld_pattern) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_pattern));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_pattern) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_pattern));
        LASSERTF((int)offsetof(struct lov_desc, ld_default_stripe_size) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_default_stripe_size));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_default_stripe_size) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_default_stripe_size));
        LASSERTF((int)offsetof(struct lov_desc, ld_default_stripe_offset) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_default_stripe_offset));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_default_stripe_offset) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_default_stripe_offset));
        LASSERTF((int)offsetof(struct lov_desc, ld_default_stripe_offset) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_default_stripe_offset));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_default_stripe_offset) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_default_stripe_offset));
        LASSERTF((int)offsetof(struct lov_desc, ld_padding_1) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_padding_1));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_padding_1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_padding_1));
        LASSERTF((int)offsetof(struct lov_desc, ld_padding_2) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_padding_2));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_padding_2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_padding_2));
        LASSERTF((int)offsetof(struct lov_desc, ld_padding_3) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_padding_3));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_padding_3) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_padding_3));
        LASSERTF((int)offsetof(struct lov_desc, ld_padding_4) == 44, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_padding_4));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_padding_4) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_padding_4));
        LASSERTF((int)offsetof(struct lov_desc, ld_uuid) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct lov_desc, ld_uuid));
        LASSERTF((int)sizeof(((struct lov_desc *)0)->ld_uuid) == 40, " found %lld\n",
                 (long long)(int)sizeof(((struct lov_desc *)0)->ld_uuid));

        /* Checks for struct ldlm_res_id */
        LASSERTF((int)sizeof(struct ldlm_res_id) == 32, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_res_id));
        LASSERTF((int)offsetof(struct ldlm_res_id, name[4]) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_res_id, name[4]));
        LASSERTF((int)sizeof(((struct ldlm_res_id *)0)->name[4]) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_res_id *)0)->name[4]));

        /* Checks for struct ldlm_extent */
        LASSERTF((int)sizeof(struct ldlm_extent) == 24, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_extent));
        LASSERTF((int)offsetof(struct ldlm_extent, start) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_extent, start));
        LASSERTF((int)sizeof(((struct ldlm_extent *)0)->start) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_extent *)0)->start));
        LASSERTF((int)offsetof(struct ldlm_extent, end) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_extent, end));
        LASSERTF((int)sizeof(((struct ldlm_extent *)0)->end) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_extent *)0)->end));
        LASSERTF((int)offsetof(struct ldlm_extent, gid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_extent, gid));
        LASSERTF((int)sizeof(((struct ldlm_extent *)0)->gid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_extent *)0)->gid));

        /* Checks for struct ldlm_flock */
        LASSERTF((int)sizeof(struct ldlm_flock) == 32, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_flock));
        LASSERTF((int)offsetof(struct ldlm_flock, start) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_flock, start));
        LASSERTF((int)sizeof(((struct ldlm_flock *)0)->start) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_flock *)0)->start));
        LASSERTF((int)offsetof(struct ldlm_flock, end) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_flock, end));
        LASSERTF((int)sizeof(((struct ldlm_flock *)0)->end) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_flock *)0)->end));
        LASSERTF((int)offsetof(struct ldlm_flock, blocking_pid) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_flock, blocking_pid));
        LASSERTF((int)sizeof(((struct ldlm_flock *)0)->blocking_pid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_flock *)0)->blocking_pid));
        LASSERTF((int)offsetof(struct ldlm_flock, pid) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_flock, pid));
        LASSERTF((int)sizeof(((struct ldlm_flock *)0)->pid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_flock *)0)->pid));

        /* Checks for struct ldlm_intent */
        LASSERTF((int)sizeof(struct ldlm_intent) == 8, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_intent));
        LASSERTF((int)offsetof(struct ldlm_intent, opc) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_intent, opc));
        LASSERTF((int)sizeof(((struct ldlm_intent *)0)->opc) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_intent *)0)->opc));

        /* Checks for struct ldlm_resource_desc */
        LASSERTF((int)sizeof(struct ldlm_resource_desc) == 40, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_resource_desc));
        LASSERTF((int)offsetof(struct ldlm_resource_desc, lr_type) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_resource_desc, lr_type));
        LASSERTF((int)sizeof(((struct ldlm_resource_desc *)0)->lr_type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_resource_desc *)0)->lr_type));
        LASSERTF((int)offsetof(struct ldlm_resource_desc, lr_padding) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_resource_desc, lr_padding));
        LASSERTF((int)sizeof(((struct ldlm_resource_desc *)0)->lr_padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_resource_desc *)0)->lr_padding));
        LASSERTF((int)offsetof(struct ldlm_resource_desc, lr_name) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_resource_desc, lr_name));
        LASSERTF((int)sizeof(((struct ldlm_resource_desc *)0)->lr_name) == 32, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_resource_desc *)0)->lr_name));

        /* Checks for struct ldlm_lock_desc */
        LASSERTF((int)sizeof(struct ldlm_lock_desc) == 80, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_lock_desc));
        LASSERTF((int)offsetof(struct ldlm_lock_desc, l_resource) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_lock_desc, l_resource));
        LASSERTF((int)sizeof(((struct ldlm_lock_desc *)0)->l_resource) == 40, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_lock_desc *)0)->l_resource));
        LASSERTF((int)offsetof(struct ldlm_lock_desc, l_req_mode) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_lock_desc, l_req_mode));
        LASSERTF((int)sizeof(((struct ldlm_lock_desc *)0)->l_req_mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_lock_desc *)0)->l_req_mode));
        LASSERTF((int)offsetof(struct ldlm_lock_desc, l_granted_mode) == 44, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_lock_desc, l_granted_mode));
        LASSERTF((int)sizeof(((struct ldlm_lock_desc *)0)->l_granted_mode) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_lock_desc *)0)->l_granted_mode));
        LASSERTF((int)offsetof(struct ldlm_lock_desc, l_policy_data) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_lock_desc, l_policy_data));
        LASSERTF((int)sizeof(((struct ldlm_lock_desc *)0)->l_policy_data) == 32, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_lock_desc *)0)->l_policy_data));

        /* Checks for struct ldlm_request */
        LASSERTF((int)sizeof(struct ldlm_request) == 104, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_request));
        LASSERTF((int)offsetof(struct ldlm_request, lock_flags) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_flags));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_flags));
        LASSERTF((int)offsetof(struct ldlm_request, lock_padding) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_padding));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_padding));
        LASSERTF((int)offsetof(struct ldlm_request, lock_desc) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_desc));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_desc) == 80, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_desc));
        LASSERTF((int)offsetof(struct ldlm_request, lock_handle1) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_handle1));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_handle1) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_handle1));
        LASSERTF((int)offsetof(struct ldlm_request, lock_handle2) == 96, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_handle2));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_handle2) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_handle2));

        /* Checks for struct ldlm_reply */
        LASSERTF((int)sizeof(struct ldlm_reply) == 112, " found %lld\n",
                 (long long)(int)sizeof(struct ldlm_reply));
        LASSERTF((int)offsetof(struct ldlm_reply, lock_flags) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_reply, lock_flags));
        LASSERTF((int)sizeof(((struct ldlm_reply *)0)->lock_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_reply *)0)->lock_flags));
        LASSERTF((int)offsetof(struct ldlm_request, lock_padding) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_padding));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_padding));
        LASSERTF((int)offsetof(struct ldlm_request, lock_desc) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_request, lock_desc));
        LASSERTF((int)sizeof(((struct ldlm_request *)0)->lock_desc) == 80, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_request *)0)->lock_desc));
        LASSERTF((int)offsetof(struct ldlm_reply, lock_handle) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_reply, lock_handle));
        LASSERTF((int)sizeof(((struct ldlm_reply *)0)->lock_handle) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_reply *)0)->lock_handle));
        LASSERTF((int)offsetof(struct ldlm_reply, lock_policy_res1) == 96, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_reply, lock_policy_res1));
        LASSERTF((int)sizeof(((struct ldlm_reply *)0)->lock_policy_res1) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_reply *)0)->lock_policy_res1));
        LASSERTF((int)offsetof(struct ldlm_reply, lock_policy_res2) == 104, " found %lld\n",
                 (long long)(int)offsetof(struct ldlm_reply, lock_policy_res2));
        LASSERTF((int)sizeof(((struct ldlm_reply *)0)->lock_policy_res2) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ldlm_reply *)0)->lock_policy_res2));

        /* Checks for struct ost_lvb */
        LASSERTF((int)sizeof(struct ost_lvb) == 40, " found %lld\n",
                 (long long)(int)sizeof(struct ost_lvb));
        LASSERTF((int)offsetof(struct ost_lvb, lvb_size) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct ost_lvb, lvb_size));
        LASSERTF((int)sizeof(((struct ost_lvb *)0)->lvb_size) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ost_lvb *)0)->lvb_size));
        LASSERTF((int)offsetof(struct ost_lvb, lvb_mtime) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct ost_lvb, lvb_mtime));
        LASSERTF((int)sizeof(((struct ost_lvb *)0)->lvb_mtime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ost_lvb *)0)->lvb_mtime));
        LASSERTF((int)offsetof(struct ost_lvb, lvb_atime) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct ost_lvb, lvb_atime));
        LASSERTF((int)sizeof(((struct ost_lvb *)0)->lvb_atime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ost_lvb *)0)->lvb_atime));
        LASSERTF((int)offsetof(struct ost_lvb, lvb_ctime) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct ost_lvb, lvb_ctime));
        LASSERTF((int)sizeof(((struct ost_lvb *)0)->lvb_ctime) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ost_lvb *)0)->lvb_ctime));
        LASSERTF((int)offsetof(struct ost_lvb, lvb_blocks) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct ost_lvb, lvb_blocks));
        LASSERTF((int)sizeof(((struct ost_lvb *)0)->lvb_blocks) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct ost_lvb *)0)->lvb_blocks));

        /* Checks for struct llog_logid */
        LASSERTF((int)sizeof(struct llog_logid) == 20, " found %lld\n",
                 (long long)(int)sizeof(struct llog_logid));
        LASSERTF((int)offsetof(struct llog_logid, lgl_oid) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid, lgl_oid));
        LASSERTF((int)sizeof(((struct llog_logid *)0)->lgl_oid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid *)0)->lgl_oid));
        LASSERTF((int)offsetof(struct llog_logid, lgl_ogr) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid, lgl_ogr));
        LASSERTF((int)sizeof(((struct llog_logid *)0)->lgl_ogr) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid *)0)->lgl_ogr));
        LASSERTF((int)offsetof(struct llog_logid, lgl_ogen) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid, lgl_ogen));
        LASSERTF((int)sizeof(((struct llog_logid *)0)->lgl_ogen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid *)0)->lgl_ogen));
        LASSERTF(OST_SZ_REC == 274730752, " found %lld\n",
                 (long long)OST_SZ_REC);
        LASSERTF(OST_RAID1_REC == 274731008, " found %lld\n",
                 (long long)OST_RAID1_REC);
        LASSERTF(MDS_UNLINK_REC == 274801668, " found %lld\n",
                 (long long)MDS_UNLINK_REC);
        LASSERTF(MDS_SETATTR_REC == 274801665, " found %lld\n",
                 (long long)MDS_SETATTR_REC);
        LASSERTF(OBD_CFG_REC == 274857984, " found %lld\n",
                 (long long)OBD_CFG_REC);
        LASSERTF(PTL_CFG_REC == 274923520, " found %lld\n",
                 (long long)PTL_CFG_REC);
        LASSERTF(LLOG_GEN_REC == 274989056, " found %lld\n",
                 (long long)LLOG_GEN_REC);
        LASSERTF(LLOG_HDR_MAGIC == 275010873, " found %lld\n",
                 (long long)LLOG_HDR_MAGIC);
        LASSERTF(LLOG_LOGID_MAGIC == 275010875, " found %lld\n",
                 (long long)LLOG_LOGID_MAGIC);

        /* Checks for struct llog_catid */
        LASSERTF((int)sizeof(struct llog_catid) == 32, " found %lld\n",
                 (long long)(int)sizeof(struct llog_catid));
        LASSERTF((int)offsetof(struct llog_catid, lci_logid) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_catid, lci_logid));
        LASSERTF((int)sizeof(((struct llog_catid *)0)->lci_logid) == 20, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_catid *)0)->lci_logid));
        LASSERTF((int)offsetof(struct llog_catid, lci_padding1) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct llog_catid, lci_padding1));
        LASSERTF((int)sizeof(((struct llog_catid *)0)->lci_padding1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_catid *)0)->lci_padding1));
        LASSERTF((int)offsetof(struct llog_catid, lci_padding2) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llog_catid, lci_padding2));
        LASSERTF((int)sizeof(((struct llog_catid *)0)->lci_padding2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_catid *)0)->lci_padding2));
        LASSERTF((int)offsetof(struct llog_catid, lci_padding3) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llog_catid, lci_padding3));
        LASSERTF((int)sizeof(((struct llog_catid *)0)->lci_padding3) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_catid *)0)->lci_padding3));

        /* Checks for struct llog_rec_hdr */
        LASSERTF((int)sizeof(struct llog_rec_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(struct llog_rec_hdr));
        LASSERTF((int)offsetof(struct llog_rec_hdr, lrh_len) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_rec_hdr, lrh_len));
        LASSERTF((int)sizeof(((struct llog_rec_hdr *)0)->lrh_len) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_rec_hdr *)0)->lrh_len));
        LASSERTF((int)offsetof(struct llog_rec_hdr, lrh_index) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct llog_rec_hdr, lrh_index));
        LASSERTF((int)sizeof(((struct llog_rec_hdr *)0)->lrh_index) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_rec_hdr *)0)->lrh_index));
        LASSERTF((int)offsetof(struct llog_rec_hdr, lrh_type) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct llog_rec_hdr, lrh_type));
        LASSERTF((int)sizeof(((struct llog_rec_hdr *)0)->lrh_type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_rec_hdr *)0)->lrh_type));
        LASSERTF((int)offsetof(struct llog_rec_hdr, padding) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct llog_rec_hdr, padding));
        LASSERTF((int)sizeof(((struct llog_rec_hdr *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_rec_hdr *)0)->padding));

        /* Checks for struct llog_rec_tail */
        LASSERTF((int)sizeof(struct llog_rec_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(struct llog_rec_tail));
        LASSERTF((int)offsetof(struct llog_rec_tail, lrt_len) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_rec_tail, lrt_len));
        LASSERTF((int)sizeof(((struct llog_rec_tail *)0)->lrt_len) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_rec_tail *)0)->lrt_len));
        LASSERTF((int)offsetof(struct llog_rec_tail, lrt_index) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct llog_rec_tail, lrt_index));
        LASSERTF((int)sizeof(((struct llog_rec_tail *)0)->lrt_index) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_rec_tail *)0)->lrt_index));

        /* Checks for struct llog_logid_rec */
        LASSERTF((int)sizeof(struct llog_logid_rec) == 64, " found %lld\n",
                 (long long)(int)sizeof(struct llog_logid_rec));
        LASSERTF((int)offsetof(struct llog_logid_rec, lid_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, lid_hdr));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->lid_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->lid_hdr));
        LASSERTF((int)offsetof(struct llog_logid_rec, lid_id) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, lid_id));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->lid_id) == 20, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->lid_id));
        LASSERTF((int)offsetof(struct llog_logid_rec, padding1) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, padding1));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->padding1) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->padding1));
        LASSERTF((int)offsetof(struct llog_logid_rec, padding2) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, padding2));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->padding2) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->padding2));
        LASSERTF((int)offsetof(struct llog_logid_rec, padding3) == 44, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, padding3));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->padding3) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->padding3));
        LASSERTF((int)offsetof(struct llog_logid_rec, padding4) == 48, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, padding4));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->padding4) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->padding4));
        LASSERTF((int)offsetof(struct llog_logid_rec, padding5) == 52, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, padding5));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->padding5) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->padding5));
        LASSERTF((int)offsetof(struct llog_logid_rec, lid_tail) == 56, " found %lld\n",
                 (long long)(int)offsetof(struct llog_logid_rec, lid_tail));
        LASSERTF((int)sizeof(((struct llog_logid_rec *)0)->lid_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_logid_rec *)0)->lid_tail));

        /* Checks for struct llog_create_rec */
        LASSERTF((int)sizeof(struct llog_create_rec) == 56, " found %lld\n",
                 (long long)(int)sizeof(struct llog_create_rec));
        LASSERTF((int)offsetof(struct llog_create_rec, lcr_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_create_rec, lcr_hdr));
        LASSERTF((int)sizeof(((struct llog_create_rec *)0)->lcr_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_create_rec *)0)->lcr_hdr));
        LASSERTF((int)offsetof(struct llog_create_rec, lcr_fid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_create_rec, lcr_fid));
        LASSERTF((int)sizeof(((struct llog_create_rec *)0)->lcr_fid) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_create_rec *)0)->lcr_fid));
        LASSERTF((int)offsetof(struct llog_create_rec, lcr_oid) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_create_rec, lcr_oid));
        LASSERTF((int)sizeof(((struct llog_create_rec *)0)->lcr_oid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_create_rec *)0)->lcr_oid));
        LASSERTF((int)offsetof(struct llog_create_rec, lcr_ogen) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct llog_create_rec, lcr_ogen));
        LASSERTF((int)sizeof(((struct llog_create_rec *)0)->lcr_ogen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_create_rec *)0)->lcr_ogen));
        LASSERTF((int)offsetof(struct llog_create_rec, padding) == 44, " found %lld\n",
                 (long long)(int)offsetof(struct llog_create_rec, padding));
        LASSERTF((int)sizeof(((struct llog_create_rec *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_create_rec *)0)->padding));

        /* Checks for struct llog_orphan_rec */
        LASSERTF((int)sizeof(struct llog_orphan_rec) == 40, " found %lld\n",
                 (long long)(int)sizeof(struct llog_orphan_rec));
        LASSERTF((int)offsetof(struct llog_orphan_rec, lor_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_orphan_rec, lor_hdr));
        LASSERTF((int)sizeof(((struct llog_orphan_rec *)0)->lor_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_orphan_rec *)0)->lor_hdr));
        LASSERTF((int)offsetof(struct llog_orphan_rec, lor_oid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_orphan_rec, lor_oid));
        LASSERTF((int)sizeof(((struct llog_orphan_rec *)0)->lor_oid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_orphan_rec *)0)->lor_oid));
        LASSERTF((int)offsetof(struct llog_orphan_rec, lor_ogen) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llog_orphan_rec, lor_ogen));
        LASSERTF((int)sizeof(((struct llog_orphan_rec *)0)->lor_ogen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_orphan_rec *)0)->lor_ogen));
        LASSERTF((int)offsetof(struct llog_orphan_rec, padding) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llog_orphan_rec, padding));
        LASSERTF((int)sizeof(((struct llog_orphan_rec *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_orphan_rec *)0)->padding));
        LASSERTF((int)offsetof(struct llog_orphan_rec, lor_tail) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_orphan_rec, lor_tail));
        LASSERTF((int)sizeof(((struct llog_orphan_rec *)0)->lor_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_orphan_rec *)0)->lor_tail));

        /* Checks for struct llog_unlink_rec */
        LASSERTF((int)sizeof(struct llog_unlink_rec) == 40, " found %lld\n",
                 (long long)(int)sizeof(struct llog_unlink_rec));
        LASSERTF((int)offsetof(struct llog_unlink_rec, lur_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_unlink_rec, lur_hdr));
        LASSERTF((int)sizeof(((struct llog_unlink_rec *)0)->lur_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_unlink_rec *)0)->lur_hdr));
        LASSERTF((int)offsetof(struct llog_unlink_rec, lur_oid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_unlink_rec, lur_oid));
        LASSERTF((int)sizeof(((struct llog_unlink_rec *)0)->lur_oid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_unlink_rec *)0)->lur_oid));
        LASSERTF((int)offsetof(struct llog_unlink_rec, lur_ogen) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llog_unlink_rec, lur_ogen));
        LASSERTF((int)sizeof(((struct llog_unlink_rec *)0)->lur_ogen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_unlink_rec *)0)->lur_ogen));
        LASSERTF((int)offsetof(struct llog_unlink_rec, padding) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llog_unlink_rec, padding));
        LASSERTF((int)sizeof(((struct llog_unlink_rec *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_unlink_rec *)0)->padding));
        LASSERTF((int)offsetof(struct llog_unlink_rec, lur_tail) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_unlink_rec, lur_tail));
        LASSERTF((int)sizeof(((struct llog_unlink_rec *)0)->lur_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_unlink_rec *)0)->lur_tail));

        /* Checks for struct llog_setattr_rec */
        LASSERTF((int)sizeof(struct llog_setattr_rec) == 48, " found %lld\n",
                 (long long)(int)sizeof(struct llog_setattr_rec));
        LASSERTF((int)offsetof(struct llog_setattr_rec, lsr_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, lsr_hdr));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->lsr_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->lsr_hdr));
        LASSERTF((int)offsetof(struct llog_setattr_rec, lsr_oid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, lsr_oid));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->lsr_oid) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->lsr_oid));
        LASSERTF((int)offsetof(struct llog_setattr_rec, lsr_ogen) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, lsr_ogen));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->lsr_ogen) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->lsr_ogen));
        LASSERTF((int)offsetof(struct llog_setattr_rec, lsr_uid) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, lsr_uid));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->lsr_uid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->lsr_uid));
        LASSERTF((int)offsetof(struct llog_setattr_rec, lsr_gid) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, lsr_gid));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->lsr_gid) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->lsr_gid));
        LASSERTF((int)offsetof(struct llog_setattr_rec, padding) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, padding));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->padding));
        LASSERTF((int)offsetof(struct llog_setattr_rec, lsr_tail) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct llog_setattr_rec, lsr_tail));
        LASSERTF((int)sizeof(((struct llog_setattr_rec *)0)->lsr_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_setattr_rec *)0)->lsr_tail));

        /* Checks for struct llog_size_change_rec */
        LASSERTF((int)sizeof(struct llog_size_change_rec) == 48, " found %lld\n",
                 (long long)(int)sizeof(struct llog_size_change_rec));
        LASSERTF((int)offsetof(struct llog_size_change_rec, lsc_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_size_change_rec, lsc_hdr));
        LASSERTF((int)sizeof(((struct llog_size_change_rec *)0)->lsc_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_size_change_rec *)0)->lsc_hdr));
        LASSERTF((int)offsetof(struct llog_size_change_rec, lsc_fid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_size_change_rec, lsc_fid));
        LASSERTF((int)sizeof(((struct llog_size_change_rec *)0)->lsc_fid) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_size_change_rec *)0)->lsc_fid));
        LASSERTF((int)offsetof(struct llog_size_change_rec, lsc_io_epoch) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_size_change_rec, lsc_io_epoch));
        LASSERTF((int)sizeof(((struct llog_size_change_rec *)0)->lsc_io_epoch) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_size_change_rec *)0)->lsc_io_epoch));
        LASSERTF((int)offsetof(struct llog_size_change_rec, padding) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct llog_size_change_rec, padding));
        LASSERTF((int)sizeof(((struct llog_size_change_rec *)0)->padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_size_change_rec *)0)->padding));
        LASSERTF((int)offsetof(struct llog_size_change_rec, lsc_tail) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct llog_size_change_rec, lsc_tail));
        LASSERTF((int)sizeof(((struct llog_size_change_rec *)0)->lsc_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_size_change_rec *)0)->lsc_tail));

        /* Checks for struct llog_gen */
        LASSERTF((int)sizeof(struct llog_gen) == 16, " found %lld\n",
                 (long long)(int)sizeof(struct llog_gen));
        LASSERTF((int)offsetof(struct llog_gen, mnt_cnt) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_gen, mnt_cnt));
        LASSERTF((int)sizeof(((struct llog_gen *)0)->mnt_cnt) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_gen *)0)->mnt_cnt));
        LASSERTF((int)offsetof(struct llog_gen, conn_cnt) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct llog_gen, conn_cnt));
        LASSERTF((int)sizeof(((struct llog_gen *)0)->conn_cnt) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_gen *)0)->conn_cnt));

        /* Checks for struct llog_gen_rec */
        LASSERTF((int)sizeof(struct llog_gen_rec) == 40, " found %lld\n",
                 (long long)(int)sizeof(struct llog_gen_rec));
        LASSERTF((int)offsetof(struct llog_gen_rec, lgr_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_gen_rec, lgr_hdr));
        LASSERTF((int)sizeof(((struct llog_gen_rec *)0)->lgr_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_gen_rec *)0)->lgr_hdr));
        LASSERTF((int)offsetof(struct llog_gen_rec, lgr_gen) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_gen_rec, lgr_gen));
        LASSERTF((int)sizeof(((struct llog_gen_rec *)0)->lgr_gen) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_gen_rec *)0)->lgr_gen));
        LASSERTF((int)offsetof(struct llog_gen_rec, lgr_tail) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_gen_rec, lgr_tail));
        LASSERTF((int)sizeof(((struct llog_gen_rec *)0)->lgr_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_gen_rec *)0)->lgr_tail));

        /* Checks for struct llog_log_hdr */
        LASSERTF((int)sizeof(struct llog_log_hdr) == 8192, " found %lld\n",
                 (long long)(int)sizeof(struct llog_log_hdr));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_hdr) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_hdr));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_hdr) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_hdr));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_timestamp) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_timestamp));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_timestamp) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_timestamp));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_count) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_count));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_count) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_count));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_bitmap_offset) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_bitmap_offset));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_bitmap_offset) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_bitmap_offset));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_size) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_size));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_size) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_size));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_flags) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_flags));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_flags));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_cat_idx) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_cat_idx));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_cat_idx) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_cat_idx));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_tgtuuid) == 44, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_tgtuuid));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_tgtuuid) == 40, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_tgtuuid));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_reserved) == 84, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_reserved));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_reserved) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_reserved));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_bitmap) == 88, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_bitmap));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_bitmap) == 8096, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_bitmap));
        LASSERTF((int)offsetof(struct llog_log_hdr, llh_tail) == 8184, " found %lld\n",
                 (long long)(int)offsetof(struct llog_log_hdr, llh_tail));
        LASSERTF((int)sizeof(((struct llog_log_hdr *)0)->llh_tail) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_log_hdr *)0)->llh_tail));

        /* Checks for struct llog_cookie */
        LASSERTF((int)sizeof(struct llog_cookie) == 32, " found %lld\n",
                 (long long)(int)sizeof(struct llog_cookie));
        LASSERTF((int)offsetof(struct llog_cookie, lgc_lgl) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llog_cookie, lgc_lgl));
        LASSERTF((int)sizeof(((struct llog_cookie *)0)->lgc_lgl) == 20, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_cookie *)0)->lgc_lgl));
        LASSERTF((int)offsetof(struct llog_cookie, lgc_subsys) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct llog_cookie, lgc_subsys));
        LASSERTF((int)sizeof(((struct llog_cookie *)0)->lgc_subsys) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_cookie *)0)->lgc_subsys));
        LASSERTF((int)offsetof(struct llog_cookie, lgc_index) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llog_cookie, lgc_index));
        LASSERTF((int)sizeof(((struct llog_cookie *)0)->lgc_index) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_cookie *)0)->lgc_index));
        LASSERTF((int)offsetof(struct llog_cookie, lgc_padding) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llog_cookie, lgc_padding));
        LASSERTF((int)sizeof(((struct llog_cookie *)0)->lgc_padding) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llog_cookie *)0)->lgc_padding));

        /* Checks for struct llogd_body */
        LASSERTF((int)sizeof(struct llogd_body) == 48, " found %lld\n",
                 (long long)(int)sizeof(struct llogd_body));
        LASSERTF((int)offsetof(struct llogd_body, lgd_logid) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_logid));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_logid) == 20, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_logid));
        LASSERTF((int)offsetof(struct llogd_body, lgd_ctxt_idx) == 20, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_ctxt_idx));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_ctxt_idx) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_ctxt_idx));
        LASSERTF((int)offsetof(struct llogd_body, lgd_llh_flags) == 24, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_llh_flags));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_llh_flags) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_llh_flags));
        LASSERTF((int)offsetof(struct llogd_body, lgd_index) == 28, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_index));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_index) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_index));
        LASSERTF((int)offsetof(struct llogd_body, lgd_saved_index) == 32, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_saved_index));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_saved_index) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_saved_index));
        LASSERTF((int)offsetof(struct llogd_body, lgd_len) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_len));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_len) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_len));
        LASSERTF((int)offsetof(struct llogd_body, lgd_cur_offset) == 40, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_body, lgd_cur_offset));
        LASSERTF((int)sizeof(((struct llogd_body *)0)->lgd_cur_offset) == 8, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_body *)0)->lgd_cur_offset));
        LASSERTF(LLOG_ORIGIN_HANDLE_CREATE == 501, " found %lld\n",
                 (long long)LLOG_ORIGIN_HANDLE_CREATE);
        LASSERTF(LLOG_ORIGIN_HANDLE_NEXT_BLOCK == 502, " found %lld\n",
                 (long long)LLOG_ORIGIN_HANDLE_NEXT_BLOCK);
        LASSERTF(LLOG_ORIGIN_HANDLE_READ_HEADER == 503, " found %lld\n",
                 (long long)LLOG_ORIGIN_HANDLE_READ_HEADER);
        LASSERTF(LLOG_ORIGIN_HANDLE_WRITE_REC == 504, " found %lld\n",
                 (long long)LLOG_ORIGIN_HANDLE_WRITE_REC);
        LASSERTF(LLOG_ORIGIN_HANDLE_CLOSE == 505, " found %lld\n",
                 (long long)LLOG_ORIGIN_HANDLE_CLOSE);
        LASSERTF(LLOG_ORIGIN_CONNECT == 506, " found %lld\n",
                 (long long)LLOG_ORIGIN_CONNECT);
        LASSERTF(LLOG_CATINFO == 507, " found %lld\n",
                 (long long)LLOG_CATINFO);

        /* Checks for struct llogd_conn_body */
        LASSERTF((int)sizeof(struct llogd_conn_body) == 40, " found %lld\n",
                 (long long)(int)sizeof(struct llogd_conn_body));
        LASSERTF((int)offsetof(struct llogd_conn_body, lgdc_gen) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_conn_body, lgdc_gen));
        LASSERTF((int)sizeof(((struct llogd_conn_body *)0)->lgdc_gen) == 16, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_conn_body *)0)->lgdc_gen));
        LASSERTF((int)offsetof(struct llogd_conn_body, lgdc_logid) == 16, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_conn_body, lgdc_logid));
        LASSERTF((int)sizeof(((struct llogd_conn_body *)0)->lgdc_logid) == 20, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_conn_body *)0)->lgdc_logid));
        LASSERTF((int)offsetof(struct llogd_conn_body, lgdc_ctxt_idx) == 36, " found %lld\n",
                 (long long)(int)offsetof(struct llogd_conn_body, lgdc_ctxt_idx));
        LASSERTF((int)sizeof(((struct llogd_conn_body *)0)->lgdc_ctxt_idx) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct llogd_conn_body *)0)->lgdc_ctxt_idx));

        /* Checks for struct qunit_data */
        LASSERTF((int)sizeof(struct qunit_data) == 16, " found %lld\n",
                 (long long)(int)sizeof(struct qunit_data));
        LASSERTF((int)offsetof(struct qunit_data, qd_id) == 0, " found %lld\n",
                 (long long)(int)offsetof(struct qunit_data, qd_id));
        LASSERTF((int)sizeof(((struct qunit_data *)0)->qd_id) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct qunit_data *)0)->qd_id));
        LASSERTF((int)offsetof(struct qunit_data, qd_type) == 4, " found %lld\n",
                 (long long)(int)offsetof(struct qunit_data, qd_type));
        LASSERTF((int)sizeof(((struct qunit_data *)0)->qd_type) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct qunit_data *)0)->qd_type));
        LASSERTF((int)offsetof(struct qunit_data, qd_count) == 8, " found %lld\n",
                 (long long)(int)offsetof(struct qunit_data, qd_count));
        LASSERTF((int)sizeof(((struct qunit_data *)0)->qd_count) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct qunit_data *)0)->qd_count));
        LASSERTF((int)offsetof(struct qunit_data, qd_isblk) == 12, " found %lld\n",
                 (long long)(int)offsetof(struct qunit_data, qd_isblk));
        LASSERTF((int)sizeof(((struct qunit_data *)0)->qd_isblk) == 4, " found %lld\n",
                 (long long)(int)sizeof(((struct qunit_data *)0)->qd_isblk));
}

