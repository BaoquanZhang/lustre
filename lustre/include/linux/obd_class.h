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

struct lu_device_type;

/* genops.c */
struct obd_export *class_conn2export(struct lustre_handle *);
int class_register_type(struct obd_ops *, struct md_ops *,
                        struct lprocfs_vars *, const char *nm,
                        struct lu_device_type *ldt);
int class_unregister_type(const char *nm);

struct obd_device *class_newdev(struct obd_type *type, char *name);
void class_release_dev(struct obd_device *obd);

int class_name2dev(const char *name);
struct obd_device *class_name2obd(const char *name);
int class_uuid2dev(struct obd_uuid *uuid);
struct obd_device *class_uuid2obd(struct obd_uuid *uuid);
void class_obd_list(void);
struct obd_device * class_find_client_obd(struct obd_uuid *tgt_uuid,
                                          const char * typ_name,
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

char *obd_export_nid2str(struct obd_export *exp);

int obd_export_evict_by_nid(struct obd_device *obd, const char *nid);
int obd_export_evict_by_uuid(struct obd_device *obd, const char *uuid);

/* obd_config.c */
int class_process_config(struct lustre_cfg *lcfg);
int class_attach(struct lustre_cfg *lcfg);
int class_setup(struct obd_device *obd, struct lustre_cfg *lcfg);
int class_cleanup(struct obd_device *obd, struct lustre_cfg *lcfg);
int class_detach(struct obd_device *obd, struct lustre_cfg *lcfg);
struct obd_device *class_incref(struct obd_device *obd);
void class_decref(struct obd_device *obd);

#define CFG_F_START     0x01   /* Set when we start updating from a log */
#define CFG_F_MARKER    0x02   /* We are within a maker */
#define CFG_F_SKIP      0x04   /* We should ignore this cfg command */
#define CFG_F_COMPAT146 0x08   /* Translation to new obd names required */
#define CFG_F_EXCLUDE   0x10   /* OST exclusion list */


/* Passed as data param to class_config_parse_llog */
struct config_llog_instance {
        char *              cfg_instance;
        struct super_block *cfg_sb;
        struct obd_uuid     cfg_uuid;
        int                 cfg_last_idx; /* for partial llog processing */
        int                 cfg_flags;
};
int class_config_parse_llog(struct llog_ctxt *ctxt, char *name,
                            struct config_llog_instance *cfg);
int class_config_dump_llog(struct llog_ctxt *ctxt, char *name,
                           struct config_llog_instance *cfg);

/* list of active configuration logs  */
struct config_llog_data {
        char               *cld_logname;
        struct ldlm_res_id  cld_resid;
        struct config_llog_instance cld_cfg;
        struct list_head    cld_list_chain;
        atomic_t            cld_refcount;
        unsigned int        cld_stopping:1;
};

struct lustre_profile {
        struct list_head lp_list;
        char * lp_profile;
        char * lp_osc;
        char * lp_mdc;
};

struct lustre_profile *class_get_profile(const char * prof);
void class_del_profile(const char *prof);

/* genops.c */
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
struct obd_export *class_new_export(struct obd_device *obddev,
                                    struct obd_uuid *cluuid);
void class_unlink_export(struct obd_export *exp);

struct obd_import *class_import_get(struct obd_import *);
void class_import_put(struct obd_import *);
struct obd_import *class_new_import(struct obd_device *obd);
void class_destroy_import(struct obd_import *exp);

struct obd_type *class_search_type(const char *name);
struct obd_type *class_get_type(const char *name);
void class_put_type(struct obd_type *type);
int class_connect(struct lustre_handle *conn, struct obd_device *obd,
                  struct obd_uuid *cluuid);
int class_disconnect(struct obd_export *exp);
void class_fail_export(struct obd_export *exp);
void class_disconnect_exports(struct obd_device *obddev);
void class_disconnect_stale_exports(struct obd_device *obddev);
int class_manual_cleanup(struct obd_device *obd);

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
#define OBP(dev, op)    (dev)->obd_type->typ_dt_ops->o_ ## op
#define MDP(dev, op)    (dev)->obd_type->typ_md_ops->m_ ## op
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

#define MD_COUNTER_OFFSET(op)                                  \
        ((offsetof(struct md_ops, m_ ## op) -                  \
          offsetof(struct md_ops, m_getstatus))                \
         / sizeof(((struct md_ops *)(0))->m_getstatus))

#define MD_COUNTER_INCREMENT(obd, op)                           \
        if ((obd)->md_stats != NULL) {                          \
                unsigned int coffset;                           \
                coffset = (unsigned int)(obd)->md_cntr_base +   \
                        MD_COUNTER_OFFSET(op);                  \
                LASSERT(coffset < (obd)->md_stats->ls_num);     \
                lprocfs_counter_incr((obd)->md_stats, coffset); \
        }

#else
#define OBD_COUNTER_OFFSET(op)
#define OBD_COUNTER_INCREMENT(obd, op)
#define MD_COUNTER_INCREMENT(obd, op)
#endif

#define OBD_CHECK_MD_OP(obd, op, err)                           \
do {                                                            \
        if (!OBT(obd) || !MDP((obd), op)) {                     \
                if (err)                                        \
                        CERROR("md_" #op ": dev %s/%d no operation\n", \
                               obd->obd_name, obd->obd_minor);  \
                RETURN(err);                                    \
        }                                                       \
} while (0)

#define EXP_CHECK_MD_OP(exp, op)                                \
do {                                                            \
        if ((exp) == NULL) {                                    \
                CERROR("obd_" #op ": NULL export\n");           \
                RETURN(-ENODEV);                                \
        }                                                       \
        if ((exp)->exp_obd == NULL || !OBT((exp)->exp_obd)) {   \
                CERROR("obd_" #op ": cleaned up obd\n");        \
                RETURN(-EOPNOTSUPP);                            \
        }                                                       \
        if (!OBT((exp)->exp_obd) || !MDP((exp)->exp_obd, op)) { \
                CERROR("obd_" #op ": dev %s/%d no operation\n", \
                       (exp)->exp_obd->obd_name,                \
		       (exp)->exp_obd->obd_minor);              \
                RETURN(-EOPNOTSUPP);                            \
        }                                                       \
} while (0)


#define OBD_CHECK_DT_OP(obd, op, err)                           \
do {                                                            \
        if (!OBT(obd) || !OBP((obd), op)) {                     \
                if (err)                                        \
                        CERROR("obd_" #op ": dev %d no operation\n",    \
                               obd->obd_minor);                 \
                RETURN(err);                                    \
        }                                                       \
} while (0)

#define EXP_CHECK_DT_OP(exp, op)                                \
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

        EXP_CHECK_DT_OP(exp, get_info);
        OBD_COUNTER_INCREMENT(exp->exp_obd, get_info);

        rc = OBP(exp->exp_obd, get_info)(exp, keylen, key, vallen, val);
        RETURN(rc);
}

static inline int obd_set_info(struct obd_export *exp, obd_count keylen,
                               void *key, obd_count vallen, void *val)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, set_info);
        OBD_COUNTER_INCREMENT(exp->exp_obd, set_info);

        rc = OBP(exp->exp_obd, set_info)(exp, keylen, key, vallen, val);
        RETURN(rc);
}

static inline int obd_setup(struct obd_device *obd, struct lustre_cfg *cfg)
{
        int rc;
        struct lu_device_type *ldt;
        ENTRY;

        ldt = obd->obd_type->typ_lu;
        if (ldt != NULL) {
                struct lu_device *d;

                d = ldt->ldt_ops->ldto_device_alloc(ldt, cfg);
                if (!IS_ERR(d)) {
                        obd->obd_lu_dev = d;
                        d->ld_obd = obd;
                        rc = 0;
                } else
                        rc = PTR_ERR(d);
        } else {
                OBD_CHECK_DT_OP(obd, setup, -EOPNOTSUPP);
                OBD_COUNTER_INCREMENT(obd, setup);
                rc = OBP(obd, setup)(obd, cfg);
        }
        RETURN(rc);
}

static inline int obd_precleanup(struct obd_device *obd,
                                 enum obd_cleanup_stage cleanup_stage)
{
        int rc;
        ENTRY;

        OBD_CHECK_DT_OP(obd, precleanup, 0);
        OBD_COUNTER_INCREMENT(obd, precleanup);

        rc = OBP(obd, precleanup)(obd, cleanup_stage);
        RETURN(rc);
}

static inline int obd_cleanup(struct obd_device *obd)
{
        int rc;
        struct lu_device *d;
        struct lu_device_type *ldt;
        ENTRY;

        OBD_CHECK_DEV(obd);

        ldt = obd->obd_type->typ_lu;
        d = obd->obd_lu_dev;
        if (ldt != NULL && d != NULL) {
                ldt->ldt_ops->ldto_device_free(d);
                obd->obd_lu_dev = NULL;
                rc = 0;
        } else {
                OBD_CHECK_DT_OP(obd, cleanup, 0);
                rc = OBP(obd, cleanup)(obd);
        }
        OBD_COUNTER_INCREMENT(obd, cleanup);
        RETURN(rc);
}

static inline int
obd_process_config(struct obd_device *obd, int datalen, void *data)
{
        int rc;
        struct lu_device *d;
        struct lu_device_type *ldt;
        ENTRY;

        OBD_CHECK_DEV(obd);

        ldt = obd->obd_type->typ_lu;
        d = obd->obd_lu_dev;
        if (ldt != NULL && d != NULL) {
                struct lu_context ctx;

                rc = lu_context_init(&ctx);
                if (rc == 0) {
                        lu_context_enter(&ctx);
                        rc = d->ld_ops->ldo_process_config(&ctx, d, data);
                        lu_context_exit(&ctx);
                        lu_context_fini(&ctx);
                }
        } else {
                OBD_CHECK_DT_OP(obd, process_config, -EOPNOTSUPP);
                rc = OBP(obd, process_config)(obd, datalen, data);
        }
        OBD_COUNTER_INCREMENT(obd, process_config);

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

        EXP_CHECK_DT_OP(exp, packmd);
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

        EXP_CHECK_DT_OP(exp, unpackmd);
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

static inline int obd_checkmd(struct obd_export *exp,
                              struct obd_export *md_exp,
                              struct lov_stripe_md *mem_tgt)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, checkmd);
        OBD_COUNTER_INCREMENT(exp->exp_obd, checkmd);

        rc = OBP(exp->exp_obd, checkmd)(exp, md_exp, mem_tgt);
        RETURN(rc);
}

