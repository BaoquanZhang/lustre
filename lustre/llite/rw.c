/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Lite I/O page cache routines shared by different kernel revs
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

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/version.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <asm/segment.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/lustre_mds.h>
#include <linux/lustre_lite.h>
#include "llite_internal.h"
#include <linux/lustre_compat25.h>

#ifndef list_for_each_prev_safe
#define list_for_each_prev_safe(pos, n, head) \
        for (pos = (head)->prev, n = pos->prev; pos != (head); \
                pos = n, n = pos->prev )
#endif

kmem_cache_t *ll_async_page_slab = NULL;
size_t ll_async_page_slab_size = 0;

/* SYNCHRONOUS I/O to object storage for an inode */
static int ll_brw(int cmd, struct inode *inode, struct obdo *oa,
                  struct page *page, int flags)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *lsm = lli->lli_smd;
        struct brw_page pg;
        int rc;
        ENTRY;

        pg.pg = page;
        pg.off = ((obd_off)page->index) << PAGE_SHIFT;

        if (cmd == OBD_BRW_WRITE && (pg.off + PAGE_SIZE > inode->i_size))
                pg.count = inode->i_size % PAGE_SIZE;
        else
                pg.count = PAGE_SIZE;

        LL_CDEBUG_PAGE(D_PAGE, page, "%s %d bytes ino %lu at "LPU64"/"LPX64"\n",
                       cmd & OBD_BRW_WRITE ? "write" : "read", pg.count,
                       inode->i_ino, pg.off, pg.off);
        if (pg.count == 0) {
                CERROR("ZERO COUNT: ino %lu: size %p:%Lu(%p:%Lu) idx %lu off "
                       LPU64"\n",
                       inode->i_ino, inode, inode->i_size, page->mapping->host,
                       page->mapping->host->i_size, page->index, pg.off);
        }

        pg.flag = flags;

        if (cmd == OBD_BRW_WRITE)
                lprocfs_counter_add(ll_i2sbi(inode)->ll_stats,
                                    LPROC_LL_BRW_WRITE, pg.count);
        else
                lprocfs_counter_add(ll_i2sbi(inode)->ll_stats,
                                    LPROC_LL_BRW_READ, pg.count);
        rc = obd_brw(cmd, ll_i2obdexp(inode), oa, lsm, 1, &pg, NULL);
        if (rc == 0)
                obdo_to_inode(inode, oa, OBD_MD_FLBLOCKS);
        else if (rc != -EIO)
                CERROR("error from obd_brw: rc = %d\n", rc);
        RETURN(rc);
}

__u64 lov_merge_size(struct lov_stripe_md *lsm, int kms);

/* this isn't where truncate starts.   roughly:
 * sys_truncate->ll_setattr_raw->vmtruncate->ll_truncate
 * we grab the lock back in setattr_raw to avoid races.
 *
 * must be called with lli_size_sem held */
void ll_truncate(struct inode *inode)
{
        struct lov_stripe_md *lsm = ll_i2info(inode)->lli_smd;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct obdo oa;
        int rc;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p) to %llu\n", inode->i_ino,
               inode->i_generation, inode, inode->i_size);

        if (lli->lli_size_pid != current->pid) {
                EXIT;
                return;
        }

        if (!lsm) {
                CDEBUG(D_INODE, "truncate on inode %lu with no objects\n",
                       inode->i_ino);
                GOTO(out_unlock, 0);
        }

        LASSERT(atomic_read(&lli->lli_size_sem.count) <= 0);

        if (lov_merge_size(lsm, 0) == inode->i_size) {
                CDEBUG(D_VFSTRACE, "skipping punch for "LPX64" (size = %llu)\n",
                       lsm->lsm_object_id, inode->i_size);
                GOTO(out_unlock, 0);
        }

        CDEBUG(D_INFO, "calling punch for "LPX64" (new size %llu)\n",
               lsm->lsm_object_id, inode->i_size);

        oa.o_id = lsm->lsm_object_id;
        oa.o_valid = OBD_MD_FLID;
        obdo_from_inode(&oa, inode, OBD_MD_FLTYPE | OBD_MD_FLMODE |
                        OBD_MD_FLATIME |OBD_MD_FLMTIME |OBD_MD_FLCTIME);

        obd_adjust_kms(ll_i2obdexp(inode), lsm, inode->i_size, 1);

        lli->lli_size_pid = 0;
        up(&lli->lli_size_sem);

        rc = obd_punch(ll_i2obdexp(inode), &oa, lsm, inode->i_size,
                       OBD_OBJECT_EOF, NULL);
        if (rc)
                CERROR("obd_truncate fails (%d) ino %lu\n", rc,
                       inode->i_ino);
        else
                obdo_to_inode(inode, &oa, OBD_MD_FLSIZE|OBD_MD_FLBLOCKS|
                              OBD_MD_FLATIME | OBD_MD_FLMTIME |
                              OBD_MD_FLCTIME);
        EXIT;
        return;

 out_unlock:
        lli->lli_size_pid = 0;
        up(&lli->lli_size_sem);
} /* ll_truncate */

