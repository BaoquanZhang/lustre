/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/fs/obdfilter/filter.c
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

/*
 * Invariant: Get O/R i_sem for lookup, if needed, before any journal ops
 *            (which need to get journal_lock, may block if journal full).
 *
 * Invariant: Call filter_start_transno() before any journal ops to avoid the
 *            same deadlock problem.  We can (and want) to get rid of the
 *            transno sem in favour of the dir/inode i_sem to avoid single
 *            threaded operation on the OST.
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/config.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/init.h>
#include <linux/version.h>
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
# include <linux/mount.h>
# include <linux/buffer_head.h>
#endif

#include <linux/obd_class.h>
#include <linux/obd_lov.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_log.h>
#include <linux/lustre_commit_confd.h>
#include <portals/list.h>

#include "filter_internal.h"

static struct lvfs_callback_ops filter_lvfs_ops;

static int filter_destroy(struct obd_export *exp, struct obdo *oa,
                          struct lov_stripe_md *ea, struct obd_trans_info *);

static void filter_commit_cb(struct obd_device *obd, __u64 transno,
                             void *cb_data, int error)
{
        obd_transno_commit_cb(obd, transno, error);
}

/* Assumes caller has already pushed us into the kernel context. */
int filter_finish_transno(struct obd_export *exp, struct obd_trans_info *oti,
                          int rc)
{
        struct filter_obd *filter = &exp->exp_obd->u.filter;
        struct filter_export_data *fed = &exp->exp_filter_data;
        struct filter_client_data *fcd = fed->fed_fcd;
        __u64 last_rcvd;
        loff_t off;
        int err, log_pri = D_HA;

        /* Propagate error code. */
        if (rc)
                RETURN(rc);

        if (!exp->exp_obd->obd_replayable || oti == NULL)
                RETURN(rc);

        /* we don't allocate new transnos for replayed requests */
        if (oti->oti_transno == 0) {
                spin_lock(&filter->fo_translock);
                last_rcvd = le64_to_cpu(filter->fo_fsd->fsd_last_transno) + 1;
                filter->fo_fsd->fsd_last_transno = cpu_to_le64(last_rcvd);
                spin_unlock(&filter->fo_translock);
                oti->oti_transno = last_rcvd;
        } else {
                spin_lock(&filter->fo_translock);
                last_rcvd = oti->oti_transno;
                if (last_rcvd > le64_to_cpu(filter->fo_fsd->fsd_last_transno))
                        filter->fo_fsd->fsd_last_transno =
                                cpu_to_le64(last_rcvd);
                spin_unlock(&filter->fo_translock);
        }
        fcd->fcd_last_rcvd = cpu_to_le64(last_rcvd);

        /* could get xid from oti, if it's ever needed */
        fcd->fcd_last_xid = 0;

        off = fed->fed_lr_off;

        fsfilt_add_journal_cb(exp->exp_obd, filter->fo_sb, last_rcvd,
                              oti->oti_handle, filter_commit_cb, NULL);

        err = fsfilt_write_record(exp->exp_obd, filter->fo_rcvd_filp, fcd,
                                  sizeof(*fcd), &off, 0);
        if (err) {
                log_pri = D_ERROR;
                if (rc == 0)
                        rc = err;
        }

        CDEBUG(log_pri, "wrote trans "LPU64" for client %s at #%d: err = %d\n",
               last_rcvd, fcd->fcd_uuid, fed->fed_lr_idx, err);

        RETURN(rc);
}

void f_dput(struct dentry *dentry)
{
        /* Can't go inside filter_ddelete because it can block */
        CDEBUG(D_INODE, "putting %s: %p, count = %d\n",
               dentry->d_name.name, dentry, atomic_read(&dentry->d_count) - 1);
        LASSERT(atomic_read(&dentry->d_count) > 0);

        dput(dentry);
}

/* Add client data to the FILTER.  We use a bitmap to locate a free space
 * in the last_rcvd file if cl_idx is -1 (i.e. a new client).
 * Otherwise, we have just read the data from the last_rcvd file and
 * we know its offset. */
static int filter_client_add(struct obd_device *obd, struct filter_obd *filter,
                             struct filter_export_data *fed, int cl_idx)
{
        unsigned long *bitmap = filter->fo_last_rcvd_slots;
        int new_client = (cl_idx == -1);
        ENTRY;

        LASSERT(bitmap != NULL);

        /* XXX if fcd_uuid were a real obd_uuid, I could use obd_uuid_equals */
        if (!strcmp(fed->fed_fcd->fcd_uuid, obd->obd_uuid.uuid))
                RETURN(0);

        /* the bitmap operations can handle cl_idx > sizeof(long) * 8, so
         * there's no need for extra complication here
         */
        if (new_client) {
                cl_idx = find_first_zero_bit(bitmap, FILTER_LR_MAX_CLIENTS);
        repeat:
                if (cl_idx >= FILTER_LR_MAX_CLIENTS) {
                        CERROR("no client slots - fix FILTER_LR_MAX_CLIENTS\n");
                        RETURN(-ENOMEM);
                }
                if (test_and_set_bit(cl_idx, bitmap)) {
                        CERROR("FILTER client %d: found bit is set in bitmap\n",
                               cl_idx);
                        cl_idx = find_next_zero_bit(bitmap,
                                                    FILTER_LR_MAX_CLIENTS,
                                                    cl_idx);
                        goto repeat;
                }
        } else {
                if (test_and_set_bit(cl_idx, bitmap)) {
                        CERROR("FILTER client %d: bit already set in bitmap!\n",
                               cl_idx);
                        LBUG();
                }
        }

        fed->fed_lr_idx = cl_idx;
        fed->fed_lr_off = le32_to_cpu(filter->fo_fsd->fsd_client_start) +
                cl_idx * le16_to_cpu(filter->fo_fsd->fsd_client_size);

        CDEBUG(D_INFO, "client at index %d (%llu) with UUID '%s' added\n",
               fed->fed_lr_idx, fed->fed_lr_off, fed->fed_fcd->fcd_uuid);

        if (new_client) {
                struct lvfs_run_ctxt saved;
                loff_t off = fed->fed_lr_off;
                int err;
                void *handle;

                CDEBUG(D_INFO, "writing client fcd at idx %u (%llu) (len %u)\n",
                       fed->fed_lr_idx,off,(unsigned int)sizeof(*fed->fed_fcd));

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                /* Transaction needed to fix bug 1403 */
                handle = fsfilt_start(obd,
                                      filter->fo_rcvd_filp->f_dentry->d_inode,
                                      FSFILT_OP_SETATTR, NULL);
                if (IS_ERR(handle)) {
                        err = PTR_ERR(handle);
                        CERROR("unable to start transaction: rc %d\n", err);
                } else {
                        err = fsfilt_write_record(obd, filter->fo_rcvd_filp,
                                                  fed->fed_fcd,
                                                  sizeof(*fed->fed_fcd),
                                                  &off, 1);
                        fsfilt_commit(obd, filter->fo_sb,
                                      filter->fo_rcvd_filp->f_dentry->d_inode,
                                      handle, 1);
                }
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                if (err) {
                        CERROR("error writing %s client idx %u: rc %d\n",
                               LAST_RCVD, fed->fed_lr_idx, err);
                        RETURN(err);
                }
        }
        RETURN(0);
}

static int filter_client_free(struct obd_export *exp, int flags)
{
        struct filter_export_data *fed = &exp->exp_filter_data;
        struct filter_obd *filter = &exp->exp_obd->u.filter;
        struct obd_device *obd = exp->exp_obd;
        struct filter_client_data zero_fcd;
        struct lvfs_run_ctxt saved;
        int rc;
        loff_t off;
        ENTRY;

        if (fed->fed_fcd == NULL)
                RETURN(0);

        if (flags & OBD_OPT_FAILOVER)
                GOTO(free, 0);

        /* XXX if fcd_uuid were a real obd_uuid, I could use obd_uuid_equals */
        if (strcmp(fed->fed_fcd->fcd_uuid, obd->obd_uuid.uuid ) == 0)
                GOTO(free, 0);

        LASSERT(filter->fo_last_rcvd_slots != NULL);

        off = fed->fed_lr_off;

        CDEBUG(D_INFO, "freeing client at idx %u (%lld) with UUID '%s'\n",
               fed->fed_lr_idx, fed->fed_lr_off, fed->fed_fcd->fcd_uuid);

        /* Clear the bit _after_ zeroing out the client so we don't
           race with filter_client_add and zero out new clients.*/
        if (!test_bit(fed->fed_lr_idx, filter->fo_last_rcvd_slots)) {
                CERROR("FILTER client %u: bit already clear in bitmap!!\n",
                       fed->fed_lr_idx);
                LBUG();
        }

        memset(&zero_fcd, 0, sizeof zero_fcd);
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = fsfilt_write_record(obd, filter->fo_rcvd_filp, &zero_fcd,
                                 sizeof(zero_fcd), &off, 1);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        CDEBUG(rc == 0 ? D_INFO : D_ERROR,
               "zeroing disconnecting client %s at idx %u (%llu) in %s rc %d\n",
               fed->fed_fcd->fcd_uuid, fed->fed_lr_idx, fed->fed_lr_off,
               LAST_RCVD, rc);

        if (!test_and_clear_bit(fed->fed_lr_idx, filter->fo_last_rcvd_slots)) {
                CERROR("FILTER client %u: bit already clear in bitmap!!\n",
                       fed->fed_lr_idx);
                LBUG();
        }

free:
        OBD_FREE(fed->fed_fcd, sizeof(*fed->fed_fcd));

        RETURN(0);
}

static int filter_free_server_data(struct filter_obd *filter)
{
        OBD_FREE(filter->fo_fsd, sizeof(*filter->fo_fsd));
        filter->fo_fsd = NULL;
        OBD_FREE(filter->fo_last_rcvd_slots,
                 FILTER_LR_MAX_CLIENT_WORDS * sizeof(unsigned long));
        filter->fo_last_rcvd_slots = NULL;
        return 0;
}

/* assumes caller is already in kernel ctxt */
int filter_update_server_data(struct obd_device *obd, struct file *filp,
                              struct filter_server_data *fsd, int force_sync)
{
        loff_t off = 0;
        int rc;
        ENTRY;

        CDEBUG(D_INODE, "server uuid      : %s\n", fsd->fsd_uuid);
        CDEBUG(D_INODE, "server last_rcvd : "LPU64"\n",
               le64_to_cpu(fsd->fsd_last_transno));
        CDEBUG(D_INODE, "server last_mount: "LPU64"\n",
               le64_to_cpu(fsd->fsd_mount_count));

        rc = fsfilt_write_record(obd, filp, fsd, sizeof(*fsd), &off,force_sync);
        if (rc)
                CERROR("error writing filter_server_data: rc = %d\n", rc);

        RETURN(rc);
}

int filter_update_last_objid(struct obd_device *obd, obd_gr group,
                             int force_sync)
{
        struct filter_obd *filter = &obd->u.filter;
        __u64 tmp;
        loff_t off = 0;
        int rc;
        ENTRY;

        if (filter->fo_last_objid_files[group] == NULL) {
                CERROR("Object group "LPU64" not fully setup; not updating "
                       "last_objid\n", group);
                RETURN(0);
        }

        CDEBUG(D_INODE, "server last_objid for group "LPU64": "LPU64"\n",
               group, filter->fo_last_objids[group]);

        tmp = cpu_to_le64(filter->fo_last_objids[group]);
        rc = fsfilt_write_record(obd, filter->fo_last_objid_files[group],
                                 &tmp, sizeof(tmp), &off, force_sync);
        if (rc)
                CERROR("error writing group "LPU64" last objid: rc = %d\n",
                       group, rc);
        RETURN(rc);
}

