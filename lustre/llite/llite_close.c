/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Lite routines to issue a secondary close after writeback
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
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

#include <linux/module.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <lustre_mdc.h>
#include <lustre_lite.h>
#include "llite_internal.h"

/* record that a write is in flight */
void llap_write_pending(struct inode *inode, struct ll_async_page *llap)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        
        ENTRY;
        spin_lock(&lli->lli_lock);
        lli->lli_flags |= LLIF_SOM_DIRTY;
        if (llap && list_empty(&llap->llap_pending_write))
                list_add(&llap->llap_pending_write, 
                         &lli->lli_pending_write_llaps);
        spin_unlock(&lli->lli_lock);
        EXIT;
}

/* record that a write has completed */
int llap_write_complete(struct inode *inode, struct ll_async_page *llap)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        int rc = 0;
        
        ENTRY;
        spin_lock(&lli->lli_lock);
        if (llap && !list_empty(&llap->llap_pending_write)) {
                list_del_init(&llap->llap_pending_write);
                rc = 1;
        }
        spin_unlock(&lli->lli_lock);
        RETURN(rc);
}

/* Queue DONE_WRITING if 
 * - done writing is allowed;
 * - inode has no no dirty pages; */
void ll_queue_done_writing(struct inode *inode, unsigned long flags)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        
        spin_lock(&lli->lli_lock);
        lli->lli_flags |= flags;
        
        if ((lli->lli_flags & LLIF_DONE_WRITING) &&
            list_empty(&lli->lli_pending_write_llaps)) {
                struct ll_close_queue *lcq = ll_i2sbi(inode)->ll_lcq;

                /* DONE_WRITING is allowed and inode has no dirty page. */
                spin_lock(&lcq->lcq_lock);
                LASSERT(list_empty(&lli->lli_close_list));
                CDEBUG(D_INODE, "adding inode %lu/%u to close list\n",
                       inode->i_ino, inode->i_generation);
                
                list_add_tail(&lli->lli_close_list, &lcq->lcq_head);
                wake_up(&lcq->lcq_waitq);
                spin_unlock(&lcq->lcq_lock);
        }
        spin_unlock(&lli->lli_lock);
}

/* Close epoch and send Size-on-MDS attribute update if possible. 
 * Call this under @lli->lli_lock spinlock. */
void ll_epoch_close(struct inode *inode, struct md_op_data *op_data,
                    struct obd_client_handle **och, unsigned long flags)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        spin_lock(&lli->lli_lock);
        if (!(list_empty(&lli->lli_pending_write_llaps)) && 
            !(lli->lli_flags & LLIF_EPOCH_PENDING)) {
                LASSERT(*och != NULL);
                LASSERT(lli->lli_pending_och == NULL);
                /* Inode is dirty and there is no pending write done request
                 * yet, DONE_WRITE is to be sent later. */
                lli->lli_flags |= LLIF_EPOCH_PENDING;
                lli->lli_pending_och = *och;
                spin_unlock(&lli->lli_lock);
                
                inode = igrab(inode);
                LASSERT(inode);
                goto out;
        }

        CDEBUG(D_INODE, "Epoch "LPU64" closed on "DFID"\n",
               op_data->ioepoch, PFID(&lli->lli_fid));
        op_data->flags |= MF_EPOCH_CLOSE;

        if (flags & LLIF_DONE_WRITING) {
                LASSERT(lli->lli_flags & LLIF_SOM_DIRTY);
                *och = lli->lli_pending_och;
                lli->lli_pending_och = NULL;
                lli->lli_flags &= ~(LLIF_DONE_WRITING | LLIF_EPOCH_PENDING | 
                                    LLIF_EPOCH_PENDING);
        } else {
                /* Pack Size-on-MDS inode attributes only if they has changed */
                if (!(lli->lli_flags & LLIF_SOM_DIRTY)) {
                        spin_unlock(&lli->lli_lock);
                        goto out;
                }

                /* There is already 1 pending DONE_WRITE, do not create another
                 * one -- close epoch with no attribute change. */
                if (lli->lli_flags & LLIF_EPOCH_PENDING) {
                        spin_unlock(&lli->lli_lock);
                        goto out;
                }
        }
        
        spin_unlock(&lli->lli_lock);
        op_data->flags |= MF_SOM_CHANGE;

        /* Check if Size-on-MDS attributes are valid. */
        LASSERT(!(lli->lli_flags & LLIF_MDS_SIZE_LOCK));
        if (!ll_local_size(inode)) {
                /* Send Size-on-MDS Attributes if valid. */
                op_data->attr.ia_valid |= ATTR_MTIME_SET | ATTR_CTIME_SET |
                                          ATTR_SIZE | ATTR_BLOCKS;
        }
out:
        EXIT;
}