__u64 lov_merge_size(struct lov_stripe_md *lsm, int kms);
int ll_prepare_write(struct file *file, struct page *page, unsigned from,
                     unsigned to)
{
        struct inode *inode = page->mapping->host;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *lsm = lli->lli_smd;
        obd_off offset = ((obd_off)page->index) << PAGE_SHIFT;
        struct brw_page pga;
        struct obdo oa;
        __u64 kms;
        int rc = 0;
        ENTRY;

        LASSERT(PageLocked(page));
        (void)llap_cast_private(page); /* assertion */

        /* Check to see if we should return -EIO right away */
        pga.pg = page;
        pga.off = offset;
        pga.count = PAGE_SIZE;
        pga.flag = 0;

        oa.o_id = lsm->lsm_object_id;
        oa.o_mode = inode->i_mode;
        oa.o_valid = OBD_MD_FLID | OBD_MD_FLMODE | OBD_MD_FLTYPE;

        rc = obd_brw(OBD_BRW_CHECK, ll_i2obdexp(inode), &oa, lsm, 1, &pga,
                     NULL);
        if (rc)
                RETURN(rc);

        if (PageUptodate(page)) {
                LL_CDEBUG_PAGE(D_PAGE, page, "uptodate\n");
                RETURN(0);
        }

        /* We're completely overwriting an existing page, so _don't_ set it up
         * to date until commit_write */
        if (from == 0 && to == PAGE_SIZE) {
                LL_CDEBUG_PAGE(D_PAGE, page, "full page write\n");
                POISON_PAGE(page, 0x11);
                RETURN(0);
        }

        /* If are writing to a new page, no need to read old data.  The extent
         * locking will have updated the KMS, and for our purposes here we can
         * treat it like i_size. */
        down(&lli->lli_size_sem);
        kms = lov_merge_size(lsm, 1);
        up(&lli->lli_size_sem);
        if (kms <= offset) {
                LL_CDEBUG_PAGE(D_PAGE, page, "kms "LPU64" <= offset "LPU64"\n",
                               kms, offset);
                memset(kmap(page), 0, PAGE_SIZE);
                kunmap(page);
                GOTO(prepare_done, rc = 0);
        }

        /* XXX could be an async ocp read.. read-ahead? */
        rc = ll_brw(OBD_BRW_READ, inode, &oa, page, 0);
        if (rc == 0) {
                /* bug 1598: don't clobber blksize */
                oa.o_valid &= ~(OBD_MD_FLSIZE | OBD_MD_FLBLKSZ);
                obdo_refresh_inode(inode, &oa, oa.o_valid);
        }

        EXIT;
 prepare_done:
        if (rc == 0)
                SetPageUptodate(page);

        return rc;
}

struct ll_async_page *llap_from_cookie(void *cookie)
{
        struct ll_async_page *llap = cookie;
        if (llap->llap_magic != LLAP_MAGIC)
                return ERR_PTR(-EINVAL);
        return llap;
};

static int ll_ap_make_ready(void *data, int cmd)
{
        struct ll_async_page *llap;
        struct page *page;
        ENTRY;

        llap = llap_from_cookie(data);
        if (IS_ERR(llap))
                RETURN(-EINVAL);

        page = llap->llap_page;

        LASSERT(cmd != OBD_BRW_READ);

        /* we're trying to write, but the page is locked.. come back later */
        if (TryLockPage(page))
                RETURN(-EAGAIN);

        LL_CDEBUG_PAGE(D_PAGE, page, "made ready\n");
        page_cache_get(page);

        /* if we left PageDirty we might get another writepage call
         * in the future.  list walkers are bright enough
         * to check page dirty so we can leave it on whatever list
         * its on.  XXX also, we're called with the cli list so if
         * we got the page cache list we'd create a lock inversion
         * with the removepage path which gets the page lock then the
         * cli lock */
        clear_page_dirty(page);
        RETURN(0);
}

/* We have two reasons for giving llite the opportunity to change the
 * write length of a given queued page as it builds the RPC containing
 * the page:
 *
 * 1) Further extending writes may have landed in the page cache
 *    since a partial write first queued this page requiring us
 *    to write more from the page cache.  (No further races are possible, since
 *    by the time this is called, the page is locked.)
 * 2) We might have raced with truncate and want to avoid performing
 *    write RPCs that are just going to be thrown away by the
 *    truncate's punch on the storage targets.
 *
 * The kms serves these purposes as it is set at both truncate and extending
 * writes.
 */
static int ll_ap_refresh_count(void *data, int cmd)
{
        struct ll_inode_info *lli;
        struct ll_async_page *llap;
        struct lov_stripe_md *lsm;
        struct page *page;
        __u64 kms, retval;
        ENTRY;

        /* readpage queues with _COUNT_STABLE, shouldn't get here. */
        LASSERT(cmd != OBD_BRW_READ);

        llap = llap_from_cookie(data);
        if (IS_ERR(llap))
                RETURN(PTR_ERR(llap));

        page = llap->llap_page;
        lli = ll_i2info(page->mapping->host);
        lsm = lli->lli_smd;

        down(&lli->lli_size_sem);
        kms = lov_merge_size(lsm, 1);
        up(&lli->lli_size_sem);

        /* catch race with truncate */
        if (((__u64)page->index << PAGE_SHIFT) >= kms)
                return 0;

        /* catch sub-page write at end of file */
        if (((__u64)page->index << PAGE_SHIFT) + PAGE_SIZE > kms)
                return kms % PAGE_SIZE;

        return PAGE_SIZE;
}

