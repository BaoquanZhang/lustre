/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2003 Cluster File Systems, Inc.
 *   Author: Lai Siyao <lsy@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org/
 *
 *   No redistribution or use is permitted outside of Cluster File Systems, Inc.
 *
 * A kernel module which tests the fsfilt quotactl API from the OBD setup function.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/module.h>
#include <linux/init.h>

#include <linux/obd_class.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_mds.h>
#include <linux/obd_ost.h>

char *test_quotafile[] = {"aquotactl.user", "aquotactl.group"};

/* Test quotaon */
static int quotactl_test_1(struct obd_device *obd, struct super_block *sb)
{
        struct obd_quotactl oqctl;
        int rc;
        ENTRY;

        oqctl.qc_cmd = Q_QUOTAON;
        oqctl.qc_id = QFMT_LDISKFS;
        oqctl.qc_type = UGQUOTA;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("1a: quotactl Q_QUOTAON failed: %d\n", rc);
                RETURN(rc);
        }

        RETURN(0);
}

#if 0 /* set/getinfo not supported, this is for cluster-wide quotas */
/* Test set/getinfo */
static int quotactl_test_2(struct obd_device *obd, struct super_block *sb)
{
        struct obd_quotactl oqctl;
        int rc;
        ENTRY;

        oqctl.qc_cmd = Q_SETINFO;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_dqinfo.dqi_bgrace = 1616;
        oqctl.qc_dqinfo.dqi_igrace = 2828;
        oqctl.qc_dqinfo.dqi_flags = 0;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("2a: quotactl Q_SETINFO failed: %d\n", rc);
                RETURN(rc);
        }

        oqctl.qc_cmd = Q_GETINFO;
        oqctl.qc_type = USRQUOTA;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("2b: quotactl Q_GETINFO failed: %d\n", rc);
                RETURN(rc);
        }
        if (oqctl.qc_dqinfo.dqi_bgrace != 1616 ||
            oqctl.qc_dqinfo.dqi_igrace != 2828 ||
            oqctl.qc_dqinfo.dqi_flags != 0) {
                CERROR("2c: quotactl Q_GETINFO get wrong result: %d, %d, %d\n",
                       oqctl.qc_dqinfo.dqi_bgrace,
                       oqctl.qc_dqinfo.dqi_igrace,
                       oqctl.qc_dqinfo.dqi_flags);
                RETURN(-EINVAL);
        }

        RETURN(0);
}
#endif
       
