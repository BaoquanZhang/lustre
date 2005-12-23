/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/quota/quota_master.c
 *  Lustre Quota Master request handler
 *
 *  Copyright (c) 2001-2005 Cluster File Systems, Inc.
 *   Author: Niu YaWei <niu@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   No redistribution or use is permitted outside of Cluster File Systems, Inc.
 *
 */
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_MDS

#include <linux/version.h>
#include <linux/fs.h>
#include <asm/unistd.h>
#include <linux/slab.h>
#include <linux/quotaops.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/quota.h>

#include <linux/obd_class.h>
#include <linux/lustre_quota.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_mds.h>

#include "quota_internal.h"

/* lock ordering: 
 * mds->mds_qonoff_sem > dquot->dq_sem */
static struct list_head lustre_dquot_hash[NR_DQHASH];
static spinlock_t dquot_hash_lock = SPIN_LOCK_UNLOCKED;

kmem_cache_t *lustre_dquot_cachep;

int lustre_dquot_init(void)
{
        int i;
        ENTRY;

        LASSERT(lustre_dquot_cachep == NULL);
        lustre_dquot_cachep = kmem_cache_create("lustre_dquot_cache",
                                                sizeof(struct lustre_dquot),
                                                0, 0, NULL, NULL);
        if (!lustre_dquot_cachep)
                return (-ENOMEM);

        for (i = 0; i < NR_DQHASH; i++) {
                INIT_LIST_HEAD(lustre_dquot_hash + i);
        }
        RETURN(0);
}

void lustre_dquot_exit(void)
{
        int i;
        ENTRY;
        /* FIXME cleanup work ?? */

        for (i = 0; i < NR_DQHASH; i++) {
                LASSERT(list_empty(lustre_dquot_hash + i));
        }
        if (lustre_dquot_cachep) {
                int rc;
                rc = kmem_cache_destroy(lustre_dquot_cachep);
                LASSERT(rc == 0);
                lustre_dquot_cachep = NULL;
        }
        EXIT;
}

static inline int const dquot_hashfn(struct lustre_quota_info *info,
                                     unsigned int id, int type)
{
        unsigned long tmp = ((unsigned long)info >> L1_CACHE_SHIFT) ^ id;
        tmp = (tmp * (MAXQUOTAS - type)) % NR_DQHASH;
        return tmp;
}

/* caller must hold dquot_hash_lock */
static struct lustre_dquot *find_dquot(int hashent,
                                       struct lustre_quota_info *lqi, qid_t id,
                                       int type)
{
        struct lustre_dquot *dquot;
        ENTRY;

        LASSERT_SPIN_LOCKED(&dquot_hash_lock);
        list_for_each_entry(dquot, &lustre_dquot_hash[hashent], dq_hash) {
                if (dquot->dq_info == lqi &&
                    dquot->dq_id == id && dquot->dq_type == type)
                        RETURN(dquot);
        }
        RETURN(NULL);
}

static struct lustre_dquot *alloc_dquot(struct lustre_quota_info *lqi,
                                        qid_t id, int type)
{
        struct lustre_dquot *dquot = NULL;
        ENTRY;

        OBD_SLAB_ALLOC(dquot, lustre_dquot_cachep, SLAB_NOFS, sizeof(*dquot));
        if (dquot == NULL)
                RETURN(NULL);

        INIT_LIST_HEAD(&dquot->dq_hash);
        init_mutex_locked(&dquot->dq_sem);
        dquot->dq_refcnt = 1;
        dquot->dq_info = lqi;
        dquot->dq_id = id;
        dquot->dq_type = type;
        dquot->dq_status = DQ_STATUS_AVAIL;

        RETURN(dquot);
}

static void free_dquot(struct lustre_dquot *dquot)
{
        OBD_SLAB_FREE(dquot, lustre_dquot_cachep, sizeof(*dquot));
}

static void insert_dquot_nolock(struct lustre_dquot *dquot)
{
        struct list_head *head = lustre_dquot_hash +
            dquot_hashfn(dquot->dq_info, dquot->dq_id, dquot->dq_type);
        LASSERT(list_empty(&dquot->dq_hash));
        list_add(&dquot->dq_hash, head);
}

static void remove_dquot_nolock(struct lustre_dquot *dquot)
{
        LASSERT(!list_empty(&dquot->dq_hash));
        list_del_init(&dquot->dq_hash);
}

static void lustre_dqput(struct lustre_dquot *dquot)
{
        ENTRY;
        spin_lock(&dquot_hash_lock);
        LASSERT(dquot->dq_refcnt);
        dquot->dq_refcnt--;
        if (!dquot->dq_refcnt) {
                remove_dquot_nolock(dquot);
                free_dquot(dquot);
        }
        spin_unlock(&dquot_hash_lock);
        EXIT;
}