static inline int obd_create(struct obd_export *exp, struct obdo *obdo,
                             struct lov_stripe_md **ea,
                             struct obd_trans_info *oti)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, create);
        OBD_COUNTER_INCREMENT(exp->exp_obd, create);

        rc = OBP(exp->exp_obd, create)(exp, obdo, ea, oti);
        RETURN(rc);
}

static inline int obd_destroy(struct obd_export *exp, struct obdo *obdo,
                              struct lov_stripe_md *ea,
                              struct obd_trans_info *oti,
                              struct obd_export *md_exp)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, destroy);
        OBD_COUNTER_INCREMENT(exp->exp_obd, destroy);

        rc = OBP(exp->exp_obd, destroy)(exp, obdo, ea, oti, md_exp);
        RETURN(rc);
}

static inline int obd_getattr(struct obd_export *exp, struct obdo *obdo,
                              struct lov_stripe_md *ea)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, getattr);
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

        EXP_CHECK_DT_OP(exp, getattr);
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

        EXP_CHECK_DT_OP(exp, setattr);
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

        EXP_CHECK_DT_OP(exp, setattr_async);
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
        OBD_CHECK_DT_OP(obd, add_conn, -EOPNOTSUPP);
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
        OBD_CHECK_DT_OP(obd, del_conn, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, del_conn);

        rc = OBP(obd, del_conn)(imp, uuid);
        RETURN(rc);
}

