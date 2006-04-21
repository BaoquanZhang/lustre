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

#include <linux/lustre_mdc.h>
#include <linux/lustre_lite.h>
#include "llite_internal.h"

/* record that a write is in flight */
void llap_write_pending(struct inode *inode, struct ll_async_page *llap)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        spin_lock(&lli->lli_lock);
        list_add(&llap->llap_pending_write, &lli->lli_pending_write_llaps);
        spin_unlock(&lli->lli_lock);
}

/* record that a write has completed */
void llap_write_complete(struct inode *inode, struct ll_async_page *llap)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        spin_lock(&lli->lli_lock);
        list_del_init(&llap->llap_pending_write);
        spin_unlock(&lli->lli_lock);
}

void ll_open_complete(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        spin_lock(&lli->lli_lock);
        lli->lli_send_done_writing = 0;
        spin_unlock(&lli->lli_lock);
}

/* if we close with writes in flight then we want the completion or cancelation
 * of those writes to send a DONE_WRITING rpc to the MDS */
int ll_is_inode_dirty(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        int rc = 0;
        ENTRY;

        spin_lock(&lli->lli_lock);
        if (!list_empty(&lli->lli_pending_write_llaps))
                rc = 1;
        spin_unlock(&lli->lli_lock);
        RETURN(rc);
}

void ll_try_done_writing(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_close_queue *lcq = ll_i2sbi(inode)->ll_lcq;

        spin_lock(&lli->lli_lock);

        if (lli->lli_send_done_writing &&
            list_empty(&lli->lli_pending_write_llaps)) {

                spin_lock(&lcq->lcq_lock);
                if (list_empty(&lli->lli_close_item)) {
                        CDEBUG(D_INODE, "adding inode %lu/%u to close list\n",
                               inode->i_ino, inode->i_generation);
                        igrab(inode);
                        list_add_tail(&lli->lli_close_item, &lcq->lcq_list);
                        wake_up(&lcq->lcq_waitq);
                }
                spin_unlock(&lcq->lcq_lock);
        }

        spin_unlock(&lli->lli_lock);
}

/* The MDS needs us to get the real file attributes, then send a DONE_WRITING */
void ll_queue_done_writing(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        spin_lock(&lli->lli_lock);
        lli->lli_send_done_writing = 1;
        spin_unlock(&lli->lli_lock);

        ll_try_done_writing(inode);
        EXIT;
}

#if 0
/* If we know the file size and have the cookies:
 *  - send a DONE_WRITING rpc
 *
 * Otherwise:
 *  - get a whole-file lock
 *  - get the authoritative size and all cookies with GETATTRs
 *  - send a DONE_WRITING rpc
 */
static void ll_close_done_writing(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        ldlm_policy_data_t policy = { .l_extent = {0, OBD_OBJECT_EOF } };
        struct lustre_handle lockh = { 0 };
        struct md_op_data op_data;
        struct obdo obdo;
        obd_flag valid;
        int rc, ast_flags = 0;
        ENTRY;

        memset(&obdo, 0, sizeof(obdo));
        if (test_bit(LLI_F_HAVE_OST_SIZE_LOCK, &lli->lli_flags))
                goto rpc;

        rc = ll_extent_lock(NULL, inode, lli->lli_smd, LCK_PW, &policy, &lockh,
                            ast_flags);
        if (rc != 0) {
                CERROR("lock acquisition failed (%d): unable to send "
                       "DONE_WRITING for inode %lu/%u\n", rc, inode->i_ino,
                       inode->i_generation);
                GOTO(out, rc);
        }

        rc = ll_lsm_getattr(ll_i2dtexp(inode), lli->lli_smd, &obdo);
        if (rc) {
                CERROR("inode_getattr failed (%d): unable to send DONE_WRITING "
                       "for inode %lu/%u\n", rc, inode->i_ino,
                       inode->i_generation);
                ll_extent_unlock(NULL, inode, lli->lli_smd, LCK_PW, &lockh);
                GOTO(out, rc);
        }

        obdo_refresh_inode(inode, &obdo, valid);

        CDEBUG(D_INODE, "objid "LPX64" size %Lu, blocks %lu, blksize %lu\n",
               lli->lli_smd->lsm_object_id, inode->i_size, inode->i_blocks,
               inode->i_blksize);

        set_bit(LLI_F_HAVE_OST_SIZE_LOCK, &lli->lli_flags);

        rc = ll_extent_unlock(NULL, inode, lli->lli_smd, LCK_PW, &lockh);
        if (rc != ELDLM_OK)
                CERROR("unlock failed (%d)?  proceeding anyways...\n", rc);

 rpc:
        op_data.fid1 = lli->lli_fid;
        op_data.size = inode->i_size;
        op_data.blocks = inode->i_blocks;
        op_data.valid = OBD_MD_FLID | OBD_MD_FLSIZE | OBD_MD_FLBLOCKS;

        rc = md_done_writing(ll_i2sbi(inode)->ll_md_exp, &op_data);
 out:
}
#endif

static struct ll_inode_info *ll_close_next_lli(struct ll_close_queue *lcq)
{
        struct ll_inode_info *lli = NULL;

        spin_lock(&lcq->lcq_lock);

        if (lcq->lcq_list.next == NULL)
                lli = ERR_PTR(-1);
        else if (!list_empty(&lcq->lcq_list)) {
                lli = list_entry(lcq->lcq_list.next, struct ll_inode_info,
                                 lli_close_item);
                list_del(&lli->lli_close_item);
        }

        spin_unlock(&lcq->lcq_lock);
        return lli;
}

static int ll_close_thread(void *arg)
{
        struct ll_close_queue *lcq = arg;
        ENTRY;

        /* XXX boiler-plate */
        {
                char name[sizeof(current->comm)];
                unsigned long flags;
                snprintf(name, sizeof(name) - 1, "ll_close");
                libcfs_daemonize(name);
                SIGNAL_MASK_LOCK(current, flags);
                sigfillset(&current->blocked);
                RECALC_SIGPENDING;
                SIGNAL_MASK_UNLOCK(current, flags);
        }

        complete(&lcq->lcq_comp);

        while (1) {
                struct l_wait_info lwi = { 0 };
                struct ll_inode_info *lli;
                //struct inode *inode;

                l_wait_event_exclusive(lcq->lcq_waitq,
                                       (lli = ll_close_next_lli(lcq)) != NULL,
                                       &lwi);
                if (IS_ERR(lli))
                        break;

                //inode = ll_info2i(lli);
                //ll_close_done_writing(inode);
                //iput(inode);
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
        INIT_LIST_HEAD(&lcq->lcq_list);
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
        lcq->lcq_list.next = NULL;
        wake_up(&lcq->lcq_waitq);
        wait_for_completion(&lcq->lcq_comp);
        OBD_FREE(lcq, sizeof(*lcq));
}
