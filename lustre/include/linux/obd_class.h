/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
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
 *
 */

#ifndef __LINUX_CLASS_OBD_H
#define __LINUX_CLASS_OBD_H

#ifndef __KERNEL__
#include <sys/types.h>
#include <libcfs/list.h>
#else
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/time.h>
#include <linux/timer.h>
#endif

#include <linux/obd_support.h>
#include <linux/lustre_import.h>
#include <linux/lustre_net.h>
#include <linux/obd.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lprocfs_status.h>

/* OBD Device Declarations */
#define MAX_OBD_DEVICES 520
extern struct obd_device obd_dev[MAX_OBD_DEVICES];
extern spinlock_t obd_dev_lock;

/* OBD Operations Declarations */
extern struct obd_device *class_conn2obd(struct lustre_handle *);
extern struct obd_device *class_exp2obd(struct obd_export *);

/* genops.c */
struct obd_export *class_conn2export(struct lustre_handle *);
int class_register_type(struct obd_ops *ops, struct lprocfs_vars *, char *nm);
int class_unregister_type(char *nm);

struct obd_device *class_newdev(struct obd_type *type, char *name);
void class_release_dev(struct obd_device *obd);

int class_name2dev(char *name);
struct obd_device *class_name2obd(char *name);
int class_uuid2dev(struct obd_uuid *uuid);
struct obd_device *class_uuid2obd(struct obd_uuid *uuid);
struct obd_device * class_find_client_obd(struct obd_uuid *tgt_uuid, 
                                          char * typ_name,
                                          struct obd_uuid *grp_uuid);
struct obd_device * class_find_client_notype(struct obd_uuid *tgt_uuid,
                                             struct obd_uuid *grp_uuid);
struct obd_device * class_devices_in_group(struct obd_uuid *grp_uuid, 
                                           int *next);

int oig_init(struct obd_io_group **oig);
void oig_add_one(struct obd_io_group *oig,
                  struct oig_callback_context *occ);
void oig_complete_one(struct obd_io_group *oig, 
                       struct oig_callback_context *occ, int rc);
void oig_release(struct obd_io_group *oig);
int oig_wait(struct obd_io_group *oig);
/* ping evictor */
#ifdef __KERNEL__
void ping_evictor_start(void);
void ping_evictor_stop(void);
#else
#define ping_evictor_start()    do {} while (0)
#define ping_evictor_stop()     do {} while (0)
#endif


char *obd_export_nid2str(struct obd_export *exp);

/* config.c */
int class_process_config(struct lustre_cfg *lcfg);
int class_attach(struct lustre_cfg *lcfg);
int class_setup(struct obd_device *obd, struct lustre_cfg *lcfg);
int class_cleanup(struct obd_device *obd, struct lustre_cfg *lcfg);
int class_detach(struct obd_device *obd, struct lustre_cfg *lcfg);
void class_decref(struct obd_device *obd);

/* Passed as data param to class_config_parse_llog */
struct config_llog_instance {
        char * cfg_instance;
        struct obd_uuid cfg_uuid;
};
int class_config_parse_llog(struct llog_ctxt *ctxt, char *name,
                            struct config_llog_instance *cfg);
int class_config_dump_llog(struct llog_ctxt *ctxt, char *name,
                           struct config_llog_instance *cfg);

struct lustre_profile {
        struct list_head lp_list;
        char * lp_profile;
        char * lp_osc;
        char * lp_mdc;
};

struct lustre_profile *class_get_profile(char * prof);
void class_del_profile(char *prof);

#define class_export_get(exp)                                                  \
({                                                                             \
        struct obd_export *exp_ = exp;                                         \
        atomic_inc(&exp_->exp_refcount);                                       \
        CDEBUG(D_INFO, "GETting export %p : new refcount %d\n", exp_,          \
               atomic_read(&exp_->exp_refcount));                              \
        exp_;                                                                  \
})

#define class_export_put(exp)                                                  \
do {                                                                           \
        LASSERT((exp) != NULL);                                                \
        CDEBUG(D_INFO, "PUTting export %p : new refcount %d\n", (exp),         \
               atomic_read(&(exp)->exp_refcount) - 1);                         \
        LASSERT(atomic_read(&(exp)->exp_refcount) > 0);                        \
        LASSERT(atomic_read(&(exp)->exp_refcount) < 0x5a5a5a);                 \
        __class_export_put(exp);                                               \
} while (0)
void __class_export_put(struct obd_export *);
struct obd_export *class_new_export(struct obd_device *obddev);
void class_unlink_export(struct obd_export *exp);
void class_update_export_timer(struct obd_export *exp, time_t extra_delay);

