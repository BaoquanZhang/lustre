/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
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

#define EXPORT_SYMTAB

#include <linux/version.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/ext2_fs.h>
#include <linux/quotaops.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <asm/unistd.h>

#define DEBUG_SUBSYSTEM S_ECHO

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/obd_echo.h>
#include <linux/lustre_debug.h>
#include <linux/lustre_dlm.h>
#include <linux/lprocfs_status.h>

#define ECHO_INIT_OBJID      0x1000000000000000ULL
#define ECHO_HANDLE_MAGIC    0xabcd0123fedc9876ULL

#define ECHO_OBJECT0_NPAGES  16
static struct page *echo_object0_pages[ECHO_OBJECT0_NPAGES];

enum {
        LPROC_ECHO_READ_BYTES = 1,
        LPROC_ECHO_WRITE_BYTES = 2,
        LPROC_ECHO_LAST = LPROC_ECHO_WRITE_BYTES +1
};

static int echo_connect(struct lustre_handle *conn, struct obd_device *obd,
                        struct obd_uuid *cluuid)
{
        return class_connect(conn, obd, cluuid);
}

static int echo_disconnect(struct lustre_handle *conn, int flags)
{
        struct obd_export *exp = class_conn2export(conn);

        LASSERT (exp != NULL);

        ldlm_cancel_locks_for_export(exp);
        class_export_put(exp);
        return class_disconnect(conn, flags);
}

static __u64 echo_next_id(struct obd_device *obddev)
{
        obd_id id;

        spin_lock(&obddev->u.echo.eo_lock);
        id = ++obddev->u.echo.eo_lastino;
        spin_unlock(&obddev->u.echo.eo_lock);

        return id;
}

int echo_create(struct lustre_handle *conn, struct obdo *oa,
                struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct obd_device *obd = class_conn2obd(conn);

        if (!obd) {
                CERROR("invalid client cookie "LPX64"\n", conn->cookie);
                return -EINVAL;
        }

        if (!(oa->o_mode && S_IFMT)) {
                CERROR("echo obd: no type!\n");
                return -ENOENT;
        }

        if (!(oa->o_valid & OBD_MD_FLTYPE)) {
                CERROR("invalid o_valid %08x\n", oa->o_valid);
                return -EINVAL;
        }

        oa->o_id = echo_next_id(obd);
        oa->o_valid = OBD_MD_FLID;
        atomic_inc(&obd->u.echo.eo_create);

        return 0;
}

int echo_destroy(struct lustre_handle *conn, struct obdo *oa,
                 struct lov_stripe_md *ea, struct obd_trans_info *oti)
{
        struct obd_device *obd = class_conn2obd(conn);

        if (!obd) {
                CERROR("invalid client cookie "LPX64"\n", conn->cookie);
                RETURN(-EINVAL);
        }

        if (!(oa->o_valid & OBD_MD_FLID)) {
                CERROR("obdo missing FLID valid flag: %08x\n", oa->o_valid);
                RETURN(-EINVAL);
        }

        if (oa->o_id > obd->u.echo.eo_lastino || oa->o_id < ECHO_INIT_OBJID) {
                CERROR("bad destroy objid: "LPX64"\n", oa->o_id);
                RETURN(-EINVAL);
        }

        atomic_inc(&obd->u.echo.eo_destroy);

        return 0;
}

static int echo_open(struct lustre_handle *conn, struct obdo *oa,
                     struct lov_stripe_md *md, struct obd_trans_info *oti,
                     struct obd_client_handle *och)
{
        struct lustre_handle *fh = obdo_handle (oa);
        struct obd_device    *obd = class_conn2obd (conn);

        if (!obd) {
                CERROR("invalid client cookie "LPX64"\n", conn->cookie);
                return (-EINVAL);
        }

        if (!(oa->o_valid & OBD_MD_FLID)) {
                CERROR ("obdo missing FLID valid flag: %08x\n", oa->o_valid);
                return (-EINVAL);
        }

        fh->cookie = ECHO_HANDLE_MAGIC;

        oa->o_valid |= OBD_MD_FLHANDLE;
        return 0;
}

static int echo_close(struct lustre_handle *conn, struct obdo *oa,
                      struct lov_stripe_md *md, struct obd_trans_info *oti)
{
        struct lustre_handle *fh = obdo_handle (oa);
        struct obd_device    *obd = class_conn2obd(conn);

