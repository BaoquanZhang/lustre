/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
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
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#include <asm/statfs.h>
#endif
#include <obd.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include "mds_internal.h"

#ifdef LPROCFS
static int lprocfs_mds_rd_mntdev(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        struct obd_device* obd = (struct obd_device *)data;

        LASSERT(obd != NULL);
        LASSERT(obd->u.mds.mds_vfsmnt->mnt_devname);
        *eof = 1;

        return snprintf(page, count, "%s\n",obd->u.mds.mds_vfsmnt->mnt_devname);
}

static int lprocfs_mds_rd_evictostnids(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
        struct obd_device* obd = (struct obd_device *)data;

        LASSERT(obd != NULL);

        return snprintf(page, count, "%d\n", obd->u.mds.mds_evict_ost_nids);
}

static int lprocfs_mds_wr_evictostnids(struct file *file, const char *buffer,
                                       unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        obd->u.mds.mds_evict_ost_nids = !!val;

        return count;
}

static int lprocfs_mds_wr_evict_client(struct file *file, const char *buffer,
                                       unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mds_obd *mds = &obd->u.mds;
        char tmpbuf[sizeof(struct obd_uuid)];
        struct ptlrpc_request_set *set;
        int rc;

        sscanf(buffer, "%40s", tmpbuf);

        if (strncmp(tmpbuf, "nid:", 4) != 0)
                return lprocfs_wr_evict_client(file, buffer, count, data);

        set = ptlrpc_prep_set();
        if (!set)
                return -ENOMEM;

        if (obd->u.mds.mds_evict_ost_nids) {
                rc = obd_set_info_async(mds->mds_osc_exp,strlen("evict_by_nid"),
                                        "evict_by_nid", strlen(tmpbuf + 4) + 1,
                                        tmpbuf + 4, set);
                if (rc)
                        CERROR("Failed to evict nid %s from OSTs: rc %d\n",
                               tmpbuf + 4, rc);
                ptlrpc_check_set(set);
        }

        /* See the comments in function lprocfs_wr_evict_client() 
         * in ptlrpc/lproc_ptlrpc.c for details. - jay */
        class_incref(obd);
        LPROCFS_EXIT();

        obd_export_evict_by_nid(obd, tmpbuf+4);

        LPROCFS_ENTRY();
        class_decref(obd);

        rc = ptlrpc_set_wait(set);
        if (rc)
                CERROR("Failed to evict nid %s from OSTs: rc %d\n", tmpbuf + 4,
                       rc);
        ptlrpc_set_destroy(set);
        return count;
}

static int lprocfs_wr_group_info(struct file *file, const char *buffer,
                                 unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mds_obd *mds = &obd->u.mds;
        struct mds_grp_downcall_data sparam, *param = &sparam;
        int size = 0, rc = count;

        if (count < sizeof(param)) {
                CERROR("%s: invalid data size %lu\n", obd->obd_name, count);
                return count;
        }

        if (copy_from_user(param, buffer, sizeof(*param)) ||
            param->mgd_magic != MDS_GRP_DOWNCALL_MAGIC) {
                CERROR("%s: MDS group downcall bad params\n", obd->obd_name);
                return count;
        }

        if (param->mgd_ngroups > NGROUPS_MAX) {
                CWARN("%s: uid %u groups %d more than maximum %d\n",
                      obd->obd_name, param->mgd_uid, param->mgd_ngroups,
                      NGROUPS_MAX);
                param->mgd_ngroups = NGROUPS_MAX;
        }

        if (param->mgd_ngroups > 0) {
                size = offsetof(struct mds_grp_downcall_data,
                                mgd_groups[param->mgd_ngroups]);
                OBD_ALLOC(param, size);
                if (!param) {
                        CERROR("%s: fail to alloc %d bytes for uid %u"
                               " with %d groups\n", obd->obd_name, size,
                               sparam.mgd_uid, sparam.mgd_ngroups);
                        param = &sparam;
                        param->mgd_ngroups = 0;
                } else if (copy_from_user(param, buffer, size)) {
                        CERROR("%s: uid %u bad supplementary group data\n",
                               obd->obd_name, sparam.mgd_uid);
                        OBD_FREE(param, size);
                        param = &sparam;
                        param->mgd_ngroups = 0;
                }
        }
        rc = upcall_cache_downcall(mds->mds_group_hash, param->mgd_err,
                                   param->mgd_uid, param->mgd_gid,
                                   param->mgd_ngroups, param->mgd_groups);

        if (param && param != &sparam)
                OBD_FREE(param, size);

        return rc;
}

static int lprocfs_rd_group_expire(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        *eof = 1;
        return snprintf(page, count, "%lu\n",
                        obd->u.mds.mds_group_hash->uc_entry_expire / HZ);
}

static int lprocfs_wr_group_expire(struct file *file, const char *buffer,
                                   unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val > 5)
                obd->u.mds.mds_group_hash->uc_entry_expire = val * HZ;
        else
                CERROR("invalid expire time %u for group cache\n", val);

        return count;
}

static int lprocfs_rd_group_acquire_expire(char *page, char **start, off_t off,
                                           int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        *eof = 1;
        return snprintf(page, count, "%lu\n",
                        obd->u.mds.mds_group_hash->uc_acquire_expire / HZ);
}

static int lprocfs_wr_group_acquire_expire(struct file *file,const char *buffer,
                                           unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc = 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val > 2)
                obd->u.mds.mds_group_hash->uc_acquire_expire = val * HZ;

        return count;
}

static int lprocfs_rd_group_upcall(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        *eof = 1;
        return snprintf(page, count, "%s\n",
                        obd->u.mds.mds_group_hash->uc_upcall);
}