struct obd_import *class_import_get(struct obd_import *);
void class_import_put(struct obd_import *);
struct obd_import *class_new_import(void);
void class_destroy_import(struct obd_import *exp);

struct obd_type *class_get_type(char *name);
void class_put_type(struct obd_type *type);
int class_connect(struct lustre_handle *conn, struct obd_device *obd,
                  struct obd_uuid *cluuid);
int class_disconnect(struct obd_export *exp);
void class_fail_export(struct obd_export *exp);
void class_disconnect_exports(struct obd_device *obddev);
void class_disconnect_stale_exports(struct obd_device *obddev);
void class_manual_cleanup(struct obd_device *obd, char *flags);

/* obdo.c */
#ifdef __KERNEL__
void obdo_from_iattr(struct obdo *oa, struct iattr *attr, unsigned ia_valid);
void iattr_from_obdo(struct iattr *attr, struct obdo *oa, obd_flag valid);
void obdo_from_inode(struct obdo *dst, struct inode *src, obd_flag valid);
void obdo_refresh_inode(struct inode *dst, struct obdo *src, obd_flag valid);
void obdo_to_inode(struct inode *dst, struct obdo *src, obd_flag valid);
#endif
void obdo_cpy_md(struct obdo *dst, struct obdo *src, obd_flag valid);
int obdo_cmp_md(struct obdo *dst, struct obdo *src, obd_flag compare);
void obdo_to_ioobj(struct obdo *oa, struct obd_ioobj *ioobj);


#define OBT(dev)        (dev)->obd_type
#define OBP(dev, op)    (dev)->obd_type->typ_ops->o_ ## op
#define CTXTP(ctxt, op) (ctxt)->loc_logops->lop_##op

/* Ensure obd_setup: used for cleanup which must be called
   while obd is stopping */
#define OBD_CHECK_DEV(obd)                                      \
do {                                                            \
        if (!(obd)) {                                           \
                CERROR("NULL device\n");                        \
                RETURN(-ENODEV);                                \
        }                                                       \
} while (0)

/* ensure obd_setup and !obd_stopping */
#define OBD_CHECK_DEV_ACTIVE(obd)                               \
do {                                                            \
        OBD_CHECK_DEV(obd);                                     \
        if (!(obd)->obd_set_up || (obd)->obd_stopping) {        \
                CERROR("Device %d not setup\n",                 \
                       (obd)->obd_minor);                       \
                RETURN(-ENODEV);                                \
        }                                                       \
} while (0)


#ifdef LPROCFS
#define OBD_COUNTER_OFFSET(op)                                  \
        ((offsetof(struct obd_ops, o_ ## op) -                  \
          offsetof(struct obd_ops, o_iocontrol))                \
         / sizeof(((struct obd_ops *)(0))->o_iocontrol))

#define OBD_COUNTER_INCREMENT(obd, op)                          \
        if ((obd)->obd_stats != NULL) {                         \
                unsigned int coffset;                           \
                coffset = (unsigned int)(obd)->obd_cntr_base +  \
                        OBD_COUNTER_OFFSET(op);                 \
                LASSERT(coffset < obd->obd_stats->ls_num);      \
                lprocfs_counter_incr(obd->obd_stats, coffset);  \
        }
#else
#define OBD_COUNTER_OFFSET(op)
#define OBD_COUNTER_INCREMENT(obd, op)
#endif

#define OBD_CHECK_OP(obd, op, err)                              \
do {                                                            \
        if (!OBT(obd) || !OBP((obd), op)) {\
                if (err)                                        \
                        CERROR("obd_" #op ": dev %d no operation\n",    \
                               obd->obd_minor);                         \
                RETURN(err);                                    \
        }                                                       \
} while (0)

#define EXP_CHECK_OP(exp, op)                                   \
do {                                                            \
        if ((exp) == NULL) {                                    \
                CERROR("obd_" #op ": NULL export\n");           \
                RETURN(-ENODEV);                                \
        }                                                       \
        if ((exp)->exp_obd == NULL || !OBT((exp)->exp_obd)) {   \
                CERROR("obd_" #op ": cleaned up obd\n");        \
                RETURN(-EOPNOTSUPP);                            \
        }                                                       \
        if (!OBT((exp)->exp_obd) || !OBP((exp)->exp_obd, op)) { \
                CERROR("obd_" #op ": dev %d no operation\n",    \
                       (exp)->exp_obd->obd_minor);              \
                RETURN(-EOPNOTSUPP);                            \
        }                                                       \
} while (0)

#define CTXT_CHECK_OP(ctxt, op, err)                                         \
do {                                                            \
        if (!OBT(ctxt->loc_obd) || !CTXTP((ctxt), op)) {                     \
                if (err)                                        \
                        CERROR("lop_" #op ": dev %d no operation\n",    \
                               ctxt->loc_obd->obd_minor);                         \
                RETURN(err);                                    \
        }                                                       \
} while (0)

static inline int obd_get_info(struct obd_export *exp, __u32 keylen,
                               void *key, __u32 *vallen, void *val)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, get_info);
        OBD_COUNTER_INCREMENT(exp->exp_obd, get_info);

        rc = OBP(exp->exp_obd, get_info)(exp, keylen, key, vallen, val);
        RETURN(rc);
}

static inline int obd_set_info(struct obd_export *exp, obd_count keylen,
                               void *key, obd_count vallen, void *val)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, set_info);
        OBD_COUNTER_INCREMENT(exp->exp_obd, set_info);

        rc = OBP(exp->exp_obd, set_info)(exp, keylen, key, vallen, val);
        RETURN(rc);
}