static struct lustre_dquot *lustre_dqget(struct obd_device *obd,
                                         struct lustre_quota_info *lqi,
                                         qid_t id, int type)
{
        unsigned int hashent = dquot_hashfn(lqi, id, type);
        struct lustre_dquot *dquot, *empty;
        ENTRY;

        if ((empty = alloc_dquot(lqi, id, type)) == NULL)
                RETURN(ERR_PTR(-ENOMEM));
        
        spin_lock(&dquot_hash_lock);
        if ((dquot = find_dquot(hashent, lqi, id, type)) != NULL) {
                dquot->dq_refcnt++;
                spin_unlock(&dquot_hash_lock);
                free_dquot(empty);
        } else {
                int rc;

                dquot = empty;
                insert_dquot_nolock(dquot);
                spin_unlock(&dquot_hash_lock);

                rc = fsfilt_dquot(obd, dquot, QFILE_RD_DQUOT);
                up(&dquot->dq_sem);
                if (rc) {
                        CERROR("can't read dquot from admin quotafile! "
                               "(rc:%d)\n", rc);
                        lustre_dqput(dquot);
                        RETURN(ERR_PTR(rc));
                }

        }

        LASSERT(dquot);
        RETURN(dquot);
}

int dqacq_handler(struct obd_device *obd, struct qunit_data *qdata, int opc)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *info = &mds->mds_quota_info;
        struct lustre_dquot *dquot = NULL;
        __u64 *usage = NULL;
        __u32 hlimit = 0, slimit = 0;
        time_t *time = NULL;
        unsigned int grace = 0;
        int rc = 0;
        ENTRY;

        OBD_FAIL_RETURN(OBD_FAIL_OBD_DQACQ, -EIO);

        /* slaves never acquires qunit for user root */
        LASSERT(qdata->qd_id || qdata->qd_type == GRPQUOTA);

        dquot = lustre_dqget(obd, info, qdata->qd_id, qdata->qd_type);
        if (IS_ERR(dquot))
                RETURN(PTR_ERR(dquot));

        DQUOT_DEBUG(dquot, "get dquot in dqacq_handler\n");
        QINFO_DEBUG(dquot->dq_info, "get dquot in dqadq_handler\n");

        down(&mds->mds_qonoff_sem);
        down(&dquot->dq_sem);

        if (dquot->dq_status & DQ_STATUS_RECOVERY) {
                DQUOT_DEBUG(dquot, "this dquot is under recovering.\n");
                GOTO(out, rc = -EBUSY);
        }

        if (qdata->qd_isblk) {
                grace = info->qi_info[qdata->qd_type].dqi_bgrace;
                usage = &dquot->dq_dqb.dqb_curspace;
                hlimit = dquot->dq_dqb.dqb_bhardlimit;
                slimit = dquot->dq_dqb.dqb_bsoftlimit;
                time = &dquot->dq_dqb.dqb_btime;
        } else {
                grace = info->qi_info[qdata->qd_type].dqi_igrace;
                usage = (__u64 *) & dquot->dq_dqb.dqb_curinodes;
                hlimit = dquot->dq_dqb.dqb_ihardlimit;
                slimit = dquot->dq_dqb.dqb_isoftlimit;
                time = &dquot->dq_dqb.dqb_itime;
        }

        /* if the quota limit in admin quotafile is zero, we just inform
         * slave to clear quota limit with zero qd_count */
        if (hlimit == 0 && slimit == 0) {
                qdata->qd_count = 0;
                GOTO(out, rc);
        }

        switch (opc) {
        case QUOTA_DQACQ:
                if (hlimit && 
                    QUSG(*usage + qdata->qd_count, qdata->qd_isblk) > hlimit)
                        GOTO(out, rc = -EDQUOT);

                if (slimit &&
                    QUSG(*usage + qdata->qd_count, qdata->qd_isblk) > slimit) {
                        if (*time && cfs_time_current_sec() >= *time)
                                GOTO(out, rc = -EDQUOT);
                        else if (!*time)
                                *time = cfs_time_current_sec() + grace;
                }

                *usage += qdata->qd_count;
                break;
        case QUOTA_DQREL:
                /* The usage in administrative file might be incorrect before
                 * recovery done */
                if (*usage - qdata->qd_count < 0)
                        *usage = 0;
                else
                        *usage -= qdata->qd_count;

                /* (usage <= soft limit) but not (usage < soft limit) */
                if (!slimit || QUSG(*usage, qdata->qd_isblk) <= slimit)
                        *time = 0;
                break;
        default:
                LBUG();
        }

        rc = fsfilt_dquot(obd, dquot, QFILE_WR_DQUOT);
        EXIT;