void ll_inode_fill_obdo(struct inode *inode, int cmd, struct obdo *oa)
{
        struct lov_stripe_md *lsm;
        obd_flag valid_flags;

        lsm = ll_i2info(inode)->lli_smd;

        oa->o_id = lsm->lsm_object_id;
        oa->o_valid = OBD_MD_FLID;
        valid_flags = OBD_MD_FLTYPE | OBD_MD_FLATIME;
        if (cmd == OBD_BRW_WRITE) {
                oa->o_valid |= OBD_MD_FLIFID | OBD_MD_FLEPOCH;
                mdc_pack_fid(obdo_fid(oa), inode->i_ino, 0, inode->i_mode);
                oa->o_easize = ll_i2info(inode)->lli_io_epoch;

                valid_flags |= OBD_MD_FLMTIME | OBD_MD_FLCTIME;
        }

        obdo_from_inode(oa, inode, valid_flags);
}

static void ll_ap_fill_obdo(void *data, int cmd, struct obdo *oa)
{
        struct ll_async_page *llap;
        ENTRY;

        llap = llap_from_cookie(data);
        if (IS_ERR(llap)) {
                EXIT;
                return;
        }

        ll_inode_fill_obdo(llap->llap_page->mapping->host, cmd, oa);
        EXIT;
}

static struct obd_async_page_ops ll_async_page_ops = {
        .ap_make_ready =        ll_ap_make_ready,
        .ap_refresh_count =     ll_ap_refresh_count,
        .ap_fill_obdo =         ll_ap_fill_obdo,
        .ap_completion =        ll_ap_completion,
};

struct ll_async_page *llap_cast_private(struct page *page)
{
        struct ll_async_page *llap = (struct ll_async_page *)page->private;

        LASSERTF(llap == NULL || llap->llap_magic == LLAP_MAGIC,
                 "page %p private %lu gave magic %d which != %d\n",
                 page, page->private, llap->llap_magic, LLAP_MAGIC);

        return llap;
}

/* Try to shrink the page cache for the @sbi filesystem by 1/@shrink_fraction.
 *
 * There is an llap attached onto every page in lustre, linked off @sbi.
 * We add an llap to the list so we don't lose our place during list walking.
 * If llaps in the list are being moved they will only move to the end
 * of the LRU, and we aren't terribly interested in those pages here (we
 * start at the beginning of the list where the least-used llaps are.
 */
int llap_shrink_cache(struct ll_sb_info *sbi, int shrink_fraction)
{
        struct ll_async_page *llap, dummy_llap = { .llap_magic = 0xd11ad11a };
        unsigned long total, want, count = 0;

        total = sbi->ll_async_page_count;

        /* There can be a large number of llaps (600k or more in a large
         * memory machine) so the VM 1/6 shrink ratio is likely too much.
         * Since we are freeing pages also, we don't necessarily want to
         * shrink so much.  Limit to 40MB of pages + llaps per call. */
        if (shrink_fraction == 0)
                want = sbi->ll_async_page_count - sbi->ll_async_page_max + 32;
        else
                want = (total + shrink_fraction - 1) / shrink_fraction;

        if (want > 40 << (20 - PAGE_CACHE_SHIFT))
                want = 40 << (20 - PAGE_CACHE_SHIFT);

        CDEBUG(D_CACHE, "shrinking %lu of %lu pages (1/%d)\n",
               want, total, shrink_fraction);

        spin_lock(&sbi->ll_lock);
        list_add(&dummy_llap.llap_pglist_item, &sbi->ll_pglist);

        while (--total >= 0 && count < want) {
                struct page *page;

                if (unlikely(need_resched())) {
                        spin_unlock(&sbi->ll_lock);
                        cond_resched();
                        spin_lock(&sbi->ll_lock);
                }

                llap = llite_pglist_next_llap(sbi,&dummy_llap.llap_pglist_item);
                list_del_init(&dummy_llap.llap_pglist_item);
                if (llap == NULL)
                        break;

                page = llap->llap_page;
                LASSERT(page != NULL);

                list_add(&dummy_llap.llap_pglist_item, &llap->llap_pglist_item);

                /* Page needs/undergoing IO */
                if (TryLockPage(page)) {
                        LL_CDEBUG_PAGE(D_PAGE, page, "can't lock\n");
                        continue;
                }

                /* If page is dirty or undergoing IO don't discard it */
                if (llap->llap_write_queued || PageDirty(page) ||
                    (!PageUptodate(page) &&
                     llap->llap_origin != LLAP_ORIGIN_READAHEAD)) {
                        unlock_page(page);
                        LL_CDEBUG_PAGE(D_PAGE, page, "can't drop from cache: "
                                       "%s%s%s%s origin %s\n",
                                       llap->llap_write_queued ? "wq " : "",
                                       PageDirty(page) ? "pd " : "",
                                       PageUptodate(page) ? "" : "!pu ",
                                       llap->llap_defer_uptodate ? "" : "!du",
                                       llap_origins[llap->llap_origin]);
                        continue;
                }

                page_cache_get(page);
                spin_unlock(&sbi->ll_lock);

                ++count;
                LL_CDEBUG_PAGE(D_PAGE, page, "drop from cache %lu/%lu\n",
                               count, want);
                if (page->mapping != NULL) {
                        ll_ra_accounting(page, page->mapping);
                        ll_truncate_complete_page(page);
                }
                unlock_page(page);
                page_cache_release(page);

                spin_lock(&sbi->ll_lock);
        }
        list_del(&dummy_llap.llap_pglist_item);
        spin_unlock(&sbi->ll_lock);

        CDEBUG(D_CACHE, "shrank %lu/%lu and left %lu unscanned\n",
               count, want, total);

        return count;
}

struct ll_async_page *llap_from_page(struct page *page, unsigned origin)
{
        struct ll_async_page *llap;
        struct obd_export *exp;
        struct inode *inode = page->mapping->host;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        int rc;
        ENTRY;

