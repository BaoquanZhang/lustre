/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
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

#define DEBUG_SUBSYSTEM S_ECHO
#ifdef __KERNEL__
#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/completion.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
#include <linux/iobuf.h>
#endif
#include <asm/div64.h>
#else
#include <liblustre.h>
#endif

#include <linux/obd.h>
#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/obd_echo.h>
#include <linux/lustre_debug.h>
#include <linux/lprocfs_status.h>

static obd_id last_object_id;

#if 0
static void
echo_printk_object (char *msg, struct ec_object *eco)
{
        struct lov_stripe_md *lsm = eco->eco_lsm;
        int                   i;

        printk (KERN_INFO "Lustre: %s: object %p: "LPX64", refs %d%s: "LPX64
                "=%u!%u\n", msg, eco, eco->eco_id, eco->eco_refcount,
                eco->eco_deleted ? "(deleted) " : "",
                lsm->lsm_object_id, lsm->lsm_stripe_size,
                lsm->lsm_stripe_count);

        for (i = 0; i < lsm->lsm_stripe_count; i++)
                printk (KERN_INFO "Lustre:   @%2u:"LPX64"\n",
                        lsm->lsm_oinfo[i].loi_ost_idx,
                        lsm->lsm_oinfo[i].loi_id);
}
#endif

static struct ec_object *
echo_find_object_locked (struct obd_device *obd, obd_id id)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct ec_object       *eco = NULL;
        struct list_head       *el;

        list_for_each (el, &ec->ec_objects) {
                eco = list_entry (el, struct ec_object, eco_obj_chain);

                if (eco->eco_id == id)
                        return (eco);
        }
        return (NULL);
}

static int
echo_copyout_lsm (struct lov_stripe_md *lsm, void *ulsm, int ulsm_nob)
{
        int nob;

        nob = offsetof (struct lov_stripe_md, lsm_oinfo[lsm->lsm_stripe_count]);
        if (nob > ulsm_nob)
                return (-EINVAL);

        if (copy_to_user (ulsm, lsm, nob))
                return (-EFAULT);

        return (0);
}

static int
echo_copyin_lsm (struct obd_device *obd, struct lov_stripe_md *lsm,
                 void *ulsm, int ulsm_nob)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        int                     nob;

        if (ulsm_nob < sizeof (*lsm))
                return (-EINVAL);

        if (copy_from_user (lsm, ulsm, sizeof (*lsm)))
                return (-EFAULT);

        nob = lsm->lsm_stripe_count * sizeof (lsm->lsm_oinfo[0]);

        if (ulsm_nob < nob ||
            lsm->lsm_stripe_count > ec->ec_nstripes ||
            lsm->lsm_magic != LOV_MAGIC ||
            (lsm->lsm_stripe_size & (PAGE_SIZE - 1)) != 0 ||
            ((__u64)lsm->lsm_stripe_size * lsm->lsm_stripe_count > ~0UL))
                return (-EINVAL);

        if (copy_from_user(lsm->lsm_oinfo,
                           ((struct lov_stripe_md *)ulsm)->lsm_oinfo, nob))
                return (-EFAULT);

        return (0);
}

static struct ec_object *
echo_allocate_object (struct obd_device *obd)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct ec_object       *eco;
        int rc;

        OBD_ALLOC(eco, sizeof (*eco));
        if (eco == NULL)
                return NULL;

        rc = obd_alloc_memmd(ec->ec_exp, &eco->eco_lsm);
        if (rc < 0) {
                OBD_FREE(eco, sizeof (*eco));
                return NULL;
        }

        eco->eco_device = obd;
        eco->eco_deleted = 0;
        eco->eco_refcount = 0;
        eco->eco_lsm->lsm_magic = LOV_MAGIC;
        /* leave stripe count 0 by default */

        return (eco);
}

static void
echo_free_object (struct ec_object *eco)
{
        struct obd_device      *obd = eco->eco_device;
        struct echo_client_obd *ec = &obd->u.echo_client;

        LASSERT (eco->eco_refcount == 0);
        obd_free_memmd(ec->ec_exp, &eco->eco_lsm);
        OBD_FREE (eco, sizeof (*eco));
}