static inline int obd_connect(struct lustre_handle *conn,struct obd_device *obd,
                              struct obd_uuid *cluuid,
                              struct obd_connect_data *d)
{
        int rc;
        __u64 ocf = d ? d->ocd_connect_flags : 0; /* for post-condition check */
        ENTRY;

        OBD_CHECK_DEV_ACTIVE(obd);
        OBD_CHECK_DT_OP(obd, connect, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(obd, connect);

        rc = OBP(obd, connect)(conn, obd, cluuid, d);
        /* check that only subset is granted */
        LASSERT(ergo(d != NULL,
                     (d->ocd_connect_flags & ocf) == d->ocd_connect_flags));
        RETURN(rc);
}

static inline int obd_reconnect(struct obd_export *exp,
                                struct obd_device *obd,
                                struct obd_uuid *cluuid,
                                struct obd_connect_data *d)
{
        int rc;
        __u64 ocf = d ? d->ocd_connect_flags : 0; /* for post-condition check */
        ENTRY;

        OBD_CHECK_DEV_ACTIVE(obd);
        OBD_CHECK_DT_OP(obd, reconnect, 0);
        OBD_COUNTER_INCREMENT(obd, reconnect);

        rc = OBP(obd, reconnect)(exp, obd, cluuid, d);
        /* check that only subset is granted */
        LASSERT(ergo(d != NULL,
                     (d->ocd_connect_flags & ocf) == d->ocd_connect_flags));
        RETURN(rc);
}

static inline int obd_disconnect(struct obd_export *exp)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, disconnect);
        OBD_COUNTER_INCREMENT(exp->exp_obd, disconnect);

        rc = OBP(exp->exp_obd, disconnect)(exp);
        RETURN(rc);
}