/* assumes caller has already in kernel ctxt */
static int filter_init_server_data(struct obd_device *obd, struct file * filp)
{
        struct filter_obd *filter = &obd->u.filter;
        struct filter_server_data *fsd;
        struct filter_client_data *fcd = NULL;
        struct inode *inode = filp->f_dentry->d_inode;
        unsigned long last_rcvd_size = inode->i_size;
        __u64 mount_count;
        int cl_idx;
        loff_t off = 0;
        int rc;

        /* ensure padding in the struct is the correct size */
        LASSERT (offsetof(struct filter_server_data, fsd_padding) +
                 sizeof(fsd->fsd_padding) == FILTER_LR_SERVER_SIZE);
        LASSERT (offsetof(struct filter_client_data, fcd_padding) +
                 sizeof(fcd->fcd_padding) == FILTER_LR_CLIENT_SIZE);

        OBD_ALLOC(fsd, sizeof(*fsd));
        if (!fsd)
                RETURN(-ENOMEM);
        filter->fo_fsd = fsd;

        OBD_ALLOC(filter->fo_last_rcvd_slots,
                  FILTER_LR_MAX_CLIENT_WORDS * sizeof(unsigned long));
        if (filter->fo_last_rcvd_slots == NULL) {
                OBD_FREE(fsd, sizeof(*fsd));
                RETURN(-ENOMEM);
        }

        if (last_rcvd_size == 0) {
                CWARN("%s: initializing new %s\n", obd->obd_name, LAST_RCVD);

                memcpy(fsd->fsd_uuid, obd->obd_uuid.uuid,sizeof(fsd->fsd_uuid));
                fsd->fsd_last_transno = 0;
                mount_count = fsd->fsd_mount_count = 0;
                fsd->fsd_server_size = cpu_to_le32(FILTER_LR_SERVER_SIZE);
                fsd->fsd_client_start = cpu_to_le32(FILTER_LR_CLIENT_START);
                fsd->fsd_client_size = cpu_to_le16(FILTER_LR_CLIENT_SIZE);
                fsd->fsd_subdir_count = cpu_to_le16(FILTER_SUBDIR_COUNT);
                filter->fo_subdir_count = FILTER_SUBDIR_COUNT;
        } else {
                rc = fsfilt_read_record(obd, filp, fsd, sizeof(*fsd), &off);
                if (rc) {
                        CDEBUG(D_INODE,"OBD filter: error reading %s: rc %d\n",
                               LAST_RCVD, rc);
                        GOTO(err_fsd, rc);
                }
                if (strcmp(fsd->fsd_uuid, obd->obd_uuid.uuid) != 0) {
                        CERROR("OBD UUID %s does not match last_rcvd UUID %s\n",
                               obd->obd_uuid.uuid, fsd->fsd_uuid);
                        GOTO(err_fsd, rc = -EINVAL);
                }
                mount_count = le64_to_cpu(fsd->fsd_mount_count);
                filter->fo_subdir_count = le16_to_cpu(fsd->fsd_subdir_count);
        }

        if (fsd->fsd_feature_incompat & ~cpu_to_le32(FILTER_INCOMPAT_SUPP)) {
                CERROR("unsupported feature %x\n",
                       le32_to_cpu(fsd->fsd_feature_incompat) &
                       ~FILTER_INCOMPAT_SUPP);
                GOTO(err_fsd, rc = -EINVAL);
        }
        if (fsd->fsd_feature_rocompat & ~cpu_to_le32(FILTER_ROCOMPAT_SUPP)) {
                CERROR("read-only feature %x\n",
                       le32_to_cpu(fsd->fsd_feature_rocompat) &
                       ~FILTER_ROCOMPAT_SUPP);
                /* Do something like remount filesystem read-only */
                GOTO(err_fsd, rc = -EINVAL);
        }

        CDEBUG(D_INODE, "%s: server last_rcvd : "LPU64"\n",
               obd->obd_name, le64_to_cpu(fsd->fsd_last_transno));
        CDEBUG(D_INODE, "%s: server mount_count: "LPU64"\n",
               obd->obd_name, mount_count + 1);
        CDEBUG(D_INODE, "%s: server data size: %u\n",
               obd->obd_name, le32_to_cpu(fsd->fsd_server_size));
        CDEBUG(D_INODE, "%s: per-client data start: %u\n",
               obd->obd_name, le32_to_cpu(fsd->fsd_client_start));
        CDEBUG(D_INODE, "%s: per-client data size: %u\n",
               obd->obd_name, le32_to_cpu(fsd->fsd_client_size));
        CDEBUG(D_INODE, "%s: server subdir_count: %u\n",
               obd->obd_name, le16_to_cpu(fsd->fsd_subdir_count));
        CDEBUG(D_INODE, "%s: last_rcvd clients: %lu\n", obd->obd_name,
               last_rcvd_size <= le32_to_cpu(fsd->fsd_client_start) ? 0 :
               (last_rcvd_size - le32_to_cpu(fsd->fsd_client_start)) /
                le16_to_cpu(fsd->fsd_client_size));

        if (!obd->obd_replayable) {
                CWARN("%s: recovery support OFF\n", obd->obd_name);
                GOTO(out, rc = 0);
        }

        for (cl_idx = 0, off = le32_to_cpu(fsd->fsd_client_start);
             off < last_rcvd_size; cl_idx++) {
                __u64 last_rcvd;
                struct obd_export *exp;
                struct filter_export_data *fed;

                if (!fcd) {
                        OBD_ALLOC(fcd, sizeof(*fcd));
                        if (!fcd)
                                GOTO(err_client, rc = -ENOMEM);
                }

                /* Don't assume off is incremented properly by
                 * fsfilt_read_record(), in case sizeof(*fcd)
                 * isn't the same as fsd->fsd_client_size.  */
                off = le32_to_cpu(fsd->fsd_client_start) +
                        cl_idx * le16_to_cpu(fsd->fsd_client_size);
                rc = fsfilt_read_record(obd, filp, fcd, sizeof(*fcd), &off);
                if (rc) {
                        CERROR("error reading FILT %s idx %d off %llu: rc %d\n",
                               LAST_RCVD, cl_idx, off, rc);
                        break; /* read error shouldn't cause startup to fail */
                }

                if (fcd->fcd_uuid[0] == '\0') {
                        CDEBUG(D_INFO, "skipping zeroed client at offset %d\n",
                               cl_idx);
                        continue;
                }

                last_rcvd = le64_to_cpu(fcd->fcd_last_rcvd);

                /* These exports are cleaned up by filter_disconnect(), so they
                 * need to be set up like real exports as filter_connect() does.
                 */
                exp = class_new_export(obd);
                CDEBUG(D_HA, "RCVRNG CLIENT uuid: %s idx: %d lr: "LPU64
                       " srv lr: "LPU64"\n", fcd->fcd_uuid, cl_idx,
                       last_rcvd, le64_to_cpu(fsd->fsd_last_transno));
                if (exp == NULL)
                        GOTO(err_client, rc = -ENOMEM);

                memcpy(&exp->exp_client_uuid.uuid, fcd->fcd_uuid,
                       sizeof exp->exp_client_uuid.uuid);
                fed = &exp->exp_filter_data;
                fed->fed_fcd = fcd;
                filter_client_add(obd, filter, fed, cl_idx);
                /* create helper if export init gets more complex */
                spin_lock_init(&fed->fed_lock);

                fcd = NULL;
                obd->obd_recoverable_clients++;
                class_export_put(exp);

                CDEBUG(D_OTHER, "client at idx %d has last_rcvd = "LPU64"\n",
                       cl_idx, last_rcvd);

                if (last_rcvd > le64_to_cpu(fsd->fsd_last_transno))
                        fsd->fsd_last_transno = cpu_to_le64(last_rcvd);

        }

        obd->obd_last_committed = le64_to_cpu(fsd->fsd_last_transno);

        if (obd->obd_recoverable_clients) {
                CWARN("RECOVERY: %d recoverable clients, last_rcvd "
                      LPU64"\n", obd->obd_recoverable_clients,
                      le64_to_cpu(fsd->fsd_last_transno));
                obd->obd_next_recovery_transno = obd->obd_last_committed + 1;
                obd->obd_recovering = 1;
        }

        if (fcd)
                OBD_FREE(fcd, sizeof(*fcd));

out:
        filter->fo_mount_count = mount_count + 1;
        fsd->fsd_mount_count = cpu_to_le64(filter->fo_mount_count);

        /* save it, so mount count and last_transno is current */
        rc = filter_update_server_data(obd, filp, filter->fo_fsd, 1);

        RETURN(rc);

err_client:
        class_disconnect_exports(obd, 0);
err_fsd:
        filter_free_server_data(filter);
        RETURN(rc);
}

static int filter_cleanup_groups(struct obd_device *obd)
{
        struct filter_obd *filter = &obd->u.filter;
        struct dentry *dentry;
        int i, k;
        ENTRY;

        for (i = 0; i < filter->fo_group_count; i++) {
                if (filter->fo_subdirs != NULL) {
                        for (k = 0; k < filter->fo_subdir_count; k++) {
                                dentry = filter->fo_subdirs[i].dentry[k];
                                if (dentry == NULL)
                                        continue;
                                f_dput(dentry);
                                filter->fo_subdirs[i].dentry[k] = NULL;
                        }
                }
                if (filter->fo_last_objid_files[i] != NULL) {
                        filp_close(filter->fo_last_objid_files[i], 0);
                        filter->fo_last_objid_files[i] = NULL;
                }
                if (filter->fo_groups[i] != NULL) {
                        dput(filter->fo_groups[i]);
                        filter->fo_groups[i] = NULL;
                }
        }
        if (filter->fo_subdirs != NULL)
                OBD_FREE(filter->fo_subdirs,
                         filter->fo_group_count * sizeof(*filter->fo_subdirs));
        if (filter->fo_groups != NULL)
                OBD_FREE(filter->fo_groups,
                         filter->fo_group_count * sizeof(*filter->fo_groups));
        if (filter->fo_last_objids != NULL)
                OBD_FREE(filter->fo_last_objids,
                         filter->fo_group_count * sizeof(__u64));
        if (filter->fo_last_objid_files != NULL)
                OBD_FREE(filter->fo_last_objid_files,
                         filter->fo_group_count * sizeof(struct file *));
        f_dput(filter->fo_dentry_O);
        RETURN(0);
}

static int filter_read_group_internal(struct obd_device *obd, int group,
                                      int create)
{
        struct filter_obd *filter = &obd->u.filter;
        __u64 *new_objids = NULL;
        struct filter_subdirs *new_subdirs = NULL, *tmp_subdirs;
        struct dentry **new_groups = NULL;
        struct file **new_files = NULL;
        struct dentry *dentry;
        struct file *filp;
        int old_count = filter->fo_group_count, rc, stage = 0, i;
        char name[25];
        __u64 last_objid;
        loff_t off = 0;

        snprintf(name, 24, "%d", group);
        name[24] = '\0';

        if (!create) {
                dentry = ll_lookup_one_len(name, filter->fo_dentry_O,
                                           strlen(name));
                if (IS_ERR(dentry)) {
                        CERROR("Cannot lookup expected object group %d: %ld\n",
                               group, PTR_ERR(dentry));
                        RETURN(PTR_ERR(dentry));
                }
        } else {
                dentry = simple_mkdir(filter->fo_dentry_O, name, 0700, 1);
                if (IS_ERR(dentry)) {
                        CERROR("cannot lookup/create O/%s: rc = %ld\n", name,
                               PTR_ERR(dentry));
                        RETURN(PTR_ERR(dentry));
                }
        }
        stage = 1;

        snprintf(name, 24, "O/%d/LAST_ID", group);
        name[24] = '\0';
        filp = filp_open(name, O_CREAT | O_RDWR, 0700);
        if (IS_ERR(filp)) {
                CERROR("cannot create %s: rc = %ld\n", name, PTR_ERR(filp));
                GOTO(cleanup, rc = PTR_ERR(filp));
        }
        stage = 2;

        rc = fsfilt_read_record(obd, filp, &last_objid, sizeof(__u64), &off);
        if (rc) {
                CDEBUG(D_INODE, "error reading %s: rc %d\n", name, rc);
                GOTO(cleanup, rc);
        }

        if (filter->fo_subdir_count) {
                OBD_ALLOC(tmp_subdirs, sizeof(*tmp_subdirs));
                if (tmp_subdirs == NULL)
                        GOTO(cleanup, rc = -ENOMEM);
                stage = 3;

                for (i = 0; i < filter->fo_subdir_count; i++) {
                        char dir[20];
                        snprintf(dir, sizeof(dir), "d%u", i);

                        tmp_subdirs->dentry[i] =
                                simple_mkdir(dentry, dir, 0700, 1);
                        if (IS_ERR(tmp_subdirs->dentry[i])) {
                                rc = PTR_ERR(tmp_subdirs->dentry[i]);
                                CERROR("can't lookup/create O/%d/%s: rc = %d\n",
                                       group, dir, rc);
                                GOTO(cleanup, rc);
                        }
                        CDEBUG(D_INODE, "got/created O/%d/%s: %p\n", group, dir,
                               tmp_subdirs->dentry[i]);
                }
        }

        /* 'group' is an index; we need an array of length 'group + 1' */
        if (group + 1 > old_count) {
                int len = group + 1;
                OBD_ALLOC(new_objids, len * sizeof(*new_objids));
                OBD_ALLOC(new_subdirs, len * sizeof(*new_subdirs));
                OBD_ALLOC(new_groups, len * sizeof(*new_groups));
                OBD_ALLOC(new_files, len * sizeof(*new_files));
                stage = 4;
                if (new_objids == NULL || new_subdirs == NULL ||
                    new_groups == NULL || new_files == NULL)
                        GOTO(cleanup, rc = -ENOMEM);

                memcpy(new_objids, filter->fo_last_objids,
                       old_count * sizeof(*new_objids));
                memcpy(new_subdirs, filter->fo_subdirs,
                       old_count * sizeof(*new_subdirs));
                memcpy(new_groups, filter->fo_groups,
                       old_count * sizeof(*new_groups));
                memcpy(new_files, filter->fo_last_objid_files,
                       old_count * sizeof(*new_files));

                if (old_count) {
                        OBD_FREE(filter->fo_last_objids,
                                 old_count * sizeof(*new_objids));
                        OBD_FREE(filter->fo_subdirs,
                                 old_count * sizeof(*new_subdirs));
                        OBD_FREE(filter->fo_groups,
                                 old_count * sizeof(*new_groups));
                        OBD_FREE(filter->fo_last_objid_files,
                                 old_count * sizeof(*new_files));
                }
                filter->fo_last_objids = new_objids;
                filter->fo_subdirs = new_subdirs;
                filter->fo_groups = new_groups;
                filter->fo_last_objid_files = new_files;
                filter->fo_group_count = len;
        }

        filter->fo_groups[group] = dentry;
        filter->fo_last_objid_files[group] = filp;
        if (filter->fo_subdir_count) {
                filter->fo_subdirs[group] = *tmp_subdirs;
                OBD_FREE(tmp_subdirs, sizeof(*tmp_subdirs));
        }

        if (filp->f_dentry->d_inode->i_size == 0) {
                filter->fo_last_objids[group] = FILTER_INIT_OBJID;
                RETURN(0);
        }

        filter->fo_last_objids[group] = le64_to_cpu(last_objid);
        CDEBUG(D_INODE, "%s: server last_objid group %d: "LPU64"\n",
               obd->obd_name, group, last_objid);
        RETURN(0);
 cleanup:
        switch (stage) {
        case 4:
                if (new_objids != NULL)
                        OBD_FREE(new_objids, group * sizeof(*new_objids));
                if (new_subdirs != NULL)
                        OBD_FREE(new_subdirs, group * sizeof(*new_subdirs));
                if (new_groups != NULL)
                        OBD_FREE(new_groups, group * sizeof(*new_groups));
                if (new_files != NULL)
                        OBD_FREE(new_files, group * sizeof(*new_files));
        case 3:
                if (filter->fo_subdir_count) {
                        for (i = 0; i < filter->fo_subdir_count; i++) {
                                if (tmp_subdirs->dentry[i] != NULL)
                                        dput(tmp_subdirs->dentry[i]);
                        }
                        OBD_FREE(tmp_subdirs, sizeof(*tmp_subdirs));
                }
        case 2:
                filp_close(filp, 0);
        case 1:
                dput(dentry);
        }
        RETURN(rc);
}