static inline int obd_setup(struct obd_device *obd, int datalen, void *data)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(obd, setup, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, setup);

        rc = OBP(obd, setup)(obd, datalen, data);
        RETURN(rc);
}

static inline int obd_precleanup(struct obd_device *obd, int cleanup_stage)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(obd, precleanup, 0);
        OBD_COUNTER_INCREMENT(obd, precleanup);

        rc = OBP(obd, precleanup)(obd, cleanup_stage);
        RETURN(rc);
}

static inline int obd_cleanup(struct obd_device *obd)
{
        int rc;
        ENTRY;

        OBD_CHECK_DEV(obd);
        OBD_CHECK_OP(obd, cleanup, 0);
        OBD_COUNTER_INCREMENT(obd, cleanup);

        rc = OBP(obd, cleanup)(obd);
        RETURN(rc);
}

static inline int
obd_process_config(struct obd_device *obd, int datalen, void *data)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(obd, process_config, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, process_config);

        rc = OBP(obd, process_config)(obd, datalen, data);
        RETURN(rc);
}

/* Pack an in-memory MD struct for storage on disk.
 * Returns +ve size of packed MD (0 for free), or -ve error.
 *
 * If @disk_tgt == NULL, MD size is returned (max size if @mem_src == NULL).
 * If @*disk_tgt != NULL and @mem_src == NULL, @*disk_tgt will be freed.
 * If @*disk_tgt == NULL, it will be allocated
 */
static inline int obd_packmd(struct obd_export *exp,
                             struct lov_mds_md **disk_tgt,
                             struct lov_stripe_md *mem_src)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, packmd);
        OBD_COUNTER_INCREMENT(exp->exp_obd, packmd);

        rc = OBP(exp->exp_obd, packmd)(exp, disk_tgt, mem_src);
        RETURN(rc);
}

static inline int obd_size_diskmd(struct obd_export *exp,
                                  struct lov_stripe_md *mem_src)
{
        return obd_packmd(exp, NULL, mem_src);
}

/* helper functions */
static inline int obd_alloc_diskmd(struct obd_export *exp,
                                   struct lov_mds_md **disk_tgt)
{
        LASSERT(disk_tgt);
        LASSERT(*disk_tgt == NULL);
        return obd_packmd(exp, disk_tgt, NULL);
}

static inline int obd_free_diskmd(struct obd_export *exp,
                                  struct lov_mds_md **disk_tgt)
{
        LASSERT(disk_tgt);
        LASSERT(*disk_tgt);
        return obd_packmd(exp, disk_tgt, NULL);
}

/* Unpack an MD struct from disk to in-memory format.
 * Returns +ve size of unpacked MD (0 for free), or -ve error.
 *
 * If @mem_tgt == NULL, MD size is returned (max size if @disk_src == NULL).
 * If @*mem_tgt != NULL and @disk_src == NULL, @*mem_tgt will be freed.
 * If @*mem_tgt == NULL, it will be allocated
 */