static int echo_create_object(struct obd_device *obd, int on_target,
                              struct obdo *oa, void *ulsm, int ulsm_nob,
                              struct obd_trans_info *oti)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct ec_object       *eco2;
        struct ec_object       *eco;
        struct lov_stripe_md   *lsm;
        int                     rc;
        int                     i, idx;

        if ((oa->o_valid & OBD_MD_FLID) == 0 && /* no obj id */
            (on_target ||                       /* set_stripe */
             ec->ec_nstripes != 0)) {           /* LOV */
                CERROR ("No valid oid\n");
                return (-EINVAL);
        }

        if (ulsm != NULL) {
                eco = echo_allocate_object (obd);
                if (eco == NULL)
                        return (-ENOMEM);

                lsm = eco->eco_lsm;

                rc = echo_copyin_lsm (obd, lsm, ulsm, ulsm_nob);
                if (rc != 0)
                        goto failed;

                /* setup object ID here for !on_target and LOV hint */
                if ((oa->o_valid & OBD_MD_FLID) != 0)
                        eco->eco_id = lsm->lsm_object_id = oa->o_id;

                if (lsm->lsm_stripe_count == 0)
                        lsm->lsm_stripe_count = ec->ec_nstripes;

                if (lsm->lsm_stripe_size == 0)
                        lsm->lsm_stripe_size = PAGE_SIZE;

                idx = ll_insecure_random_int();

                /* setup stripes: indices + default ids if required */
                for (i = 0; i < lsm->lsm_stripe_count; i++) {
                        if (lsm->lsm_oinfo[i].loi_id == 0)
                                lsm->lsm_oinfo[i].loi_id = lsm->lsm_object_id;

                        lsm->lsm_oinfo[i].loi_ost_idx =
                                (idx + i) % ec->ec_nstripes;
                }
        } else {
                OBD_ALLOC(eco, sizeof(*eco));
                eco->eco_device = obd;
                lsm = NULL;
        }

        if (oa->o_id == 0)
                oa->o_id = ++last_object_id;

        if (on_target) {
                /* XXX get some filter group constants */
                oa->o_gr = 2;
                oa->o_valid |= OBD_MD_FLGROUP;
                rc = obd_create(ec->ec_exp, oa, &lsm, oti);
                if (rc != 0)
                        goto failed;

                /* See what object ID we were given */
                eco->eco_id = oa->o_id = lsm->lsm_object_id;
                oa->o_valid |= OBD_MD_FLID;

                LASSERT(eco->eco_lsm == NULL || eco->eco_lsm == lsm);
                eco->eco_lsm = lsm;
        }

        spin_lock (&ec->ec_lock);

        eco2 = echo_find_object_locked (obd, oa->o_id);
        if (eco2 != NULL) {                     /* conflict */
                spin_unlock (&ec->ec_lock);

                CERROR ("Can't create object id "LPX64": id already exists%s\n",
                        oa->o_id, on_target ? " (undoing create)" : "");

                if (on_target)
                        obd_destroy(ec->ec_exp, oa, lsm, oti);

                rc = -EEXIST;
                goto failed;
        }

        list_add (&eco->eco_obj_chain, &ec->ec_objects);
        spin_unlock (&ec->ec_lock);
        CDEBUG (D_INFO,
                "created %p: "LPX64"=%u#%u@%u refs %d del %d\n",
                eco, eco->eco_id,
                eco->eco_lsm->lsm_stripe_size,
                eco->eco_lsm->lsm_stripe_count,
                eco->eco_lsm->lsm_oinfo[0].loi_ost_idx,
                eco->eco_refcount, eco->eco_deleted);
        return (0);

 failed:
        echo_free_object (eco);
        return (rc);
}

static int
echo_get_object (struct ec_object **ecop, struct obd_device *obd,
                 struct obdo *oa)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct ec_object       *eco;
        struct ec_object       *eco2;
        int                     rc;

        if ((oa->o_valid & OBD_MD_FLID) == 0)
        {
                CERROR ("No valid oid\n");
                return (-EINVAL);
        }

        spin_lock (&ec->ec_lock);
        eco = echo_find_object_locked (obd, oa->o_id);
        if (eco != NULL) {
                if (eco->eco_deleted)           /* being deleted */
                        return (-EAGAIN);       /* (see comment in cleanup) */

                eco->eco_refcount++;
                spin_unlock (&ec->ec_lock);
                *ecop = eco;
                CDEBUG (D_INFO,
                        "found %p: "LPX64"=%u#%u@%u refs %d del %d\n",
                        eco, eco->eco_id,
                        eco->eco_lsm->lsm_stripe_size,
                        eco->eco_lsm->lsm_stripe_count,
                        eco->eco_lsm->lsm_oinfo[0].loi_ost_idx,
                        eco->eco_refcount, eco->eco_deleted);
                return (0);
        }
        spin_unlock (&ec->ec_lock);

        if (ec->ec_nstripes != 0)               /* striping required */
                return (-ENOENT);

        eco = echo_allocate_object (obd);
        if (eco == NULL)
                return (-ENOMEM);

        eco->eco_id = eco->eco_lsm->lsm_object_id = oa->o_id;

        spin_lock (&ec->ec_lock);

        eco2 = echo_find_object_locked (obd, oa->o_id);
        if (eco2 == NULL) {                     /* didn't race */
                list_add (&eco->eco_obj_chain, &ec->ec_objects);
                spin_unlock (&ec->ec_lock);
                eco->eco_refcount = 1;
                *ecop = eco;
                CDEBUG (D_INFO,
                        "created %p: "LPX64"=%u#%u@%d refs %d del %d\n",
                        eco, eco->eco_id,
                        eco->eco_lsm->lsm_stripe_size,
                        eco->eco_lsm->lsm_stripe_count,
                        eco->eco_lsm->lsm_oinfo[0].loi_ost_idx,
                        eco->eco_refcount, eco->eco_deleted);
                return (0);
        }

        if (eco2->eco_deleted)
                rc = -EAGAIN;                   /* lose race */
        else {
                eco2->eco_refcount++;           /* take existing */
                *ecop = eco2;
                rc = 0;
                LASSERT (eco2->eco_id == eco2->eco_lsm->lsm_object_id);
                CDEBUG (D_INFO,
                        "found(2) %p: "LPX64"=%u#%u@%d refs %d del %d\n",
                        eco2, eco2->eco_id,
                        eco2->eco_lsm->lsm_stripe_size,
                        eco2->eco_lsm->lsm_stripe_count,
                        eco2->eco_lsm->lsm_oinfo[0].loi_ost_idx,
                        eco2->eco_refcount, eco2->eco_deleted);
        }

        spin_unlock (&ec->ec_lock);

        echo_free_object (eco);
        return (rc);
}

static void
echo_put_object (struct ec_object *eco)
{
        struct obd_device      *obd = eco->eco_device;
        struct echo_client_obd *ec = &obd->u.echo_client;

        /* Release caller's ref on the object.
         * delete => mark for deletion when last ref goes
         */

        spin_lock (&ec->ec_lock);

        eco->eco_refcount--;
        LASSERT (eco->eco_refcount >= 0);

        CDEBUG(D_INFO, "put %p: "LPX64"=%u#%u@%d refs %d del %d\n",
               eco, eco->eco_id,
               eco->eco_lsm->lsm_stripe_size,
               eco->eco_lsm->lsm_stripe_count,
               eco->eco_lsm->lsm_oinfo[0].loi_ost_idx,
               eco->eco_refcount, eco->eco_deleted);

        if (eco->eco_refcount != 0 || !eco->eco_deleted) {
                spin_unlock (&ec->ec_lock);
                return;
        }

        spin_unlock (&ec->ec_lock);

        /* NB leave obj in the object list.  We must prevent anyone from
         * attempting to enqueue on this object number until we can be
         * sure there will be no more lock callbacks.
         */
        obd_cancel_unused(ec->ec_exp, eco->eco_lsm, 0, NULL);

        /* now we can let it go */
        spin_lock (&ec->ec_lock);
        list_del (&eco->eco_obj_chain);
        spin_unlock (&ec->ec_lock);

        LASSERT (eco->eco_refcount == 0);

        echo_free_object (eco);
}