static int filter_read_groups(struct obd_device *obd, int last_group,
                              int create)
{
        struct filter_obd *filter = &obd->u.filter;
        int old_count = filter->fo_group_count, group = old_count, rc = 0;

        for (group = old_count; group <= last_group; group++) {
                if (group == 0)
                        continue; /* no group zero */

                rc = filter_read_group_internal(obd, group, create);
                if (rc != 0)
                        break;
        }
        return rc;
}

static int filter_prep_groups(struct obd_device *obd)
{
        struct filter_obd *filter = &obd->u.filter;
        struct dentry *dentry, *O_dentry;
        int rc = 0, cleanup_phase = 0;
        ENTRY;

        O_dentry = simple_mkdir(current->fs->pwd, "O", 0700, 1);
        CDEBUG(D_INODE, "got/created O: %p\n", O_dentry);
        if (IS_ERR(O_dentry)) {
                rc = PTR_ERR(O_dentry);
                CERROR("cannot open/create O: rc = %d\n", rc);
                GOTO(cleanup, rc);
        }
        filter->fo_dentry_O = O_dentry;
        cleanup_phase = 1; /* O_dentry */

        /* Lookup "R" to tell if we're on an old OST FS and need to convert
         * from O/R/<dir>/<objid> to O/0/<dir>/<objid>.  This can be removed
         * some time post 1.0 when all old-style OSTs have converted along
         * with the init_objid hack. */
        dentry = ll_lookup_one_len("R", O_dentry, 1);
        if (IS_ERR(dentry))
                GOTO(cleanup, rc = PTR_ERR(dentry));
        if (dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode)) {
                struct dentry *O0_dentry = lookup_one_len("0", O_dentry, 1);
                ENTRY;

                CWARN("converting OST to new object layout\n");
                if (IS_ERR(O0_dentry)) {
                        rc = PTR_ERR(O0_dentry);
                        CERROR("error looking up O/0: rc %d\n", rc);
                        GOTO(cleanup_R, rc);
                }

                if (O0_dentry->d_inode) {
                        CERROR("Both O/R and O/0 exist. Fix manually.\n");
                        GOTO(cleanup_O0, rc = -EEXIST);
                }

                down(&O_dentry->d_inode->i_sem);
                rc = vfs_rename(O_dentry->d_inode, dentry,
                                O_dentry->d_inode, O0_dentry);
                up(&O_dentry->d_inode->i_sem);

                if (rc) {
                        CERROR("error renaming O/R to O/0: rc %d\n", rc);
                        GOTO(cleanup_O0, rc);
                }
                filter->fo_fsd->fsd_feature_incompat |=
                        cpu_to_le32(FILTER_INCOMPAT_GROUPS);
                rc = filter_update_server_data(obd, filter->fo_rcvd_filp,
                                               filter->fo_fsd, 1);
                GOTO(cleanup_O0, rc);

        cleanup_O0:
                f_dput(O0_dentry);
        cleanup_R:
                f_dput(dentry);
                if (rc)
                        GOTO(cleanup, rc);
        } else {
                f_dput(dentry);
        }

        cleanup_phase = 2; /* groups */

        /* Group 0 is no longer a legal group, to catch uninitialized IDs */
#define FILTER_MIN_GROUPS 3
        rc = filter_read_groups(obd, FILTER_MIN_GROUPS, 1);
        if (rc)
                GOTO(cleanup, rc);

        RETURN(0);

 cleanup:
        switch (cleanup_phase) {
        case 2:
                filter_cleanup_groups(obd);
        case 1:
                f_dput(filter->fo_dentry_O);
                filter->fo_dentry_O = NULL;
        default:
                break;
        }
        return rc;
}

/* setup the object store with correct subdirectories */
static int filter_prep(struct obd_device *obd)
{
        struct lvfs_run_ctxt saved;
        struct filter_obd *filter = &obd->u.filter;
        struct file *file;
        struct inode *inode;
        int rc = 0;
        ENTRY;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        file = filp_open(LAST_RCVD, O_RDWR | O_CREAT | O_LARGEFILE, 0700);
        if (!file || IS_ERR(file)) {
                rc = PTR_ERR(file);
                CERROR("OBD filter: cannot open/create %s: rc = %d\n",
                       LAST_RCVD, rc);
                GOTO(out, rc);
        }

        if (!S_ISREG(file->f_dentry->d_inode->i_mode)) {
                CERROR("%s is not a regular file!: mode = %o\n", LAST_RCVD,
                       file->f_dentry->d_inode->i_mode);
                GOTO(err_filp, rc = -ENOENT);
        }

        /* steal operations */
        inode = file->f_dentry->d_inode;
        filter->fo_fop = file->f_op;
        filter->fo_iop = inode->i_op;
        filter->fo_aops = inode->i_mapping->a_ops;

        rc = filter_init_server_data(obd, file);
        if (rc) {
                CERROR("cannot read %s: rc = %d\n", LAST_RCVD, rc);
                GOTO(err_filp, rc);
        }
        filter->fo_rcvd_filp = file;

        rc = filter_prep_groups(obd);
        if (rc)
                GOTO(err_server_data, rc);

 out:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        return(rc);

 err_server_data:
        //class_disconnect_exports(obd, 0);
        filter_free_server_data(filter);
 err_filp:
        if (filp_close(file, 0))
                CERROR("can't close %s after error\n", LAST_RCVD);
        filter->fo_rcvd_filp = NULL;
        goto out;
}

/* cleanup the filter: write last used object id to status file */
static void filter_post(struct obd_device *obd)
{
        struct lvfs_run_ctxt saved;
        struct filter_obd *filter = &obd->u.filter;
        int rc, i;

        /* XXX: filter_update_lastobjid used to call fsync_dev.  It might be
         * best to start a transaction with h_sync, because we removed this
         * from lastobjid */

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = filter_update_server_data(obd, filter->fo_rcvd_filp,
                                       filter->fo_fsd, 0);
        if (rc)
                CERROR("error writing server data: rc = %d\n", rc);

        for (i = 1; i < filter->fo_group_count; i++) {
                rc = filter_update_last_objid(obd, i,
                                             (i == filter->fo_group_count - 1));
                if (rc)
                        CERROR("error writing group %d lastobjid: rc = %d\n",
                               i, rc);
        }

        rc = filp_close(filter->fo_rcvd_filp, 0);
        filter->fo_rcvd_filp = NULL;
        if (rc)
                CERROR("error closing %s: rc = %d\n", LAST_RCVD, rc);

        filter_cleanup_groups(obd);
        filter_free_server_data(filter);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
}

static void filter_set_last_id(struct filter_obd *filter, int group, obd_id id)
{
        LASSERT(filter->fo_fsd != NULL);
        LASSERT(group > 0);
        LASSERT(group < filter->fo_group_count);

        spin_lock(&filter->fo_objidlock);
        filter->fo_last_objids[group] = id;
        spin_unlock(&filter->fo_objidlock);
}

__u64 filter_last_id(struct filter_obd *filter, int group)
{
        obd_id id;
        LASSERT(filter->fo_fsd != NULL);
        LASSERT(group > 0);
        LASSERT(group < filter->fo_group_count);

        spin_lock(&filter->fo_objidlock);
        id = filter->fo_last_objids[group];
        spin_unlock(&filter->fo_objidlock);

        return id;
}

/* direct cut-n-paste of mds_blocking_ast() */
static int filter_blocking_ast(struct ldlm_lock *lock,
                               struct ldlm_lock_desc *desc,
                               void *data, int flag)
{
        int do_ast;
        ENTRY;

        if (flag == LDLM_CB_CANCELING) {
                /* Don't need to do anything here. */
                RETURN(0);
        }

        /* XXX layering violation!  -phil */
        l_lock(&lock->l_resource->lr_namespace->ns_lock);
        /* Get this: if filter_blocking_ast is racing with ldlm_intent_policy,
         * such that filter_blocking_ast is called just before l_i_p takes the
         * ns_lock, then by the time we get the lock, we might not be the
         * correct blocking function anymore.  So check, and return early, if
         * so. */
        if (lock->l_blocking_ast != filter_blocking_ast) {
                l_unlock(&lock->l_resource->lr_namespace->ns_lock);
                RETURN(0);
        }

        lock->l_flags |= LDLM_FL_CBPENDING;
        do_ast = (!lock->l_readers && !lock->l_writers);
        l_unlock(&lock->l_resource->lr_namespace->ns_lock);

        if (do_ast) {
                struct lustre_handle lockh;
                int rc;

                LDLM_DEBUG(lock, "already unused, calling ldlm_cli_cancel");
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                if (rc < 0)
                        CERROR("ldlm_cli_cancel: %d\n", rc);
        } else {
                LDLM_DEBUG(lock, "Lock still has references, will be "
                           "cancelled later");
        }
        RETURN(0);
}

static int filter_lock_dentry(struct obd_device *obd, struct dentry *dparent)
{
        down(&dparent->d_inode->i_sem);
        return 0;
}

/* We never dget the object parent, so DON'T dput it either */
static void filter_parent_unlock(struct dentry *dparent)
{
        up(&dparent->d_inode->i_sem);
}

/* We never dget the object parent, so DON'T dput it either */
struct dentry *filter_parent(struct obd_device *obd, obd_gr group, obd_id objid)
{
        struct filter_obd *filter = &obd->u.filter;
        LASSERT(group < filter->fo_group_count);
        LASSERT(group > 0);

        if (filter->fo_subdir_count == 0)
                return filter->fo_groups[group];

        return filter->fo_subdirs[group].dentry[objid & (filter->fo_subdir_count - 1)];
}

/* We never dget the object parent, so DON'T dput it either */
struct dentry *filter_parent_lock(struct obd_device *obd, obd_gr group,
                                  obd_id objid)
{
        unsigned long now = jiffies;
        struct dentry *dparent = filter_parent(obd, group, objid);
        int rc;

        if (IS_ERR(dparent))
                return dparent;

        LASSERT(dparent);
        LASSERT(dparent->d_inode);

        rc = filter_lock_dentry(obd, dparent);
        if (time_after(jiffies, now + 15 * HZ))
                CERROR("slow parent lock %lus\n", (jiffies - now) / HZ);
        return rc ? ERR_PTR(rc) : dparent;
}

/* How to get files, dentries, inodes from object id's.
 *
 * If dir_dentry is passed, the caller has already locked the parent
 * appropriately for this operation (normally a write lock).  If
 * dir_dentry is NULL, we do a read lock while we do the lookup to
 * avoid races with create/destroy and such changing the directory
 * internal to the filesystem code. */
struct dentry *filter_fid2dentry(struct obd_device *obd,
                                 struct dentry *dir_dentry,
                                 obd_gr group, obd_id id)
{
        struct dentry *dparent = dir_dentry;
        struct dentry *dchild;
        char name[32];
        int len;
        ENTRY;

        if (id == 0) {
                CERROR("fatal: invalid object id 0\n");
                RETURN(ERR_PTR(-ESTALE));
        }

        len = sprintf(name, LPU64, id);
        if (dir_dentry == NULL) {
                dparent = filter_parent_lock(obd, group, id);
                if (IS_ERR(dparent))
                        RETURN(dparent);
        }
        CDEBUG(D_INODE, "looking up object O/%*s/%s\n",
               dparent->d_name.len, dparent->d_name.name, name);
        dchild = /*ll_*/lookup_one_len(name, dparent, len);
        if (dir_dentry == NULL)
                filter_parent_unlock(dparent);
        if (IS_ERR(dchild)) {
                CERROR("child lookup error %ld\n", PTR_ERR(dchild));
                RETURN(dchild);
        }

        CDEBUG(D_INODE, "got child objid %s: %p, count = %d\n",
               name, dchild, atomic_read(&dchild->d_count));

        LASSERT(atomic_read(&dchild->d_count) > 0);

        RETURN(dchild);
}