static inline int obd_fid_alloc(struct obd_export *exp,
                                struct lu_fid *fid,
                                struct placement_hint *hint)
{
        int rc;
        ENTRY;

        if (OBP(exp->exp_obd, fid_alloc) == NULL)
                RETURN(-ENOTSUPP);

        OBD_COUNTER_INCREMENT(exp->exp_obd, fid_alloc);

        rc = OBP(exp->exp_obd, fid_alloc)(exp, fid, hint);
        RETURN(rc);
}

static inline int obd_fid_delete(struct obd_export *exp,
                                 struct lu_fid *fid)
{
        int rc;
        ENTRY;

        if (OBP(exp->exp_obd, fid_delete) == NULL)
                RETURN(0);

        OBD_COUNTER_INCREMENT(exp->exp_obd, fid_delete);
        rc = OBP(exp->exp_obd, fid_delete)(exp, fid);
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
        CERROR("FIXME what's the story here?  This needs to be an obd fn?\n");
#if 0
        return lvfs_open_llog(&exp->exp_obd->obd_lvfs_ctxt, id_ino,
                              dentry, exp->exp_obd);
#endif
        return 0;
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

        OBD_CHECK_DT_OP(obd, statfs, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, sync, -EOPNOTSUPP);
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

        EXP_CHECK_DT_OP(exp, punch);
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

        EXP_CHECK_DT_OP(exp, brw);
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

        EXP_CHECK_DT_OP(exp, brw_async);
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

        OBD_CHECK_DT_OP(exp->exp_obd, prep_async_page, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, queue_async_io, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, set_async_flags, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, queue_group_io, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, trigger_group_io, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, teardown_async_page, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, preprw, -EOPNOTSUPP);
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

        OBD_CHECK_DT_OP(exp->exp_obd, commitrw, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, commitrw);

        rc = OBP(exp->exp_obd, commitrw)(cmd, exp, oa, objcount, obj, niocount,
                                         local, oti, rc);
        RETURN(rc);
}

static inline int obd_merge_lvb(struct obd_export *exp,
                                struct lov_stripe_md *lsm,
                                struct ost_lvb *lvb, int kms_only)
{
        int rc;
        ENTRY;

        OBD_CHECK_DT_OP(exp->exp_obd, merge_lvb, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, merge_lvb);

        rc = OBP(exp->exp_obd, merge_lvb)(exp, lsm, lvb, kms_only);
        RETURN(rc);
}

static inline int obd_adjust_kms(struct obd_export *exp,
                                 struct lov_stripe_md *lsm, obd_off size,
                                 int shrink)
{
        int rc;
        ENTRY;

        OBD_CHECK_DT_OP(exp->exp_obd, adjust_kms, -EOPNOTSUPP);
        OBD_COUNTER_INCREMENT(exp->exp_obd, adjust_kms);

        rc = OBP(exp->exp_obd, adjust_kms)(exp, lsm, size, shrink);
        RETURN(rc);
}