static inline int obd_unpackmd(struct obd_export *exp,
                               struct lov_stripe_md **mem_tgt,
                               struct lov_mds_md *disk_src,
                               int disk_len)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, unpackmd);
        OBD_COUNTER_INCREMENT(exp->exp_obd, unpackmd);

        rc = OBP(exp->exp_obd, unpackmd)(exp, mem_tgt, disk_src, disk_len);
        RETURN(rc);
}

/* helper functions */
static inline int obd_alloc_memmd(struct obd_export *exp,
                                  struct lov_stripe_md **mem_tgt)
{
        LASSERT(mem_tgt);
        LASSERT(*mem_tgt == NULL);
        return obd_unpackmd(exp, mem_tgt, NULL, 0);
}

static inline int obd_free_memmd(struct obd_export *exp,
                                 struct lov_stripe_md **mem_tgt)
{
        LASSERT(mem_tgt);
        LASSERT(*mem_tgt);
        return obd_unpackmd(exp, mem_tgt, NULL, 0);
}

static inline int obd_create(struct obd_export *exp, struct obdo *obdo,
                             struct lov_stripe_md **ea,
                             struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, create);
        OBD_COUNTER_INCREMENT(exp->exp_obd, create);

        rc = OBP(exp->exp_obd, create)(exp, obdo, ea, oti);
        RETURN(rc);
}

static inline int obd_destroy(struct obd_export *exp, struct obdo *obdo,
                              struct lov_stripe_md *ea,
                              struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, destroy);
        OBD_COUNTER_INCREMENT(exp->exp_obd, destroy);

        rc = OBP(exp->exp_obd, destroy)(exp, obdo, ea, oti);
        RETURN(rc);
}

static inline int obd_getattr(struct obd_export *exp, struct obdo *obdo,
                              struct lov_stripe_md *ea)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, getattr);
        OBD_COUNTER_INCREMENT(exp->exp_obd, getattr);

        rc = OBP(exp->exp_obd, getattr)(exp, obdo, ea);
        RETURN(rc);
}

static inline int obd_getattr_async(struct obd_export *exp,
                                    struct obdo *obdo, struct lov_stripe_md *ea,
                                    struct ptlrpc_request_set *set)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, getattr);
        OBD_COUNTER_INCREMENT(exp->exp_obd, getattr);

        rc = OBP(exp->exp_obd, getattr_async)(exp, obdo, ea, set);
        RETURN(rc);
}

static inline int obd_setattr(struct obd_export *exp, struct obdo *obdo,
                              struct lov_stripe_md *ea,
                              struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, setattr);
        OBD_COUNTER_INCREMENT(exp->exp_obd, setattr);

        rc = OBP(exp->exp_obd, setattr)(exp, obdo, ea, oti);
        RETURN(rc);
}

static inline int obd_setattr_async(struct obd_export *exp,
                                    struct obdo *obdo,
                                    struct lov_stripe_md *ea,
                                    struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, setattr_async);
        OBD_COUNTER_INCREMENT(exp->exp_obd, setattr_async);

        rc = OBP(exp->exp_obd, setattr_async)(exp, obdo, ea, oti);
        RETURN(rc);
}

static inline int obd_add_conn(struct obd_import *imp, struct obd_uuid *uuid,
                               int priority)
{
        struct obd_device *obd = imp->imp_obd;
        int rc;
        ENTRY;

        OBD_CHECK_DEV_ACTIVE(obd);
        OBD_CHECK_OP(obd, add_conn, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, add_conn);

        rc = OBP(obd, add_conn)(imp, uuid, priority);
        RETURN(rc);
}

static inline int obd_del_conn(struct obd_import *imp, struct obd_uuid *uuid)
{
        struct obd_device *obd = imp->imp_obd;
        int rc;
        ENTRY;

        OBD_CHECK_DEV_ACTIVE(obd);
        OBD_CHECK_OP(obd, del_conn, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, del_conn);

        rc = OBP(obd, del_conn)(imp, uuid);
        RETURN(rc);
}

static inline int obd_connect(struct lustre_handle *conn, struct obd_device *obd,
                              struct obd_uuid *cluuid,
                              struct obd_connect_data *data)
{
        int rc;
        ENTRY;

        OBD_CHECK_DEV_ACTIVE(obd);
        OBD_CHECK_OP(obd, connect, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, connect);

        rc = OBP(obd, connect)(conn, obd, cluuid, data);
        RETURN(rc);
}