static void
echo_get_stripe_off_id (struct lov_stripe_md *lsm, obd_off *offp, obd_id *idp)
{
        unsigned long stripe_count;
        unsigned long stripe_size;
        unsigned long width;
        unsigned long woffset;
        int           stripe_index;
        obd_off       offset;

        if (lsm->lsm_stripe_count <= 1)
                return;

        offset       = *offp;
        stripe_size  = lsm->lsm_stripe_size;
        stripe_count = lsm->lsm_stripe_count;

        /* width = # bytes in all stripes */
        width = stripe_size * stripe_count;

        /* woffset = offset within a width; offset = whole number of widths */
        woffset = do_div (offset, width);

        stripe_index = woffset / stripe_size;

        *idp = lsm->lsm_oinfo[stripe_index].loi_id;
        *offp = offset * stripe_size + woffset % stripe_size;
}

static void echo_page_debug_setup(struct lov_stripe_md *lsm, 
                                  struct page *page, int rw, obd_id id, 
                                  obd_off offset, obd_off count)
{
        void *addr;
        obd_off stripe_off;
        obd_id stripe_id;

        if (id == 0)
                return;

        addr = kmap(page);

        if (rw == OBD_BRW_WRITE) {
                stripe_off = offset;
                stripe_id = id;
                echo_get_stripe_off_id(lsm, &stripe_off, &stripe_id);
        } else {
                stripe_off = 0xdeadbeef00c0ffeeULL;
                stripe_id = 0xdeadbeef00c0ffeeULL;
        }
        page_debug_setup(addr, count, stripe_off, stripe_id);

        kunmap(page);
}

static int echo_page_debug_check(struct lov_stripe_md *lsm, 
                                  struct page *page, obd_id id, 
                                  obd_off offset, obd_off count)
{
        obd_off stripe_off = offset;
        obd_id stripe_id = id;
        void *addr;
        int rc;

        if (id == 0)
                return 0;

        addr = kmap(page);
        echo_get_stripe_off_id (lsm, &stripe_off, &stripe_id);
        rc = page_debug_check("test_brw", addr, count, stripe_off, stripe_id);
        kunmap(page);
        return rc;
}

static int echo_client_kbrw(struct obd_device *obd, int rw, struct obdo *oa,
                            struct lov_stripe_md *lsm, obd_off offset,
                            obd_size count, struct obd_trans_info *oti)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        obd_count               npages;
        struct brw_page        *pga;
        struct brw_page        *pgp;
        obd_off                 off;
        int                     i;
        int                     rc;
        int                     verify = 0;
        int                     gfp_mask;

        /* oa_id  == 0    => speed test (no verification) else...
         * oa & 1         => use HIGHMEM
         */
        gfp_mask = ((oa->o_id & 1) == 0) ? GFP_KERNEL : GFP_HIGHUSER;

        LASSERT(rw == OBD_BRW_WRITE || rw == OBD_BRW_READ);

        if (count <= 0 ||
            (count & (PAGE_SIZE - 1)) != 0 ||
            (lsm != NULL &&
             lsm->lsm_object_id != oa->o_id))
                return (-EINVAL);

        /* XXX think again with misaligned I/O */
        npages = count >> PAGE_SHIFT;

        OBD_ALLOC(pga, npages * sizeof(*pga));
        if (pga == NULL)
                return (-ENOMEM);

        for (i = 0, pgp = pga, off = offset;
             i < npages;
             i++, pgp++, off += PAGE_SIZE) {

                LASSERT (pgp->pg == NULL);      /* for cleanup */

                rc = -ENOMEM;
                pgp->pg = alloc_pages (gfp_mask, 0);
                if (pgp->pg == NULL)
                        goto out;

                pgp->count = PAGE_SIZE;
                pgp->off = off;
                pgp->flag = 0;

                echo_page_debug_setup(lsm, pgp->pg, rw, oa->o_id, off, 
                                      pgp->count);
        }

        rc = obd_brw(rw, ec->ec_exp, oa, lsm, npages, pga, oti);

 out:
        if (rc == 0 && rw == OBD_BRW_READ)
                verify = 1;

        for (i = 0, pgp = pga; i < npages; i++, pgp++) {
                if (pgp->pg == NULL)
                        continue;

                if (verify) {
                        int vrc;
                        vrc = echo_page_debug_check(lsm, pgp->pg, oa->o_id,
                                                    pgp->off, pgp->count);
                        if (vrc != 0 && rc == 0)
                                rc = vrc;
                }
                __free_pages(pgp->pg, 0);
        }
        OBD_FREE(pga, npages * sizeof(*pga));
        return (rc);
}

#ifdef __KERNEL__
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
static int echo_client_ubrw(struct obd_device *obd, int rw,
                            struct obdo *oa, struct lov_stripe_md *lsm,
                            obd_off offset, obd_size count, char *buffer,
                            struct obd_trans_info *oti)
{
        struct echo_client_obd *ec = &obd->u.echo_client;
        obd_count               npages;
        struct brw_page        *pga;
        struct brw_page        *pgp;
        obd_off                 off;
        struct kiobuf          *kiobuf;
        int                     i;
        int                     rc;

        LASSERT (rw == OBD_BRW_WRITE ||
                 rw == OBD_BRW_READ);