int ll_sizeonmds_update(struct inode *inode, struct lustre_handle *fh)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct md_op_data *op_data;
        struct obdo *oa;
        int rc;
        ENTRY;
        
        LASSERT(!(lli->lli_flags & LLIF_MDS_SIZE_LOCK));
        
        oa = obdo_alloc();
        OBD_ALLOC_PTR(op_data);
        if (!oa || !op_data) {
                CERROR("can't allocate memory for Size-on-MDS update.\n");
                RETURN(-ENOMEM);
        }
        rc = ll_inode_getattr(inode, oa);
        if (rc) {
                CERROR("inode_getattr failed (%d): unable to send a "
                       "Size-on-MDS attribute update for inode %lu/%u\n",
                       rc, inode->i_ino, inode->i_generation);
                GOTO(out, rc);
        }
        CDEBUG(D_INODE, "Size-on-MDS update on "DFID"\n", PFID(&lli->lli_fid));
        
        md_from_obdo(op_data, oa, oa->o_valid);
        memcpy(&op_data->handle, fh, sizeof(*fh));
        
        op_data->ioepoch = lli->lli_ioepoch;
        op_data->flags |= MF_SOM_CHANGE;
        
        rc = ll_md_setattr(inode, op_data);
        EXIT;
out:
        if (oa)
                obdo_free(oa);
        if (op_data)
                ll_finish_md_op_data(op_data);
        return rc;
}

/* Send a DONE_WRITING rpc, pack Size-on-MDS attributes into it, if possible */
static void ll_done_writing(struct inode *inode)
{
        struct obd_client_handle *och = NULL;
        struct md_op_data *op_data;
        int rc;
        ENTRY;

        OBD_ALLOC_PTR(op_data);
        if (op_data == NULL) {
                CERROR("can't allocate op_data\n");
                EXIT;
                return;
        }

        ll_epoch_close(inode, op_data, &och, LLIF_DONE_WRITING);
        ll_pack_inode2opdata(inode, op_data, &och->och_fh);

        rc = md_done_writing(ll_i2sbi(inode)->ll_md_exp, op_data, och);
        ll_finish_md_op_data(op_data);
        if (rc == -EAGAIN) {
                /* MDS has instructed us to obtain Size-on-MDS attribute from 
                 * OSTs and send setattr to back to MDS. */
                rc = ll_sizeonmds_update(inode, &och->och_fh);
        } else if (rc) {
                CERROR("inode %lu mdc done_writing failed: rc = %d\n",
                       inode->i_ino, rc);
        }
        OBD_FREE_PTR(och);
        EXIT;
}

static struct ll_inode_info *ll_close_next_lli(struct ll_close_queue *lcq)
{
        struct ll_inode_info *lli = NULL;

        spin_lock(&lcq->lcq_lock);

        if (!list_empty(&lcq->lcq_head)) {
                lli = list_entry(lcq->lcq_head.next, struct ll_inode_info,
                                 lli_close_list);
                list_del_init(&lli->lli_close_list);
        } else if (atomic_read(&lcq->lcq_stop))
                lli = ERR_PTR(-EALREADY);

        spin_unlock(&lcq->lcq_lock);
        return lli;
}

static int ll_close_thread(void *arg)
{
        struct ll_close_queue *lcq = arg;
        ENTRY;

        {
                char name[CFS_CURPROC_COMM_MAX];
                snprintf(name, sizeof(name) - 1, "ll_close");
                cfs_daemonize(name);
        }
        
        complete(&lcq->lcq_comp);

        while (1) {
                struct l_wait_info lwi = { 0 };
                struct ll_inode_info *lli;
                struct inode *inode;

                l_wait_event_exclusive(lcq->lcq_waitq,
                                       (lli = ll_close_next_lli(lcq)) != NULL,
                                       &lwi);
                if (IS_ERR(lli))
                        break;

                inode = ll_info2i(lli);
                ll_done_writing(inode);
                iput(inode);
        }

        complete(&lcq->lcq_comp);
        RETURN(0);
}

int ll_close_thread_start(struct ll_close_queue **lcq_ret)
{
        struct ll_close_queue *lcq;
        pid_t pid;

        OBD_ALLOC(lcq, sizeof(*lcq));
        if (lcq == NULL)
                return -ENOMEM;

        spin_lock_init(&lcq->lcq_lock);
        INIT_LIST_HEAD(&lcq->lcq_head);
        init_waitqueue_head(&lcq->lcq_waitq);
        init_completion(&lcq->lcq_comp);

        pid = kernel_thread(ll_close_thread, lcq, 0);
        if (pid < 0) {
                OBD_FREE(lcq, sizeof(*lcq));
                return pid;
        }

        wait_for_completion(&lcq->lcq_comp);
        *lcq_ret = lcq;
        return 0;
}

void ll_close_thread_shutdown(struct ll_close_queue *lcq)
{
        init_completion(&lcq->lcq_comp);
        atomic_inc(&lcq->lcq_stop);
        wake_up(&lcq->lcq_waitq);
        wait_for_completion(&lcq->lcq_comp);
        OBD_FREE(lcq, sizeof(*lcq));
}