static int filter_prepare_destroy(struct obd_device *obd, obd_id objid,
                                  obd_id group)
{
        struct lustre_handle lockh;
        int flags = LDLM_AST_DISCARD_DATA, rc;
        struct ldlm_res_id res_id = { .name = { objid, 0, group, 0 } };
        ldlm_policy_data_t policy = { .l_extent = { 0, OBD_OBJECT_EOF } };

        ENTRY;
        /* Tell the clients that the object is gone now and that they should
         * throw away any cached pages. */
        rc = ldlm_cli_enqueue(NULL, NULL, obd->obd_namespace, res_id,
                              LDLM_EXTENT, &policy, LCK_PW,
                              &flags, filter_blocking_ast, ldlm_completion_ast,
                              NULL, NULL, NULL, 0, NULL, &lockh);

        /* We only care about the side-effects, just drop the lock. */
        if (rc == ELDLM_OK)
                ldlm_lock_decref(&lockh, LCK_PW);

        RETURN(rc);
}

/* Caller must hold LCK_PW on parent and push us into kernel context.
 * Caller is also required to ensure that dchild->d_inode exists. */
static int filter_destroy_internal(struct obd_device *obd, obd_id objid,
                                   struct dentry *dparent,
                                   struct dentry *dchild)
{
        struct inode *inode = dchild->d_inode;
        int rc;
        ENTRY;

        if (inode->i_nlink != 1 || atomic_read(&inode->i_count) != 1) {
                CERROR("destroying objid %*s nlink = %lu, count = %d\n",
                       dchild->d_name.len, dchild->d_name.name,
                       (unsigned long)inode->i_nlink,
                       atomic_read(&inode->i_count));
        }

        rc = vfs_unlink(dparent->d_inode, dchild);

        if (rc)
                CERROR("error unlinking objid %*s: rc %d\n",
                       dchild->d_name.len, dchild->d_name.name, rc);

        RETURN(rc);
}

static int filter_intent_policy(struct ldlm_namespace *ns,
                                struct ldlm_lock **lockp, void *req_cookie,
                                ldlm_mode_t mode, int flags, void *data)
{
        struct list_head rpc_list = LIST_HEAD_INIT(rpc_list);
        struct ptlrpc_request *req = req_cookie;
        struct ldlm_lock *lock = *lockp, *l = NULL;
        struct ldlm_resource *res = lock->l_resource;
        ldlm_processing_policy policy;
        struct ost_lvb *res_lvb, *reply_lvb;
        struct list_head *tmp;
        ldlm_error_t err;
        int tmpflags = 0, rc, repsize[2] = {sizeof(struct ldlm_reply),
                                            sizeof(struct ost_lvb) };
        ENTRY;

        policy = ldlm_get_processing_policy(res);
        LASSERT(policy != NULL);
        LASSERT(req != NULL);

        rc = lustre_pack_reply(req, 2, repsize, NULL);
        if (rc)
                RETURN(req->rq_status = rc);

        reply_lvb = lustre_msg_buf(req->rq_repmsg, 1, sizeof(*reply_lvb));
        LASSERT(reply_lvb != NULL);

        //fixup_handle_for_resent_req(req, lock, &lockh);

        /* If we grant any lock at all, it will be a whole-file read lock.
         * Call the extent policy function to see if our request can be
         * granted, or is blocked. */
        lock->l_policy_data.l_extent.start = 0;
        lock->l_policy_data.l_extent.end = OBD_OBJECT_EOF;
        lock->l_req_mode = LCK_PR;

        l_lock(&res->lr_namespace->ns_lock);

        res->lr_tmp = &rpc_list;
        rc = policy(lock, &tmpflags, 0, &err);
        res->lr_tmp = NULL;

        /* FIXME: we should change the policy function slightly, to not make
         * this list at all, since we just turn around and free it */
        while (!list_empty(&rpc_list)) {
                struct ldlm_ast_work *w =
                        list_entry(rpc_list.next, struct ldlm_ast_work, w_list);
                list_del(&w->w_list);
                LDLM_LOCK_PUT(w->w_lock);
                OBD_FREE(w, sizeof(*w));
        }

        if (rc == LDLM_ITER_CONTINUE) {
                /* The lock met with no resistance; we're finished. */
                l_unlock(&res->lr_namespace->ns_lock);
                RETURN(ELDLM_LOCK_REPLACED);
        }

        /* Do not grant any lock, but instead send GL callbacks.  The extent
         * policy nicely created a list of all PW locks for us.  We will choose
         * the highest of those which are larger than the size in the LVB, if
         * any, and perform a glimpse callback. */
        down(&res->lr_lvb_sem);
        res_lvb = res->lr_lvb_data;
        LASSERT(res_lvb != NULL);
        reply_lvb->lvb_size = res_lvb->lvb_size;
        reply_lvb->lvb_blocks = res_lvb->lvb_blocks;
        up(&res->lr_lvb_sem);

        list_for_each(tmp, &res->lr_granted) {
                struct ldlm_lock *tmplock =
                        list_entry(tmp, struct ldlm_lock, l_res_link);

                if (tmplock->l_granted_mode == LCK_PR)
                        continue;

                if (tmplock->l_policy_data.l_extent.end <= reply_lvb->lvb_size)
                        continue;

                if (l == NULL) {
                        l = LDLM_LOCK_GET(tmplock);
                        continue;
                }

                if (l->l_policy_data.l_extent.start >
                    tmplock->l_policy_data.l_extent.start)
                        continue;

                LDLM_LOCK_PUT(l);
                l = LDLM_LOCK_GET(tmplock);
        }
        l_unlock(&res->lr_namespace->ns_lock);

        /* There were no PW locks beyond the size in the LVB; finished. */
        if (l == NULL)
                RETURN(ELDLM_LOCK_ABORTED);

        LASSERT(l->l_glimpse_ast != NULL);
        rc = l->l_glimpse_ast(l, NULL); /* this will update the LVB */
        if (rc != 0 && res->lr_namespace->ns_lvbo &&
            res->lr_namespace->ns_lvbo->lvbo_update) {
                res->lr_namespace->ns_lvbo->lvbo_update(res, NULL, 0, 1);
        }

        down(&res->lr_lvb_sem);
        reply_lvb->lvb_size = res_lvb->lvb_size;
        reply_lvb->lvb_blocks = res_lvb->lvb_blocks;
        up(&res->lr_lvb_sem);

        LDLM_LOCK_PUT(l);

        RETURN(ELDLM_LOCK_ABORTED);
}

static int filter_post_fs_cleanup(struct obd_device *obd)
{
        int rc = 0;

        rc = fsfilt_post_cleanup(obd);

        RETURN(rc);
}

static int filter_group_set_kml_flags(struct obd_device *obd, int group)
{
        struct filter_obd *filter = &obd->u.filter;
        int rc = 0, i = 0;
        ENTRY;        
        
        /* zero group is not longer valid. */
        if (group== 0)
                RETURN(rc); 
        for (i = 0; i < filter->fo_subdir_count; i++) {
                struct dentry *dentry;
                dentry = (filter->fo_subdirs + group)->dentry[i];
                rc = fsfilt_set_kml_flags(obd, dentry->d_inode);
                if (rc)
                        RETURN(rc);
        }
        RETURN(rc);
}
static int filter_post_fs_setup(struct obd_device *obd)
{
        struct filter_obd *filter = &obd->u.filter;
        int rc = 0, j = 0;
        struct llog_ctxt *ctxt = NULL;

        rc = fsfilt_post_setup(obd);
        if (rc)
                RETURN(rc);
        
        for (j = 0; j < filter->fo_group_count; j++) {
                rc = filter_group_set_kml_flags(obd, j);
                if (rc)
                        return rc;
        } 

        fsfilt_get_reint_log_ctxt(obd, filter->fo_sb, &ctxt);
        if (ctxt) {
                ctxt->loc_obd = obd;
                ctxt->loc_idx = LLOG_REINT_ORIG_CTXT;
                obd->obd_llog_ctxt[LLOG_REINT_ORIG_CTXT] = ctxt;
        }
        fsfilt_set_ost_flags(obd, filter->fo_sb);
        return rc;
}

/* mount the file system (secretly) */
int filter_common_setup(struct obd_device *obd, obd_count len,
                        void *buf, char *option)
{
        struct lustre_cfg* lcfg = buf;
        struct filter_obd *filter = &obd->u.filter;
        struct vfsmount *mnt;
        char name[32] = "CATLIST";
        int rc = 0;
        ENTRY;

        dev_clear_rdonly(2);

        if (!lcfg->lcfg_inlbuf1 || !lcfg->lcfg_inlbuf2)
                RETURN(-EINVAL);

        obd->obd_fsops = fsfilt_get_ops(lcfg->lcfg_inlbuf2);
        if (IS_ERR(obd->obd_fsops))
                RETURN(PTR_ERR(obd->obd_fsops));

        mnt = do_kern_mount(lcfg->lcfg_inlbuf2, MS_NOATIME | MS_NODIRATIME,
                            lcfg->lcfg_inlbuf1, option);
        rc = PTR_ERR(mnt);
        if (IS_ERR(mnt))
                GOTO(err_ops, rc);

        if (lcfg->lcfg_inllen3 > 0 && lcfg->lcfg_inlbuf3) {
                if (*lcfg->lcfg_inlbuf3 == 'f') {
                        obd->obd_replayable = 1;
                        obd_sync_filter = 1;
                        CWARN("%s: recovery enabled\n", obd->obd_name);
                } else {
                        if (*lcfg->lcfg_inlbuf3 != 'n') {
                                CERROR("unrecognised flag '%c'\n",
                                       *lcfg->lcfg_inlbuf3);
                        }
                        // XXX Robert? Why do we get errors here
                        // GOTO(err_mntput, rc = -EINVAL);
                }
        }

        filter->fo_vfsmnt = mnt;
        filter->fo_sb = mnt->mnt_sb;
        filter->fo_fstype = mnt->mnt_sb->s_type->name;
        CDEBUG(D_SUPER, "%s: mnt = %p\n", filter->fo_fstype, mnt);

        OBD_SET_CTXT_MAGIC(&obd->obd_lvfs_ctxt);
        obd->obd_lvfs_ctxt.pwdmnt = mnt;
        obd->obd_lvfs_ctxt.pwd = mnt->mnt_root;
        obd->obd_lvfs_ctxt.fs = get_ds();
        obd->obd_lvfs_ctxt.cb_ops = filter_lvfs_ops;

        rc = fsfilt_setup(obd, mnt->mnt_sb);
        if (rc)
                GOTO(err_mntput, rc);

        rc = filter_prep(obd);
        if (rc)
                GOTO(err_mntput, rc);


        filter->fo_destroy_in_progress = 0;
        sema_init(&filter->fo_create_lock, 1);

        spin_lock_init(&filter->fo_translock);
        spin_lock_init(&filter->fo_objidlock);
        INIT_LIST_HEAD(&filter->fo_export_list);
        sema_init(&filter->fo_alloc_lock, 1);
        spin_lock_init(&filter->fo_r_pages.oh_lock);
        spin_lock_init(&filter->fo_w_pages.oh_lock);
        spin_lock_init(&filter->fo_r_discont_pages.oh_lock);
        spin_lock_init(&filter->fo_w_discont_pages.oh_lock);
        spin_lock_init(&filter->fo_r_discont_blocks.oh_lock);
        spin_lock_init(&filter->fo_w_discont_blocks.oh_lock);
        filter->fo_readcache_max_filesize = FILTER_MAX_CACHE_SIZE;

        INIT_LIST_HEAD(&filter->fo_llog_list);
        spin_lock_init(&filter->fo_llog_list_lock);

        obd->obd_namespace = ldlm_namespace_new("filter-tgt",
                                                LDLM_NAMESPACE_SERVER);
        if (obd->obd_namespace == NULL)
                GOTO(err_post, rc = -ENOMEM);
        obd->obd_namespace->ns_lvbp = obd;
        obd->obd_namespace->ns_lvbo = &filter_lvbo;
        ldlm_register_intent(obd->obd_namespace, filter_intent_policy);

        ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                           "filter_ldlm_cb_client", &obd->obd_ldlm_client);

        rc = obd_llog_cat_initialize(obd, &obd->obd_llogs, 1, name);
        if (rc) {
                CERROR("failed to setup llogging subsystems\n");
                GOTO(err_post, rc);
        }
        RETURN(0);

err_post:
        filter_post(obd);
err_mntput:
        unlock_kernel();
        mntput(mnt);
        filter->fo_sb = 0;
        lock_kernel();
err_ops:
        fsfilt_put_ops(obd->obd_fsops);
        return rc;
}

static int filter_attach(struct obd_device *obd, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;
        int rc;

        lprocfs_init_vars(filter, &lvars);
        rc = lprocfs_obd_attach(obd, lvars.obd_vars);
        if (rc != 0)
                return rc;

        rc = lprocfs_alloc_obd_stats(obd, LPROC_FILTER_LAST);
        if (rc != 0)
                return rc;

        /* Init obdfilter private stats here */
        lprocfs_counter_init(obd->obd_stats, LPROC_FILTER_READ_BYTES,
                             LPROCFS_CNTR_AVGMINMAX, "read_bytes", "bytes");
        lprocfs_counter_init(obd->obd_stats, LPROC_FILTER_WRITE_BYTES,
                             LPROCFS_CNTR_AVGMINMAX, "write_bytes", "bytes");

        return lproc_filter_attach_seqstat(obd);
}