out:
        up(&dquot->dq_sem);
        up(&mds->mds_qonoff_sem);
        lustre_dqput(dquot);
        return rc;
}

int mds_quota_adjust(struct obd_device *obd, unsigned int qcids[],
                     unsigned int qpids[], int rc, int opc)
{
        struct lustre_quota_ctxt *qctxt = &obd->u.obt.obt_qctxt;
        int rc2 = 0;
        ENTRY;

        if (rc && rc != -EDQUOT)
                RETURN(0);

        switch (opc) {
        case FSFILT_OP_RENAME:
                /* acquire/release block quota on owner of original parent */
                rc = qctxt_adjust_qunit(obd, qctxt, qpids[2], qpids[3], 1, 0);
                /* fall-through */
        case FSFILT_OP_CREATE:
        case FSFILT_OP_UNLINK:
                /* acquire/release file quota on owner of child, acquire/release
                 * block quota on owner of parent */
                rc = qctxt_adjust_qunit(obd, qctxt, qcids[0], qcids[1], 0, 0);
                rc2 = qctxt_adjust_qunit(obd, qctxt, qpids[0], qpids[1], 1, 0);
                break;
        case FSFILT_OP_SETATTR:
                /* acquire/release file quota on original & current owner 
                 * of child*/
                rc = qctxt_adjust_qunit(obd, qctxt, qcids[0], qcids[1], 0, 0);
                rc2 = qctxt_adjust_qunit(obd, qctxt, qpids[0], qpids[1], 0, 0);
                break;
        default:
                LBUG();
                break;
        }

        if (rc || rc2)
                CERROR("mds adjust qunit failed! (opc:%d rc:%d)\n", 
                       opc, rc ?: rc2);
        RETURN(0);
}

int filter_quota_adjust(struct obd_device *obd, unsigned int qcids[],
                        unsigned int qpids[], int rc, int opc)
{
        struct lustre_quota_ctxt *qctxt = &obd->u.obt.obt_qctxt;
        int rc2 = 0;
        ENTRY;

        if (rc && rc != -EDQUOT)
                RETURN(0);

        switch (opc) {
        case FSFILT_OP_SETATTR:
                /* acquire/release block quota on original & current owner */
                rc = qctxt_adjust_qunit(obd, qctxt, qcids[0], qcids[1], 1, 0);
                rc2 = qctxt_adjust_qunit(obd, qctxt, qpids[0], qpids[1], 1, 0);
                break;
        case FSFILT_OP_UNLINK:
                /* release block quota on this owner */
        case FSFILT_OP_CREATE: /* XXX for write operation on obdfilter */
                /* acquire block quota on this owner */
                rc = qctxt_adjust_qunit(obd, qctxt, qcids[0], qcids[1], 1, 0);
                break;
        default:
                LBUG();
                break;
        }

        if (rc || rc2)
                CERROR("filter adjust qunit failed! (opc:%d rc%d)\n",
                       opc, rc ?: rc2);
        RETURN(0);
}

#define LUSTRE_ADMIN_QUOTAFILES {\
	"admin_quotafile.usr",	/* user admin quotafile */\
	"admin_quotafile.grp"	/* group admin quotafile */\
}
static const char prefix[] = "OBJECTS/";

int init_admin_quotafiles(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        const char *quotafiles[] = LUSTRE_ADMIN_QUOTAFILES;
        struct lvfs_run_ctxt saved;
        char name[64];
        int i, rc = 0;
        struct dentry *dparent = mds->mds_objects_dir;
        struct inode *iparent = dparent->d_inode;
        ENTRY;

        LASSERT(iparent);
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        down(&mds->mds_qonoff_sem);
        for (i = 0; i < MAXQUOTAS; i++) {
                struct dentry *de;
                struct file *fp;

                if (!Q_TYPESET(oqctl, i))
                        continue;

                /* quota file has been opened ? */
                if (qinfo->qi_files[i]) {
                        CWARN("init %s admin quotafile while quota on.\n",
                              i == USRQUOTA ? "user" : "group");
                        continue;
                }

                /* lookup quota file */
                rc = 0;
                down(&iparent->i_sem);
                de = lookup_one_len(quotafiles[i], dparent,
                                    strlen(quotafiles[i]));
                up(&iparent->i_sem);
                if (IS_ERR(de) || de->d_inode == NULL || 
                    !S_ISREG(de->d_inode->i_mode))
                        rc = IS_ERR(de) ? PTR_ERR(de) : -ENOENT;
                if (!IS_ERR(de))
                        dput(de);

                if (rc && rc != -ENOENT) {
                        CERROR("error lookup quotafile %s! (rc:%d)\n",
                               name, rc);
                        break;
                } else if (!rc) {
                        continue;
                }

                LASSERT(strlen(quotafiles[i]) + sizeof(prefix) <= sizeof(name));
                sprintf(name, "%s%s", prefix, quotafiles[i]);

                LASSERT(rc == -ENOENT);
                /* create quota file */
                fp = filp_open(name, O_CREAT | O_EXCL, 0644);
                if (IS_ERR(fp) || !S_ISREG(fp->f_dentry->d_inode->i_mode)) {
                        rc = PTR_ERR(fp);
                        CERROR("error creating admin quotafile %s (rc:%d)\n",
                               name, rc);
                        break;
                }

                qinfo->qi_files[i] = fp;
                rc = fsfilt_quotainfo(obd, qinfo, i, QFILE_INIT_INFO);
                filp_close(fp, 0);
                qinfo->qi_files[i] = NULL;

                if (rc) {
                        CERROR("error init %s admin quotafile! (rc:%d)\n",
                               i == USRQUOTA ? "user" : "group", rc);
                        break;
                }
        }
        up(&mds->mds_qonoff_sem);

        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        RETURN(rc);
}

