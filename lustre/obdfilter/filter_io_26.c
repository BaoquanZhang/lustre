/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/fs/obdfilter/filter_io.c
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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

#include <linux/config.h>
#include <linux/module.h>
#include <linux/pagemap.h> // XXX kill me soon
#include <linux/version.h>

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/obd_class.h>
#include <linux/lustre_fsfilt.h>
#include "filter_internal.h"

#warning "implement writeback mode -bzzz"

/* 512byte block min */
#define MAX_BLOCKS_PER_PAGE (PAGE_SIZE / 512)
struct dio_request {
        atomic_t numreqs;       /* number of reqs being processed */
        struct bio *bio_list;   /* list of completed bios */
        wait_queue_head_t wait;
        int created[MAX_BLOCKS_PER_PAGE];
        unsigned long blocks[MAX_BLOCKS_PER_PAGE];
        spinlock_t lock;
};

static int dio_complete_routine(struct bio *bio, unsigned int done, int error)
{
        struct dio_request *dreq = bio->bi_private;
        unsigned long flags;

        spin_lock_irqsave(&dreq->lock, flags);
        bio->bi_private = dreq->bio_list;
        dreq->bio_list = bio;
        spin_unlock_irqrestore(&dreq->lock, flags);
        if (atomic_dec_and_test(&dreq->numreqs))
                wake_up(&dreq->wait);

        return 0;
}

static int can_be_merged(struct bio *bio, sector_t sector)
{
        int size;

        if (!bio)
                return 0;

        size = bio->bi_size >> 9;
        return bio->bi_sector + size == sector ? 1 : 0;
}

/* See if there are unallocated parts in given file region */
static int filter_range_is_mapped(struct inode *inode, obd_size offset, int len)
{
        sector_t (*fs_bmap)(struct address_space *, sector_t) =
                inode->i_mapping->a_ops->bmap;
        int j;

        /* We can't know if we are overwriting or not */
        if (fs_bmap == NULL)
                return 0;

        offset >>= inode->i_blkbits;
        len >>= inode->i_blkbits;

        for (j = 0; j <= len; j++)
                if (fs_bmap(inode->i_mapping, offset + j) == 0)
                        return 0;

        return 1;
}