static int filter_detach(struct obd_device *dev)
{
        lprocfs_free_obd_stats(dev);
        return lprocfs_obd_detach(dev);
}

static int filter_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct lustre_cfg* lcfg = buf;
        int rc;
        ENTRY;
        /* all mount options including errors=remount-ro and asyncdel are passed
         * using 4th lcfg param. And it is good, finally we have got rid of
         * hardcoded fs types in the code. */
        rc = filter_common_setup(obd, len, buf, lcfg->lcfg_inlbuf4);
        if (rc)
                RETURN(rc);
        rc = filter_post_fs_setup(obd);
        RETURN(rc);
}

static int filter_cleanup(struct obd_device *obd, int flags)
{
        struct filter_obd *filter = &obd->u.filter;
        ENTRY;

        if (flags & OBD_OPT_FAILOVER)
                CERROR("%s: shutting down for failover; client state will"
                       " be preserved.\n", obd->obd_name);

        if (!list_empty(&obd->obd_exports)) {
                CERROR("%s: still has clients!\n", obd->obd_name);
                class_disconnect_exports(obd, flags);
                if (!list_empty(&obd->obd_exports)) {
                        CERROR("still has exports after forced cleanup?\n");
                        RETURN(-EBUSY);
                }
        }

        ldlm_namespace_free(obd->obd_namespace, flags & OBD_OPT_FORCE);

        if (filter->fo_sb == NULL)
                RETURN(0);

        filter_post_fs_cleanup(obd);
        filter_post(obd);

        shrink_dcache_parent(filter->fo_sb->s_root);
        filter->fo_sb = 0;

        if (atomic_read(&filter->fo_vfsmnt->mnt_count) > 1)
                CERROR("%s: mount point %p busy, mnt_count: %d\n",
                       obd->obd_name, filter->fo_vfsmnt,
                       atomic_read(&filter->fo_vfsmnt->mnt_count));

        unlock_kernel();
        mntput(filter->fo_vfsmnt);
        //destroy_buffers(filter->fo_sb->s_dev);
        filter->fo_sb = NULL;
        fsfilt_put_ops(obd->obd_fsops);
        lock_kernel();

        dev_clear_rdonly(2);

        RETURN(0);
}

/* nearly identical to mds_connect */
static int filter_connect(struct lustre_handle *conn, struct obd_device *obd,
                          struct obd_uuid *cluuid)
{
        struct obd_export *exp;
        struct filter_export_data *fed;
        struct filter_client_data *fcd = NULL;
        struct filter_obd *filter = &obd->u.filter;
        int rc;
        ENTRY;

        if (conn == NULL || obd == NULL || cluuid == NULL)
                RETURN(-EINVAL);

        rc = class_connect(conn, obd, cluuid);
        if (rc)
                RETURN(rc);
        exp = class_conn2export(conn);
        LASSERT(exp != NULL);

        fed = &exp->exp_filter_data;

        spin_lock_init(&fed->fed_lock);

        if (!obd->obd_replayable)
                GOTO(cleanup, rc = 0);

        OBD_ALLOC(fcd, sizeof(*fcd));
        if (!fcd) {
                CERROR("filter: out of memory for client data\n");
                GOTO(cleanup, rc = -ENOMEM);
        }

        memcpy(fcd->fcd_uuid, cluuid, sizeof(fcd->fcd_uuid));
        fed->fed_fcd = fcd;

        rc = filter_client_add(obd, filter, fed, -1);

cleanup:
        if (rc) {
                if (fcd)
                        OBD_FREE(fcd, sizeof(*fcd));
                class_disconnect(exp, 0);
        } else {
                class_export_put(exp);
        }
        return rc;
}

static int filter_precleanup(struct obd_device *obd, int flags)
{
        struct filter_group_llog *log;
        struct filter_obd *filter;
        int rc = 0;
        ENTRY;

        filter = &obd->u.filter;

        spin_lock(&filter->fo_llog_list_lock);
        while (!list_empty(&filter->fo_llog_list)) {
                log = list_entry(filter->fo_llog_list.next,
                                 struct filter_group_llog, list);
                list_del(&log->list);
                spin_unlock(&filter->fo_llog_list_lock);

                rc = obd_llog_finish(obd, log->llogs, 0);
                if (rc)
                        CERROR("failed to cleanup llogging subsystem for %u\n",
                                log->group);
                OBD_FREE(log->llogs, sizeof(*(log->llogs)));
                OBD_FREE(log, sizeof(*log));
                spin_lock(&filter->fo_llog_list_lock);
        }
        spin_unlock(&filter->fo_llog_list_lock);

        rc = obd_llog_finish(obd, &obd->obd_llogs, 0);
        if (rc)
                CERROR("failed to cleanup llogging subsystem\n");

        RETURN(rc);
}

/* Do extra sanity checks for grant accounting.  We do this at connect,
 * disconnect, and statfs RPC time, so it shouldn't be too bad.  We can
 * always get rid of it or turn it off when we know accounting is good. */
static void filter_grant_sanity_check(struct obd_device *obd, const char *func)
{
        struct filter_export_data *fed;
        struct obd_export *exp;
        obd_size maxsize = obd->obd_osfs.os_blocks * obd->obd_osfs.os_bsize;
        obd_size tot_dirty = 0, tot_pending = 0, tot_granted = 0;
        obd_size fo_tot_dirty, fo_tot_pending, fo_tot_granted;

        if (list_empty(&obd->obd_exports))
                return;

        spin_lock(&obd->obd_osfs_lock);
        spin_lock(&obd->obd_dev_lock);
        list_for_each_entry(exp, &obd->obd_exports, exp_obd_chain) {
                fed = &exp->exp_filter_data;
                LASSERTF(fed->fed_grant + fed->fed_pending <= maxsize,
                         "cli %s/%p %lu+%lu > "LPU64"\n",
                         exp->exp_client_uuid.uuid, exp,
                         fed->fed_grant, fed->fed_pending, maxsize);
                LASSERTF(fed->fed_dirty <= maxsize, "cli %s/%p %lu > "LPU64"\n",
                         exp->exp_client_uuid.uuid, exp,fed->fed_dirty,maxsize);
                CDEBUG(D_CACHE,"%s: cli %s/%p dirty %lu pend %lu grant %lu\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       fed->fed_dirty, fed->fed_pending, fed->fed_grant);
                tot_granted += fed->fed_grant + fed->fed_pending;
                tot_pending += fed->fed_pending;
                tot_dirty += fed->fed_dirty;
        }
        fo_tot_granted = obd->u.filter.fo_tot_granted;
        fo_tot_pending = obd->u.filter.fo_tot_pending;
        fo_tot_dirty = obd->u.filter.fo_tot_dirty;
        spin_unlock(&obd->obd_dev_lock);
        spin_unlock(&obd->obd_osfs_lock);

        /* Do these assertions outside the spinlocks so we don't kill system */
        if (tot_granted != fo_tot_granted)
                CERROR("%s: tot_granted "LPU64" != fo_tot_granted "LPU64"\n",
                       func, tot_granted, fo_tot_granted);
        if (tot_pending != fo_tot_pending)
                CERROR("%s: tot_pending "LPU64" != fo_tot_pending "LPU64"\n",
                       func, tot_pending, fo_tot_pending);
        if (tot_dirty != fo_tot_dirty)
                CERROR("%s: tot_dirty "LPU64" != fo_tot_dirty "LPU64"\n",
                       func, tot_dirty, fo_tot_dirty);
        if (tot_pending > tot_granted)
                CERROR("%s: tot_pending "LPU64" > tot_granted "LPU64"\n",
                       func, tot_pending, tot_granted);
        if (tot_granted > maxsize)
                CERROR("%s: tot_granted "LPU64" > maxsize "LPU64"\n",
                       func, tot_granted, maxsize);
        if (tot_dirty > maxsize)
                CERROR("%s: tot_dirty "LPU64" > maxsize "LPU64"\n",
                       func, tot_dirty, maxsize);
}

/* Remove this client from the grant accounting totals.  We also remove
 * the export from the obd device under the osfs and dev locks to ensure
 * that the filter_grant_sanity_check() calculations are always valid.
 * The client should do something similar when it invalidates its import. */
static void filter_grant_discard(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct filter_obd *filter = &obd->u.filter;
        struct filter_export_data *fed = &exp->exp_filter_data;

        spin_lock(&obd->obd_osfs_lock);
        spin_lock(&exp->exp_obd->obd_dev_lock);
        list_del_init(&exp->exp_obd_chain);
        spin_unlock(&exp->exp_obd->obd_dev_lock);

        CDEBUG(D_CACHE, "%s: cli %s/%p dirty %lu pend %lu grant %lu\n",
               obd->obd_name, exp->exp_client_uuid.uuid, exp,
               fed->fed_dirty, fed->fed_pending, fed->fed_grant);

        LASSERTF(filter->fo_tot_granted >= fed->fed_grant,
                 "%s: tot_granted "LPU64" cli %s/%p fed_grant %lu\n",
                 obd->obd_name, filter->fo_tot_granted,
                 exp->exp_client_uuid.uuid, exp, fed->fed_grant);
        filter->fo_tot_granted -= fed->fed_grant;
        LASSERTF(exp->exp_obd->u.filter.fo_tot_pending >= fed->fed_pending,
                 "%s: tot_pending "LPU64" cli %s/%p fed_pending %lu\n",
                 obd->obd_name, filter->fo_tot_pending,
                 exp->exp_client_uuid.uuid, exp, fed->fed_pending);
        LASSERTF(filter->fo_tot_dirty >= fed->fed_dirty,
                 "%s: tot_dirty "LPU64" cli %s/%p fed_dirty %lu\n",
                 obd->obd_name, filter->fo_tot_dirty,
                 exp->exp_client_uuid.uuid, exp, fed->fed_dirty);
        filter->fo_tot_dirty -= fed->fed_dirty;
        fed->fed_dirty = 0;
        fed->fed_grant = 0;

        spin_unlock(&obd->obd_osfs_lock);
}

static int filter_destroy_export(struct obd_export *exp)
{
        ENTRY;

        if (exp->exp_filter_data.fed_pending)
                CERROR("%s: cli %s/%p has %lu pending on destroyed export\n",
                       exp->exp_obd->obd_name, exp->exp_client_uuid.uuid,
                       exp, exp->exp_filter_data.fed_pending);

        target_destroy_export(exp);

        if (exp->exp_obd->obd_replayable)
                filter_client_free(exp, exp->exp_flags);

        filter_grant_discard(exp);
        if (!(exp->exp_flags & OBD_OPT_FORCE))
                filter_grant_sanity_check(exp->exp_obd, __FUNCTION__);

        RETURN(0);
}

static void filter_sync_llogs(struct obd_export *dexp)
{
        struct filter_group_llog *fglog, *nlog;
        struct obd_device *obd = dexp->exp_obd;
        struct filter_obd *filter;
        int worked = 0, group;
        struct llog_ctxt *ctxt;

        filter = &obd->u.filter;

        /* we can't sync log holding spinlock. also, we do not want to get
         * into livelock. so we do following: loop over MDS's exports in
         * group order and skip already synced llogs -bzzz */
        do {
                /* look for group with min. number, but > worked */
                fglog = NULL;
                group = 1 << 30;
                spin_lock(&filter->fo_llog_list_lock);
                list_for_each_entry(nlog, &filter->fo_llog_list, list) {
                       
                        if (nlog->group <= worked) {
                                /* this group is already synced */
                                continue;
                        }
        
                        if (group < nlog->group) {
                                /* we have group with smaller number to sync */
                                continue;
                        }

                        /* store current minimal group */
                        fglog = nlog;
                        group = nlog->group;
                }
                spin_lock(&filter->fo_llog_list_lock);

                if (fglog) {
                        worked = fglog->group;
                        ctxt = llog_get_context(fglog->llogs,
                                                LLOG_UNLINK_REPL_CTXT);
                        llog_sync(ctxt, dexp);
                }
        } while (fglog != NULL);
}

/* also incredibly similar to mds_disconnect */
static int filter_disconnect(struct obd_export *exp, int flags)
{
        struct obd_device *obd = exp->exp_obd;
        unsigned long irqflags;
        int rc;
        ENTRY;

        LASSERT(exp);
        class_export_get(exp);

        spin_lock_irqsave(&exp->exp_lock, irqflags);
        exp->exp_flags = flags;
        spin_unlock_irqrestore(&exp->exp_lock, irqflags);

        if (!(flags & OBD_OPT_FORCE))
                filter_grant_sanity_check(obd, __FUNCTION__);
        filter_grant_discard(exp);

        /* Disconnect early so that clients can't keep using export */
        rc = class_disconnect(exp, flags);

        ldlm_cancel_locks_for_export(exp);

        fsfilt_sync(obd, obd->u.filter.fo_sb);

        /* flush any remaining cancel messages out to the target */
        filter_sync_llogs(exp);

        class_export_put(exp);
        RETURN(rc);
}

struct dentry *__filter_oa2dentry(struct obd_device *obd,
                                  struct obdo *oa, const char *what)
{
        struct dentry *dchild = NULL;
        obd_gr group = 0;

        if (oa->o_valid & OBD_MD_FLGROUP)
                group = oa->o_gr;

        dchild = filter_fid2dentry(obd, NULL, group, oa->o_id);

        if (IS_ERR(dchild)) {
                CERROR("%s error looking up object: "LPU64"\n", what, oa->o_id);
                RETURN(dchild);
        }

        if (dchild->d_inode == NULL) {
                CERROR("%s: %s on non-existent object: "LPU64"\n",
                       obd->obd_name, what, oa->o_id);
                f_dput(dchild);
                RETURN(ERR_PTR(-ENOENT));
        }

        return dchild;
}

static int filter_getattr(struct obd_export *exp, struct obdo *oa,
                          struct lov_stripe_md *md)
{
        struct dentry *dentry = NULL;
        struct obd_device *obd;
        int rc = 0;
        ENTRY;

        obd = class_exp2obd(exp);
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid client cookie "LPX64"\n",
                       exp->exp_handle.h_cookie);
                RETURN(-EINVAL);
        }

        dentry = filter_oa2dentry(obd, oa);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));

        /* Limit the valid bits in the return data to what we actually use */
        oa->o_valid = OBD_MD_FLID;
        obdo_from_inode(oa, dentry->d_inode, FILTER_VALID_FLAGS);

        f_dput(dentry);
        RETURN(rc);
}