        if (!obd) {
                CERROR("invalid client cookie "LPX64"\n", conn->cookie);
                return (-EINVAL);
        }

        if (!(oa->o_valid & OBD_MD_FLHANDLE)) {
                CERROR("obdo missing FLHANDLE valid flag: %08x\n", oa->o_valid);
                return (-EINVAL);
        }

        if (fh->cookie != ECHO_HANDLE_MAGIC) {
                CERROR ("invalid file handle on close: "LPX64"\n", fh->cookie);
                return (-EINVAL);
        }

        return 0;
}

static int echo_getattr(struct lustre_handle *conn, struct obdo *oa,
                        struct lov_stripe_md *md)
{
        struct obd_device *obd = class_conn2obd(conn);
        obd_id id = oa->o_id;

        if (!obd) {
                CERROR("invalid client cookie "LPX64"\n", conn->cookie);
                RETURN(-EINVAL);
        }

        if (!(oa->o_valid & OBD_MD_FLID)) {
                CERROR("obdo missing FLID valid flag: %08x\n", oa->o_valid);
                RETURN(-EINVAL);
        }

        obdo_cpy_md(oa, &obd->u.echo.oa, oa->o_valid);
        oa->o_id = id;

        return 0;
}

static int echo_setattr(struct lustre_handle *conn, struct obdo *oa,
                        struct lov_stripe_md *md, struct obd_trans_info *oti)
{
        struct obd_device *obd = class_conn2obd(conn);

        if (!obd) {
                CERROR("invalid client cookie "LPX64"\n", conn->cookie);
                RETURN(-EINVAL);
        }

        if (!(oa->o_valid & OBD_MD_FLID)) {
                CERROR("obdo missing FLID valid flag: %08x\n", oa->o_valid);
                RETURN(-EINVAL);
        }

        memcpy(&obd->u.echo.oa, oa, sizeof(*oa));

        atomic_inc(&obd->u.echo.eo_setattr);

        return 0;
}

/* This allows us to verify that desc_private is passed unmolested */
#define DESC_PRIV 0x10293847

int echo_preprw(int cmd, struct obd_export *export, struct obdo *oa,
                int objcount, struct obd_ioobj *obj, int niocount,
                struct niobuf_remote *nb, struct niobuf_local *res,
                struct obd_trans_info *oti)
{
        struct obd_device *obd;
        struct niobuf_local *r = res;
        int tot_bytes = 0;
        int rc = 0;
        int i;
        ENTRY;

        obd = export->exp_obd;
        if (obd == NULL)
                RETURN(-EINVAL);

        memset(res, 0, sizeof(*res) * niocount);

        CDEBUG(D_PAGE, "%s %d obdos with %d IOs\n",
               cmd == OBD_BRW_READ ? "reading" : "writing", objcount, niocount);

        if (oti)
                oti->oti_handle = (void *)DESC_PRIV;

        for (i = 0; i < objcount; i++, obj++) {
                int gfp_mask = (obj->ioo_id & 1) ? GFP_HIGHUSER : GFP_KERNEL;
                int isobj0 = obj->ioo_id == 0;
                int verify = !isobj0;
                int j;

                for (j = 0 ; j < obj->ioo_bufcnt ; j++, nb++, r++) {

                        if (isobj0 &&
                            (nb->offset >> PAGE_SHIFT) < ECHO_OBJECT0_NPAGES) {
                                r->page = echo_object0_pages[nb->offset >>
                                                             PAGE_SHIFT];
                                /* Take extra ref so __free_pages() can be called OK */
                                get_page (r->page);
                        } else {
                                r->page = alloc_pages(gfp_mask, 0);
                                if (r->page == NULL) {
                                        CERROR("can't get page %u/%u for id "
                                               LPU64"\n",
                                               j, obj->ioo_bufcnt, obj->ioo_id);
                                        GOTO(preprw_cleanup, rc = -ENOMEM);
                                }
                        }

                        tot_bytes += r->len;

                        atomic_inc(&obd->u.echo.eo_prep);

                        r->offset = nb->offset;
                        r->len = nb->len;
                        LASSERT((r->offset & ~PAGE_MASK) + r->len <= PAGE_SIZE);

                        CDEBUG(D_PAGE, "$$$$ get page %p @ "LPU64" for %d\n",
                               r->page, r->offset, r->len);

                        if (cmd & OBD_BRW_READ) {
                                r->rc = r->len;
                                if (verify) {
                                        page_debug_setup(kmap (r->page), r->len,
                                                         r->offset,obj->ioo_id);
                                        kunmap (r->page);
                                }
                                r->rc = r->len;
                        } else {
                                if (verify) {
                                        page_debug_setup(kmap (r->page), r->len,
                                                         0xecc0ecc0ecc0ecc0,
                                                         0xecc0ecc0ecc0ecc0);
                                        kunmap (r->page);
                                }
                        }
                }
        }
        if (cmd & OBD_BRW_READ)
                lprocfs_counter_add(obd->obd_stats, LPROC_ECHO_READ_BYTES,
                                    tot_bytes);
        else
                lprocfs_counter_add(obd->obd_stats, LPROC_ECHO_WRITE_BYTES,
                                    tot_bytes);