static int close_quota_files(struct obd_quotactl *oqctl, 
                             struct lustre_quota_info *qinfo)
{
        int i, rc = 0;
        ENTRY;

        for (i = 0; i < MAXQUOTAS; i++) {
                if (!Q_TYPESET(oqctl, i))
                        continue;
                if (qinfo->qi_files[i] == NULL) {
                        rc = -ESRCH;
                        continue;
                }
                filp_close(qinfo->qi_files[i], 0);
                qinfo->qi_files[i] = NULL;
        }
        RETURN(rc);
}

int mds_admin_quota_on(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        const char *quotafiles[] = LUSTRE_ADMIN_QUOTAFILES;
        char name[64];
        int i, rc = 0;
        struct inode *iparent = mds->mds_objects_dir->d_inode;
        ENTRY;

        LASSERT(iparent);

        /* open admin quota files and read quotafile info */
        for (i = 0; i < MAXQUOTAS; i++) {
                struct file *fp;

                if (!Q_TYPESET(oqctl, i))
                        continue;

                LASSERT(strlen(quotafiles[i]) + sizeof(prefix) <= sizeof(name));
                sprintf(name, "%s%s", prefix, quotafiles[i]);

                if (qinfo->qi_files[i] != NULL) {
                        rc = -EBUSY;
                        break;
                }

                fp = filp_open(name, O_RDWR | O_EXCL, 0644);
                if (IS_ERR(fp) || !S_ISREG(fp->f_dentry->d_inode->i_mode)) {
                        rc = PTR_ERR(fp);
                        CERROR("error open %s! (rc:%d)\n", name, rc);
                        break;
                }
                qinfo->qi_files[i] = fp;

                rc = fsfilt_quotainfo(obd, qinfo, i, QFILE_RD_INFO);
                if (rc) {
                        CERROR("error read quotainfo of %s! (rc:%d)\n",
                               name, rc);
                        break;
                }
        }

        if (rc && rc != -EBUSY)
                close_quota_files(oqctl, qinfo);

        RETURN(rc);
}

static int mds_admin_quota_off(struct obd_device *obd, 
                               struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        int rc;
        ENTRY;

        /* close admin quota files */
        rc = close_quota_files(oqctl, qinfo);
        RETURN(rc);
}

int mds_quota_on(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lvfs_run_ctxt saved;
        int rc;
        ENTRY;

        down(&mds->mds_qonoff_sem);
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = mds_admin_quota_on(obd, oqctl);
        if (rc)
                goto out;

        rc = obd_quotactl(mds->mds_osc_exp, oqctl);
        if (rc)
                goto out;

        rc = fsfilt_quotactl(obd, obd->u.obt.obt_sb, oqctl);
out:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        up(&mds->mds_qonoff_sem);
        RETURN(rc);
}

int mds_quota_off(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lvfs_run_ctxt saved;
        int rc, rc2;
        ENTRY;

        down(&mds->mds_qonoff_sem);
        /* close admin quota files */
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        mds_admin_quota_off(obd, oqctl);

        rc = obd_quotactl(mds->mds_osc_exp, oqctl);
        rc2 = fsfilt_quotactl(obd, obd->u.obt.obt_sb, oqctl);

        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        up(&mds->mds_qonoff_sem);
        RETURN(rc ?: rc2);
}

int mds_set_dqinfo(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        struct obd_dqinfo *dqinfo = &oqctl->qc_dqinfo;
        int rc;
        ENTRY;

        down(&mds->mds_qonoff_sem);
        if (qinfo->qi_files[oqctl->qc_type] == NULL) {
                rc = -ESRCH;
                goto out;
        }

        qinfo->qi_info[oqctl->qc_type].dqi_bgrace = dqinfo->dqi_bgrace;
        qinfo->qi_info[oqctl->qc_type].dqi_igrace = dqinfo->dqi_igrace;
        qinfo->qi_info[oqctl->qc_type].dqi_flags = dqinfo->dqi_flags;

        rc = fsfilt_quotainfo(obd, qinfo, oqctl->qc_type, QFILE_WR_INFO);

out:
        up(&mds->mds_qonoff_sem);
        RETURN(rc);
}