static int lprocfs_wr_group_upcall(struct file *file, const char *buffer,
                                   unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct upcall_cache *hash = obd->u.mds.mds_group_hash;
        char kernbuf[UC_CACHE_UPCALL_MAXPATH] = { '\0' };

        if (count >= UC_CACHE_UPCALL_MAXPATH) {
                CERROR("%s: group upcall too long\n", obd->obd_name);
                return -EINVAL;
        }

        if (copy_from_user(kernbuf, buffer,
                           min(count, UC_CACHE_UPCALL_MAXPATH - 1)))
                return -EFAULT;

        /* Remove any extraneous bits from the upcall (e.g. linefeeds) */
        sscanf(kernbuf, "%s", hash->uc_upcall);

        if (strcmp(hash->uc_name, obd->obd_name) != 0)
                CWARN("%s: write to upcall name %s for MDS %s\n",
                      obd->obd_name, hash->uc_upcall, obd->obd_name);
        CWARN("%s: group upcall set to %s\n", obd->obd_name, hash->uc_upcall);

        return count;
}

static int lprocfs_wr_group_flush(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        struct obd_device *obd = data;

        upcall_cache_flush_idle(obd->u.mds.mds_group_hash);
        return count;
}

static int lprocfs_wr_atime_diff(struct file *file, const char *buffer,
                                 unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mds_obd *mds = &obd->u.mds;
        char kernbuf[20], *end;
        unsigned long diff = 0;

        if (count > (sizeof(kernbuf) - 1))
                return -EINVAL;

        if (copy_from_user(kernbuf, buffer, count))
                return -EFAULT;

        kernbuf[count] = '\0';

        diff = simple_strtoul(kernbuf, &end, 0);
        if (kernbuf == end)
                return -EINVAL;

        mds->mds_atime_diff = diff;
        return count;
}

static int lprocfs_rd_atime_diff(char *page, char **start, off_t off,
                                 int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mds_obd *mds = &obd->u.mds;

        *eof = 1;
        return snprintf(page, count, "%lu\n", mds->mds_atime_diff);
}

struct lprocfs_vars lprocfs_mds_obd_vars[] = {
        { "uuid",            lprocfs_rd_uuid,        0, 0 },
        { "blocksize",       lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",     lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",      lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",     lprocfs_rd_kbytesavail, 0, 0 },
        { "filestotal",      lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",       lprocfs_rd_filesfree,   0, 0 },
        { "fstype",          lprocfs_rd_fstype,      0, 0 },
        { "mntdev",          lprocfs_mds_rd_mntdev,  0, 0 },
        { "recovery_status", lprocfs_obd_rd_recovery_status, 0, 0 },
        { "evict_client",    0,                lprocfs_mds_wr_evict_client, 0 },
        { "evict_ost_nids",  lprocfs_mds_rd_evictostnids,
                                               lprocfs_mds_wr_evictostnids, 0 },
        { "num_exports",     lprocfs_rd_num_exports, 0, 0 },
#ifdef HAVE_QUOTA_SUPPORT
        { "quota_bunit_sz",  lprocfs_rd_bunit, lprocfs_wr_bunit, 0 },
        { "quota_btune_sz",  lprocfs_rd_btune, lprocfs_wr_btune, 0 },
        { "quota_iunit_sz",  lprocfs_rd_iunit, lprocfs_wr_iunit, 0 },
        { "quota_itune_sz",  lprocfs_rd_itune, lprocfs_wr_itune, 0 },
        { "quota_type",      lprocfs_rd_type, lprocfs_wr_type, 0 },
#endif
        { "group_expire_interval", lprocfs_rd_group_expire,
                             lprocfs_wr_group_expire, 0},
        { "group_acquire_expire", lprocfs_rd_group_acquire_expire,
                             lprocfs_wr_group_acquire_expire, 0},
        { "group_upcall",    lprocfs_rd_group_upcall,
                             lprocfs_wr_group_upcall, 0},
        { "group_flush",     0, lprocfs_wr_group_flush, 0},
        { "group_info",      0, lprocfs_wr_group_info, 0 },
        { "atime_diff",      lprocfs_rd_atime_diff, lprocfs_wr_atime_diff, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mds_module_vars[] = {
        { "num_refs",     lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mdt_obd_vars[] = {
        { "uuid",         lprocfs_rd_uuid,        0, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mdt_module_vars[] = {
        { "num_refs",     lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

void mds_counter_incr(struct obd_export *exp, int opcode)
{
        if (exp->exp_obd && exp->exp_obd->obd_stats)
                lprocfs_counter_incr(exp->exp_obd->obd_stats, opcode);
        if (exp->exp_nid_stats && exp->exp_nid_stats->nid_stats != NULL)
                lprocfs_counter_incr(exp->exp_nid_stats->nid_stats, opcode);

}

void mds_stats_counter_init(struct lprocfs_stats *stats)
{
        lprocfs_counter_init(stats, LPROC_MDS_OPEN, 0, "open", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_CLOSE, 0, "close", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_MKNOD, 0, "mknod", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_LINK, 0, "link", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_UNLINK, 0, "unlink", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_MKDIR, 0, "mkdir", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_RMDIR, 0, "rmdir", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_RENAME, 0, "rename", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_GETXATTR, 0, "getxattr", "reqs");
        lprocfs_counter_init(stats, LPROC_MDS_SETXATTR, 0, "setxattr", "reqs");
}

LPROCFS_INIT_VARS(mds, lprocfs_mds_module_vars, lprocfs_mds_obd_vars);
LPROCFS_INIT_VARS(mdt, lprocfs_mdt_module_vars, lprocfs_mdt_obd_vars);
#endif