/* this is called from filter_truncate() until we have filter_punch() */
static int filter_setattr(struct obd_export *exp, struct obdo *oa,
                          struct lov_stripe_md *md, struct obd_trans_info *oti)
{
        struct lvfs_run_ctxt saved;
        struct filter_obd *filter;
        struct dentry *dentry;
        struct iattr iattr;
        struct ldlm_res_id res_id = { .name = { oa->o_id, 0, oa->o_gr, 0 } };
        struct ldlm_resource *res;
        void *handle;
        int rc, rc2;
        ENTRY;

        LASSERT(oti != NULL);

        dentry = filter_oa2dentry(exp->exp_obd, oa);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));

        filter = &exp->exp_obd->u.filter;

        iattr_from_obdo(&iattr, oa, oa->o_valid);

        push_ctxt(&saved, &exp->exp_obd->obd_lvfs_ctxt, NULL);
        lock_kernel();

        if (iattr.ia_valid & ATTR_SIZE)
                down(&dentry->d_inode->i_sem);
        handle = fsfilt_start(exp->exp_obd, dentry->d_inode, FSFILT_OP_SETATTR,
                              oti);
        if (IS_ERR(handle))
                GOTO(out_unlock, rc = PTR_ERR(handle));

        /* XXX this could be a rwsem instead, if filter_preprw played along */
        if (iattr.ia_valid & ATTR_ATTR_FLAG)
                rc = fsfilt_iocontrol(exp->exp_obd, dentry->d_inode, NULL,
                                      EXT3_IOC_SETFLAGS,
                                      (long)&iattr.ia_attr_flags);
        else
                rc = fsfilt_setattr(exp->exp_obd, dentry, handle, &iattr, 1);
        rc = filter_finish_transno(exp, oti, rc);
        rc2 = fsfilt_commit(exp->exp_obd, filter->fo_sb, dentry->d_inode, handle, 0);
        if (rc2) {
                CERROR("error on commit, err = %d\n", rc2);
                if (!rc)
                        rc = rc2;
        }

        if (iattr.ia_valid & ATTR_SIZE) {
                res = ldlm_resource_get(exp->exp_obd->obd_namespace, NULL,
                                        res_id, LDLM_EXTENT, 0);
                if (res == NULL) {
                        CERROR("!!! resource_get failed for object "LPU64" -- "
                               "filter_setattr with no lock?\n", oa->o_id);
                } else {
                        if (res->lr_namespace->ns_lvbo &&
                            res->lr_namespace->ns_lvbo->lvbo_update) {
                                rc = res->lr_namespace->ns_lvbo->lvbo_update
                                        (res, NULL, 0, 0);
                        }
                        ldlm_resource_putref(res);
                }
        }

        oa->o_valid = OBD_MD_FLID;
        obdo_from_inode(oa, dentry->d_inode, FILTER_VALID_FLAGS);

out_unlock:
        if (iattr.ia_valid & ATTR_SIZE)
                up(&dentry->d_inode->i_sem);
        unlock_kernel();
        pop_ctxt(&saved, &exp->exp_obd->obd_lvfs_ctxt, NULL);

        f_dput(dentry);
        RETURN(rc);
}

/* XXX identical to osc_unpackmd */
static int filter_unpackmd(struct obd_export *exp, struct lov_stripe_md **lsmp,
                           struct lov_mds_md *lmm, int lmm_bytes)
{
        int lsm_size;
        ENTRY;

        if (lmm != NULL) {
                if (lmm_bytes < sizeof (*lmm)) {
                        CERROR("lov_mds_md too small: %d, need %d\n",
                               lmm_bytes, (int)sizeof(*lmm));
                        RETURN(-EINVAL);
                }
                /* XXX LOV_MAGIC etc check? */

                if (lmm->lmm_object_id == cpu_to_le64(0)) {
                        CERROR("lov_mds_md: zero lmm_object_id\n");
                        RETURN(-EINVAL);
                }
        }

        lsm_size = lov_stripe_md_size(1);
        if (lsmp == NULL)
                RETURN(lsm_size);

        if (*lsmp != NULL && lmm == NULL) {
                OBD_FREE(*lsmp, lsm_size);
                *lsmp = NULL;
                RETURN(0);
        }

        if (*lsmp == NULL) {
                OBD_ALLOC(*lsmp, lsm_size);
                if (*lsmp == NULL)
                        RETURN(-ENOMEM);

                loi_init((*lsmp)->lsm_oinfo);
        }

        if (lmm != NULL) {
                /* XXX zero *lsmp? */
                (*lsmp)->lsm_object_id = le64_to_cpu (lmm->lmm_object_id);
                LASSERT((*lsmp)->lsm_object_id);
        }

        (*lsmp)->lsm_maxbytes = LUSTRE_STRIPE_MAXBYTES;

        RETURN(lsm_size);
}

static void filter_destroy_precreated(struct obd_export *exp, struct obdo *oa,
                                      struct filter_obd *filter)
{
        struct obdo doa; /* XXX obdo on stack */
        __u64 last, id;
        ENTRY;
        LASSERT(oa);

        memset(&doa, 0, sizeof(doa));
        if (oa->o_valid & OBD_MD_FLGROUP) {
                doa.o_valid |= OBD_MD_FLGROUP;
                doa.o_gr = oa->o_gr;
        } else {
                doa.o_gr = 0;
        }
        doa.o_mode = S_IFREG;
        doa.o_gr = oa->o_gr;
        doa.o_valid = oa->o_valid & OBD_MD_FLGROUP;

        filter->fo_destroy_in_progress = 1;
        down(&filter->fo_create_lock);
        if (!filter->fo_destroy_in_progress) {
                CERROR("%s: destroy_in_progress already cleared\n",
                        exp->exp_obd->obd_name);
                up(&filter->fo_create_lock);
                EXIT;
                return;
        }

        last = filter_last_id(filter, doa.o_gr);
        CWARN("%s: deleting orphan objects from "LPU64" to "LPU64"\n",
               exp->exp_obd->obd_name, oa->o_id + 1, last);
        for (id = oa->o_id + 1; id <= last; id++) {
                doa.o_id = id;
                filter_destroy(exp, &doa, NULL, NULL);
        }

        CDEBUG(D_HA, "%s: after destroy: set last_objids["LPU64"] = "LPU64"\n",
               exp->exp_obd->obd_name, doa.o_gr, oa->o_id);

        spin_lock(&filter->fo_objidlock);
        filter->fo_last_objids[doa.o_gr] = oa->o_id;
        spin_unlock(&filter->fo_objidlock);

        filter->fo_destroy_in_progress = 0;
        up(&filter->fo_create_lock);

        EXIT;
}

/* returns a negative error or a nonnegative number of files to create */
static int filter_should_precreate(struct obd_export *exp, struct obdo *oa,
                                   obd_gr group)
{
        struct obd_device *obd = exp->exp_obd;
        struct filter_obd *filter = &obd->u.filter;
        int diff, rc;
        ENTRY;

        diff = oa->o_id - filter_last_id(filter, oa->o_gr);
        CDEBUG(D_INFO, "filter_last_id() = "LPU64" -> diff = %d\n",
               filter_last_id(filter, oa->o_gr), diff);

        /* delete orphans request */
        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
            (oa->o_flags & OBD_FL_DELORPHAN)) {
                if (diff >= 0)
                        RETURN(diff);
                if (-diff > 10000) { /* XXX make this smarter */
                        CERROR("ignoring bogus orphan destroy request: obdid "
                               LPU64" last_id "LPU64"\n",
                               oa->o_id, filter_last_id(filter, oa->o_gr));
                        RETURN(-EINVAL);
                }
                filter_destroy_precreated(exp, oa, filter);
                rc = filter_update_last_objid(obd, group, 0);
                if (rc)
                        CERROR("unable to write lastobjid, but orphans"
                               "were deleted\n");
                RETURN(0);
        } else {
                /* only precreate if group == 0 and o_id is specfied */
                if (!(oa->o_valid & OBD_FL_DELORPHAN) &&
                    (group != 0 || oa->o_id == 0))
                        RETURN(1);

                LASSERT(diff >= 0);
                RETURN(diff);
        }
}
static int filter_precreate_rec(struct obd_device *obd, struct dentry *dentry, 
                                int *number, struct obdo *oa)
{
        int    rc = 0;
        ENTRY;       
         
        rc = fsfilt_precreate_rec(obd, dentry, number, oa);
  
        RETURN(rc);
}
/* We rely on the fact that only one thread will be creating files in a given
 * group at a time, which is why we don't need an atomic filter_get_new_id.
 * Even if we had that atomic function, the following race would exist:
 *
 * thread 1: gets id x from filter_next_id
 * thread 2: gets id (x + 1) from filter_next_id
 * thread 2: creates object (x + 1)
 * thread 1: tries to create object x, gets -ENOSPC
 */
static int filter_precreate(struct obd_device *obd, struct obdo *oa,
                            obd_gr group, int *num)
{
        struct dentry *dchild = NULL, *dparent = NULL;
        struct filter_obd *filter;
        int err = 0, rc = 0, recreate_obj = 0, i;
        __u64 next_id;
        void *handle = NULL;
        ENTRY;

        filter = &obd->u.filter;

        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
            (oa->o_flags & OBD_FL_RECREATE_OBJS)) {
                recreate_obj = 1;
        }

        CDEBUG(D_HA, "%s: precreating %d objects\n", obd->obd_name, *num);

        down(&filter->fo_create_lock);

        for (i = 0; i < *num && err == 0; i++) {
                int cleanup_phase = 0;

                if (filter->fo_destroy_in_progress) {
                        CWARN("%s: precreate aborted by destroy\n",
                              obd->obd_name);
                        break;
                }

                if (recreate_obj) {
                        __u64 last_id;
                        next_id = oa->o_id;
                        last_id = filter_last_id(filter, group);
                        if (next_id > last_id) {
                                CERROR("Error: Trying to recreate obj greater"
                                       "than last id "LPD64" > "LPD64"\n",
                                       next_id, last_id);
                                GOTO(cleanup, rc = -EINVAL);
                        }
                } else {
                        next_id = filter_last_id(filter, group) + 1;
                }

                CDEBUG(D_INFO, "precreate objid "LPU64"\n", next_id);

                dparent = filter_parent_lock(obd, group, next_id);
                if (IS_ERR(dparent))
                        GOTO(cleanup, rc = PTR_ERR(dparent));
                cleanup_phase = 1;

                /*only do precreate rec record. so clean kml flags here*/
                fsfilt_clear_kml_flags(obd, dparent->d_inode);
                
                dchild = filter_fid2dentry(obd, dparent, group, next_id);
                if (IS_ERR(dchild))
                        GOTO(cleanup, rc = PTR_ERR(dchild));
                cleanup_phase = 2;

                if (dchild->d_inode != NULL) {
                        /* This would only happen if lastobjid was bad on disk*/
                        /* Could also happen if recreating missing obj but
                         * already exists
                         */
                        if (recreate_obj) {
                                CERROR("%s: Serious error: recreating obj %*s "
                                       "but obj already exists \n",
                                       obd->obd_name, dchild->d_name.len,
                                       dchild->d_name.name);
                                LBUG();
                        } else {
                                CERROR("%s: Serious error: objid %*s already "
                                       "exists; is this filesystem corrupt?\n",
                                       obd->obd_name, dchild->d_name.len,
                                       dchild->d_name.name);
                                LBUG();
                        }
                        GOTO(cleanup, rc = -EEXIST);
                }

                handle = fsfilt_start_log(obd, dparent->d_inode,
                                          FSFILT_OP_CREATE, NULL, 1);
                if (IS_ERR(handle))
                        GOTO(cleanup, rc = PTR_ERR(handle));
                cleanup_phase = 3;

                rc = ll_vfs_create(dparent->d_inode, dchild, S_IFREG, NULL);
                if (rc) {
                        CERROR("create failed rc = %d\n", rc);
                        GOTO(cleanup, rc);
                }

                if (!recreate_obj) {
                        filter_set_last_id(filter, group, next_id);
                        err = filter_update_last_objid(obd, group, 0);
                        if (err)
                                CERROR("unable to write lastobjid "
                                       "but file created\n");
                }
                fsfilt_set_kml_flags(obd, dparent->d_inode);
        
        cleanup:
                switch(cleanup_phase) {
                case 3:
                        err = fsfilt_commit(obd, filter->fo_sb, dparent->d_inode, handle, 0);
                        if (err) {
                                CERROR("error on commit, err = %d\n", err);
                                if (!rc)
                                        rc = err;
                        }
                case 2:
                        f_dput(dchild);
                case 1:
                        filter_parent_unlock(dparent);
                case 0:
                        break;
                }

                if (rc)
                        break;
        }
        *num = i;

        rc = filter_precreate_rec(obd, dparent, num, oa);
        
        up(&filter->fo_create_lock);
        
        CDEBUG(D_HA, "%s: server last_objid for group "LPU64": "LPU64"\n",
               obd->obd_name, group, filter->fo_last_objids[group]);

        CDEBUG(D_HA, "%s: filter_precreate() created %d objects\n",
               obd->obd_name, i);
        RETURN(rc);
}