        /* NB: for now, only whole pages, page aligned */

        if (count <= 0 ||
            ((long)buffer & (PAGE_SIZE - 1)) != 0 ||
            (count & (PAGE_SIZE - 1)) != 0 ||
            (lsm != NULL && lsm->lsm_object_id != oa->o_id))
                return (-EINVAL);

        /* XXX think again with misaligned I/O */
        npages = count >> PAGE_SHIFT;

        OBD_ALLOC(pga, npages * sizeof(*pga));
        if (pga == NULL)
                return (-ENOMEM);

        rc = alloc_kiovec (1, &kiobuf);
        if (rc != 0)
                goto out_1;

        rc = map_user_kiobuf ((rw == OBD_BRW_READ) ? READ : WRITE,
                              kiobuf, (unsigned long)buffer, count);
        if (rc != 0)
                goto out_2;

        LASSERT (kiobuf->offset == 0);
        LASSERT (kiobuf->nr_pages == npages);

        for (i = 0, off = offset, pgp = pga;
             i < npages;
             i++, off += PAGE_SIZE, pgp++) {
                pgp->off = off;
                pgp->pg = kiobuf->maplist[i];
                pgp->count = PAGE_SIZE;
                pgp->flag = 0;
        }

        rc = obd_brw(rw, ec->ec_exp, oa, lsm, npages, pga, oti);

        //        if (rw == OBD_BRW_READ)
        //                mark_dirty_kiobuf (kiobuf, count);

        unmap_kiobuf (kiobuf);
 out_2:
        free_kiovec (1, &kiobuf);
 out_1:
        OBD_FREE(pga, npages * sizeof(*pga));
        return (rc);
}
#else
static int echo_client_ubrw(struct obd_device *obd, int rw,
                            struct obdo *oa, struct lov_stripe_md *lsm,
                            obd_off offset, obd_size count, char *buffer,
                            struct obd_trans_info *oti)
{
#warning "echo_client_ubrw() needs to be ported on 2.6 yet"
        LBUG();
        return 0;
}
#endif
#endif

struct echo_async_state;

#define EAP_MAGIC 79277927
struct echo_async_page {
        int                     eap_magic;
        struct page             *eap_page;
        void                    *eap_cookie;
        obd_off                 eap_off;
        struct echo_async_state *eap_eas;
        struct list_head        eap_item;
};

struct echo_async_state {
        spinlock_t              eas_lock;
        obd_off                 eas_next_offset;
        obd_off                 eas_end_offset;
        int                     eas_in_flight;
        int                     eas_rc;
        wait_queue_head_t       eas_waitq;
        struct list_head        eas_avail;
        struct obdo             eas_oa;
        struct lov_stripe_md    *eas_lsm;
};

static int eas_should_wake(struct echo_async_state *eas)
{
        unsigned long flags;
        int rc = 0;
        spin_lock_irqsave(&eas->eas_lock, flags);
        if (eas->eas_rc == 0 && !list_empty(&eas->eas_avail))
            rc = 1;
        spin_unlock_irqrestore(&eas->eas_lock, flags);
        return rc;
};

struct echo_async_page *eap_from_cookie(void *cookie)
{
        struct echo_async_page *eap = cookie;
        if (eap->eap_magic != EAP_MAGIC)
                return ERR_PTR(-EINVAL);
        return eap;
};

static int ec_ap_make_ready(void *data, int cmd)
{
        /* our pages are issued ready */
        LBUG();
        return 0;
}
static int ec_ap_refresh_count(void *data, int cmd)
{
        /* our pages are issued with a stable count */
        LBUG();
        return PAGE_SIZE;
}
static void ec_ap_fill_obdo(void *data, int cmd, struct obdo *oa)
{
        struct echo_async_page *eap;
        eap = eap_from_cookie(data);
        if (IS_ERR(eap))
                return;

        memcpy(oa, &eap->eap_eas->eas_oa, sizeof(*oa));
}
static void ec_ap_completion(void *data, int cmd, int rc)
{
        struct echo_async_page *eap = eap_from_cookie(data);
        struct echo_async_state *eas;
        unsigned long flags;

        if (IS_ERR(eap))
                return;
        eas = eap->eap_eas;

        if (cmd == OBD_BRW_READ)
                echo_page_debug_check(eas->eas_lsm, eap->eap_page, 
                                      eas->eas_oa.o_id, eap->eap_off, 
                                      PAGE_SIZE);

        spin_lock_irqsave(&eas->eas_lock, flags);
        if (rc && !eas->eas_rc)
                eas->eas_rc = rc;
        eas->eas_in_flight--;
        list_add(&eap->eap_item, &eas->eas_avail);
        wake_up(&eas->eas_waitq);
        spin_unlock_irqrestore(&eas->eas_lock, flags);
}

static struct obd_async_page_ops ec_async_page_ops = {
        .ap_make_ready =        ec_ap_make_ready,
        .ap_refresh_count =     ec_ap_refresh_count,
        .ap_fill_obdo =         ec_ap_fill_obdo,
        .ap_completion =        ec_ap_completion,
};