        LASSERT(ll_async_page_slab);
        LASSERTF(origin < LLAP__ORIGIN_MAX, "%u\n", origin);

        llap = llap_cast_private(page);
        if (llap != NULL) {
                /* move to end of LRU list */
                spin_lock(&sbi->ll_lock);
                sbi->ll_pglist_gen++;
                list_del_init(&llap->llap_pglist_item);
                list_add_tail(&llap->llap_pglist_item, &sbi->ll_pglist);
                spin_unlock(&sbi->ll_lock);
                GOTO(out, llap);
        }

        exp = ll_i2obdexp(page->mapping->host);
        if (exp == NULL)
                RETURN(ERR_PTR(-EINVAL));

        /* limit the number of lustre-cached pages */
        if (sbi->ll_async_page_count >= sbi->ll_async_page_max)
                llap_shrink_cache(sbi, 0);

        OBD_SLAB_ALLOC(llap, ll_async_page_slab, SLAB_KERNEL,
                       ll_async_page_slab_size);
        if (llap == NULL)
                RETURN(ERR_PTR(-ENOMEM));
        llap->llap_magic = LLAP_MAGIC;
        llap->llap_cookie = (void *)llap + size_round(sizeof(*llap));
        rc = obd_prep_async_page(exp, ll_i2info(inode)->lli_smd, NULL, page,
                                 (obd_off)page->index << PAGE_SHIFT,
                                 &ll_async_page_ops, llap, &llap->llap_cookie);
        if (rc) {
                OBD_SLAB_FREE(llap, ll_async_page_slab,
                              ll_async_page_slab_size);
                RETURN(ERR_PTR(rc));
        }

        CDEBUG(D_CACHE, "llap %p page %p cookie %p obj off "LPU64"\n", llap,
               page, llap->llap_cookie, (obd_off)page->index << PAGE_SHIFT);
        /* also zeroing the PRIVBITS low order bitflags */
        __set_page_ll_data(page, llap);
        llap->llap_page = page;

        spin_lock(&sbi->ll_lock);
        sbi->ll_pglist_gen++;
        sbi->ll_async_page_count++;
        list_add_tail(&llap->llap_pglist_item, &sbi->ll_pglist);
        spin_unlock(&sbi->ll_lock);

out:
        llap->llap_origin = origin;
        RETURN(llap);
}

static int queue_or_sync_write(struct obd_export *exp, struct inode *inode,
                               struct ll_async_page *llap,
                               unsigned to, obd_flag async_flags)
{
        unsigned long size_index = inode->i_size >> PAGE_SHIFT;
        struct obd_io_group *oig;
        int rc;
        ENTRY;

        /* _make_ready only sees llap once we've unlocked the page */
        llap->llap_write_queued = 1;
        rc = obd_queue_async_io(exp, ll_i2info(inode)->lli_smd, NULL,
                                llap->llap_cookie, OBD_BRW_WRITE, 0, 0, 0,
                                async_flags);
        if (rc == 0) {
                LL_CDEBUG_PAGE(D_PAGE, llap->llap_page, "write queued\n");
                //llap_write_pending(inode, llap);
                GOTO(out, 0);
        }

        llap->llap_write_queued = 0;

        rc = oig_init(&oig);
        if (rc)
                GOTO(out, rc);

        /* make full-page requests if we are not at EOF (bug 4410) */
        if (to != PAGE_SIZE && llap->llap_page->index < size_index) {
                LL_CDEBUG_PAGE(D_PAGE, llap->llap_page,
                               "sync write before EOF: size_index %lu, to %d\n",
                               size_index, to);
                to = PAGE_SIZE;
        } else if (to != PAGE_SIZE && llap->llap_page->index == size_index) {
                int size_to = inode->i_size & ~PAGE_MASK;
                LL_CDEBUG_PAGE(D_PAGE, llap->llap_page,
                               "sync write at EOF: size_index %lu, to %d/%d\n",
                               size_index, to, size_to);
                if (to < size_to)
                        to = size_to;
        }

        rc = obd_queue_group_io(exp, ll_i2info(inode)->lli_smd, NULL, oig,
                                llap->llap_cookie, OBD_BRW_WRITE, 0, to, 0,
                                ASYNC_READY | ASYNC_URGENT |
                                ASYNC_COUNT_STABLE | ASYNC_GROUP_SYNC);
        if (rc)
                GOTO(free_oig, rc);

        rc = obd_trigger_group_io(exp, ll_i2info(inode)->lli_smd, NULL, oig);
        if (rc)
                GOTO(free_oig, rc);

        rc = oig_wait(oig);

        if (!rc && async_flags & ASYNC_READY)
                unlock_page(llap->llap_page);

        LL_CDEBUG_PAGE(D_PAGE, llap->llap_page, "sync write returned %d\n", rc);

free_oig:
        oig_release(oig);
out:
        RETURN(rc);
}

/* update our write count to account for i_size increases that may have
 * happened since we've queued the page for io. */

/* be careful not to return success without setting the page Uptodate or
 * the next pass through prepare_write will read in stale data from disk. */