static inline int obd_iocontrol(unsigned int cmd, struct obd_export *exp,
                                int len, void *karg, void *uarg)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, iocontrol);
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

        EXP_CHECK_DT_OP(exp, enqueue);
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

        EXP_CHECK_DT_OP(exp, match);
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

        EXP_CHECK_DT_OP(exp, change_cbdata);
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

        EXP_CHECK_DT_OP(exp, cancel);
        OBD_COUNTER_INCREMENT(exp->exp_obd, cancel);

        rc = OBP(exp->exp_obd, cancel)(exp, ea, mode, lockh);
        RETURN(rc);
}

static inline int obd_cancel_unused(struct obd_export *exp,
                                    struct lov_stripe_md *ea,
                                    int flags, void *opaque)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, cancel_unused);
        OBD_COUNTER_INCREMENT(exp->exp_obd, cancel_unused);

        rc = OBP(exp->exp_obd, cancel_unused)(exp, ea, flags, opaque);
        RETURN(rc);
}

static inline int obd_join_lru(struct obd_export *exp,
                               struct lov_stripe_md *ea, int join)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, join_lru);
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

        EXP_CHECK_DT_OP(exp, preprw);
        OBD_COUNTER_INCREMENT(exp->exp_obd, preprw);

        rc = OBP(exp->exp_obd, san_preprw)(cmd, exp, oa, objcount, obj,
                                           niocount, remote);
        class_export_put(exp);
        return(rc);
}

static inline int obd_pin(struct obd_export *exp, struct lu_fid *fid,
                          struct obd_client_handle *handle, int flag)
{
        int rc;

        EXP_CHECK_DT_OP(exp, pin);
        OBD_COUNTER_INCREMENT(exp->exp_obd, pin);

        rc = OBP(exp->exp_obd, pin)(exp, fid, handle, flag);
        return(rc);
}

static inline int obd_unpin(struct obd_export *exp,
                            struct obd_client_handle *handle, int flag)
{
        int rc;

        EXP_CHECK_DT_OP(exp, unpin);
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
                             enum obd_notify_event ev,
                             void *data)
{
        OBD_CHECK_DEV(obd);

        /* the check for async_recov is a complete hack - I'm hereby
           overloading the meaning to also mean "this was called from
           mds_postsetup".  I know that my mds is able to handle notifies
           by this point, and it needs to get them to execute mds_postrecov. */
        if (!obd->obd_set_up && !obd->obd_async_recov) {
                CDEBUG(D_HA, "obd %s not set up\n", obd->obd_name);
                return -EINVAL;
        }

        if (!OBP(obd, notify)) {
                CERROR("obd %s has no notify handler\n", obd->obd_name);
                return -ENOSYS;
        }

        OBD_COUNTER_INCREMENT(obd, notify);
        return OBP(obd, notify)(obd, watched, ev, data);
}

static inline int obd_notify_observer(struct obd_device *observer,
                                      struct obd_device *observed,
                                      enum obd_notify_event ev,
                                      void *data)
{
        int rc1;
        int rc2;

        struct obd_notify_upcall *onu;

        if (observer->obd_observer)
                rc1 = obd_notify(observer->obd_observer, observed, ev, data);
        else
                rc1 = 0;
        /*
         * Also, call non-obd listener, if any
         */
        onu = &observer->obd_upcall;
        if (onu->onu_upcall != NULL)
                rc2 = onu->onu_upcall(observer, observed, ev, onu->onu_owner);
        else
                rc2 = 0;

        return rc1 ? rc1 : rc2;
}

static inline int obd_quotacheck(struct obd_export *exp,
                                 struct obd_quotactl *oqctl)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, quotacheck);
        OBD_COUNTER_INCREMENT(exp->exp_obd, quotacheck);

        rc = OBP(exp->exp_obd, quotacheck)(exp, oqctl);
        RETURN(rc);
}

static inline int obd_quotactl(struct obd_export *exp,
                               struct obd_quotactl *oqctl)
{
        int rc;
        ENTRY;

        EXP_CHECK_DT_OP(exp, quotactl);
        OBD_COUNTER_INCREMENT(exp->exp_obd, quotactl);