/* Test set/getquota */
static int quotactl_test_3(struct obd_device *obd, struct super_block *sb)
{
        struct obd_quotactl oqctl;
        int rc;
        ENTRY;

        oqctl.qc_cmd = Q_SETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        oqctl.qc_dqblk.dqb_bhardlimit = 919;
        oqctl.qc_dqblk.dqb_bsoftlimit = 818;
        oqctl.qc_dqblk.dqb_ihardlimit = 616;
        oqctl.qc_dqblk.dqb_isoftlimit = 515;
        oqctl.qc_dqblk.dqb_valid = QIF_LIMITS;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3a: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }

        oqctl.qc_cmd = Q_GETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3b: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }
        if (oqctl.qc_dqblk.dqb_bhardlimit != 919 ||
            oqctl.qc_dqblk.dqb_bsoftlimit != 818 ||
            oqctl.qc_dqblk.dqb_ihardlimit != 616 ||
            oqctl.qc_dqblk.dqb_isoftlimit != 515) {
                CERROR("3c: quotactl Q_GETQUOTA get wrong result:"
                       LPU64", "LPU64", "LPU64", "LPU64"\n",
                       oqctl.qc_dqblk.dqb_bhardlimit,
                       oqctl.qc_dqblk.dqb_bsoftlimit,
                       oqctl.qc_dqblk.dqb_ihardlimit,
                       oqctl.qc_dqblk.dqb_isoftlimit);
                RETURN(-EINVAL);
        }

        oqctl.qc_cmd = Q_SETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        oqctl.qc_dqblk.dqb_curspace = 717;
        oqctl.qc_dqblk.dqb_curinodes = 414;
        oqctl.qc_dqblk.dqb_valid = QIF_USAGE;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3d: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }

        oqctl.qc_cmd = Q_GETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3e: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }
        if (oqctl.qc_dqblk.dqb_curspace != 717 ||
            oqctl.qc_dqblk.dqb_curinodes != 414) {
                CERROR("3f: quotactl Q_GETQUOTA get wrong result: "
                       LPU64", "LPU64"\n", oqctl.qc_dqblk.dqb_curspace,
                       oqctl.qc_dqblk.dqb_curinodes);
                RETURN(-EINVAL);
        }

        oqctl.qc_cmd = Q_SETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_dqblk.dqb_btime = 313;
        oqctl.qc_dqblk.dqb_itime = 212;
        oqctl.qc_id = 500;
        oqctl.qc_dqblk.dqb_valid = QIF_TIMES;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3g: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }

        oqctl.qc_cmd = Q_GETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3h: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }
        if (oqctl.qc_dqblk.dqb_btime != 313 ||
            oqctl.qc_dqblk.dqb_itime != 212) {
                CERROR("3i: quotactl Q_GETQUOTA get wrong result: "
                       LPU64", "LPU64"\n", oqctl.qc_dqblk.dqb_btime,
                       oqctl.qc_dqblk.dqb_itime);
                RETURN(-EINVAL);
        }

        oqctl.qc_cmd = Q_SETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        oqctl.qc_dqblk.dqb_bhardlimit = 919;
        oqctl.qc_dqblk.dqb_bsoftlimit = 818;
        oqctl.qc_dqblk.dqb_curspace = 717;
        oqctl.qc_dqblk.dqb_ihardlimit = 616;
        oqctl.qc_dqblk.dqb_isoftlimit = 515;
        oqctl.qc_dqblk.dqb_curinodes = 414;
        oqctl.qc_dqblk.dqb_btime = 313;
        oqctl.qc_dqblk.dqb_itime = 212;
        oqctl.qc_dqblk.dqb_valid = QIF_ALL;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3j: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }

        oqctl.qc_cmd = Q_GETQUOTA;
        oqctl.qc_type = USRQUOTA;
        oqctl.qc_id = 500;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("3k: quotactl Q_SETQUOTA failed: %d\n", rc);
                RETURN(rc);
        }
        if (oqctl.qc_dqblk.dqb_bhardlimit != 919 ||
            oqctl.qc_dqblk.dqb_bsoftlimit != 818 ||
            oqctl.qc_dqblk.dqb_ihardlimit != 616 ||
            oqctl.qc_dqblk.dqb_isoftlimit != 515 ||
            oqctl.qc_dqblk.dqb_curspace != 717 ||
            oqctl.qc_dqblk.dqb_curinodes != 414 ||
            oqctl.qc_dqblk.dqb_btime != 0 ||
            oqctl.qc_dqblk.dqb_itime != 0) {
                CERROR("3l: quotactl Q_GETQUOTA get wrong result:"
                       LPU64", "LPU64", "LPU64", "LPU64", "LPU64", "LPU64", "
                       LPU64", "LPU64"\n", oqctl.qc_dqblk.dqb_bhardlimit,
                       oqctl.qc_dqblk.dqb_bsoftlimit,
                       oqctl.qc_dqblk.dqb_ihardlimit,
                       oqctl.qc_dqblk.dqb_isoftlimit,
                       oqctl.qc_dqblk.dqb_curspace,
                       oqctl.qc_dqblk.dqb_curinodes,
                       oqctl.qc_dqblk.dqb_btime,
                       oqctl.qc_dqblk.dqb_itime);
                RETURN(-EINVAL);
        }

        RETURN(0);
}