static int echo_client_async_page(struct obd_export *exp, int rw,
                                   struct obdo *oa, struct lov_stripe_md *lsm,
                                   obd_off offset, obd_size count,
                                   obd_size batching)
{
        obd_count npages, i;
        struct echo_async_page *eap;
        struct echo_async_state eas;
        struct list_head *pos, *n;
        int rc = 0;
        unsigned long flags;
        LIST_HEAD(pages);
#if 0
        int                     verify;
        int                     gfp_mask;
        /* oa_id  == 0    => speed test (no verification) else...
         * oa & 1         => use HIGHMEM
         */
        verify = (oa->o_id != 0);
        gfp_mask = ((oa->o_id & 1) == 0) ? GFP_KERNEL : GFP_HIGHUSER;
#endif

        LASSERT(rw == OBD_BRW_WRITE || rw == OBD_BRW_READ);

        if (count <= 0 ||
            (count & (PAGE_SIZE - 1)) != 0 ||
            (lsm != NULL &&
             lsm->lsm_object_id != oa->o_id))
                return (-EINVAL);

        /* XXX think again with misaligned I/O */
        npages = batching >> PAGE_SHIFT;

        memcpy(&eas.eas_oa, oa, sizeof(*oa));
        eas.eas_next_offset = offset;
        eas.eas_end_offset = offset + count;
        spin_lock_init(&eas.eas_lock);
        init_waitqueue_head(&eas.eas_waitq);
        eas.eas_in_flight = 0;
        eas.eas_rc = 0;
        eas.eas_lsm = lsm;
        INIT_LIST_HEAD(&eas.eas_avail);

        /* prepare the group of pages that we're going to be keeping
         * in flight */
        for (i = 0; i < npages; i++) {
                struct page *page = alloc_page(GFP_KERNEL);
                if (page == NULL)
                        GOTO(out, rc = -ENOMEM);

                page->private = 0;
                list_add_tail(&page->list, &pages);

                OBD_ALLOC(eap, sizeof(*eap));
                if (eap == NULL)
                        GOTO(out, rc = -ENOMEM);

                eap->eap_magic = EAP_MAGIC;
                eap->eap_page = page;
                eap->eap_eas = &eas;
                page->private = (unsigned long)eap;
                list_add_tail(&eap->eap_item, &eas.eas_avail);
        }

        /* first we spin queueing io and being woken by its completion */
        spin_lock_irqsave(&eas.eas_lock, flags);
        for(;;) {
                int rc;

                /* sleep until we have a page to send */
                spin_unlock_irqrestore(&eas.eas_lock, flags);
                rc = wait_event_interruptible(eas.eas_waitq, 
                                              eas_should_wake(&eas));
                spin_lock_irqsave(&eas.eas_lock, flags);
                if (rc && !eas.eas_rc)
                        eas.eas_rc = rc;
                if (eas.eas_rc)
                        break;
                if (list_empty(&eas.eas_avail))
                        continue;
                eap = list_entry(eas.eas_avail.next, struct echo_async_page,
                                 eap_item);
                list_del(&eap->eap_item);
                spin_unlock_irqrestore(&eas.eas_lock, flags);

                /* unbind the eap from its old page offset */
                if (eap->eap_cookie != NULL) {
                        obd_teardown_async_page(exp, lsm, NULL, 
                                                eap->eap_cookie);
                        eap->eap_cookie = NULL;
                }

                eas.eas_next_offset += PAGE_SIZE;
                eap->eap_off = eas.eas_next_offset;

                rc = obd_prep_async_page(exp, lsm, NULL, eap->eap_page,
                                         eap->eap_off, &ec_async_page_ops,
                                         eap, &eap->eap_cookie);
                if (rc) {
                        spin_lock_irqsave(&eas.eas_lock, flags);
                        eas.eas_rc = rc;
                        break;
                }

                if (rw == OBD_BRW_WRITE)
                        echo_page_debug_setup(lsm, eap->eap_page, rw, oa->o_id,
                                              eap->eap_off, PAGE_SIZE);

                /* always asserts urgent, which isn't quite right */
                rc = obd_queue_async_io(exp, lsm, NULL, eap->eap_cookie,
                                        rw, 0, PAGE_SIZE, 0,
                                        ASYNC_READY | ASYNC_URGENT |
                                        ASYNC_COUNT_STABLE);
                spin_lock_irqsave(&eas.eas_lock, flags);
                if (rc && !eas.eas_rc) {
                        eas.eas_rc = rc;
                        break;
                }
                eas.eas_in_flight++;
                if (eas.eas_next_offset == eas.eas_end_offset)
                        break;
        } 

        /* still hold the eas_lock here.. */

        /* now we just spin waiting for all the rpcs to complete */
        while(eas.eas_in_flight) {
                spin_unlock_irqrestore(&eas.eas_lock, flags);
                wait_event_interruptible(eas.eas_waitq, 
                                         eas.eas_in_flight == 0);
                spin_lock_irqsave(&eas.eas_lock, flags);
        }
        spin_unlock_irqrestore(&eas.eas_lock, flags);

out:
        list_for_each_safe(pos, n, &pages) {
                struct page *page = list_entry(pos, struct page, list);

                list_del(&page->list);
                if (page->private != 0) {
                        eap = (struct echo_async_page *)page->private;
                        if (eap->eap_cookie != NULL)
                                obd_teardown_async_page(exp, lsm, NULL, 
                                                        eap->eap_cookie);
                        OBD_FREE(eap, sizeof(*eap));
                }
                __free_page(page);
        }

        RETURN(rc);
}