int ll_commit_write(struct file *file, struct page *page, unsigned from,
                    unsigned to)
{
        struct inode *inode = page->mapping->host;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *lsm = lli->lli_smd;
        struct obd_export *exp;
        struct ll_async_page *llap;
        loff_t size;
        int rc = 0;
        ENTRY;

        SIGNAL_MASK_ASSERT(); /* XXX BUG 1511 */
        LASSERT(inode == file->f_dentry->d_inode);
        LASSERT(PageLocked(page));

        CDEBUG(D_INODE, "inode %p is writing page %p from %d to %d at %lu\n",
               inode, page, from, to, page->index);

        llap = llap_from_page(page, LLAP_ORIGIN_COMMIT_WRITE);
        if (IS_ERR(llap))
                RETURN(PTR_ERR(llap));

        exp = ll_i2obdexp(inode);
        if (exp == NULL)
                RETURN(-EINVAL);

        /* queue a write for some time in the future the first time we
         * dirty the page */
        if (!PageDirty(page)) {
                lprocfs_counter_incr(ll_i2sbi(inode)->ll_stats,
                                     LPROC_LL_DIRTY_MISSES);

                rc = queue_or_sync_write(exp, inode, llap, to, 0);
                if (rc)
                        GOTO(out, rc);
        } else {
                lprocfs_counter_incr(ll_i2sbi(inode)->ll_stats,
                                     LPROC_LL_DIRTY_HITS);
        }

        /* put the page in the page cache, from now on ll_removepage is
         * responsible for cleaning up the llap.
         * only set page dirty when it's queued to be write out */
        if (llap->llap_write_queued)
                set_page_dirty(page);

out:
        size = (((obd_off)page->index) << PAGE_SHIFT) + to;
        down(&lli->lli_size_sem);
        if (rc == 0) {
                obd_adjust_kms(exp, lsm, size, 0);
                if (size > inode->i_size)
                        inode->i_size = size;
                SetPageUptodate(page);
        } else if (size > inode->i_size) {
                /* this page beyond the pales of i_size, so it can't be
                 * truncated in ll_p_r_e during lock revoking. we must
                 * teardown our book-keeping here. */
                ll_removepage(page);
        }
        up(&lli->lli_size_sem);
        RETURN(rc);
}

static unsigned long ll_ra_count_get(struct ll_sb_info *sbi, unsigned long len)
{
        struct ll_ra_info *ra = &sbi->ll_ra_info;
        unsigned long ret;
        ENTRY;

        spin_lock(&sbi->ll_lock);
        ret = min(ra->ra_max_pages - ra->ra_cur_pages, len);
        ra->ra_cur_pages += ret;
        spin_unlock(&sbi->ll_lock);

        RETURN(ret);
}

static void ll_ra_count_put(struct ll_sb_info *sbi, unsigned long len)
{
        struct ll_ra_info *ra = &sbi->ll_ra_info;
        spin_lock(&sbi->ll_lock);
        LASSERTF(ra->ra_cur_pages >= len, "r_c_p %lu len %lu\n",
                 ra->ra_cur_pages, len);
        ra->ra_cur_pages -= len;
        spin_unlock(&sbi->ll_lock);
}

/* called for each page in a completed rpc.*/
void ll_ap_completion(void *data, int cmd, struct obdo *oa, int rc)
{
        struct ll_async_page *llap;
        struct page *page;
        ENTRY;

        llap = llap_from_cookie(data);
        if (IS_ERR(llap)) {
                EXIT;
                return;
        }

        page = llap->llap_page;
        LASSERT(PageLocked(page));

        LL_CDEBUG_PAGE(D_PAGE, page, "completing cmd %d with %d\n", cmd, rc);

        if (cmd == OBD_BRW_READ && llap->llap_defer_uptodate)
                ll_ra_count_put(ll_i2sbi(page->mapping->host), 1);

        if (rc == 0)  {
                if (cmd == OBD_BRW_READ) {
                        if (!llap->llap_defer_uptodate)
                                SetPageUptodate(page);
                } else {
                        llap->llap_write_queued = 0;
                }
                ClearPageError(page);
        } else {
                if (cmd == OBD_BRW_READ) {
                        llap->llap_defer_uptodate = 0;
                } else {
                        set_page_dirty(page);
                        ClearPageLaunder(page);
                }
                SetPageError(page);
        }

        unlock_page(page);

        if (0 && cmd == OBD_BRW_WRITE) {
                llap_write_complete(page->mapping->host, llap);
                ll_try_done_writing(page->mapping->host);
        }

        if (PageWriteback(page)) {
                end_page_writeback(page);
        }
        page_cache_release(page);
        EXIT;
}

/* the kernel calls us here when a page is unhashed from the page cache.
 * the page will be locked and the kernel is holding a spinlock, so
 * we need to be careful.  we're just tearing down our book-keeping
 * here. */
void ll_removepage(struct page *page)
{
        struct inode *inode = page->mapping->host;
        struct obd_export *exp;
        struct ll_async_page *llap;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        int rc;
        ENTRY;

        LASSERT(!in_interrupt());

        /* sync pages or failed read pages can leave pages in the page
         * cache that don't have our data associated with them anymore */
        if (page->private == 0) {
                EXIT;
                return;
        }

        LL_CDEBUG_PAGE(D_PAGE, page, "being evicted\n");

        exp = ll_i2obdexp(inode);
        if (exp == NULL) {
                CERROR("page %p ind %lu gave null export\n", page, page->index);
                EXIT;
                return;
        }

        llap = llap_from_page(page, 0);
        if (IS_ERR(llap)) {
                CERROR("page %p ind %lu couldn't find llap: %ld\n", page,
                       page->index, PTR_ERR(llap));
                EXIT;
                return;
        }

        //llap_write_complete(inode, llap);
        rc = obd_teardown_async_page(exp, ll_i2info(inode)->lli_smd, NULL,
                                     llap->llap_cookie);
        if (rc != 0)
                CERROR("page %p ind %lu failed: %d\n", page, page->index, rc);

        /* this unconditional free is only safe because the page lock
         * is providing exclusivity to memory pressure/truncate/writeback..*/
        __clear_page_ll_data(page);

        spin_lock(&sbi->ll_lock);
        if (!list_empty(&llap->llap_pglist_item))
                list_del_init(&llap->llap_pglist_item);
        sbi->ll_pglist_gen++;
        sbi->ll_async_page_count--;
        spin_unlock(&sbi->ll_lock);
        OBD_SLAB_FREE(llap, ll_async_page_slab, ll_async_page_slab_size);
        EXIT;
}