static inline int obd_disconnect(struct obd_export *exp)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, disconnect);
        OBD_COUNTER_INCREMENT(exp->exp_obd, disconnect);

        rc = OBP(exp->exp_obd, disconnect)(exp);
        RETURN(rc);
}

static inline int obd_init_export(struct obd_export *exp)
{
        int rc = 0;

        ENTRY;
        if ((exp)->exp_obd != NULL && OBT((exp)->exp_obd) &&
            OBP((exp)->exp_obd, init_export))
                rc = OBP(exp->exp_obd, init_export)(exp);
        RETURN(rc);
}

static inline int obd_destroy_export(struct obd_export *exp)
{
        ENTRY;
        if ((exp)->exp_obd != NULL && OBT((exp)->exp_obd) &&
            OBP((exp)->exp_obd, destroy_export))
                OBP(exp->exp_obd, destroy_export)(exp);
        RETURN(0);
}

static inline struct dentry *
obd_lvfs_fid2dentry(struct obd_export *exp, __u64 id_ino, __u32 gen, __u64 gr)
{
        LASSERT(exp->exp_obd);

        return lvfs_fid2dentry(&exp->exp_obd->obd_lvfs_ctxt, id_ino, gen, gr,
                               exp->exp_obd);
}

static inline int 
obd_lvfs_open_llog(struct obd_export *exp, __u64 id_ino, struct dentry *dentry)
{
        LASSERT(exp->exp_obd);

        return lvfs_open_llog(&exp->exp_obd->obd_lvfs_ctxt, id_ino, 
                              dentry, exp->exp_obd);
}

#ifndef time_before
#define time_before(t1, t2) ((long)t2 - (long)t1 > 0)
#endif

/* @max_age is the oldest time in jiffies that we accept using a cached data.
 * If the cache is older than @max_age we will get a new value from the
 * target.  Use a value of "jiffies + HZ" to guarantee freshness. */
static inline int obd_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                             unsigned long max_age)
{
        int rc = 0;
        ENTRY;

        if (obd == NULL)
                RETURN(-EINVAL);

        OBD_CHECK_OP(obd, statfs, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, statfs);

        CDEBUG(D_SUPER, "osfs %lu, max_age %lu\n", obd->obd_osfs_age, max_age);
        if (time_before(obd->obd_osfs_age, max_age)) {
                rc = OBP(obd, statfs)(obd, osfs, max_age);
                if (rc == 0) {
                        spin_lock(&obd->obd_osfs_lock);
                        memcpy(&obd->obd_osfs, osfs, sizeof(obd->obd_osfs));
                        obd->obd_osfs_age = jiffies;
                        spin_unlock(&obd->obd_osfs_lock);
                }
        } else {
                CDEBUG(D_SUPER, "using cached obd_statfs data\n");
                spin_lock(&obd->obd_osfs_lock);
                memcpy(osfs, &obd->obd_osfs, sizeof(*osfs));
                spin_unlock(&obd->obd_osfs_lock);
        }
        RETURN(rc);
}

static inline int obd_sync(struct obd_export *exp, struct obdo *oa,
                           struct lov_stripe_md *ea, obd_size start,
                           obd_size end)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, sync, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, sync);

        rc = OBP(exp->exp_obd, sync)(exp, oa, ea, start, end);
        RETURN(rc);
}

static inline int obd_punch(struct obd_export *exp, struct obdo *oa,
                            struct lov_stripe_md *ea, obd_size start,
                            obd_size end, struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, punch);
        OBD_COUNTER_INCREMENT(exp->exp_obd, punch);

        rc = OBP(exp->exp_obd, punch)(exp, oa, ea, start, end, oti);
        RETURN(rc);
}

static inline int obd_brw(int cmd, struct obd_export *exp, struct obdo *oa,
                          struct lov_stripe_md *ea, obd_count oa_bufs,
                          struct brw_page *pg, struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, brw);
        OBD_COUNTER_INCREMENT(exp->exp_obd, brw);

        if (!(cmd & (OBD_BRW_RWMASK | OBD_BRW_CHECK))) {
                CERROR("obd_brw: cmd must be OBD_BRW_READ, OBD_BRW_WRITE, "
                       "or OBD_BRW_CHECK\n");
                LBUG();
        }

        rc = OBP(exp->exp_obd, brw)(cmd, exp, oa, ea, oa_bufs, pg, oti);
        RETURN(rc);
}