int filter_commitrw_write(struct obd_export *exp, struct obdo *oa,
                          int objcount, struct obd_ioobj *obj, int niocount,
                          struct niobuf_local *res, struct obd_trans_info *oti,
                          int rc)
{
        struct bio *bio = NULL;
        int blocks_per_page, err;
        struct niobuf_local *lnb;
        struct obd_run_ctxt saved;
        struct fsfilt_objinfo fso;
        struct iattr iattr = { 0 };
        struct inode *inode = NULL;
        unsigned long now = jiffies;
        int i, k, cleanup_phase = 0;

        struct dio_request *dreq = NULL;
        struct obd_device *obd = exp->exp_obd;

        ENTRY;
        LASSERT(oti != NULL);
        LASSERT(objcount == 1);
        LASSERT(current->journal_info == NULL);

        if (rc != 0)
                GOTO(cleanup, rc);

        inode = res->dentry->d_inode;
        blocks_per_page = PAGE_SIZE >> inode->i_blkbits;
        LASSERT(blocks_per_page <= MAX_BLOCKS_PER_PAGE);

        OBD_ALLOC(dreq, sizeof(*dreq));

        if (dreq == NULL)
                RETURN(-ENOMEM);

        dreq->bio_list = NULL;
        init_waitqueue_head(&dreq->wait);
        atomic_set(&dreq->numreqs, 0);
        spin_lock_init(&dreq->lock);

        cleanup_phase = 1;
        fso.fso_dentry = res->dentry;
        fso.fso_bufcnt = obj->ioo_bufcnt;

        push_ctxt(&saved, &obd->obd_ctxt, NULL);
        cleanup_phase = 2;

        generic_osync_inode(inode, inode->i_mapping, OSYNC_DATA|OSYNC_METADATA);

        oti->oti_handle = fsfilt_brw_start(obd, objcount, &fso,
                                           niocount, res, oti);
        
        if (IS_ERR(oti->oti_handle)) {
                rc = PTR_ERR(oti->oti_handle);
                CDEBUG(rc == -ENOSPC ? D_INODE : D_ERROR,
                       "error starting transaction: rc = %d\n", rc);
                oti->oti_handle = NULL;
                GOTO(cleanup, rc);
        }

        if (time_after(jiffies, now + 15 * HZ))
                CERROR("slow brw_start %lus\n", (jiffies - now) / HZ);

        iattr_from_obdo(&iattr,oa,OBD_MD_FLATIME|OBD_MD_FLMTIME|OBD_MD_FLCTIME);
        for (i = 0, lnb = res; i < obj->ioo_bufcnt; i++, lnb++) {
                loff_t this_size;
                sector_t sector;
                int offs;

                /* If overwriting an existing block, we don't need a grant */
                if (!(lnb->flags & OBD_BRW_GRANTED) && lnb->rc == -ENOSPC &&
                    filter_range_is_mapped(inode, lnb->offset, lnb->len))
                        lnb->rc = 0;

                if (lnb->rc) /* ENOSPC, network RPC error, etc. */ 
                        continue;

                /* get block number for next page */
                rc = fsfilt_map_inode_page(obd, inode, lnb->page, dreq->blocks,
                                           dreq->created, 1);
                if (rc != 0)
                        GOTO(cleanup, rc);

                for (k = 0; k < blocks_per_page; k++) {
                        sector = dreq->blocks[k] *(inode->i_sb->s_blocksize>>9);
                        offs = k * inode->i_sb->s_blocksize;

                        if (!bio || !can_be_merged(bio, sector) ||
                            !bio_add_page(bio, lnb->page, PAGE_SIZE, offs)) {
                                if (bio) {
                                        atomic_inc(&dreq->numreqs);
                                        submit_bio(WRITE, bio);
                                        bio = NULL;
                                }
                                /* allocate new bio */
                                bio = bio_alloc(GFP_NOIO, obj->ioo_bufcnt);
                                bio->bi_bdev = inode->i_sb->s_bdev;
                                bio->bi_sector = sector;
                                bio->bi_end_io = dio_complete_routine;
                                bio->bi_private = dreq;

                                if (!bio_add_page(bio, lnb->page, PAGE_SIZE, 
                                                  offs))
                                        LBUG();
                        }
                }

                /* We expect these pages to be in offset order, but we'll
                 * be forgiving */
                this_size = lnb->offset + lnb->len;
                if (this_size > iattr.ia_size)
                        iattr.ia_size = this_size;
        }

        if (bio) {
                atomic_inc(&dreq->numreqs);
                submit_bio(WRITE, bio);
        }

        /* time to wait for I/O completion */
        wait_event(dreq->wait, atomic_read(&dreq->numreqs) == 0);

        /* free all bios */
        while (dreq->bio_list) {
                bio = dreq->bio_list;
                dreq->bio_list = bio->bi_private;
                bio_put(bio);
        }

        down(&inode->i_sem);
        if (iattr.ia_size > inode->i_size) {
                CDEBUG(D_INFO, "setting i_size to "LPU64"\n",
                       iattr.ia_size);

                iattr.ia_valid |= ATTR_SIZE;

                fsfilt_setattr(obd, res->dentry, oti->oti_handle,
                               &iattr, 0);
        }
        up(&inode->i_sem);

        if (time_after(jiffies, now + 15 * HZ))
                CERROR("slow direct_io %lus\n", (jiffies - now) / HZ);

        rc = filter_finish_transno(exp, oti, rc);

        err = fsfilt_commit(obd, inode, oti->oti_handle, obd_sync_filter);
        if (err)
                rc = err;

        if (obd_sync_filter)
                LASSERT(oti->oti_transno <= obd->obd_last_committed);

        if (time_after(jiffies, now + 15 * HZ))
                CERROR("slow commitrw commit %lus\n", (jiffies - now) / HZ);

cleanup:
        filter_grant_commit(exp, niocount, res);

        switch (cleanup_phase) {
        case 2:
                pop_ctxt(&saved, &obd->obd_ctxt, NULL);
                LASSERT(current->journal_info == NULL);
        case 1:
                OBD_FREE(dreq, sizeof(*dreq));
        case 0:
                for (i = 0, lnb = res; i < obj->ioo_bufcnt; i++, lnb++) {
                        /* flip_.. gets a ref, while free_page only frees
                         * when it decrefs to 0 */
                        if (rc == 0)
                                flip_into_page_cache(inode, lnb->page);
                        __free_page(lnb->page);
                }
                f_dput(res->dentry);
        }

        RETURN(rc);
}