        CDEBUG(D_PAGE, "%d pages allocated after prep\n",
               atomic_read(&obd->u.echo.eo_prep));

        RETURN(0);

preprw_cleanup:
        /* It is possible that we would rather handle errors by  allow
         * any already-set-up pages to complete, rather than tearing them
         * all down again.  I believe that this is what the in-kernel
         * prep/commit operations do.
         */
        CERROR("cleaning up %ld pages (%d obdos)\n", (long)(r - res), objcount);
        while (r-- > res) {
                kunmap(r->page);
                /* NB if this is an 'object0' page, __free_pages will just
                 * lose the extra ref gained above */
                __free_pages(r->page, 0);
                atomic_dec(&obd->u.echo.eo_prep);
        }
        memset(res, 0, sizeof(*res) * niocount);

        return rc;
}

int echo_commitrw(int cmd, struct obd_export *export, struct obdo *oa,
                  int objcount, struct obd_ioobj *obj, int niocount,
                  struct niobuf_local *res, struct obd_trans_info *oti)
{
        struct obd_device *obd;
        struct niobuf_local *r = res;
        int i, vrc = 0, rc = 0;
        ENTRY;

        obd = export->exp_obd;
        if (obd == NULL)
                RETURN(-EINVAL);

        if ((cmd & OBD_BRW_RWMASK) == OBD_BRW_READ) {
                CDEBUG(D_PAGE, "reading %d obdos with %d IOs\n",
                       objcount, niocount);
        } else {
                CDEBUG(D_PAGE, "writing %d obdos with %d IOs\n",
                       objcount, niocount);
        }

        if (niocount && !r) {
                CERROR("NULL res niobuf with niocount %d\n", niocount);
                RETURN(-EINVAL);
        }

        LASSERT(oti == NULL || oti->oti_handle == (void *)DESC_PRIV);

        for (i = 0; i < objcount; i++, obj++) {
                int verify = obj->ioo_id != 0;
                int j;

                for (j = 0 ; j < obj->ioo_bufcnt ; j++, r++) {
                        struct page *page = r->page;
                        void *addr;

                        if (!page || !(addr = kmap(page)) ||
                            !kern_addr_valid(addr)) {

                                CERROR("bad page objid "LPU64":%p, buf %d/%d\n",
                                       obj->ioo_id, page, j, obj->ioo_bufcnt);
                                kunmap(page);
                                GOTO(commitrw_cleanup, rc = -EFAULT);
                        }

                        CDEBUG(D_PAGE, "$$$$ use page %p, addr %p@"LPU64"\n",
                               r->page, addr, r->offset);

                        if (verify) {
                                vrc = page_debug_check("echo", addr, r->len,
                                                       r->offset, obj->ioo_id);
                                /* check all the pages always */
                                if (vrc != 0 && rc == 0)
                                        rc = vrc;
                        }

                        kunmap(page);
                        /* NB see comment above regarding object0 pages */
                        __free_pages(page, 0);
                        atomic_dec(&obd->u.echo.eo_prep);
                }
        }
        CDEBUG(D_PAGE, "%d pages remain after commit\n",
               atomic_read(&obd->u.echo.eo_prep));
        RETURN(rc);

commitrw_cleanup:
        CERROR("cleaning up %ld pages (%d obdos)\n",
               niocount - (long)(r - res) - 1, objcount);
        while (++r < res + niocount) {
                struct page *page = r->page;

                /* NB see comment above regarding object0 pages */
                __free_pages(page, 0);
                atomic_dec(&obd->u.echo.eo_prep);
        }
        return rc;
}