static inline int obd_brw_async(int cmd, struct obd_export *exp,
                                struct obdo *oa, struct lov_stripe_md *ea,
                                obd_count oa_bufs, struct brw_page *pg,
                                struct ptlrpc_request_set *set,
                                struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, brw_async);
        OBD_COUNTER_INCREMENT(exp->exp_obd, brw_async);

        if (!(cmd & OBD_BRW_RWMASK)) {
                CERROR("obd_brw: cmd must be OBD_BRW_READ or OBD_BRW_WRITE\n");
                LBUG();
        }

        rc = OBP(exp->exp_obd, brw_async)(cmd, exp, oa, ea, oa_bufs, pg, set,
                                          oti);
        RETURN(rc);
}

static inline  int obd_prep_async_page(struct obd_export *exp,
                                       struct lov_stripe_md *lsm,
                                       struct lov_oinfo *loi,
                                       struct page *page, obd_off offset,
                                       struct obd_async_page_ops *ops,
                                       void *data, void **res)
{
        int ret;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, prep_async_page, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, prep_async_page);

        ret = OBP(exp->exp_obd, prep_async_page)(exp, lsm, loi, page, offset,
                                                 ops, data, res);
        RETURN(ret);
}

static inline int obd_queue_async_io(struct obd_export *exp,
                                     struct lov_stripe_md *lsm,
                                     struct lov_oinfo *loi, void *cookie,
                                     int cmd, obd_off off, int count,
                                     obd_flag brw_flags, obd_flag async_flags)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, queue_async_io, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, queue_async_io);
        LASSERT(cmd & OBD_BRW_RWMASK);

        rc = OBP(exp->exp_obd, queue_async_io)(exp, lsm, loi, cookie, cmd, off,
                                               count, brw_flags, async_flags);
        RETURN(rc);
}

static inline int obd_set_async_flags(struct obd_export *exp,
                                      struct lov_stripe_md *lsm,
                                      struct lov_oinfo *loi, void *cookie,
                                      obd_flag async_flags)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, set_async_flags, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, set_async_flags);

        rc = OBP(exp->exp_obd, set_async_flags)(exp, lsm, loi, cookie,
                                                async_flags);
        RETURN(rc);
}

static inline int obd_queue_group_io(struct obd_export *exp,
                                     struct lov_stripe_md *lsm,
                                     struct lov_oinfo *loi,
                                     struct obd_io_group *oig,
                                     void *cookie, int cmd, obd_off off,
                                     int count, obd_flag brw_flags,
                                     obd_flag async_flags)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, queue_group_io, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, queue_group_io);
        LASSERT(cmd & OBD_BRW_RWMASK);

        rc = OBP(exp->exp_obd, queue_group_io)(exp, lsm, loi, oig, cookie,
                                               cmd, off, count, brw_flags,
                                               async_flags);
        RETURN(rc);
}

static inline int obd_trigger_group_io(struct obd_export *exp,
                                       struct lov_stripe_md *lsm,
                                       struct lov_oinfo *loi,
                                       struct obd_io_group *oig)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, trigger_group_io, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, trigger_group_io);

        rc = OBP(exp->exp_obd, trigger_group_io)(exp, lsm, loi, oig);
        RETURN(rc);
}

static inline int obd_teardown_async_page(struct obd_export *exp,
                                          struct lov_stripe_md *lsm,
                                          struct lov_oinfo *loi, void *cookie)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, teardown_async_page, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, teardown_async_page);

        rc = OBP(exp->exp_obd, teardown_async_page)(exp, lsm, loi, cookie);
        RETURN(rc);
}

static inline int obd_preprw(int cmd, struct obd_export *exp, struct obdo *oa,
                             int objcount, struct obd_ioobj *obj,
                             int niocount, struct niobuf_remote *remote,
                             struct niobuf_local *local,
                             struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, preprw, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, preprw);

        rc = OBP(exp->exp_obd, preprw)(cmd, exp, oa, objcount, obj, niocount,
                                       remote, local, oti);
        RETURN(rc);
}

static inline int obd_commitrw(int cmd, struct obd_export *exp, struct obdo *oa,
                               int objcount, struct obd_ioobj *obj,
                               int niocount, struct niobuf_local *local,
                               struct obd_trans_info *oti, int rc)
{
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, commitrw, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, commitrw);

        rc = OBP(exp->exp_obd, commitrw)(cmd, exp, oa, objcount, obj, niocount,
                                         local, oti, rc);
        RETURN(rc);
}