int mds_get_dqinfo(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        struct obd_dqinfo *dqinfo = &oqctl->qc_dqinfo;
        int rc = 0;
        ENTRY;

        down(&mds->mds_qonoff_sem);
        if (qinfo->qi_files[oqctl->qc_type] == NULL) {
                rc = -ESRCH;
                goto out;
        }

        dqinfo->dqi_bgrace = qinfo->qi_info[oqctl->qc_type].dqi_bgrace;
        dqinfo->dqi_igrace = qinfo->qi_info[oqctl->qc_type].dqi_igrace;
        dqinfo->dqi_flags = qinfo->qi_info[oqctl->qc_type].dqi_flags;

out:
        up(&mds->mds_qonoff_sem);
        RETURN(rc);
}

static int mds_init_slave_ilimits(struct obd_device *obd,
                                  struct obd_quotactl *oqctl)
{
        /* XXX: for file limits only adjust local now */
        unsigned int uid = 0, gid = 0;
        struct obd_quotactl *ioqc;
        int rc;
        ENTRY;

        /* if we are going to set zero limit, needn't init slaves */
        if (!oqctl->qc_dqblk.dqb_ihardlimit && !oqctl->qc_dqblk.dqb_isoftlimit)
                RETURN(0);

        OBD_ALLOC_PTR(ioqc);
        if (!ioqc)
                RETURN(-ENOMEM);

        ioqc->qc_cmd = Q_INITQUOTA;
        ioqc->qc_id = oqctl->qc_id;
        ioqc->qc_type = oqctl->qc_type;
        ioqc->qc_dqblk.dqb_valid = QIF_ILIMITS;
        ioqc->qc_dqblk.dqb_ihardlimit = MIN_QLIMIT;

        /* set local limit to MIN_QLIMIT */
        rc = fsfilt_quotactl(obd, obd->u.obt.obt_sb, ioqc);
        if (rc)
                GOTO(out, rc);

        /* trigger local qunit pre-acquire */
        if (oqctl->qc_type == USRQUOTA)
                uid = oqctl->qc_id;
        else
                gid = oqctl->qc_id;

        rc = qctxt_adjust_qunit(obd, &obd->u.obt.obt_qctxt, uid, gid, 0, 0);
        if (rc) {
                CERROR("error mds adjust local file quota! (rc:%d)\n", rc);
                GOTO(out, rc);
        }
        /* FIXME initialize all slaves in CMD */
        EXIT;
out:
        OBD_FREE_PTR(ioqc);
        return rc;
}

static int mds_init_slave_blimits(struct obd_device *obd,
                                  struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct obd_quotactl *ioqc;
        unsigned int uid = 0, gid = 0;
        int rc;
        ENTRY;

        /* if we are going to set zero limit, needn't init slaves */
        if (!oqctl->qc_dqblk.dqb_bhardlimit && !oqctl->qc_dqblk.dqb_bsoftlimit)
                RETURN(0);

        OBD_ALLOC_PTR(ioqc);
        if (!ioqc)
                RETURN(-ENOMEM);

        ioqc->qc_cmd = Q_INITQUOTA;
        ioqc->qc_id = oqctl->qc_id;
        ioqc->qc_type = oqctl->qc_type;
        ioqc->qc_dqblk.dqb_valid = QIF_BLIMITS;
        ioqc->qc_dqblk.dqb_bhardlimit = MIN_QLIMIT;

        /* set local limit to MIN_QLIMIT */
        rc = fsfilt_quotactl(obd, obd->u.obt.obt_sb, ioqc);
        if (rc)
                GOTO(out, rc);

        /* trigger local qunit pre-acquire */
        if (oqctl->qc_type == USRQUOTA)
                uid = oqctl->qc_id;
        else
                gid = oqctl->qc_id;

        rc = qctxt_adjust_qunit(obd, &obd->u.obt.obt_qctxt, uid, gid, 1, 0);
        if (rc) {
                CERROR("error mds adjust local block quota! (rc:%d)\n", rc);
                GOTO(out, rc);
        }

        /* initialize all slave's limit */
        rc = obd_quotactl(mds->mds_osc_exp, ioqc);
        EXIT;
out:
        OBD_FREE_PTR(ioqc);
        return rc;
}