static int echo_client_prep_commit(struct obd_export *exp, int rw,
                                   struct obdo *oa, struct lov_stripe_md *lsm,
                                   obd_off offset, obd_size count,  
                                   obd_size batch, struct obd_trans_info *oti)
{
        struct obd_ioobj ioo;
        struct niobuf_local *lnb;
        struct niobuf_remote *rnb;
        obd_off off;
        obd_size npages, tot_pages;
        int i, ret = 0, err = 0;
        ENTRY;

        if (count <= 0 || (count & (PAGE_SIZE - 1)) != 0 ||
            (lsm != NULL && lsm->lsm_object_id != oa->o_id))
                RETURN(-EINVAL);

        npages = batch >> PAGE_SHIFT;
        tot_pages = count >> PAGE_SHIFT;

        OBD_ALLOC(lnb, npages * sizeof(struct niobuf_local));
        OBD_ALLOC(rnb, npages * sizeof(struct niobuf_remote));

        if (lnb == NULL || rnb == NULL)
                GOTO(out, ret = -ENOMEM);

        obdo_to_ioobj(oa, &ioo);

        off = offset;

        for(; tot_pages; tot_pages -= npages) {
                if (tot_pages < npages)
                        npages = tot_pages;

                for (i = 0; i < npages; i++, off += PAGE_SIZE) {
                        rnb[i].offset = off;
                        rnb[i].len = PAGE_SIZE;
                }

                /* XXX this can't be the best.. */
                memset(oti, 0, sizeof(*oti));
                ioo.ioo_bufcnt = npages;

                ret = obd_preprw(rw, exp, oa, 1, &ioo, npages, rnb, lnb, oti);
                if (ret != 0)
                        GOTO(out, ret);

                for (i = 0; i < npages; i++) {
                        struct page *page = lnb[i].page;

                        /* read past eof? */
                        if (page == NULL && lnb[i].rc == 0) 
                                continue;

                        if (rw == OBD_BRW_WRITE) 
                                echo_page_debug_setup(lsm, page, rw, oa->o_id, 
                                                      rnb[i].offset, 
                                                      rnb[i].len);
                        else
                                echo_page_debug_check(lsm, page, oa->o_id, 
                                                      rnb[i].offset, 
                                                      rnb[i].len);
                }

                ret = obd_commitrw(rw, exp, oa, 1, &ioo, npages, lnb, oti);
                if (ret != 0)
                        GOTO(out, ret);
                if (err)
                        GOTO(out, ret = err);
        }

out:
        if (lnb)
                OBD_FREE(lnb, npages * sizeof(struct niobuf_local));
        if (rnb)
                OBD_FREE(rnb, npages * sizeof(struct niobuf_remote));
        RETURN(ret);
}

int echo_client_brw_ioctl(int rw, struct obd_export *exp, 
                          struct obd_ioctl_data *data)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct obd_trans_info dummy_oti;
        struct ec_object *eco;
        int rc;
        ENTRY;

        rc = echo_get_object(&eco, obd, &data->ioc_obdo1);
        if (rc)
                RETURN(rc);

        memset(&dummy_oti, 0, sizeof(dummy_oti));

        data->ioc_obdo1.o_valid &= ~OBD_MD_FLHANDLE;
        data->ioc_obdo1.o_valid |= OBD_MD_FLGROUP;
        data->ioc_obdo1.o_gr = 2;

        switch((long)data->ioc_pbuf1) {
        case 1:
                if (data->ioc_pbuf2 == NULL) { // NULL user data pointer
                        rc = echo_client_kbrw(obd, rw, &data->ioc_obdo1,
                                              eco->eco_lsm, data->ioc_offset,
                                              data->ioc_count, &dummy_oti);
                } else {
#ifdef __KERNEL__
                        rc = echo_client_ubrw(obd, rw, &data->ioc_obdo1,
                                              eco->eco_lsm, data->ioc_offset,
                                              data->ioc_count, data->ioc_pbuf2,
                                              &dummy_oti);
#endif
                }
                break;
        case 2:
                rc = echo_client_async_page(ec->ec_exp, rw, &data->ioc_obdo1,
                                           eco->eco_lsm, data->ioc_offset,
                                           data->ioc_count, data->ioc_plen1);
                break;
        case 3:
                rc = echo_client_prep_commit(ec->ec_exp, rw, &data->ioc_obdo1,
                                            eco->eco_lsm, data->ioc_offset,
                                            data->ioc_count, data->ioc_plen1,
                                            &dummy_oti);
                break;
        default:
                rc = -EINVAL;
        }
        echo_put_object(eco);
        RETURN(rc);
}

static int
echo_ldlm_callback (struct ldlm_lock *lock, struct ldlm_lock_desc *new,
                    void *data, int flag)
{
        struct ec_object       *eco = (struct ec_object *)data;
        struct echo_client_obd *ec = &(eco->eco_device->u.echo_client);
        struct lustre_handle    lockh;
        struct list_head       *el;
        int                     found = 0;
        int                     rc;

        ldlm_lock2handle (lock, &lockh);

        /* #ifdef this out if we're not feeling paranoid */
        spin_lock (&ec->ec_lock);
        list_for_each (el, &ec->ec_objects) {
                found = (eco == list_entry(el, struct ec_object,
                                           eco_obj_chain));
                if (found)
                        break;
        }
        spin_unlock (&ec->ec_lock);
        LASSERT (found);

        switch (flag) {
        case LDLM_CB_BLOCKING:
                CDEBUG(D_INFO, "blocking callback on "LPX64", handle "LPX64"\n",
                       eco->eco_id, lockh.cookie);
                rc = ldlm_cli_cancel (&lockh);
                if (rc != ELDLM_OK)
                        CERROR ("ldlm_cli_cancel failed: %d\n", rc);
                break;

        case LDLM_CB_CANCELING:
                CDEBUG(D_INFO, "cancel callback on "LPX64", handle "LPX64"\n",
                       eco->eco_id, lockh.cookie);
                break;

        default:
                LBUG ();
        }

        return (0);
}