/* Test quotaoff */
static int quotactl_test_4(struct obd_device *obd, struct super_block *sb)
{
        struct obd_quotactl oqctl;
        int rc;
        ENTRY;

        oqctl.qc_cmd = Q_QUOTAOFF;
        oqctl.qc_id = 500;
        oqctl.qc_type = UGQUOTA;
        rc = fsfilt_quotactl(obd, sb, &oqctl);
        if (rc) {
                CERROR("4a: quotactl Q_QUOTAOFF failed: %d\n", rc);
                RETURN(rc);
        }

        RETURN(0);
}

/* -------------------------------------------------------------------------
 * Tests above, boring obd functions below
 * ------------------------------------------------------------------------- */
static int quotactl_run_tests(struct obd_device *obd, struct obd_device *tgt)
{
        struct super_block *sb;
        struct lvfs_run_ctxt saved;
        int rc;
        ENTRY;

        if (!strcmp(tgt->obd_type->typ_name, LUSTRE_MDS_NAME))
                sb = tgt->u.mds.mds_sb;
        else if (!strcmp(tgt->obd_type->typ_name, "obdfilter"))
                sb = tgt->u.filter.fo_sb;
        else {
                CERROR("TARGET OBD should be mds or obdfilter\n");
                RETURN(-EINVAL);
        }

        push_ctxt(&saved, &tgt->obd_lvfs_ctxt, NULL);

        rc = quotactl_test_1(tgt, sb);
        if (rc)
                GOTO(cleanup, rc);

#if 0
        rc = quotactl_test_2(tgt, sb);
        if (rc)
                GOTO(cleanup, rc);
#endif

        rc = quotactl_test_3(tgt, sb);
        if (rc)
                GOTO(cleanup, rc);

 cleanup:
        quotactl_test_4(tgt, sb);

        pop_ctxt(&saved, &tgt->obd_lvfs_ctxt, NULL);

        return rc;
}

static int quotactl_test_cleanup(struct obd_device *obd)
{
        lprocfs_obd_cleanup(obd);
        return 0;
}

static int quotactl_test_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct lprocfs_static_vars lvars;
        struct lustre_cfg *lcfg = buf;
        struct obd_device *tgt;
        int rc;
        ENTRY;

        if (lcfg->lcfg_bufcount < 1) {
                CERROR("requires a mds OBD name\n");
                RETURN(-EINVAL);
        }

        tgt = class_name2obd(lustre_cfg_string(lcfg, 1));
        if (!tgt || !tgt->obd_attached || !tgt->obd_set_up) {
                CERROR("target device not attached or not set up (%s)\n",
                       lustre_cfg_string(lcfg, 1));
                RETURN(-EINVAL);
        }

        lprocfs_init_vars(quotactl_test, &lvars);
        lprocfs_obd_setup(obd, lvars.obd_vars);

        rc = quotactl_run_tests(obd, tgt);

        quotactl_test_cleanup(obd);

        RETURN(rc);
}

static struct obd_ops quotactl_obd_ops = {
        .o_owner       = THIS_MODULE,
        .o_setup       = quotactl_test_setup,
        .o_cleanup     = quotactl_test_cleanup,
};

#ifdef LPROCFS
static struct lprocfs_vars lprocfs_obd_vars[] = { {0} };
static struct lprocfs_vars lprocfs_module_vars[] = { {0} };
LPROCFS_INIT_VARS(quotactl_test, lprocfs_module_vars, lprocfs_obd_vars)
#endif

static int __init quotactl_test_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(quotactl_test, &lvars);
        return class_register_type(&quotactl_obd_ops, lvars.module_vars,
                                   "quotactl_test");
}

static void __exit quotactl_test_exit(void)
{
        class_unregister_type("quotactl_test");
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("quotactl test module");
MODULE_LICENSE("GPL");

module_init(quotactl_test_init);
module_exit(quotactl_test_exit);