static int ll_page_matches(struct page *page, int readahead)
{
        struct lustre_handle match_lockh = {0};
        struct inode *inode = page->mapping->host;
        ldlm_policy_data_t page_extent;
        int flags, matches;
        ENTRY;

        page_extent.l_extent.start = (__u64)page->index << PAGE_CACHE_SHIFT;
        page_extent.l_extent.end =
                page_extent.l_extent.start + PAGE_CACHE_SIZE - 1;
        flags = LDLM_FL_TEST_LOCK;
        if (!readahead)
                flags |= LDLM_FL_CBPENDING | LDLM_FL_BLOCK_GRANTED;
        matches = obd_match(ll_i2sbi(inode)->ll_osc_exp,
                            ll_i2info(inode)->lli_smd, LDLM_EXTENT,
                            &page_extent, LCK_PR | LCK_PW, &flags, inode,
                            &match_lockh);
        RETURN(matches);
}

static int ll_issue_page_read(struct obd_export *exp,
                              struct ll_async_page *llap,
                              struct obd_io_group *oig, int defer)
{
        struct page *page = llap->llap_page;
        int rc;

        page_cache_get(page);
        llap->llap_defer_uptodate = defer;
        llap->llap_ra_used = 0;
        rc = obd_queue_group_io(exp, ll_i2info(page->mapping->host)->lli_smd,
                                NULL, oig, llap->llap_cookie, OBD_BRW_READ, 0,
                                PAGE_SIZE, 0, ASYNC_COUNT_STABLE | ASYNC_READY
                                              | ASYNC_URGENT);
        if (rc) {
                LL_CDEBUG_PAGE(D_ERROR, page, "read queue failed: rc %d\n", rc);
                page_cache_release(page);
        }
        RETURN(rc);
}

static void ll_ra_stats_inc_unlocked(struct ll_ra_info *ra, enum ra_stat which)
{
        LASSERTF(which >= 0 && which < _NR_RA_STAT, "which: %u\n", which);
        ra->ra_stats[which]++;
}

static void ll_ra_stats_inc(struct address_space *mapping, enum ra_stat which)
{
        struct ll_sb_info *sbi = ll_i2sbi(mapping->host);
        struct ll_ra_info *ra = &ll_i2sbi(mapping->host)->ll_ra_info;

        spin_lock(&sbi->ll_lock);
        ll_ra_stats_inc_unlocked(ra, which);
        spin_unlock(&sbi->ll_lock);
}

void ll_ra_accounting(struct page *page, struct address_space *mapping)
{
        struct ll_async_page *llap;

        llap = llap_from_page(page, LLAP_ORIGIN_WRITEPAGE);
        if (IS_ERR(llap))
                return;

        if (!llap->llap_defer_uptodate || llap->llap_ra_used)
                return;

        ll_ra_stats_inc(mapping, RA_STAT_DISCARDED);
}

#define RAS_CDEBUG(ras) \
        CDEBUG(D_READA, "lrp %lu c %lu ws %lu wl %lu nra %lu\n",        \
               ras->ras_last_readpage, ras->ras_consecutive,            \
               ras->ras_window_start, ras->ras_window_len,              \
               ras->ras_next_readahead);

static int index_in_window(unsigned long index, unsigned long point,
                           unsigned long before, unsigned long after)
{
        unsigned long start = point - before, end = point + after;

        if (start > point)
               start = 0;
        if (end < point)
               end = ~0;

        return start <= index && index <= end;
}

static int ll_readahead(struct ll_readahead_state *ras,
                         struct obd_export *exp, struct address_space *mapping,
                         struct obd_io_group *oig, int flags)
{
        unsigned long i, start = 0, end = 0, reserved;
        struct ll_async_page *llap;
        struct page *page;
        int rc, ret = 0, match_failed = 0;
        __u64 kms;
        unsigned int gfp_mask;
        ENTRY;

        kms = lov_merge_size(ll_i2info(mapping->host)->lli_smd, 1);
        if (kms == 0) {
                ll_ra_stats_inc(mapping, RA_STAT_ZERO_LEN);
                RETURN(0);
        }

        spin_lock(&ras->ras_lock);
        /* reserve a part of the read-ahead window that we'll be issuing */
        if (ras->ras_window_len) {
                start = ras->ras_next_readahead;
                end = ras->ras_window_start + ras->ras_window_len - 1;
                end = min(end, (unsigned long)((kms - 1) >> PAGE_CACHE_SHIFT));
                ras->ras_next_readahead = max(end, end + 1);

                RAS_CDEBUG(ras);
        }
        spin_unlock(&ras->ras_lock);

        if (end == 0) {
                ll_ra_stats_inc(mapping, RA_STAT_ZERO_WINDOW);
                RETURN(0);
        }