static int
echo_client_enqueue(struct obd_export *exp, struct obdo *oa,
                    int mode, obd_off offset, obd_size nob)
{
        struct obd_device      *obd = exp->exp_obd;
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct lustre_handle   *ulh = obdo_handle (oa);
        struct ec_object       *eco;
        struct ec_lock         *ecl;
        int                     flags;
        int                     rc;

        if (!(mode == LCK_PR || mode == LCK_PW))
                return -EINVAL;

        if ((offset & (PAGE_SIZE - 1)) != 0 ||
            (nob & (PAGE_SIZE - 1)) != 0)
                return -EINVAL;

        rc = echo_get_object (&eco, obd, oa);
        if (rc != 0)
                return rc;

        rc = -ENOMEM;
        OBD_ALLOC (ecl, sizeof (*ecl));
        if (ecl == NULL)
                goto failed_0;

        ecl->ecl_mode = mode;
        ecl->ecl_object = eco;
        ecl->ecl_policy.l_extent.start = offset;
        ecl->ecl_policy.l_extent.end =
                (nob == 0) ? ((obd_off) -1) : (offset + nob - 1);

        flags = 0;
        rc = obd_enqueue(ec->ec_exp, eco->eco_lsm, LDLM_EXTENT,
                         &ecl->ecl_policy, mode, &flags, echo_ldlm_callback,
                         ldlm_completion_ast, NULL, eco, sizeof(struct ost_lvb),
                         lustre_swab_ost_lvb, &ecl->ecl_lock_handle);
        if (rc != 0)
                goto failed_1;

        CDEBUG(D_INFO, "enqueue handle "LPX64"\n", ecl->ecl_lock_handle.cookie);

        /* NB ecl takes object ref from echo_get_object() above */
        spin_lock(&ec->ec_lock);

        list_add(&ecl->ecl_exp_chain, &exp->exp_ec_data.eced_locks);
        ulh->cookie = ecl->ecl_cookie = ec->ec_unique++;

        spin_unlock(&ec->ec_lock);

        oa->o_valid |= OBD_MD_FLHANDLE;
        return 0;

 failed_1:
        OBD_FREE (ecl, sizeof (*ecl));
 failed_0:
        echo_put_object (eco);
        return (rc);
}

static int
echo_client_cancel(struct obd_export *exp, struct obdo *oa)
{
        struct obd_device      *obd = exp->exp_obd;
        struct echo_client_obd *ec = &obd->u.echo_client;
        struct lustre_handle   *ulh = obdo_handle (oa);
        struct ec_lock         *ecl = NULL;
        int                     found = 0;
        struct list_head       *el;
        int                     rc;

        if ((oa->o_valid & OBD_MD_FLHANDLE) == 0)
                return -EINVAL;

        spin_lock (&ec->ec_lock);

        list_for_each (el, &exp->exp_ec_data.eced_locks) {
                ecl = list_entry (el, struct ec_lock, ecl_exp_chain);
                found = (ecl->ecl_cookie == ulh->cookie);
                if (found) {
                        list_del (&ecl->ecl_exp_chain);
                        break;
                }
        }

        spin_unlock (&ec->ec_lock);

        if (!found)
                return (-ENOENT);

        rc = obd_cancel(ec->ec_exp, ecl->ecl_object->eco_lsm, ecl->ecl_mode,
                        &ecl->ecl_lock_handle);

        echo_put_object (ecl->ecl_object);
        OBD_FREE (ecl, sizeof (*ecl));

        return rc;
}

static int
echo_client_iocontrol(unsigned int cmd, struct obd_export *exp,
                      int len, void *karg, void *uarg)
{
        struct obd_device      *obd;
        struct echo_client_obd *ec;
        struct ec_object       *eco;
        struct obd_ioctl_data  *data = karg;
        struct obd_trans_info   dummy_oti;
        struct oti_req_ack_lock *ack_lock;
        struct obdo            *oa;
        int                     rw = OBD_BRW_READ;
        int                     rc = 0;
        int                     i;
        ENTRY;

        memset(&dummy_oti, 0, sizeof(dummy_oti));

        obd = exp->exp_obd;
        ec = &obd->u.echo_client;

        switch (cmd) {
        case OBD_IOC_CREATE:                    /* may create echo object */
                if (!capable (CAP_SYS_ADMIN))
                        GOTO (out, rc = -EPERM);

                rc = echo_create_object (obd, 1, &data->ioc_obdo1,
                                         data->ioc_pbuf1, data->ioc_plen1,
                                         &dummy_oti);
                GOTO(out, rc);

        case OBD_IOC_DESTROY:
                if (!capable (CAP_SYS_ADMIN))
                        GOTO (out, rc = -EPERM);

                rc = echo_get_object (&eco, obd, &data->ioc_obdo1);
                if (rc == 0) {
                        oa = &data->ioc_obdo1;
                        oa->o_gr = 2;
                        oa->o_valid |= OBD_MD_FLGROUP;
                        rc = obd_destroy(ec->ec_exp, oa, eco->eco_lsm, 
                                         &dummy_oti);
                        if (rc == 0)
                                eco->eco_deleted = 1;
                        echo_put_object(eco);
                }
                GOTO(out, rc);

        case OBD_IOC_GETATTR:
                rc = echo_get_object (&eco, obd, &data->ioc_obdo1);
                if (rc == 0) {
                        rc = obd_getattr(ec->ec_exp, &data->ioc_obdo1,
                                         eco->eco_lsm);
                        echo_put_object(eco);
                }
                GOTO(out, rc);

        case OBD_IOC_SETATTR:
                if (!capable (CAP_SYS_ADMIN))
                        GOTO (out, rc = -EPERM);

                rc = echo_get_object (&eco, obd, &data->ioc_obdo1);
                if (rc == 0) {
                        rc = obd_setattr(ec->ec_exp, &data->ioc_obdo1,
                                         eco->eco_lsm, NULL);
                        echo_put_object(eco);
                }
                GOTO(out, rc);

        case OBD_IOC_BRW_WRITE:
                if (!capable (CAP_SYS_ADMIN))
                        GOTO (out, rc = -EPERM);

                rw = OBD_BRW_WRITE;
                /* fall through */
        case OBD_IOC_BRW_READ:
                rc = echo_client_brw_ioctl(rw, exp, data);
                GOTO(out, rc);

        case ECHO_IOC_GET_STRIPE:
                rc = echo_get_object(&eco, obd, &data->ioc_obdo1);
                if (rc == 0) {
                        rc = echo_copyout_lsm(eco->eco_lsm, data->ioc_pbuf1,
                                              data->ioc_plen1);
                        echo_put_object(eco);
                }
                GOTO(out, rc);

        case ECHO_IOC_SET_STRIPE:
                if (!capable (CAP_SYS_ADMIN))
                        GOTO (out, rc = -EPERM);

                if (data->ioc_pbuf1 == NULL) {  /* unset */
                        rc = echo_get_object(&eco, obd, &data->ioc_obdo1);
                        if (rc == 0) {
                                eco->eco_deleted = 1;
                                echo_put_object(eco);
                        }
                } else {
                        rc = echo_create_object(obd, 0, &data->ioc_obdo1,
                                                data->ioc_pbuf1,
                                                data->ioc_plen1, &dummy_oti);
                }
                GOTO (out, rc);

        case ECHO_IOC_ENQUEUE:
                if (!capable (CAP_SYS_ADMIN))
                        GOTO (out, rc = -EPERM);

                rc = echo_client_enqueue(exp, &data->ioc_obdo1,
                                         data->ioc_conn1, /* lock mode */
                                   data->ioc_offset, data->ioc_count);/*extent*/
                GOTO (out, rc);

        case ECHO_IOC_CANCEL:
                rc = echo_client_cancel(exp, &data->ioc_obdo1);
                GOTO (out, rc);

        default:
                CERROR ("echo_ioctl(): unrecognised ioctl %#x\n", cmd);
                GOTO (out, rc = -ENOTTY);
        }

        EXIT;
 out:

        /* XXX this should be in a helper also called by target_send_reply */
        for (ack_lock = dummy_oti.oti_ack_locks, i = 0; i < 4; 
             i++, ack_lock++) {
                if (!ack_lock->mode)
                        break;
                ldlm_lock_decref(&ack_lock->lock, ack_lock->mode);
        }

        return rc;
}