static int filter_create(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct obd_device *obd = NULL;
        struct filter_obd *filter;
        struct lvfs_run_ctxt saved;
        struct lov_stripe_md *lsm = NULL;
        struct filter_export_data *fed;
        int group = oa->o_gr;
        char str[PTL_NALFMT_SIZE];
        int rc = 0, diff;
        ENTRY;

        if (!(oa->o_valid & OBD_MD_FLGROUP) || group == 0) {
                portals_nid2str(exp->exp_connection->c_peer.peer_ni->pni_number,
                                exp->exp_connection->c_peer.peer_nid, str);
                CERROR("!!! nid "LPX64"/%s sent invalid object group %d\n",
                       exp->exp_connection->c_peer.peer_nid, str, group);
                RETURN(-EINVAL);
        }

        obd = exp->exp_obd;
        fed = &exp->exp_filter_data;
        filter = &obd->u.filter;

        if (fed->fed_group != group && !(oa->o_valid & OBD_MD_REINT)) {
                portals_nid2str(exp->exp_connection->c_peer.peer_ni->pni_number,
                                exp->exp_connection->c_peer.peer_nid, str);
                CERROR("!!! This export (nid "LPX64"/%s) used object group %d "
                       "earlier; now it's trying to use group %d!  This could "
                       "be a bug in the MDS.  Tell CFS.\n",
                       exp->exp_connection->c_peer.peer_nid, str,
                       fed->fed_group, group);
                RETURN(-ENOTUNIQ);
        }

        CDEBUG(D_INFO, "filter_create(od->o_gr=%d,od->o_id="LPU64")\n",
               group, oa->o_id);
        if (ea != NULL) {
                lsm = *ea;
                if (lsm == NULL) {
                        rc = obd_alloc_memmd(exp, &lsm);
                        if (rc < 0)
                                RETURN(rc);
                }
        }

        obd = exp->exp_obd;
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        if (oa->o_valid & OBD_MD_REINT) {
                int num = *((int*)oa->o_inline);  
                rc = filter_precreate(obd, oa, oa->o_gr, &num);
        } else if ((oa->o_valid & OBD_MD_FLFLAGS) &&
                   (oa->o_flags & OBD_FL_RECREATE_OBJS)) {
                if (oa->o_id > filter_last_id(&obd->u.filter, group)) {
                        CERROR("recreate objid "LPU64" > last id "LPU64"\n",
                               oa->o_id, filter_last_id(&obd->u.filter, group));
                        rc = -EINVAL;
                } else {
                        diff = 1;
                        rc = filter_precreate(obd, oa, group, &diff);
                }
        } else {
                diff = filter_should_precreate(exp, oa, group);
                if (diff > 0) {
                        oa->o_id = filter_last_id(&obd->u.filter, group);
                        rc = filter_precreate(obd, oa, group, &diff);
                        oa->o_id += diff;
                        oa->o_valid = OBD_MD_FLID;
                }
        }

        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc && ea != NULL && *ea != lsm) {
                obd_free_memmd(exp, &lsm);
        } else if (rc == 0 && ea != NULL) {
                /* XXX LOV STACKING: the lsm that is passed to us from
                 * LOV does not have valid lsm_oinfo data structs, so
                 * don't go touching that.  This needs to be fixed in a
                 * big way. */
                lsm->lsm_object_id = oa->o_id;
                lsm->lsm_object_gr = oa->o_gr;
                *ea = lsm;
        }

        RETURN(rc);
}

static int filter_destroy(struct obd_export *exp, struct obdo *oa,
                          struct lov_stripe_md *ea, struct obd_trans_info *oti)
{
        struct obd_device *obd;
        struct filter_obd *filter;
        struct dentry *dchild = NULL, *dparent = NULL;
        struct lvfs_run_ctxt saved;
        void *handle = NULL;
        struct llog_cookie *fcc = NULL;
        int rc, rc2, cleanup_phase = 0, have_prepared = 0;
        ENTRY;

        LASSERT(oa->o_valid & OBD_MD_FLGROUP);

        obd = exp->exp_obd;
        filter = &obd->u.filter;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

 acquire_locks:
        dparent = filter_parent_lock(obd, oa->o_gr, oa->o_id);
        if (IS_ERR(dparent))
                GOTO(cleanup, rc = PTR_ERR(dparent));
        cleanup_phase = 1;

        dchild = filter_fid2dentry(obd, dparent, oa->o_gr, oa->o_id);
        if (IS_ERR(dchild))
                GOTO(cleanup, rc = -ENOENT);
        cleanup_phase = 2;

        if (dchild->d_inode == NULL) {
                CDEBUG(D_INODE, "destroying non-existent object "LPU64"\n",
                       oa->o_id);
                GOTO(cleanup, rc = -ENOENT);
        }

        if (!have_prepared) {
                /* If we're really going to destroy the object, get ready
                 * by getting the clients to discard their cached data.
                 *
                 * We have to drop the parent lock, because
                 * filter_prepare_destroy will acquire a PW on the object, and
                 * we don't want to deadlock with an incoming write to the
                 * object, which has the extent PW and then wants to get the
                 * parent dentry to do the lookup.
                 *
                 * We dput the child because it's not worth the extra
                 * complication of condition the above code to skip it on the
                 * second time through. */
                f_dput(dchild);
                filter_parent_unlock(dparent);

                filter_prepare_destroy(obd, oa->o_id, oa->o_gr);
                have_prepared = 1;
                goto acquire_locks;
        }

        handle = fsfilt_start_log(obd, dparent->d_inode, FSFILT_OP_UNLINK, oti, 1);
        if (IS_ERR(handle))
                GOTO(cleanup, rc = PTR_ERR(handle));

        cleanup_phase = 3;

        /* Our MDC connection is established by the MDS to us */
        if (oa->o_valid & OBD_MD_FLCOOKIE) {
                OBD_ALLOC(fcc, sizeof(*fcc));
                if (fcc != NULL)
                        memcpy(fcc, obdo_logcookie(oa), sizeof(*fcc));
        }

        rc = filter_destroy_internal(obd, oa->o_id, dparent, dchild);

cleanup:
        switch(cleanup_phase) {
        case 3:
                if (fcc != NULL) {
                        if (oti != NULL)
                                fsfilt_add_journal_cb(obd, filter->fo_sb, 0,
                                                      oti->oti_handle,
                                                      filter_cancel_cookies_cb,
                                                      fcc);
                        else
                                fsfilt_add_journal_cb(obd, filter->fo_sb, 0,
                                                      handle,
                                                      filter_cancel_cookies_cb,
                                                      fcc);
                }
                rc = filter_finish_transno(exp, oti, rc);
                rc2 = fsfilt_commit(obd, filter->fo_sb, dparent->d_inode, 
                                    handle, 0);
                if (rc2) {
                        CERROR("error on commit, err = %d\n", rc2);
                        if (!rc)
                                rc = rc2;
                }
        case 2:
                f_dput(dchild);
        case 1:
                filter_parent_unlock(dparent);
        case 0:
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                break;
        default:
                CERROR("invalid cleanup_phase %d\n", cleanup_phase);
                LBUG();
        }

        RETURN(rc);
}

/* NB start and end are used for punch, but not truncate */
static int filter_truncate(struct obd_export *exp, struct obdo *oa,
                           struct lov_stripe_md *lsm,
                           obd_off start, obd_off end,
                           struct obd_trans_info *oti)
{
        int error;
        ENTRY;

        if (end != OBD_OBJECT_EOF)
                CERROR("PUNCH not supported, only truncate: end = "LPX64"\n",
                       end);

        CDEBUG(D_INODE, "calling truncate for object "LPU64", valid = %x, "
               "o_size = "LPD64"\n", oa->o_id, oa->o_valid, start);
        oa->o_size = start;
        error = filter_setattr(exp, oa, NULL, oti);
        RETURN(error);
}

static int filter_sync(struct obd_export *exp, struct obdo *oa,
                       struct lov_stripe_md *lsm, obd_off start, obd_off end)
{
        struct obd_device *obd = exp->exp_obd;
        struct lvfs_run_ctxt saved;
        struct filter_obd *filter;
        struct dentry *dentry;
        struct llog_ctxt *ctxt;
        int rc, rc2;
        ENTRY;

        filter = &obd->u.filter;

        /* an objid of zero is taken to mean "sync whole filesystem" */
        if (!oa || !(oa->o_valid & OBD_MD_FLID)) {
                rc = fsfilt_sync(obd, filter->fo_sb);
                /* flush any remaining cancel messages out to the target */
                ctxt = llog_get_context(&obd->obd_llogs, LLOG_UNLINK_REPL_CTXT);
                llog_sync(ctxt, exp);
                RETURN(rc);
        }

        dentry = filter_oa2dentry(obd, oa);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));

        push_ctxt(&saved, &exp->exp_obd->obd_lvfs_ctxt, NULL);

        down(&dentry->d_inode->i_sem);
        rc = filemap_fdatasync(dentry->d_inode->i_mapping);
        if (rc == 0) {
                /* just any file to grab fsync method - "file" arg unused */
                struct file *file = filter->fo_rcvd_filp;

                if (file->f_op && file->f_op->fsync)
                        rc = file->f_op->fsync(NULL, dentry, 1);

                rc2 = filemap_fdatawait(dentry->d_inode->i_mapping);
                if (!rc)
                        rc = rc2;
        }
        up(&dentry->d_inode->i_sem);

        oa->o_valid = OBD_MD_FLID;
        obdo_from_inode(oa, dentry->d_inode, FILTER_VALID_FLAGS);

        pop_ctxt(&saved, &exp->exp_obd->obd_lvfs_ctxt, NULL);

        f_dput(dentry);
        RETURN(rc);
}

static int filter_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                         unsigned long max_age)
{
        struct filter_obd *filter = &obd->u.filter;
        int blockbits = filter->fo_sb->s_blocksize_bits;
        int rc;
        ENTRY;

        /* at least try to account for cached pages.  its still racey and
         * might be under-reporting if clients haven't announced their
         * caches with brw recently */
        spin_lock(&obd->obd_osfs_lock);
        rc = fsfilt_statfs(obd, filter->fo_sb, max_age);
        memcpy(osfs, &obd->obd_osfs, sizeof(*osfs));
        spin_unlock(&obd->obd_osfs_lock);

        CDEBUG(D_SUPER | D_CACHE, "blocks cached "LPU64" granted "LPU64
               " pending "LPU64" free "LPU64" avail "LPU64"\n",
               filter->fo_tot_dirty, filter->fo_tot_granted,
               filter->fo_tot_pending,
               osfs->os_bfree << blockbits, osfs->os_bavail << blockbits);

        filter_grant_sanity_check(obd, __FUNCTION__);

        osfs->os_bavail -= min(osfs->os_bavail,
                               (filter->fo_tot_dirty + filter->fo_tot_pending +
                                osfs->os_bsize -1) >> blockbits);

        RETURN(rc);
}

static int filter_get_info(struct obd_export *exp, __u32 keylen,
                           void *key, __u32 *vallen, void *val)
{
        struct filter_export_data *fed = &exp->exp_filter_data;
        struct obd_device *obd;
        ENTRY;

        obd = class_exp2obd(exp);
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid client cookie "LPX64"\n",
                       exp->exp_handle.h_cookie);
                RETURN(-EINVAL);
        }

        if (keylen == strlen("blocksize") &&
            memcmp(key, "blocksize", keylen) == 0) {
                __u32 *blocksize = val;
                *vallen = sizeof(*blocksize);
                *blocksize = obd->u.filter.fo_sb->s_blocksize;
                RETURN(0);
        }

        if (keylen == strlen("blocksize_bits") &&
            memcmp(key, "blocksize_bits", keylen) == 0) {
                __u32 *blocksize_bits = val;
                *vallen = sizeof(*blocksize_bits);
                *blocksize_bits = obd->u.filter.fo_sb->s_blocksize_bits;
                RETURN(0);
        }

        if (keylen >= strlen("last_id") && memcmp(key, "last_id", 7) == 0) {
                obd_id *last_id = val;
                *last_id = filter_last_id(&obd->u.filter, fed->fed_group);
                RETURN(0);
        }
        if (keylen >= strlen("reint_log") && memcmp(key, "reint_log", 9) == 0) {
                /*Get log_context handle*/
                unsigned long *llh_handle = val;
                *vallen = sizeof(unsigned long);
                *llh_handle = (unsigned long)obd->obd_llog_ctxt[LLOG_REINT_ORIG_CTXT];
                RETURN(0);
        }
        if (keylen >= strlen("cache_sb") && memcmp(key, "cache_sb", 8) == 0) {
                /*Get log_context handle*/
                unsigned long *sb = val;
                *vallen = sizeof(unsigned long);
                *sb = (unsigned long)obd->u.filter.fo_sb;
                RETURN(0);
        }

        CDEBUG(D_IOCTL, "invalid key\n");
        RETURN(-EINVAL);
}