        reserved = ll_ra_count_get(ll_i2sbi(mapping->host), end - start + 1);
        if (reserved < end - start + 1)
                ll_ra_stats_inc(mapping, RA_STAT_MAX_IN_FLIGHT);

        gfp_mask = GFP_HIGHUSER & ~__GFP_WAIT;
#ifdef __GFP_NOWARN
        gfp_mask |= __GFP_NOWARN;
#endif

        for (i = start; reserved > 0 && !match_failed && i <= end; i++) {
                /* skip locked pages from previous readpage calls */
                page = grab_cache_page_nowait_gfp(mapping, i, gfp_mask);
                if (page == NULL) {
                        CDEBUG(D_READA, "g_c_p_n failed\n");
                        continue;
                }

                /* we do this first so that we can see the page in the /proc
                 * accounting */
                llap = llap_from_page(page, LLAP_ORIGIN_READAHEAD);
                if (IS_ERR(llap) || llap->llap_defer_uptodate)
                        goto next_page;

                /* skip completed pages */
                if (Page_Uptodate(page))
                        goto next_page;

                /* bail when we hit the end of the lock. */
                if ((rc = ll_page_matches(page, 1)) <= 0) {
                        LL_CDEBUG_PAGE(D_READA | D_PAGE, page,
                                       "lock match failed: rc %d\n", rc);
                        ll_ra_stats_inc(mapping, RA_STAT_FAILED_MATCH);
                        match_failed = 1;
                        goto next_page;
                }

                rc = ll_issue_page_read(exp, llap, oig, 1);
                if (rc == 0) {
                        reserved--;
                        ret++;
                        LL_CDEBUG_PAGE(D_READA| D_PAGE, page,
                                       "started read-ahead\n");
                }
                if (rc) {
        next_page:
                        LL_CDEBUG_PAGE(D_READA | D_PAGE, page,
                                       "skipping read-ahead\n");

                        unlock_page(page);
                }
                page_cache_release(page);
        }

        LASSERTF(reserved >= 0, "reserved %lu\n", reserved);
        if (reserved != 0)
                ll_ra_count_put(ll_i2sbi(mapping->host), reserved);
        if (i == end + 1 && end == (kms >> PAGE_CACHE_SHIFT))
                ll_ra_stats_inc(mapping, RA_STAT_EOF);

        /* if we didn't get to the end of the region we reserved from
         * the ras we need to go back and update the ras so that the
         * next read-ahead tries from where we left off.  we only do so
         * if the region we failed to issue read-ahead on is still ahead
         * of the app and behind the next index to start read-ahead from */
        if (i != end + 1) {
                spin_lock(&ras->ras_lock);
                if (i < ras->ras_next_readahead &&
                    index_in_window(i, ras->ras_window_start, 0,
                                    ras->ras_window_len)) {
                        ras->ras_next_readahead = i;
                        RAS_CDEBUG(ras);
                }
                spin_unlock(&ras->ras_lock);
        }

        RETURN(ret);
}

static void ras_set_start(struct ll_readahead_state *ras, unsigned long index)
{
        ras->ras_window_start = index & (~(PTLRPC_MAX_BRW_PAGES - 1));
}

/* called with the ras_lock held or from places where it doesn't matter */
static void ras_reset(struct ll_readahead_state *ras, unsigned long index)
{
        ras->ras_last_readpage = index;
        ras->ras_consecutive = 1;
        ras->ras_window_len = 0;
        ras_set_start(ras, index);
        ras->ras_next_readahead = ras->ras_window_start;

        RAS_CDEBUG(ras);
}

void ll_readahead_init(struct inode *inode, struct ll_readahead_state *ras)
{
        spin_lock_init(&ras->ras_lock);
        ras_reset(ras, 0);
}