        rc = OBP(exp->exp_obd, quotactl)(exp, oqctl);
        RETURN(rc);
}

static inline int obd_health_check(struct obd_device *obd)
{
        /* returns: 0 on healthy
         *         >0 on unhealthy + reason code/flag
         *            however the only suppored reason == 1 right now
         *            We'll need to define some better reasons
         *            or flags in the future.
         *         <0 on error
         */
        int rc;
        ENTRY;

        /* don't use EXP_CHECK_OP, because NULL method is normal here */
        if (obd == NULL || !OBT(obd)) {
                CERROR("cleaned up obd\n");
                RETURN(-EOPNOTSUPP);
        }
        if (!obd->obd_set_up || obd->obd_stopping)
                RETURN(0);
        if (!OBP(obd, health_check))
                RETURN(0);

        rc = OBP(obd, health_check)(obd);
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

/* metadata helpers */
static inline int md_getstatus(struct obd_export *exp, struct lu_fid *fid)
{
        int rc;
        ENTRY;

        EXP_CHECK_MD_OP(exp, getstatus);
        MD_COUNTER_INCREMENT(exp->exp_obd, getstatus);
        rc = MDP(exp->exp_obd, getstatus)(exp, fid);
        RETURN(rc);
}

static inline int md_getattr(struct obd_export *exp, struct lu_fid *fid,
                             obd_valid valid, int ea_size,
                             struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, getattr);
        MD_COUNTER_INCREMENT(exp->exp_obd, getattr);
        rc = MDP(exp->exp_obd, getattr)(exp, fid, valid,
                                        ea_size, request);
        RETURN(rc);
}

static inline int md_change_cbdata(struct obd_export *exp, struct lu_fid *fid,
                                   ldlm_iterator_t it, void *data)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, change_cbdata);
        MD_COUNTER_INCREMENT(exp->exp_obd, change_cbdata);
        rc = MDP(exp->exp_obd, change_cbdata)(exp, fid, it, data);
        RETURN(rc);
}

static inline int md_close(struct obd_export *exp,
                           struct md_op_data *op_data,
                           struct obd_client_handle *och,
                           struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, close);
        MD_COUNTER_INCREMENT(exp->exp_obd, close);
        rc = MDP(exp->exp_obd, close)(exp, op_data, och, request);
        RETURN(rc);
}

static inline int md_create(struct obd_export *exp, struct md_op_data *op_data,
                            const void *data, int datalen, int mode,
                            __u32 uid, __u32 gid, __u32 cap_effective, __u64 rdev,
                            struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, create);
        MD_COUNTER_INCREMENT(exp->exp_obd, create);
        rc = MDP(exp->exp_obd, create)(exp, op_data, data, datalen, mode,
                                       uid, gid, cap_effective, rdev, request);
        RETURN(rc);
}

static inline int md_done_writing(struct obd_export *exp,
                                  struct md_op_data *op_data)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, done_writing);
        MD_COUNTER_INCREMENT(exp->exp_obd, done_writing);
        rc = MDP(exp->exp_obd, done_writing)(exp, op_data);
        RETURN(rc);
}

static inline int md_enqueue(struct obd_export *exp, int lock_type,
                             struct lookup_intent *it, int lock_mode,
                             struct md_op_data *op_data,
                             struct lustre_handle *lockh,
                             void *lmm, int lmmsize,
                             ldlm_completion_callback cb_completion,
                             ldlm_blocking_callback cb_blocking,
                             void *cb_data, int extra_lock_flags)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, enqueue);
        MD_COUNTER_INCREMENT(exp->exp_obd, enqueue);
        rc = MDP(exp->exp_obd, enqueue)(exp, lock_type, it, lock_mode,
                                        op_data, lockh, lmm, lmmsize,
                                        cb_completion, cb_blocking,
                                        cb_data, extra_lock_flags);
        RETURN(rc);
}

static inline int md_getattr_name(struct obd_export *exp, struct lu_fid *fid,
                                  const char *filename, int namelen,
                                  obd_valid valid, int ea_size,
                                  struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, getattr_name);
        MD_COUNTER_INCREMENT(exp->exp_obd, getattr_name);
        rc = MDP(exp->exp_obd, getattr_name)(exp, fid, filename, namelen,
                                             valid, ea_size, request);
        RETURN(rc);
}

