/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2004-2005 Cluster File Systems, Inc.
 *   Author: jacob berkman  <jacob@clusterfs.com>
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
 * Swabbing of llog datatypes (from disk or over the wire).
 *
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef __KERNEL__
#include <liblustre.h>
#endif

#include <linux/lustre_log.h>

static void print_llogd_body(struct llogd_body *d)
{
        CDEBUG(D_OTHER, "llogd body: %p\n", d);
        CDEBUG(D_OTHER, "\tlgd_logid.lgl_oid: "LPX64"\n", d->lgd_logid.lgl_oid);
        CDEBUG(D_OTHER, "\tlgd_logid.lgl_ogr: "LPX64"\n", d->lgd_logid.lgl_ogr);
        CDEBUG(D_OTHER, "\tlgd_logid.lgl_ogen: %#x\n", d->lgd_logid.lgl_ogen);
        CDEBUG(D_OTHER, "\tlgd_ctxt_idx: %#x\n", d->lgd_ctxt_idx);
        CDEBUG(D_OTHER, "\tlgd_llh_flags: %#x\n", d->lgd_llh_flags);
        CDEBUG(D_OTHER, "\tlgd_index: %#x\n", d->lgd_index);
        CDEBUG(D_OTHER, "\tlgd_saved_index: %#x\n", d->lgd_saved_index);
        CDEBUG(D_OTHER, "\tlgd_len: %#x\n", d->lgd_len);
        CDEBUG(D_OTHER, "\tlgd_cur_offset: "LPX64"\n", d->lgd_cur_offset);
}

void lustre_swab_llogd_body (struct llogd_body *d)
{
        ENTRY;
        print_llogd_body(d);
        __swab64s (&d->lgd_logid.lgl_oid);
        __swab64s (&d->lgd_logid.lgl_ogr);
        __swab32s (&d->lgd_logid.lgl_ogen);
        __swab32s (&d->lgd_ctxt_idx);
        __swab32s (&d->lgd_llh_flags);
        __swab32s (&d->lgd_index);
        __swab32s (&d->lgd_saved_index);
        __swab32s (&d->lgd_len);
        __swab64s (&d->lgd_cur_offset);
        print_llogd_body(d);
        EXIT;
}
EXPORT_SYMBOL(lustre_swab_llogd_body);

void lustre_swab_llogd_conn_body (struct llogd_conn_body *d)
{
        __swab64s (&d->lgdc_gen.mnt_cnt);
        __swab64s (&d->lgdc_gen.conn_cnt);
        __swab64s (&d->lgdc_logid.lgl_oid);
        __swab64s (&d->lgdc_logid.lgl_ogr);
        __swab32s (&d->lgdc_logid.lgl_ogen);
        __swab32s (&d->lgdc_ctxt_idx);
}
EXPORT_SYMBOL(lustre_swab_llogd_conn_body);

void lustre_swab_ll_fid (struct ll_fid *fid)
{
        __swab64s (&fid->id);
        __swab32s (&fid->generation);
        __swab32s (&fid->f_type);
}
EXPORT_SYMBOL(lustre_swab_ll_fid);

void lustre_swab_llog_rec(struct llog_rec_hdr *rec, struct llog_rec_tail *tail)
{
        __swab32s(&rec->lrh_len);
        __swab32s(&rec->lrh_index);
        __swab32s(&rec->lrh_type);

        switch (rec->lrh_type) {
        case OST_SZ_REC: {
                struct llog_size_change_rec *lsc =
                        (struct llog_size_change_rec *)rec;

                lustre_swab_ll_fid(&lsc->lsc_fid);
                __swab32s(&lsc->lsc_io_epoch);

                break;
        }

        case OST_RAID1_REC:
                break;

        case MDS_UNLINK_REC: {
                struct llog_unlink_rec *lur = (struct llog_unlink_rec *)rec;

                __swab64s(&lur->lur_oid);
                __swab32s(&lur->lur_ogen);

                break;
        }

        case MDS_SETATTR_REC: {
                struct llog_setattr_rec *lsr = (struct llog_setattr_rec *)rec;

                __swab64s(&lsr->lsr_oid);
                __swab32s(&lsr->lsr_ogen);
                __swab32s(&lsr->lsr_uid);
                __swab32s(&lsr->lsr_gid);

                break;
        }

        case OBD_CFG_REC:
        case PTL_CFG_REC:                       /* obsolete */
                /* these are swabbed as they are consumed */
                break;

        case LLOG_HDR_MAGIC: {
                struct llog_log_hdr *llh = (struct llog_log_hdr *)rec;

                __swab64s(&llh->llh_timestamp);
                __swab32s(&llh->llh_count);
                __swab32s(&llh->llh_bitmap_offset);
                __swab32s(&llh->llh_flags);
                __swab32s(&llh->llh_size);
                __swab32s(&llh->llh_cat_idx);
                if (tail != &llh->llh_tail) {
                        __swab32s(&llh->llh_tail.lrt_index);
                        __swab32s(&llh->llh_tail.lrt_len);
                }

                break;
        }

        case LLOG_LOGID_MAGIC: {
                struct llog_logid_rec *lid = (struct llog_logid_rec *)rec;

                __swab64s(&lid->lid_id.lgl_oid);
                __swab64s(&lid->lid_id.lgl_ogr);
                __swab32s(&lid->lid_id.lgl_ogen);
                break;
        }

        case LLOG_PAD_MAGIC:
        /* ignore old pad records of type 0 */
        case 0:
                break;

        default:
                CERROR("Unknown llog rec type %#x swabbing rec %p\n",
                       rec->lrh_type, rec);
        }

        if (tail) {
                __swab32s(&tail->lrt_len);
                __swab32s(&tail->lrt_index);
        }
}
EXPORT_SYMBOL(lustre_swab_llog_rec);