static int echo_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        ENTRY;

        spin_lock_init(&obddev->u.echo.eo_lock);
        obddev->u.echo.eo_lastino = ECHO_INIT_OBJID;

        obddev->obd_namespace =
                ldlm_namespace_new("echo-tgt", LDLM_NAMESPACE_SERVER);
        if (obddev->obd_namespace == NULL) {
                LBUG();
                RETURN(-ENOMEM);
        }

        ptlrpc_init_client (LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                            "echo_ldlm_cb_client", &obddev->obd_ldlm_client);
        RETURN(0);
}

static int echo_cleanup(struct obd_device *obddev, int flags)
{
        ENTRY;

        ldlm_namespace_free(obddev->obd_namespace);
        CERROR("%d prep/commitrw pages leaked\n",
               atomic_read(&obddev->u.echo.eo_prep));

        RETURN(0);
}

int echo_attach(struct obd_device *obd, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;
        int rc;

        lprocfs_init_vars(echo, &lvars);
        rc = lprocfs_obd_attach(obd, lvars.obd_vars);
        if (rc != 0)
                return rc;
        rc = lprocfs_alloc_obd_stats(obd, LPROC_ECHO_LAST);
        if (rc != 0)
                return rc;

        lprocfs_counter_init(obd->obd_stats, LPROC_ECHO_READ_BYTES,
                             LPROCFS_CNTR_AVGMINMAX, "read_bytes", "bytes");
        lprocfs_counter_init(obd->obd_stats, LPROC_ECHO_WRITE_BYTES,
                             LPROCFS_CNTR_AVGMINMAX, "write_bytes", "bytes");
        return rc;
}

int echo_detach(struct obd_device *dev)
{
        lprocfs_free_obd_stats(dev);
        return lprocfs_obd_detach(dev);
}

static struct obd_ops echo_obd_ops = {
        o_owner:       THIS_MODULE,
        o_attach:      echo_attach,
        o_detach:      echo_detach,
        o_connect:     echo_connect,
        o_disconnect:  echo_disconnect,
        o_create:      echo_create,
        o_destroy:     echo_destroy,
        o_open:        echo_open,
        o_close:       echo_close,
        o_getattr:     echo_getattr,
        o_setattr:     echo_setattr,
        o_preprw:      echo_preprw,
        o_commitrw:    echo_commitrw,
        o_setup:       echo_setup,
        o_cleanup:     echo_cleanup
};

extern int echo_client_init(void);
extern void echo_client_cleanup(void);

static void
echo_object0_pages_fini (void)
{
        int     i;

        for (i = 0; i < ECHO_OBJECT0_NPAGES; i++)
                if (echo_object0_pages[i] != NULL) {
                        __free_pages (echo_object0_pages[i], 0);
                        echo_object0_pages[i] = NULL;
                }
}

static int
echo_object0_pages_init (void)
{
        struct page *pg;
        int          i;

        for (i = 0; i < ECHO_OBJECT0_NPAGES; i++) {
                int gfp_mask = (i < ECHO_OBJECT0_NPAGES/2) ?
                        GFP_KERNEL : GFP_HIGHUSER;

                pg = alloc_pages (gfp_mask, 0);
                if (pg == NULL) {
                        echo_object0_pages_fini ();
                        return (-ENOMEM);
                }

                memset (kmap (pg), 0, PAGE_SIZE);
                kunmap (pg);

                echo_object0_pages[i] = pg;
        }

        return (0);
}

static int __init obdecho_init(void)
{
        struct lprocfs_static_vars lvars;
        int rc;

        printk(KERN_INFO "Lustre Echo OBD driver; info@clusterfs.com\n");

        lprocfs_init_vars(echo, &lvars);

        rc = echo_object0_pages_init ();
        if (rc != 0)
                goto failed_0;

        rc = class_register_type(&echo_obd_ops, lvars.module_vars,
                                 OBD_ECHO_DEVICENAME);
        if (rc != 0)
                goto failed_1;

        rc = echo_client_init();
        if (rc == 0)
                RETURN (0);

        class_unregister_type(OBD_ECHO_DEVICENAME);
 failed_1:
        echo_object0_pages_fini ();
 failed_0:
        RETURN(rc);
}

static void /*__exit*/ obdecho_exit(void)
{
        echo_client_cleanup();
        class_unregister_type(OBD_ECHO_DEVICENAME);
        echo_object0_pages_fini ();
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Testing Echo OBD driver");
MODULE_LICENSE("GPL");

module_init(obdecho_init);
module_exit(obdecho_exit);