int mds_set_dqblk(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        __u32 ihardlimit, isoftlimit, bhardlimit, bsoftlimit;
        time_t btime, itime;
        struct lustre_dquot *dquot;
        struct obd_dqblk *dqblk = &oqctl->qc_dqblk;
        int rc;
        ENTRY;

        down(&mds->mds_qonoff_sem);
        if (qinfo->qi_files[oqctl->qc_type] == NULL)
                GOTO(out_sem, rc = -ESRCH);

        dquot = lustre_dqget(obd, qinfo, oqctl->qc_id, oqctl->qc_type);
        if (IS_ERR(dquot))
                GOTO(out_sem, rc = PTR_ERR(dquot));
        DQUOT_DEBUG(dquot, "get dquot in mds_set_blk\n");
        QINFO_DEBUG(dquot->dq_info, "get dquot in mds_set_blk\n");

        down(&dquot->dq_sem);

        if (dquot->dq_status) {
                up(&dquot->dq_sem);
                lustre_dqput(dquot);
                GOTO(out_sem, rc = -EBUSY);
        }
        dquot->dq_status |= DQ_STATUS_SET;

        ihardlimit = dquot->dq_dqb.dqb_ihardlimit;
        isoftlimit = dquot->dq_dqb.dqb_isoftlimit;
        bhardlimit = dquot->dq_dqb.dqb_bhardlimit;
        bsoftlimit = dquot->dq_dqb.dqb_bsoftlimit;
        btime = dquot->dq_dqb.dqb_btime;
        itime = dquot->dq_dqb.dqb_itime;

        if (dqblk->dqb_valid & QIF_BTIME)
                dquot->dq_dqb.dqb_btime = dqblk->dqb_btime;
        if (dqblk->dqb_valid & QIF_ITIME)
                dquot->dq_dqb.dqb_itime = dqblk->dqb_itime;

        if (dqblk->dqb_valid & QIF_BLIMITS) {
                dquot->dq_dqb.dqb_bhardlimit = dqblk->dqb_bhardlimit;
                dquot->dq_dqb.dqb_bsoftlimit = dqblk->dqb_bsoftlimit;
                /* clear usage (limit pool) */
                if (!dquot->dq_dqb.dqb_bhardlimit && 
                    !dquot->dq_dqb.dqb_bsoftlimit)
                        dquot->dq_dqb.dqb_curspace = 0;

                /* clear grace time */
                if (!dqblk->dqb_bsoftlimit || 
                    toqb(dquot->dq_dqb.dqb_curspace) <= dqblk->dqb_bsoftlimit)
                        dquot->dq_dqb.dqb_btime = 0;
                /* set grace only if user hasn't provided his own */
                else if (!(dqblk->dqb_valid & QIF_BTIME))
                        dquot->dq_dqb.dqb_btime = cfs_time_current_sec() + 
                                qinfo->qi_info[dquot->dq_type].dqi_bgrace;
        }

        if (dqblk->dqb_valid & QIF_ILIMITS) {
                dquot->dq_dqb.dqb_ihardlimit = dqblk->dqb_ihardlimit;
                dquot->dq_dqb.dqb_isoftlimit = dqblk->dqb_isoftlimit;
                /* clear usage (limit pool) */
                if (!dquot->dq_dqb.dqb_ihardlimit &&
                    !dquot->dq_dqb.dqb_isoftlimit)
                        dquot->dq_dqb.dqb_curinodes = 0;

                if (!dqblk->dqb_isoftlimit ||
                    dquot->dq_dqb.dqb_curinodes <= dqblk->dqb_isoftlimit)
                        dquot->dq_dqb.dqb_itime = 0;
                else if (!(dqblk->dqb_valid & QIF_ITIME))
                        dquot->dq_dqb.dqb_itime = cfs_time_current_sec() +
                                qinfo->qi_info[dquot->dq_type].dqi_igrace;
        }

        rc = fsfilt_dquot(obd, dquot, QFILE_WR_DQUOT);

        up(&dquot->dq_sem);

        if (rc) {
                CERROR("set limit failed! (rc:%d)\n", rc);
                goto out;
        }

        up(&mds->mds_qonoff_sem);
        if (dqblk->dqb_valid & QIF_ILIMITS && !(ihardlimit || isoftlimit)) {
                rc = mds_init_slave_ilimits(obd, oqctl);
                if (rc) {
                        CERROR("init slave ilimits failed! (rc:%d)\n", rc);
                        goto revoke_out;
                }
        }

        if (dqblk->dqb_valid & QIF_BLIMITS && !(bhardlimit || bsoftlimit)) {
                rc = mds_init_slave_blimits(obd, oqctl);
                if (rc) {
                        CERROR("init slave blimits failed! (rc:%d)\n", rc);
                        goto revoke_out;
                }
        }
        down(&mds->mds_qonoff_sem);

revoke_out:
        if (rc) {
                /* cancel previous setting */
                down(&dquot->dq_sem);
                dquot->dq_dqb.dqb_ihardlimit = ihardlimit;
                dquot->dq_dqb.dqb_isoftlimit = isoftlimit;
                dquot->dq_dqb.dqb_bhardlimit = bhardlimit;
                dquot->dq_dqb.dqb_bsoftlimit = bsoftlimit;
                dquot->dq_dqb.dqb_btime = btime;
                dquot->dq_dqb.dqb_itime = itime;
                fsfilt_dquot(obd, dquot, QFILE_WR_DQUOT);
                up(&dquot->dq_sem);
        }
out:
        down(&dquot->dq_sem);
        dquot->dq_status &= ~DQ_STATUS_SET;
        up(&dquot->dq_sem);
        lustre_dqput(dquot);
        EXIT;
out_sem:
        up(&mds->mds_qonoff_sem);
        return rc;
}