static inline int md_intent_lock(struct obd_export *exp,
                                 struct md_op_data *op_data,
                                 void *lmm, int lmmsize,
                                 struct lookup_intent *it,
                                 int flags, struct ptlrpc_request **reqp,
                                 ldlm_blocking_callback cb_blocking,
                                 int extra_lock_flags)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, intent_lock);
        MD_COUNTER_INCREMENT(exp->exp_obd, intent_lock);
        rc = MDP(exp->exp_obd, intent_lock)(exp, op_data, lmm, lmmsize,
                                            it, flags, reqp, cb_blocking,
                                            extra_lock_flags);
        RETURN(rc);
}

static inline int md_link(struct obd_export *exp,
                          struct md_op_data *op_data,
                          struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, link);
        MD_COUNTER_INCREMENT(exp->exp_obd, link);
        rc = MDP(exp->exp_obd, link)(exp, op_data, request);
        RETURN(rc);
}

static inline int md_rename(struct obd_export *exp,
                            struct md_op_data *op_data,
                            const char *old, int oldlen,
                            const char *new, int newlen,
                            struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, rename);
        MD_COUNTER_INCREMENT(exp->exp_obd, rename);
        rc = MDP(exp->exp_obd, rename)(exp, op_data, old, oldlen, new,
                                       newlen, request);
        RETURN(rc);
}

static inline int md_setattr(struct obd_export *exp, struct md_op_data *op_data,
                             struct iattr *iattr, void *ea, int ealen,
                             void *ea2, int ea2len, struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, setattr);
        MD_COUNTER_INCREMENT(exp->exp_obd, setattr);
        rc = MDP(exp->exp_obd, setattr)(exp, op_data, iattr, ea, ealen,
                                        ea2, ea2len, request);
        RETURN(rc);
}

static inline int md_sync(struct obd_export *exp, struct lu_fid *fid,
                          struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, sync);
        MD_COUNTER_INCREMENT(exp->exp_obd, sync);
        rc = MDP(exp->exp_obd, sync)(exp, fid, request);
        RETURN(rc);
}

static inline int md_readpage(struct obd_export *exp, struct lu_fid *fid,
                              __u64 offset, struct page *page,
                              struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, readpage);
        MD_COUNTER_INCREMENT(exp->exp_obd, readpage);
        rc = MDP(exp->exp_obd, readpage)(exp, fid, offset, page, request);
        RETURN(rc);
}

static inline int md_unlink(struct obd_export *exp, struct md_op_data *op_data,
                            struct ptlrpc_request **request)
{
        int rc;
        ENTRY;
        EXP_CHECK_MD_OP(exp, unlink);
        MD_COUNTER_INCREMENT(exp->exp_obd, unlink);
        rc = MDP(exp->exp_obd, unlink)(exp, op_data, request);
        RETURN(rc);
}

static inline int md_get_lustre_md(struct obd_export *exp,
                                   struct ptlrpc_request *req,
                                   int offset, struct obd_export *dt_exp,
                                   struct lustre_md *md)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, get_lustre_md);
        MD_COUNTER_INCREMENT(exp->exp_obd, get_lustre_md);
        RETURN(MDP(exp->exp_obd, get_lustre_md)(exp, req, offset,
                                                dt_exp, md));
}

static inline int md_free_lustre_md(struct obd_export *exp,
                                    struct lustre_md *md)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, free_lustre_md);
        MD_COUNTER_INCREMENT(exp->exp_obd, free_lustre_md);
        RETURN(MDP(exp->exp_obd, free_lustre_md)(exp, md));
}

static inline int md_setxattr(struct obd_export *exp, struct lu_fid *fid,
                              obd_valid valid, const char *name,
                              const char *input, int input_size,
                              int output_size, int flags,
                              struct ptlrpc_request **request)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, setxattr);
        MD_COUNTER_INCREMENT(exp->exp_obd, setxattr);
        RETURN(MDP(exp->exp_obd, setxattr)(exp, fid, valid, name, input,
                                           input_size, output_size, flags,
                                           request));
}