static int
echo_client_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct lustre_cfg* lcfg = buf;
        struct echo_client_obd *ec = &obddev->u.echo_client;
        struct obd_device *tgt;
        struct lustre_handle conn = {0, };
        struct obd_uuid echo_uuid = { "ECHO_UUID" };
        int rc;
        ENTRY;

        if (lcfg->lcfg_inllen1 < 1) {
                CERROR("requires a TARGET OBD name\n");
                RETURN(-EINVAL);
        }

        tgt = class_name2obd(lcfg->lcfg_inlbuf1);
        if (!tgt || !tgt->obd_attached || !tgt->obd_set_up) {
                CERROR("device not attached or not set up (%s)\n",
                       lcfg->lcfg_inlbuf1);
                RETURN(-EINVAL);
        }

        spin_lock_init (&ec->ec_lock);
        INIT_LIST_HEAD (&ec->ec_objects);
        ec->ec_unique = 0;

        rc = obd_connect(&conn, tgt, &echo_uuid);
        if (rc) {
                CERROR("fail to connect to device %s\n", lcfg->lcfg_inlbuf1);
                return (rc);
        }
        ec->ec_exp = class_conn2export(&conn);

        RETURN(rc);
}

static int echo_client_cleanup(struct obd_device *obddev, int flags)
{
        struct list_head       *el;
        struct ec_object       *eco;
        struct echo_client_obd *ec = &obddev->u.echo_client;
        int rc;
        ENTRY;

        if (!list_empty(&obddev->obd_exports)) {
                CERROR("still has clients!\n");
                RETURN(-EBUSY);
        }

        /* XXX assuming sole access */
        while (!list_empty(&ec->ec_objects)) {
                el = ec->ec_objects.next;
                eco = list_entry(el, struct ec_object, eco_obj_chain);

                LASSERT(eco->eco_refcount == 0);
                eco->eco_refcount = 1;
                eco->eco_deleted = 1;
                echo_put_object(eco);
        }

        rc = obd_disconnect(ec->ec_exp, 0);
        if (rc != 0)
                CERROR("fail to disconnect device: %d\n", rc);

        RETURN(rc);
}

static int echo_client_connect(struct lustre_handle *conn,
                               struct obd_device *src, struct obd_uuid *cluuid)
{
        struct obd_export *exp;
        int                rc;

        rc = class_connect(conn, src, cluuid);
        if (rc == 0) {
                exp = class_conn2export(conn);
                INIT_LIST_HEAD(&exp->exp_ec_data.eced_locks);
                class_export_put(exp);
        }

        RETURN (rc);
}

static int echo_client_disconnect(struct obd_export *exp, int flags)
{
        struct obd_device      *obd;
        struct echo_client_obd *ec;
        struct ec_lock         *ecl;
        int                     rc;
        ENTRY;

        if (exp == NULL)
                GOTO(out, rc = -EINVAL);

        obd = exp->exp_obd;
        ec = &obd->u.echo_client;

        /* no more contention on export's lock list */
        while (!list_empty (&exp->exp_ec_data.eced_locks)) {
                ecl = list_entry (exp->exp_ec_data.eced_locks.next,
                                  struct ec_lock, ecl_exp_chain);
                list_del (&ecl->ecl_exp_chain);

                rc = obd_cancel(ec->ec_exp, ecl->ecl_object->eco_lsm,
                                 ecl->ecl_mode, &ecl->ecl_lock_handle);

                CDEBUG (D_INFO, "Cancel lock on object "LPX64" on disconnect "
                        "(%d)\n", ecl->ecl_object->eco_id, rc);

                echo_put_object (ecl->ecl_object);
                OBD_FREE (ecl, sizeof (*ecl));
        }

        rc = class_disconnect(exp, 0);
        GOTO(out, rc);
 out:
        return rc;
}

static struct obd_ops echo_obd_ops = {
        o_owner:       THIS_MODULE,
        o_setup:       echo_client_setup,
        o_cleanup:     echo_client_cleanup,
        o_iocontrol:   echo_client_iocontrol,
        o_connect:     echo_client_connect,
        o_disconnect:  echo_client_disconnect
};

int echo_client_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(echo, &lvars);
        return class_register_type(&echo_obd_ops, lvars.module_vars,
                                   OBD_ECHO_CLIENT_DEVICENAME);
}

void echo_client_exit(void)
{
        class_unregister_type(OBD_ECHO_CLIENT_DEVICENAME);
}