static int mds_get_space(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct obd_quotactl *soqc;
        struct lvfs_run_ctxt saved;
        int rc;
        ENTRY;

        OBD_ALLOC_PTR(soqc);
        if (!soqc)
                RETURN(-ENOMEM);

        soqc->qc_cmd = Q_GETOQUOTA;
        soqc->qc_id = oqctl->qc_id;
        soqc->qc_type = oqctl->qc_type;

        rc = obd_quotactl(obd->u.mds.mds_osc_exp, soqc);
        if (rc)
               GOTO(out, rc);

        oqctl->qc_dqblk.dqb_curspace = soqc->qc_dqblk.dqb_curspace;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        soqc->qc_dqblk.dqb_curspace = 0;
        rc = fsfilt_quotactl(obd, obd->u.obt.obt_sb, soqc);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        if (rc)
                GOTO(out, rc);

        oqctl->qc_dqblk.dqb_curinodes += soqc->qc_dqblk.dqb_curinodes;
        oqctl->qc_dqblk.dqb_curspace += soqc->qc_dqblk.dqb_curspace;
        EXIT;
out:
        OBD_FREE_PTR(soqc);
        return rc;
}

int mds_get_dqblk(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo = &mds->mds_quota_info;
        struct lustre_dquot *dquot;
        struct obd_dqblk *dqblk = &oqctl->qc_dqblk;
        int rc;
        ENTRY;

        down(&mds->mds_qonoff_sem);
        if (qinfo->qi_files[oqctl->qc_type] == NULL)
                GOTO(out, rc = -ESRCH);

        dquot = lustre_dqget(obd, qinfo, oqctl->qc_id, oqctl->qc_type);
        if (IS_ERR(dquot))
                GOTO(out, rc = PTR_ERR(dquot));

        down(&dquot->dq_sem);
        dqblk->dqb_ihardlimit = dquot->dq_dqb.dqb_ihardlimit;
        dqblk->dqb_isoftlimit = dquot->dq_dqb.dqb_isoftlimit;
        dqblk->dqb_bhardlimit = dquot->dq_dqb.dqb_bhardlimit;
        dqblk->dqb_bsoftlimit = dquot->dq_dqb.dqb_bsoftlimit;
        dqblk->dqb_btime = dquot->dq_dqb.dqb_btime;
        dqblk->dqb_itime = dquot->dq_dqb.dqb_itime;
        up(&dquot->dq_sem);

        lustre_dqput(dquot);

        /* the usages in admin quota file is inaccurate */
        dqblk->dqb_curinodes = 0;
        dqblk->dqb_curspace = 0;
        rc = mds_get_space(obd, oqctl);
        EXIT;
out:
        up(&mds->mds_qonoff_sem);
        return rc;
}

int mds_get_obd_quota(struct obd_device *obd, struct obd_quotactl *oqctl)
{
        struct lvfs_run_ctxt saved;
        int rc;
        ENTRY;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = fsfilt_quotactl(obd, obd->u.obt.obt_sb, oqctl);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        RETURN(rc);
}


/* FIXME we only recovery block limit by now, need recovery inode
 * limits also after CMD involved in */