static inline int md_getxattr(struct obd_export *exp, struct lu_fid *fid,
                              obd_valid valid, const char *name,
                              const char *input, int input_size,
                              int output_size, int flags,
                              struct ptlrpc_request **request)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, getxattr);
        MD_COUNTER_INCREMENT(exp->exp_obd, getxattr);
        RETURN(MDP(exp->exp_obd, getxattr)(exp, fid, valid, name, input,
                                           input_size, output_size, flags,
                                           request));
}

static inline int md_set_open_replay_data(struct obd_export *exp,
                                          struct obd_client_handle *och,
                                          struct ptlrpc_request *open_req)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, set_open_replay_data);
        MD_COUNTER_INCREMENT(exp->exp_obd, set_open_replay_data);
        RETURN(MDP(exp->exp_obd, set_open_replay_data)(exp, och, open_req));
}

static inline int md_clear_open_replay_data(struct obd_export *exp,
                                            struct obd_client_handle *och)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, clear_open_replay_data);
        MD_COUNTER_INCREMENT(exp->exp_obd, clear_open_replay_data);
        RETURN(MDP(exp->exp_obd, clear_open_replay_data)(exp, och));
}

static inline int md_set_lock_data(struct obd_export *exp,
                                   __u64 *lockh, void *data)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, set_lock_data);
        MD_COUNTER_INCREMENT(exp->exp_obd, set_lock_data);
        RETURN(MDP(exp->exp_obd, set_lock_data)(exp, lockh, data));
}

static inline int md_cancel_unused(struct obd_export *exp,
                                   struct lu_fid *fid,
                                   int flags, void *opaque)
{
        int rc;
        ENTRY;

        EXP_CHECK_MD_OP(exp, cancel_unused);
        MD_COUNTER_INCREMENT(exp->exp_obd, cancel_unused);

        rc = MDP(exp->exp_obd, cancel_unused)(exp, fid, flags, opaque);
        RETURN(rc);
}

static inline int md_lock_match(struct obd_export *exp, int flags,
                                struct lu_fid *fid, ldlm_type_t type,
                                ldlm_policy_data_t *policy, ldlm_mode_t mode,
                                struct lustre_handle *lockh)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, lock_match);
        MD_COUNTER_INCREMENT(exp->exp_obd, lock_match);
        RETURN(MDP(exp->exp_obd, lock_match)(exp, flags, fid, type,
                                             policy, mode, lockh));
}

static inline int md_init_ea_size(struct obd_export *exp,
                                  int easize, int def_asize,
                                  int cookiesize)
{
        ENTRY;
        EXP_CHECK_MD_OP(exp, init_ea_size);
        MD_COUNTER_INCREMENT(exp->exp_obd, init_ea_size);
        RETURN(MDP(exp->exp_obd, init_ea_size)(exp, easize,
                                               def_asize,
                                               cookiesize));
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

static inline void obdo_free(struct obdo *oa)
{
        OBD_SLAB_FREE(oa, obdo_cachep, sizeof(*oa));
}

static inline void obdo2fid(struct obdo *oa,
                            struct lu_fid *fid)
{
        /* something here */
}

static inline void fid2obdo(struct lu_fid *fid,
                            struct obdo *oa)
{
        /* something here */
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

/* sysctl.c */
extern void obd_sysctl_init (void);
extern void obd_sysctl_clean (void);

/* uuid.c  */
typedef __u8 class_uuid_t[16];
void class_generate_random_uuid(class_uuid_t uuid);
void class_uuid_unparse(class_uuid_t in, struct obd_uuid *out);

/* lustre_peer.c    */
int lustre_uuid_to_peer(const char *uuid, lnet_nid_t *peer_nid, int index);
int class_add_uuid(const char *uuid, __u64 nid);
int class_del_uuid (const char *uuid);
void class_init_uuidlist(void);
void class_exit_uuidlist(void);

/* mea.c */
int mea_name2idx(struct lmv_stripe_md *mea, char *name, int namelen);
int raw_name2idx(int hashtype, int count, const char *name, int namelen);

#endif /* __LINUX_OBD_CLASS_H */