/* b1_4_bug5047 has changes to make this an obd_merge_lvb() method */
__u64 lov_merge_size(struct lov_stripe_md *lsm, int kms_only);
__u64 lov_merge_blocks(struct lov_stripe_md *lsm);
__u64 lov_merge_mtime(struct lov_stripe_md *lsm, __u64 current_time);

static inline int obd_adjust_kms(struct obd_export *exp,
                                 struct lov_stripe_md *lsm, obd_off size,
                                 int shrink)
{
        int rc;
        ENTRY;

        OBD_CHECK_OP(exp->exp_obd, adjust_kms, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, adjust_kms);

        rc = OBP(exp->exp_obd, adjust_kms)(exp, lsm, size, shrink);
        RETURN(rc);
}

static inline int obd_iocontrol(unsigned int cmd, struct obd_export *exp,
                                int len, void *karg, void *uarg)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, iocontrol);
        OBD_COUNTER_INCREMENT(exp->exp_obd, iocontrol);

        rc = OBP(exp->exp_obd, iocontrol)(cmd, exp, len, karg, uarg);
        RETURN(rc);
}

static inline int obd_enqueue(struct obd_export *exp, struct lov_stripe_md *ea,
                              __u32 type, ldlm_policy_data_t *policy,
                              __u32 mode, int *flags, void *bl_cb, void *cp_cb,
                              void *gl_cb, void *data, __u32 lvb_len,
                              void *lvb_swabber, struct lustre_handle *lockh)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, enqueue);
        OBD_COUNTER_INCREMENT(exp->exp_obd, enqueue);

        rc = OBP(exp->exp_obd, enqueue)(exp, ea, type, policy, mode, flags,
                                        bl_cb, cp_cb, gl_cb, data, lvb_len,
                                        lvb_swabber, lockh);
        RETURN(rc);
}

static inline int obd_match(struct obd_export *exp, struct lov_stripe_md *ea,
                            __u32 type, ldlm_policy_data_t *policy, __u32 mode,
                            int *flags, void *data, struct lustre_handle *lockh)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, match);
        OBD_COUNTER_INCREMENT(exp->exp_obd, match);

        rc = OBP(exp->exp_obd, match)(exp, ea, type, policy, mode, flags, data,
                                      lockh);
        RETURN(rc);
}

static inline int obd_change_cbdata(struct obd_export *exp,
                                    struct lov_stripe_md *lsm,
                                    ldlm_iterator_t it, void *data)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, change_cbdata);
        OBD_COUNTER_INCREMENT(exp->exp_obd, change_cbdata);

        rc = OBP(exp->exp_obd, change_cbdata)(exp, lsm, it, data);
        RETURN(rc);
}

static inline int obd_cancel(struct obd_export *exp,
                             struct lov_stripe_md *ea, __u32 mode,
                             struct lustre_handle *lockh)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, cancel);
        OBD_COUNTER_INCREMENT(exp->exp_obd, cancel);

        rc = OBP(exp->exp_obd, cancel)(exp, ea, mode, lockh);
        RETURN(rc);
}

static inline int obd_cancel_unused(struct obd_export *exp,
                                    struct lov_stripe_md *ea, int flags,
                                    void *opaque)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, cancel_unused);
        OBD_COUNTER_INCREMENT(exp->exp_obd, cancel_unused);

        rc = OBP(exp->exp_obd, cancel_unused)(exp, ea, flags, opaque);
        RETURN(rc);
}

static inline int obd_join_lru(struct obd_export *exp,
                               struct lov_stripe_md *ea, int join)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, join_lru);
        OBD_COUNTER_INCREMENT(exp->exp_obd, join_lru);

        rc = OBP(exp->exp_obd, join_lru)(exp, ea, join);
        RETURN(rc);
}

static inline int obd_san_preprw(int cmd, struct obd_export *exp,
                                 struct obdo *oa,
                                 int objcount, struct obd_ioobj *obj,
                                 int niocount, struct niobuf_remote *remote)
{
        int rc;

        EXP_CHECK_OP(exp, preprw);
        OBD_COUNTER_INCREMENT(exp->exp_obd, preprw);

        rc = OBP(exp->exp_obd, san_preprw)(cmd, exp, oa, objcount, obj,
                                           niocount, remote);
        class_export_put(exp);
        return(rc);
}

static inline int obd_pin(struct obd_export *exp, obd_id ino, __u32 gen,
                          int type, struct obd_client_handle *handle, int flag)
{
        int rc;

        EXP_CHECK_OP(exp, pin);
        OBD_COUNTER_INCREMENT(exp->exp_obd, pin);

        rc = OBP(exp->exp_obd, pin)(exp, ino, gen, type, handle, flag);
        return(rc);
}