static int 
dquot_recovery(struct obd_device *obd, unsigned int id, unsigned short type)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_quota_info *qinfo= &obd->u.mds.mds_quota_info;
        struct lustre_dquot *dquot;
        struct obd_quotactl *qctl;
        __u64 total_limits = 0;
        int rc;
        ENTRY;

        OBD_ALLOC_PTR(qctl);
        if (qctl == NULL)
                RETURN(-ENOMEM);

        dquot = lustre_dqget(obd, qinfo, id, type);
        if (IS_ERR(dquot)) {
                CERROR("Get dquot failed. (rc:%ld)\n", PTR_ERR(dquot));
                OBD_FREE_PTR(qctl);
                RETURN(PTR_ERR(dquot));
        }

        down(&dquot->dq_sem);

        /* don't recovery the dquot without limits or under setting */
        if (!(dquot->dq_dqb.dqb_bhardlimit || dquot->dq_dqb.dqb_bsoftlimit) ||
            dquot->dq_status)
                GOTO(skip, rc = 0);
        dquot->dq_status |= DQ_STATUS_RECOVERY;

        up(&dquot->dq_sem);

        /* get real bhardlimit from all slaves. */
        qctl->qc_cmd = Q_GETOQUOTA;
        qctl->qc_type = type;
        qctl->qc_id = id;
        qctl->qc_stat = QUOTA_RECOVERING;
        rc = obd_quotactl(obd->u.mds.mds_osc_exp, qctl);
        if (rc)
                GOTO(out, rc);
        total_limits = qctl->qc_dqblk.dqb_bhardlimit;

        /* get real bhardlimit from master */
        rc = fsfilt_quotactl(obd, obd->u.obt.obt_sb, qctl);
        if (rc)
                GOTO(out, rc);
        total_limits += qctl->qc_dqblk.dqb_bhardlimit;

        /* amend the usage of the administrative quotafile */
        down(&mds->mds_qonoff_sem);
        down(&dquot->dq_sem);

        dquot->dq_dqb.dqb_curspace = total_limits << QUOTABLOCK_BITS;

        rc = fsfilt_dquot(obd, dquot, QFILE_WR_DQUOT);
        if (rc)
                CERROR("write dquot failed! (rc:%d)\n", rc);

        up(&dquot->dq_sem);
        up(&mds->mds_qonoff_sem);
        EXIT;
out:
        down(&dquot->dq_sem);
        dquot->dq_status &= ~DQ_STATUS_RECOVERY;
skip:
        up(&dquot->dq_sem);

        lustre_dqput(dquot);
        OBD_FREE_PTR(qctl);
        return rc;
}

struct qmaster_recov_thread_data {
        struct obd_device *obd;
        struct completion comp;
};

static int qmaster_recovery_main(void *arg)
{
        struct qmaster_recov_thread_data *data = arg;
        struct obd_device *obd = data->obd;
        unsigned long flags;
        int rc = 0;
        unsigned short type;
        ENTRY;

        lock_kernel();
        ptlrpc_daemonize();

        SIGNAL_MASK_LOCK(current, flags);
        sigfillset(&current->blocked);
        RECALC_SIGPENDING;
        SIGNAL_MASK_UNLOCK(current, flags);
        THREAD_NAME(cfs_curproc_comm(), CFS_CURPROC_COMM_MAX - 1, "%s", 
                    "qmaster_recovd");
        unlock_kernel();

        complete(&data->comp);

        for (type = USRQUOTA; type < MAXQUOTAS; type++) {
                struct mds_obd *mds = &obd->u.mds;
                struct lustre_quota_info *qinfo = &mds->mds_quota_info;
                struct list_head id_list;
                struct dquot_id *dqid, *tmp;

                down(&mds->mds_qonoff_sem);
                if (qinfo->qi_files[type] == NULL) {
                        up(&mds->mds_qonoff_sem);
                        continue;
                }
                INIT_LIST_HEAD(&id_list);
                rc = fsfilt_qids(obd, qinfo->qi_files[type], NULL, type, 
                                 &id_list);
                up(&mds->mds_qonoff_sem);

                if (rc)
                        CERROR("error get ids from admin quotafile.(%d)\n", rc);

                list_for_each_entry_safe(dqid, tmp, &id_list, di_link) {
                        list_del_init(&dqid->di_link);
                        if (rc)
                                goto free;

                        rc = dquot_recovery(obd, dqid->di_id, type);
                        if (rc)
                                CERROR("qmaster recovery failed! (id:%d type:%d"
                                       " rc:%d)\n", dqid->di_id, type, rc);
free:
                        kfree(dqid);
                }
        }
        RETURN(rc);
}

int mds_quota_recovery(struct obd_device *obd)
{
        struct lov_obd *lov = &obd->u.mds.mds_osc_obd->u.lov;
        struct qmaster_recov_thread_data data;
        int rc = 0;
        ENTRY;

        down(&lov->lov_lock);
        if (lov->desc.ld_tgt_count != lov->desc.ld_active_tgt_count) {
                CWARN("Not all osts are active, abort quota recovery\n");
                up(&lov->lov_lock);
                RETURN(rc);
        }
        up(&lov->lov_lock);

        data.obd = obd;
        init_completion(&data.comp);

        rc = kernel_thread(qmaster_recovery_main, &data, CLONE_VM|CLONE_FILES);
        if (rc < 0)
                CERROR("Cannot start quota recovery thread: rc %d\n", rc);

        wait_for_completion(&data.comp);
        RETURN(rc);
}