static void print_llog_hdr(struct llog_log_hdr *h)
{
        CDEBUG(D_OTHER, "llog header: %p\n", h);
        CDEBUG(D_OTHER, "\tllh_hdr.lrh_index: %#x\n", h->llh_hdr.lrh_index);
        CDEBUG(D_OTHER, "\tllh_hdr.lrh_len: %#x\n", h->llh_hdr.lrh_len);
        CDEBUG(D_OTHER, "\tllh_hdr.lrh_type: %#x\n", h->llh_hdr.lrh_type);
        CDEBUG(D_OTHER, "\tllh_timestamp: "LPX64"\n", h->llh_timestamp);
        CDEBUG(D_OTHER, "\tllh_count: %#x\n", h->llh_count);
        CDEBUG(D_OTHER, "\tllh_bitmap_offset: %#x\n", h->llh_bitmap_offset);
        CDEBUG(D_OTHER, "\tllh_flags: %#x\n", h->llh_flags);
        CDEBUG(D_OTHER, "\tllh_size: %#x\n", h->llh_size);
        CDEBUG(D_OTHER, "\tllh_cat_idx: %#x\n", h->llh_cat_idx);
        CDEBUG(D_OTHER, "\tllh_tail.lrt_index: %#x\n", h->llh_tail.lrt_index);
        CDEBUG(D_OTHER, "\tllh_tail.lrt_len: %#x\n", h->llh_tail.lrt_len);
}

void lustre_swab_llog_hdr (struct llog_log_hdr *h)
{
        ENTRY;
        print_llog_hdr(h);

        lustre_swab_llog_rec(&h->llh_hdr, &h->llh_tail);

        print_llog_hdr(h);
        EXIT;
}
EXPORT_SYMBOL(lustre_swab_llog_hdr);

static void print_lustre_cfg(struct lustre_cfg *lcfg)
{
        int i;
        ENTRY;

        if (!(libcfs_debug & D_OTHER)) /* don't loop on nothing */
                return;
        CDEBUG(D_OTHER, "lustre_cfg: %p\n", lcfg);
        CDEBUG(D_OTHER, "\tlcfg->lcfg_version: %#x\n", lcfg->lcfg_version);

        CDEBUG(D_OTHER, "\tlcfg->lcfg_command: %#x\n", lcfg->lcfg_command);
        CDEBUG(D_OTHER, "\tlcfg->lcfg_num: %#x\n", lcfg->lcfg_num);
        CDEBUG(D_OTHER, "\tlcfg->lcfg_flags: %#x\n", lcfg->lcfg_flags);
        CDEBUG(D_OTHER, "\tlcfg->lcfg_nid: %s\n", libcfs_nid2str(lcfg->lcfg_nid));

        CDEBUG(D_OTHER, "\tlcfg->lcfg_bufcount: %d\n", lcfg->lcfg_bufcount);
        if (lcfg->lcfg_bufcount < LUSTRE_CFG_MAX_BUFCOUNT)
                for (i = 0; i < lcfg->lcfg_bufcount; i++)
                        CDEBUG(D_OTHER, "\tlcfg->lcfg_buflens[%d]: %d\n",
                               i, lcfg->lcfg_buflens[i]);
        EXIT;
}

void lustre_swab_lustre_cfg(struct lustre_cfg *lcfg)
{
        int i;
        ENTRY;

        __swab32s(&lcfg->lcfg_version);

        if (lcfg->lcfg_version != LUSTRE_CFG_VERSION) {
                CERROR("not swabbing lustre_cfg version %#x (expecting %#x)\n",
                       lcfg->lcfg_version, LUSTRE_CFG_VERSION);
                EXIT;
                return;
        }

        __swab32s(&lcfg->lcfg_command);

        __swab32s(&lcfg->lcfg_num);
        __swab32s(&lcfg->lcfg_flags);
        __swab64s(&lcfg->lcfg_nid);

        __swab32s(&lcfg->lcfg_bufcount);
        for (i = 0; i < lcfg->lcfg_bufcount && i < LUSTRE_CFG_MAX_BUFCOUNT; i++)
                __swab32s(&lcfg->lcfg_buflens[i]);

        print_lustre_cfg(lcfg);
        EXIT;
        return;
}
EXPORT_SYMBOL(lustre_swab_lustre_cfg);