static inline int obd_unpin(struct obd_export *exp,
                            struct obd_client_handle *handle, int flag)
{
        int rc;

        EXP_CHECK_OP(exp, unpin);
        OBD_COUNTER_INCREMENT(exp->exp_obd, unpin);

        rc = OBP(exp->exp_obd, unpin)(exp, handle, flag);
        return(rc);
}


static inline void obd_import_event(struct obd_device *obd,
                                    struct obd_import *imp,
                                    enum obd_import_event event)
{
        if (!obd) {
                CERROR("NULL device\n");
                EXIT;
                return;
        }
        if (obd->obd_set_up && OBP(obd, import_event)) {
                OBD_COUNTER_INCREMENT(obd, import_event);
                OBP(obd, import_event)(obd, imp, event);
        }
}

static inline int obd_notify(struct obd_device *obd,
                             struct obd_device *watched,
                             int active)
{
        OBD_CHECK_DEV(obd);
        if (!obd->obd_set_up) {
                CERROR("obd %s not set up\n", obd->obd_name);
                return -EINVAL;
        }

        if (!OBP(obd, notify)) {
                CERROR("obd %s has no notify handler\n", obd->obd_name);
                return -ENOSYS;
        }

        OBD_COUNTER_INCREMENT(obd, notify);
        return OBP(obd, notify)(obd, watched, active);
}

static inline int obd_quotacheck(struct obd_export *exp,
                                 struct obd_quotactl *oqctl)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, quotacheck);
        OBD_COUNTER_INCREMENT(exp->exp_obd, quotacheck);

        rc = OBP(exp->exp_obd, quotacheck)(exp, oqctl);
        RETURN(rc);
}

static inline int obd_quotactl(struct obd_export *exp,
                               struct obd_quotactl *oqctl)
{
        int rc;
        ENTRY;

        EXP_CHECK_OP(exp, quotactl);
        OBD_COUNTER_INCREMENT(exp->exp_obd, quotactl);

        rc = OBP(exp->exp_obd, quotactl)(exp, oqctl);
        RETURN(rc);
}


static inline int obd_register_observer(struct obd_device *obd,
                                        struct obd_device *observer)
{
        ENTRY;
        OBD_CHECK_DEV(obd);
        if (obd->obd_observer && observer)
                RETURN(-EALREADY);
        obd->obd_observer = observer;
        RETURN(0);
}

/* OBD Metadata Support */

extern int obd_init_caches(void);
extern void obd_cleanup_caches(void);

/* support routines */
extern kmem_cache_t *obdo_cachep;
static inline struct obdo *obdo_alloc(void)
{
        struct obdo *oa;

        OBD_SLAB_ALLOC(oa, obdo_cachep, SLAB_KERNEL, sizeof(*oa));

        return oa;
}

/* qunit hash stuff */
extern kmem_cache_t *qunit_cachep;
extern struct list_head qunit_hash[];
extern spinlock_t qunit_hash_lock;

static inline void obdo_free(struct obdo *oa)
{
        OBD_SLAB_FREE(oa, obdo_cachep, sizeof(*oa));
}

#if !defined(__KERNEL__) || (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
#define to_kdev_t(dev) dev
#define kdev_t_to_nr(dev) dev
#endif

/* I'm as embarrassed about this as you are.
 *
 * <shaver> // XXX do not look into _superhack with remaining eye
 * <shaver> // XXX if this were any uglier, I'd get my own show on MTV */
extern int (*ptlrpc_put_connection_superhack)(struct ptlrpc_connection *c);
extern void (*ptlrpc_abort_inflight_superhack)(struct obd_import *imp);

/* sysctl.c */
extern void obd_sysctl_init (void);
extern void obd_sysctl_clean (void);

/* uuid.c  */
typedef __u8 class_uuid_t[16];
//int class_uuid_parse(struct obd_uuid in, class_uuid_t out);
void class_uuid_unparse(class_uuid_t in, struct obd_uuid *out);

/* lustre_peer.c    */
int lustre_uuid_to_peer(char *uuid, ptl_nid_t *peer_nid);
int class_add_uuid(char *uuid, __u64 nid);
int class_del_uuid (char *uuid);
void class_init_uuidlist(void);
void class_exit_uuidlist(void);

#endif /* __LINUX_OBD_CLASS_H */