struct obd_llogs *filter_grab_llog_for_group(struct obd_device *obd, int group)
{
        struct filter_group_llog *fglog, *nlog;
        char name[32] = "CATLIST";
        struct filter_obd *filter;
        struct list_head *cur;
        int rc;

        filter = &obd->u.filter;

        spin_lock(&filter->fo_llog_list_lock);
        list_for_each(cur, &filter->fo_llog_list) {
                fglog = list_entry(cur, struct filter_group_llog, list);
                if (fglog->group == group) {
                        spin_unlock(&filter->fo_llog_list_lock);
                        RETURN(fglog->llogs);
                }
        }
        spin_unlock(&filter->fo_llog_list_lock);

        OBD_ALLOC(fglog, sizeof(*fglog));
        if (fglog == NULL)
                RETURN(NULL);
        fglog->group = group;

        OBD_ALLOC(fglog->llogs, sizeof(struct obd_llogs));
        if (fglog->llogs == NULL) {
                OBD_FREE(fglog, sizeof(*fglog));
                RETURN(NULL);
        }

        spin_lock(&filter->fo_llog_list_lock);
        list_for_each(cur, &filter->fo_llog_list) {
                nlog = list_entry(cur, struct filter_group_llog, list);
                LASSERT(nlog->group != group);
        }
        list_add(&fglog->list, &filter->fo_llog_list);
        spin_unlock(&filter->fo_llog_list_lock);

        rc = obd_llog_cat_initialize(obd, fglog->llogs, 1, name);
        if (rc) {
                OBD_FREE(fglog->llogs, sizeof(*(fglog->llogs)));
                OBD_FREE(fglog, sizeof(*fglog));
                RETURN(NULL);
        }

        CDEBUG(D_OTHER, "%s: new llog 0x%p for group %u\n", obd->obd_name,
                fglog->llogs, group);

        RETURN(fglog->llogs);
}

static int filter_set_info(struct obd_export *exp, __u32 keylen,
                           void *key, __u32 vallen, void *val)
{
        struct lvfs_run_ctxt saved;
        struct filter_export_data *fed = &exp->exp_filter_data;
        struct obd_device *obd;
        struct lustre_handle conn;
        struct obd_llogs *llog;
        struct llog_ctxt *ctxt;
        __u32 group;
        int rc = 0;
        ENTRY;

        conn.cookie = exp->exp_handle.h_cookie;

        obd = exp->exp_obd;
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid exp %p cookie "LPX64"\n",
                       exp, conn.cookie);
                RETURN(-EINVAL);
        }

        if (keylen < strlen("mds_conn") ||
            memcmp(key, "mds_conn", keylen) != 0)
                RETURN(-EINVAL);

        group = *((__u32 *)val);
        if (fed->fed_group != 0 && fed->fed_group != group) {
                char str[PTL_NALFMT_SIZE];
                portals_nid2str(exp->exp_connection->c_peer.peer_ni->pni_number,
                                exp->exp_connection->c_peer.peer_nid, str);
                CERROR("!!! This export (nid "LPX64"/%s) used object group %d "
                       "earlier; now it's trying to use group %d!  This could "
                       "be a bug in the MDS.  Tell CFS.\n",
                       exp->exp_connection->c_peer.peer_nid, str,
                       fed->fed_group, group);
                RETURN(-EPROTO);
        }
        fed->fed_group = group;
        CWARN("Received MDS connection ("LPX64"); group %d\n", conn.cookie,
              group);

        LASSERT(rc == 0);

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = filter_read_groups(obd, group, 1);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc != 0) {
                CERROR("can't read group %u\n", group);
                RETURN(rc);
        }
        rc = filter_group_set_kml_flags(obd, group);
         if (rc != 0) {
                CERROR("can't set kml flags %u\n", group);
                RETURN(rc);
        }
        llog = filter_grab_llog_for_group(obd, group);
        LASSERT(llog != NULL);

        ctxt = llog_get_context(llog, LLOG_UNLINK_REPL_CTXT);
        LASSERT(ctxt != NULL);
        rc = llog_receptor_accept(ctxt, exp->exp_imp_reverse);
        RETURN(rc);
}

int filter_iocontrol(unsigned int cmd, struct obd_export *exp,
                     int len, void *karg, void *uarg)
{
        struct obd_device *obd = exp->exp_obd;
        struct obd_ioctl_data *data = karg;
        int rc = 0;

        switch (cmd) {
        case OBD_IOC_ABORT_RECOVERY:
                CERROR("aborting recovery for device %s\n", obd->obd_name);
                target_abort_recovery(obd);
                RETURN(0);

        case OBD_IOC_SET_READONLY: {
                void *handle;
                struct super_block *sb = obd->u.filter.fo_sb;
                struct inode *inode = sb->s_root->d_inode;
                BDEVNAME_DECLARE_STORAGE(tmp);
                CERROR("setting device %s read-only\n",
                       ll_bdevname(sb, tmp));

                handle = fsfilt_start(obd, inode, FSFILT_OP_MKNOD, NULL);
                LASSERT(handle);
                (void)fsfilt_commit(obd, sb, inode, handle, 1);

                dev_set_rdonly(ll_sbdev(obd->u.filter.fo_sb), 2);
                RETURN(0);
        }

        case OBD_IOC_CATLOGLIST: {
                rc = llog_catalog_list(obd, 1, data);
                RETURN(rc);
        }

        case OBD_IOC_LLOG_CANCEL:
        case OBD_IOC_LLOG_REMOVE:
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                /* FIXME to be finished */
                RETURN(-EOPNOTSUPP);
/*
                struct llog_ctxt *ctxt = NULL;

                push_ctxt(&saved, &ctxt->loc_ctxt, NULL);
                rc = llog_ioctl(ctxt, cmd, data);
                pop_ctxt(&saved, &ctxt->loc_ctxt, NULL);

                RETURN(rc);
*/
        }


        default:
                RETURN(-EINVAL);
        }
        RETURN(0);
}

static struct llog_operations filter_unlink_repl_logops;
static struct llog_operations filter_size_orig_logops = {
        lop_setup: llog_obd_origin_setup,
        lop_cleanup: llog_catalog_cleanup,
        lop_add: llog_catalog_add,
};

static int filter_llog_init(struct obd_device *obd, struct obd_llogs *llogs,
                            struct obd_device *tgt, int count,
                            struct llog_catid *catid)
{
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        filter_unlink_repl_logops = llog_client_ops;
        filter_unlink_repl_logops.lop_cancel = llog_obd_repl_cancel;
        filter_unlink_repl_logops.lop_connect = llog_repl_connect;
        filter_unlink_repl_logops.lop_sync = llog_obd_repl_sync;

        rc = obd_llog_setup(obd, llogs, LLOG_UNLINK_REPL_CTXT, tgt, 0, NULL,
                        &filter_unlink_repl_logops);
        if (rc)
                RETURN(rc);
        /* FIXME - assign unlink_cb for filter's recovery */
        ctxt = llog_get_context(llogs, LLOG_UNLINK_REPL_CTXT);
        ctxt->llog_proc_cb = filter_recov_log_unlink_cb;

        /* FIXME - count should be 1 to setup size log */
        rc = obd_llog_setup(obd, llogs, LLOG_SIZE_ORIG_CTXT, tgt, 0, 
                            &catid->lci_logid, &filter_size_orig_logops);
        RETURN(rc);
}

static int filter_llog_finish(struct obd_device *obd,
                              struct obd_llogs *llogs, int count)
{
        int rc;
        ENTRY;

        rc = obd_llog_cleanup(llog_get_context(llogs, LLOG_UNLINK_REPL_CTXT));
        if (rc)
                RETURN(rc);

        rc = obd_llog_cleanup(llog_get_context(llogs, LLOG_SIZE_ORIG_CTXT));
        RETURN(rc);
}

static int filter_llog_connect(struct obd_device *obd,
                               struct llogd_conn_body *body) 
{
        struct llog_ctxt *ctxt;
        struct obd_llogs *llog;
        int rc;
        ENTRY;

        CDEBUG(D_OTHER, "handle connect for %s: %u/%u/%u\n", obd->obd_name,
               (unsigned) body->lgdc_logid.lgl_ogr,
               (unsigned) body->lgdc_logid.lgl_oid,
               (unsigned) body->lgdc_logid.lgl_ogen);
        llog = filter_grab_llog_for_group(obd, body->lgdc_logid.lgl_ogr);
        LASSERT(llog != NULL);
        ctxt = llog_get_context(llog, body->lgdc_ctxt_idx);
        rc = llog_connect(ctxt, 1, &body->lgdc_logid,
                          &body->lgdc_gen, NULL);
        if (rc != 0)
                CERROR("failed to connect\n");

        RETURN(rc);
}

static struct dentry *filter_lvfs_fid2dentry(__u64 id, __u32 gen, __u64 gr,
                                             void *data)
{
        return filter_fid2dentry(data, NULL, gr, id);
}

static struct lvfs_callback_ops filter_lvfs_ops = {
        l_fid2dentry:     filter_lvfs_fid2dentry,
};

static struct obd_ops filter_obd_ops = {
        .o_owner          = THIS_MODULE,
        .o_attach         = filter_attach,
        .o_detach         = filter_detach,
        .o_get_info       = filter_get_info,
        .o_set_info       = filter_set_info,
        .o_setup          = filter_setup,
        .o_precleanup     = filter_precleanup,
        .o_cleanup        = filter_cleanup,
        .o_connect        = filter_connect,
        .o_disconnect     = filter_disconnect,
        .o_statfs         = filter_statfs,
        .o_getattr        = filter_getattr,
        .o_unpackmd       = filter_unpackmd,
        .o_create         = filter_create,
        .o_setattr        = filter_setattr,
        .o_destroy        = filter_destroy,
        .o_brw            = filter_brw,
        .o_punch          = filter_truncate,
        .o_sync           = filter_sync,
        .o_preprw         = filter_preprw,
        .o_commitrw       = filter_commitrw,
        .o_write_extents  = filter_write_extents,
        .o_destroy_export = filter_destroy_export,
        .o_llog_init      = filter_llog_init,
        .o_llog_finish    = filter_llog_finish,
        .o_llog_connect   = filter_llog_connect,
        .o_iocontrol      = filter_iocontrol,
};

static struct obd_ops filter_sanobd_ops = {
        .o_owner          = THIS_MODULE,
        .o_attach         = filter_attach,
        .o_detach         = filter_detach,
        .o_get_info       = filter_get_info,
        .o_set_info       = filter_set_info,
        .o_setup          = filter_san_setup,
        .o_precleanup     = filter_precleanup,
        .o_cleanup        = filter_cleanup,
        .o_connect        = filter_connect,
        .o_disconnect     = filter_disconnect,
        .o_statfs         = filter_statfs,
        .o_getattr        = filter_getattr,
        .o_unpackmd       = filter_unpackmd,
        .o_create         = filter_create,
        .o_setattr        = filter_setattr,
        .o_destroy        = filter_destroy,
        .o_brw            = filter_brw,
        .o_punch          = filter_truncate,
        .o_sync           = filter_sync,
        .o_preprw         = filter_preprw,
        .o_commitrw       = filter_commitrw,
        .o_write_extents  = filter_write_extents,
        .o_san_preprw     = filter_san_preprw,
        .o_destroy_export = filter_destroy_export,
        .o_llog_init      = filter_llog_init,
        .o_llog_finish    = filter_llog_finish,
        .o_llog_connect   = filter_llog_connect,
        .o_iocontrol      = filter_iocontrol,
};

static int __init obdfilter_init(void)
{
        struct lprocfs_static_vars lvars;
        int rc;

        printk(KERN_INFO "Lustre: Filtering OBD driver; info@clusterfs.com\n");

        lprocfs_init_vars(filter, &lvars);

        rc = class_register_type(&filter_obd_ops, NULL, lvars.module_vars,
                                 OBD_FILTER_DEVICENAME);
        if (rc)
                return rc;

        rc = class_register_type(&filter_sanobd_ops, NULL, lvars.module_vars,
                                 OBD_FILTER_SAN_DEVICENAME);
        if (rc)
                class_unregister_type(OBD_FILTER_DEVICENAME);
        return rc;
}

static void __exit obdfilter_exit(void)
{
        class_unregister_type(OBD_FILTER_SAN_DEVICENAME);
        class_unregister_type(OBD_FILTER_DEVICENAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Filtering OBD driver");
MODULE_LICENSE("GPL");

module_init(obdfilter_init);
module_exit(obdfilter_exit);