static void ras_update(struct ll_sb_info *sbi, struct ll_readahead_state *ras,
                       unsigned long index, unsigned hit)
{
        struct ll_ra_info *ra = &sbi->ll_ra_info;
        int zero = 0;
        ENTRY;

        spin_lock(&sbi->ll_lock);
        spin_lock(&ras->ras_lock);

        ll_ra_stats_inc_unlocked(ra, hit ? RA_STAT_HIT : RA_STAT_MISS);

        /* reset the read-ahead window in two cases.  First when the app seeks
         * or reads to some other part of the file.  Secondly if we get a
         * read-ahead miss that we think we've previously issued.  This can
         * be a symptom of there being so many read-ahead pages that the VM is
         * reclaiming it before we get to it. */
        if (!index_in_window(index, ras->ras_last_readpage, 8, 8)) {
                zero = 1;
                ll_ra_stats_inc_unlocked(ra, RA_STAT_DISTANT_READPAGE);
        } else if (!hit && ras->ras_window_len &&
                   index < ras->ras_next_readahead &&
                   index_in_window(index, ras->ras_window_start, 0,
                                   ras->ras_window_len)) {
                zero = 1;
                ll_ra_stats_inc_unlocked(ra, RA_STAT_MISS_IN_WINDOW);
        }

        if (zero) {
                ras_reset(ras, index);
                GOTO(out_unlock, 0);
        }

        ras->ras_last_readpage = index;
        ras->ras_consecutive++;
        ras_set_start(ras, index);
        ras->ras_next_readahead = max(ras->ras_window_start,
                                      ras->ras_next_readahead);

        /* wait for a few pages to arrive before issuing readahead to avoid
         * the worst overutilization */
        if (ras->ras_consecutive == 3) {
                ras->ras_window_len = PTLRPC_MAX_BRW_PAGES;
                GOTO(out_unlock, 0);
        }

        /* we need to increase the window sometimes.  we'll arbitrarily
         * do it half-way through the pages in an rpc */
        if ((index & (PTLRPC_MAX_BRW_PAGES - 1)) ==
            (PTLRPC_MAX_BRW_PAGES >> 1)) {
                ras->ras_window_len += PTLRPC_MAX_BRW_PAGES;
                ras->ras_window_len = min(ras->ras_window_len,
                                          ra->ra_max_pages);
        }

        EXIT;
out_unlock:
        RAS_CDEBUG(ras);
        spin_unlock(&ras->ras_lock);
        spin_unlock(&sbi->ll_lock);
        return;
}
int ll_writepage(struct page *page)
{
        struct inode *inode = page->mapping->host;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct obd_export *exp;
        struct ll_async_page *llap;
        int rc = 0;
        ENTRY;
        
        LASSERT(!PageDirty(page));
        LASSERT(PageLocked(page));
        
        exp = ll_i2obdexp(inode);
        if (exp == NULL)
                GOTO(out, rc = -EINVAL);
        
        llap = llap_from_page(page, LLAP_ORIGIN_WRITEPAGE);
        if (IS_ERR(llap))
                GOTO(out, rc = PTR_ERR(llap));
        
        page_cache_get(page);
        if (llap->llap_write_queued) {
                LL_CDEBUG_PAGE(D_PAGE, page, "marking urgent\n");
                rc = obd_set_async_flags(exp, ll_i2info(inode)->lli_smd, NULL,
                                         llap->llap_cookie,
                                         ASYNC_READY | ASYNC_URGENT);
        } else {
                rc = queue_or_sync_write(exp, inode, llap,
                                         PAGE_SIZE, ASYNC_READY | ASYNC_URGENT);
        }
        if (rc)
                page_cache_release(page);
out:
        if (rc) {
                if (!lli->lli_async_rc)
                        lli->lli_async_rc = rc;
                /* re-dirty page on error so it retries write */
                set_page_dirty(page);
                ClearPageLaunder(page); 
                unlock_page(page);
        }
        RETURN(rc);
}

/*
 * for now we do our readpage the same on both 2.4 and 2.5.  The kernel's
 * read-ahead assumes it is valid to issue readpage all the way up to
 * i_size, but our dlm locks make that not the case.  We disable the
 * kernel's read-ahead and do our own by walking ahead in the page cache
 * checking for dlm lock coverage.  the main difference between 2.4 and
 * 2.6 is how read-ahead gets batched and issued, but we're using our own,
 * so they look the same.
 */
int ll_readpage(struct file *filp, struct page *page)
{
        struct ll_file_data *fd = filp->private_data;
        struct inode *inode = page->mapping->host;
        struct obd_export *exp;
        struct ll_async_page *llap;
        struct obd_io_group *oig = NULL;
        int rc;
        ENTRY;

        LASSERT(PageLocked(page));
        LASSERT(!PageUptodate(page));
        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p),offset="LPX64"\n",
               inode->i_ino, inode->i_generation, inode,
               (((obd_off)page->index) << PAGE_SHIFT));
        LASSERT(atomic_read(&filp->f_dentry->d_inode->i_count) > 0);

        rc = oig_init(&oig);
        if (rc < 0)
                GOTO(out, rc);

        exp = ll_i2obdexp(inode);
        if (exp == NULL)
                GOTO(out, rc = -EINVAL);

        llap = llap_from_page(page, LLAP_ORIGIN_READPAGE);
        if (IS_ERR(llap))
                GOTO(out, rc = PTR_ERR(llap));

        if (ll_i2sbi(inode)->ll_ra_info.ra_max_pages)
                ras_update(ll_i2sbi(inode), &fd->fd_ras, page->index,
                           llap->llap_defer_uptodate);

        if (llap->llap_defer_uptodate) {
                llap->llap_ra_used = 1;
                rc = ll_readahead(&fd->fd_ras, exp, page->mapping, oig,
                                  fd->fd_flags);
                if (rc > 0)
                        obd_trigger_group_io(exp, ll_i2info(inode)->lli_smd,
                                             NULL, oig);
                LL_CDEBUG_PAGE(D_PAGE, page, "marking uptodate from defer\n");
                SetPageUptodate(page);
                unlock_page(page);
                GOTO(out_oig, rc = 0);
        }

        rc = ll_page_matches(page, 0);
        if (rc < 0) {
                LL_CDEBUG_PAGE(D_ERROR, page, "lock match failed: rc %d\n", rc);
                GOTO(out, rc);
        }

        if (rc == 0) {
                CWARN("ino %lu page %lu (%llu) not covered by "
                      "a lock (mmap?).  check debug logs.\n",
                      inode->i_ino, page->index,
                      (long long)page->index << PAGE_CACHE_SHIFT);
        }

        rc = ll_issue_page_read(exp, llap, oig, 0);
        if (rc)
                GOTO(out, rc);

        LL_CDEBUG_PAGE(D_PAGE, page, "queued readpage\n");
        if (ll_i2sbi(inode)->ll_ra_info.ra_max_pages)
                ll_readahead(&fd->fd_ras, exp, page->mapping, oig,
                             fd->fd_flags);

        rc = obd_trigger_group_io(exp, ll_i2info(inode)->lli_smd, NULL, oig);

out:
        if (rc)
                unlock_page(page);
out_oig:
        if (oig != NULL)
                oig_release(oig);
        RETURN(rc);
}
