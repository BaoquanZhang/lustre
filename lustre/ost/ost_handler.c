/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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
 *  Storage Target Handling functions
 *  Lustre Object Server Module (OST)
 *
 *  This server is single threaded at present (but can easily be multi
 *  threaded). For testing and management it is treated as an
 *  obd_device, although it does not export a full OBD method table
 *  (the requests are coming in over the wire, so object target
 *  modules do not have a full method table.)
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_OST

#include <linux/module.h>
#include <linux/obd_ost.h>
#include <linux/lustre_net.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_export.h>
#include <linux/init.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_commit_confd.h>
#include <libcfs/list.h>
#include <linux/lustre_sec.h>
#include <linux/lustre_audit.h>

void oti_init(struct obd_trans_info *oti, struct ptlrpc_request *req)
{
        if (oti == NULL)
                return;
        memset(oti, 0, sizeof *oti);

        if (req->rq_repmsg && req->rq_reqmsg != 0)
                oti->oti_transno = req->rq_repmsg->transno;
}

void oti_to_request(struct obd_trans_info *oti, struct ptlrpc_request *req)
{
        struct oti_req_ack_lock *ack_lock;
        int i;

        if (oti == NULL)
                return;

        if (req->rq_repmsg)
                req->rq_repmsg->transno = oti->oti_transno;

        /* XXX 4 == entries in oti_ack_locks??? */
        for (ack_lock = oti->oti_ack_locks, i = 0; i < 4; i++, ack_lock++) {
                if (!ack_lock->mode)
                        break;
                /* XXX not even calling target_send_reply in some cases... */
                ptlrpc_save_lock (req, &ack_lock->lock, ack_lock->mode);
        }
}

static int ost_destroy(struct obd_export *exp, struct ptlrpc_request *req, 
                       struct obd_trans_info *oti)
{
        struct ost_body *body, *repbody;
        int rc, size = sizeof(*body);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        if (body->oa.o_valid & OBD_MD_FLCOOKIE)
                oti->oti_logcookies = obdo_logcookie(&body->oa);
        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));
        req->rq_status = obd_destroy(exp, &body->oa, NULL, oti);
        RETURN(0);
}

static int ost_getattr(struct obd_export *exp, struct ptlrpc_request *req)
{
        struct ost_body *body, *repbody;
        int rc, size = sizeof(*body);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        repbody = lustre_msg_buf (req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));
        req->rq_status = obd_getattr(exp, &repbody->oa, NULL);
        RETURN(0);
}

static int ost_statfs(struct ptlrpc_request *req)
{
        struct obd_statfs *osfs;
        int rc, size = sizeof(*osfs);
        ENTRY;

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        osfs = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*osfs));

        req->rq_status = obd_statfs(req->rq_export->exp_obd, osfs, jiffies-HZ);
        if (req->rq_status != 0)
                CERROR("ost: statfs failed: rc %d\n", req->rq_status);

        RETURN(0);
}

static int ost_create(struct obd_export *exp, struct ptlrpc_request *req,
                      struct obd_trans_info *oti)
{
        struct ost_body *body, *repbody;
        int rc, size = sizeof(*repbody);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        repbody = lustre_msg_buf (req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));
        oti->oti_logcookies = obdo_logcookie(&repbody->oa);
        req->rq_status = obd_create(exp, &repbody->oa, NULL, 0, NULL, oti);
        //obd_log_cancel(conn, NULL, 1, oti->oti_logcookies, 0);
        RETURN(0);
}

static int ost_punch(struct obd_export *exp, struct ptlrpc_request *req, 
                     struct obd_trans_info *oti)
{
        struct ost_body *body, *repbody;
        int rc, size = sizeof(*repbody);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        if ((body->oa.o_valid & (OBD_MD_FLSIZE | OBD_MD_FLBLOCKS)) !=
            (OBD_MD_FLSIZE | OBD_MD_FLBLOCKS))
                RETURN(-EINVAL);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));
        req->rq_status = obd_punch(exp, &repbody->oa, NULL, repbody->oa.o_size,
                                   repbody->oa.o_blocks, oti);
        RETURN(0);
}

static int ost_sync(struct obd_export *exp, struct ptlrpc_request *req)
{
        struct ost_body *body, *repbody;
        int rc, size = sizeof(*repbody);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));
        req->rq_status = obd_sync(exp, &repbody->oa, NULL, repbody->oa.o_size,
                                  repbody->oa.o_blocks);
        RETURN(0);
}

static int ost_setattr(struct obd_export *exp, struct ptlrpc_request *req, 
                       struct obd_trans_info *oti)
{
        struct ost_body *body, *repbody;
        int rc, size = sizeof(*repbody);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));

        req->rq_status = obd_setattr(exp, &repbody->oa, NULL, oti);
        RETURN(0);
}

static int ost_bulk_timeout(void *data)
{
        ENTRY;
        /* We don't fail the connection here, because having the export
         * killed makes the (vital) call to commitrw very sad.
         */
        RETURN(1);
}

static int get_per_page_niobufs(struct obd_ioobj *ioo, int nioo,
                                struct niobuf_remote *rnb, int nrnb,
                                struct niobuf_remote **pp_rnbp)
{
        /* Copy a remote niobuf, splitting it into page-sized chunks
         * and setting ioo[i].ioo_bufcnt accordingly */
        struct niobuf_remote *pp_rnb;
        int   i;
        int   j;
        int   page;
        int   rnbidx = 0;
        int   npages = 0;

        /* first count and check the number of pages required */
        for (i = 0; i < nioo; i++)
                for (j = 0; j < ioo->ioo_bufcnt; j++, rnbidx++) {
                        obd_off offset = rnb[rnbidx].offset;
                        obd_off p0 = offset >> PAGE_SHIFT;
                        obd_off pn = (offset + rnb[rnbidx].len - 1)>>PAGE_SHIFT;

                        LASSERT(rnbidx < nrnb);

                        npages += (pn + 1 - p0);

                        if (rnb[rnbidx].len == 0) {
                                CERROR("zero len BRW: obj %d objid "LPX64
                                       " buf %u\n", i, ioo[i].ioo_id, j);
                                return -EINVAL;
                        }
                        if (j > 0 &&
                            rnb[rnbidx].offset <= rnb[rnbidx-1].offset) {
                                CERROR("unordered BRW: obj %d objid "LPX64
                                       " buf %u offset "LPX64" <= "LPX64"\n",
                                       i, ioo[i].ioo_id, j, rnb[rnbidx].offset,
                                       rnb[rnbidx].offset);
                                return -EINVAL;
                        }
                }

        LASSERT(rnbidx == nrnb);

        if (npages == nrnb) {       /* all niobufs are for single pages */
                *pp_rnbp = rnb;
                return npages;
        }

        OBD_ALLOC(pp_rnb, sizeof(*pp_rnb) * npages);
        if (pp_rnb == NULL)
                return -ENOMEM;

        /* now do the actual split */
        page = rnbidx = 0;
        for (i = 0; i < nioo; i++) {
                int  obj_pages = 0;

                for (j = 0; j < ioo[i].ioo_bufcnt; j++, rnbidx++) {
                        obd_off off = rnb[rnbidx].offset;
                        int     nob = rnb[rnbidx].len;

                        LASSERT(rnbidx < nrnb);
                        do {
                                obd_off  poff = off & (PAGE_SIZE - 1);
                                int      pnob = (poff + nob > PAGE_SIZE) ?
                                                PAGE_SIZE - poff : nob;

                                LASSERT(page < npages);
                                pp_rnb[page].len = pnob;
                                pp_rnb[page].offset = off;
                                pp_rnb[page].flags = rnb[rnbidx].flags;

                                CDEBUG(0, "   obj %d id "LPX64
                                       "page %d(%d) "LPX64" for %d, flg %x\n",
                                       i, ioo[i].ioo_id, obj_pages, page,
                                       pp_rnb[page].offset, pp_rnb[page].len,
                                       pp_rnb[page].flags);
                                page++;
                                obj_pages++;

                                off += pnob;
                                nob -= pnob;
                        } while (nob > 0);
                        LASSERT(nob == 0);
                }
                ioo[i].ioo_bufcnt = obj_pages;
        }
        LASSERT(page == npages);

        *pp_rnbp = pp_rnb;
        return npages;
}

static void free_per_page_niobufs (int npages, struct niobuf_remote *pp_rnb,
                                   struct niobuf_remote *rnb)
{
        if (pp_rnb == rnb)                      /* didn't allocate above */
                return;

        OBD_FREE(pp_rnb, sizeof(*pp_rnb) * npages);
}

#if CHECKSUM_BULK
obd_count ost_checksum_bulk(struct ptlrpc_bulk_desc *desc)
{
        obd_count cksum = 0;
        struct ptlrpc_bulk_page *bp;

        list_for_each_entry(bp, &desc->bd_page_list, bp_link) {
                ost_checksum(&cksum, kmap(bp->bp_page) + bp->bp_pageoffset,
                             bp->bp_buflen);
                kunmap(bp->bp_page);
        }

        return cksum;
}
#endif

static void ost_stime_record(struct ptlrpc_request *req, struct timeval *start,
                             unsigned rw, unsigned phase)
{
        struct obd_device *obd = req->rq_svc->srv_obddev;
        struct timeval stop;
        int ind = rw *3 + phase;
         
        if (obd && obd->obd_type && obd->obd_type->typ_name) {
                if (!strcmp(obd->obd_type->typ_name, OBD_OST_DEVICENAME)) {
                        struct ost_obd *ost = NULL;
                        
                        ost = &obd->u.ost;
                        if (ind >= (sizeof(ost->ost_stimes) / 
                                    sizeof(ost->ost_stimes[0])))
                               return;
                        do_gettimeofday(&stop);

                        spin_lock(&ost->ost_lock);
                        lprocfs_stime_record(&ost->ost_stimes[ind],&stop,start);
                        spin_unlock(&ost->ost_lock);
                        memcpy(start, &stop, sizeof(*start));
                }
       } 
}

static int ost_brw_read(struct ptlrpc_request *req)
{
        struct ptlrpc_bulk_desc *desc;
        struct niobuf_remote    *remote_nb;
        struct niobuf_remote    *pp_rnb;
        struct niobuf_local     *local_nb;
        struct obd_ioobj        *ioo;
        struct ost_body         *body, *repbody;
        struct l_wait_info       lwi;
        struct obd_trans_info    oti = { 0 };
        int                      size[1] = { sizeof(*body) };
        int                      comms_error = 0;
        int                      niocount;
        int                      npages;
        int                      nob = 0;
        int                      rc;
        int                      i;
        struct timeval           start;
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_BRW_READ_BULK))
                GOTO(out, rc = -EIO);

        OBD_FAIL_TIMEOUT(OBD_FAIL_OST_BRW_PAUSE_BULK | OBD_FAIL_ONCE,
                         (obd_timeout + 1) / 4);

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL) {
                CERROR("Missing/short ost_body\n");
                GOTO(out, rc = -EFAULT);
        }

        ioo = lustre_swab_reqbuf(req, 1, sizeof(*ioo), lustre_swab_obd_ioobj);
        if (ioo == NULL) {
                CERROR("Missing/short ioobj\n");
                GOTO(out, rc = -EFAULT);
        }

        niocount = ioo->ioo_bufcnt;
        remote_nb = lustre_swab_reqbuf(req, 2, niocount * sizeof(*remote_nb),
                                       lustre_swab_niobuf_remote);
        if (remote_nb == NULL) {
                CERROR("Missing/short niobuf\n");
                GOTO(out, rc = -EFAULT);
        }
        if (lustre_msg_swabbed(req->rq_reqmsg)) { /* swab remaining niobufs */
                for (i = 1; i < niocount; i++)
                        lustre_swab_niobuf_remote (&remote_nb[i]);
        }

        rc = lustre_pack_reply(req, 1, size, NULL);
        if (rc)
                GOTO(out, rc);

        /* FIXME all niobuf splitting should be done in obdfilter if needed */
        /* CAVEAT EMPTOR this sets ioo->ioo_bufcnt to # pages */
        npages = get_per_page_niobufs(ioo, 1, remote_nb, niocount, &pp_rnb);
        if (npages < 0)
                GOTO(out, rc = npages);

        OBD_ALLOC(local_nb, sizeof(*local_nb) * npages);
        if (local_nb == NULL)
                GOTO(out_pp_rnb, rc = -ENOMEM);

        desc = ptlrpc_prep_bulk_exp (req, npages, 
                                     BULK_PUT_SOURCE, OST_BULK_PORTAL);
        if (desc == NULL)
                GOTO(out_local, rc = -ENOMEM);

        do_gettimeofday(&start);
        rc = obd_preprw(OBD_BRW_READ, req->rq_export, &body->oa, 1,
                        ioo, npages, pp_rnb, local_nb, &oti);
        ost_stime_record(req, &start, 0, 0);
        if (rc != 0)
                GOTO(out_bulk, rc);

        /* We're finishing using body->oa as an input variable */
        body->oa.o_valid = 0;

        nob = 0;
        for (i = 0; i < npages; i++) {
                int page_rc = local_nb[i].rc;

                if (page_rc < 0) {              /* error */
                        rc = page_rc;
                        break;
                }

                LASSERT(page_rc <= pp_rnb[i].len);
                nob += page_rc;
                if (page_rc != 0) {             /* some data! */
                        LASSERT (local_nb[i].page != NULL);
                        ptlrpc_prep_bulk_page(desc, local_nb[i].page,
                                              pp_rnb[i].offset & (PAGE_SIZE-1),
                                              page_rc);
                }

                if (page_rc != pp_rnb[i].len) { /* short read */
                        /* All subsequent pages should be 0 */
                        while(++i < npages)
                                LASSERT(local_nb[i].rc == 0);
                        break;
                }
        }

        if (rc == 0) {
                rc = ptlrpc_start_bulk_transfer(desc);
                if (rc == 0) {
                        lwi = LWI_TIMEOUT(obd_timeout * HZ / 4,
                                          ost_bulk_timeout, desc);
                        rc = l_wait_event(desc->bd_waitq,
                                          !ptlrpc_bulk_active(desc), &lwi);
                        LASSERT(rc == 0 || rc == -ETIMEDOUT);
                        if (rc == -ETIMEDOUT) {
                                DEBUG_REQ(D_ERROR, req, "timeout on bulk PUT");
                                ptlrpc_abort_bulk(desc);
                        } else if (!desc->bd_success ||
                                   desc->bd_nob_transferred != desc->bd_nob) {
                                DEBUG_REQ(D_ERROR, req, "%s bulk PUT %d(%d)",
                                          desc->bd_success ?
                                          "truncated" : "network error on",
                                          desc->bd_nob_transferred,
                                          desc->bd_nob);
                                /* XXX should this be a different errno? */
                                rc = -ETIMEDOUT;
                        }
                } else {
                        DEBUG_REQ(D_ERROR, req, "bulk PUT failed: rc %d\n", rc);
                }
                comms_error = rc != 0;
        }

        ost_stime_record(req, &start, 0, 1);
        /* Must commit after prep above in all cases */
        rc = obd_commitrw(OBD_BRW_READ, req->rq_export, &body->oa, 1,
                          ioo, npages, local_nb, &oti, rc);
        ost_stime_record(req, &start, 0, 2);

        if (rc == 0) {
                repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
                memcpy(&repbody->oa, &body->oa, sizeof(repbody->oa));

#if CHECKSUM_BULK
                repbody->oa.o_cksum = ost_checksum_bulk(desc);
                repbody->oa.o_valid |= OBD_MD_FLCKSUM;
#endif
        }

 out_bulk:
        ptlrpc_free_bulk(desc);
 out_local:
        OBD_FREE(local_nb, sizeof(*local_nb) * npages);
 out_pp_rnb:
        free_per_page_niobufs(npages, pp_rnb, remote_nb);
 out:
        LASSERT(rc <= 0);
        if (rc == 0) {
                req->rq_status = nob;
                ptlrpc_reply(req);
        } else if (!comms_error) {
                /* only reply if comms OK */
                req->rq_status = rc;
                ptlrpc_error(req);
        } else {
                if (req->rq_reply_state != NULL) {
                        /* reply out callback would free */
                        lustre_free_reply_state (req->rq_reply_state);
                }
                if (req->rq_reqmsg->conn_cnt == req->rq_export->exp_conn_cnt) {
                        CERROR("bulk IO comms error: "
                               "evicting %s@%s id %s\n",
                               req->rq_export->exp_client_uuid.uuid,
                               req->rq_export->exp_connection->c_remote_uuid.uuid,
                               req->rq_peerstr);
                        ptlrpc_fail_export(req->rq_export);
                } else {
                        CERROR("ignoring bulk IO comms error: "
                               "client reconnected %s@%s id %s\n",  
                               req->rq_export->exp_client_uuid.uuid,
                               req->rq_export->exp_connection->c_remote_uuid.uuid,
                               req->rq_peerstr);
                }
        }

        RETURN(rc);
}

int ost_brw_write(struct ptlrpc_request *req, struct obd_trans_info *oti)
{
        struct ptlrpc_bulk_desc *desc;
        struct niobuf_remote    *remote_nb;
        struct niobuf_remote    *pp_rnb;
        struct niobuf_local     *local_nb;
        struct obd_ioobj        *ioo;
        struct ost_body         *body, *repbody;
        struct l_wait_info       lwi;
        __u32                   *rcs;
        int                      size[2] = { sizeof(*body) };
        int                      objcount, niocount, npages;
        int                      comms_error = 0;
        int                      rc, swab, i, j;
        struct timeval           start;        
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_BRW_WRITE_BULK))
                GOTO(out, rc = -EIO);

        /* pause before transaction has been started */
        OBD_FAIL_TIMEOUT(OBD_FAIL_OST_BRW_PAUSE_BULK | OBD_FAIL_ONCE,
                         (obd_timeout + 1) / 4);

        swab = lustre_msg_swabbed(req->rq_reqmsg);
        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL) {
                CERROR("Missing/short ost_body\n");
                GOTO(out, rc = -EFAULT);
        }

        LASSERT_REQSWAB(req, 1);
        objcount = req->rq_reqmsg->buflens[1] / sizeof(*ioo);
        if (objcount == 0) {
                CERROR("Missing/short ioobj\n");
                GOTO(out, rc = -EFAULT);
        }
        ioo = lustre_msg_buf (req->rq_reqmsg, 1, objcount * sizeof(*ioo));
        LASSERT (ioo != NULL);
        for (niocount = i = 0; i < objcount; i++) {
                if (swab)
                        lustre_swab_obd_ioobj (&ioo[i]);
                if (ioo[i].ioo_bufcnt == 0) {
                        CERROR("ioo[%d] has zero bufcnt\n", i);
                        GOTO(out, rc = -EFAULT);
                }
                niocount += ioo[i].ioo_bufcnt;
        }

        remote_nb = lustre_swab_reqbuf(req, 2, niocount * sizeof(*remote_nb),
                                       lustre_swab_niobuf_remote);
        if (remote_nb == NULL) {
                CERROR("Missing/short niobuf\n");
                GOTO(out, rc = -EFAULT);
        }
        if (swab) {                             /* swab the remaining niobufs */
                for (i = 1; i < niocount; i++)
                        lustre_swab_niobuf_remote (&remote_nb[i]);
        }

        size[1] = niocount * sizeof(*rcs);
        rc = lustre_pack_reply(req, 2, size, NULL);
        if (rc != 0)
                GOTO(out, rc);
        rcs = lustre_msg_buf(req->rq_repmsg, 1, niocount * sizeof(*rcs));

#if 0
        /* Do snap options here*/
        rc = obd_do_cow(req->rq_export, ioo, objcount, remote_nb);
        if (rc)
                GOTO(out, rc);
#endif

        /* FIXME all niobuf splitting should be done in obdfilter if needed */
        /* CAVEAT EMPTOR this sets ioo->ioo_bufcnt to # pages */
        npages = get_per_page_niobufs(ioo, objcount,remote_nb,niocount,&pp_rnb);
        if (npages < 0)
                GOTO(out, rc = npages);

        OBD_ALLOC(local_nb, sizeof(*local_nb) * npages);
        if (local_nb == NULL)
                GOTO(out_pp_rnb, rc = -ENOMEM);

        desc = ptlrpc_prep_bulk_exp (req, npages, 
                                     BULK_GET_SINK, OST_BULK_PORTAL);
        if (desc == NULL)
                GOTO(out_local, rc = -ENOMEM);

        do_gettimeofday(&start);
        rc = obd_preprw(OBD_BRW_WRITE, req->rq_export, &body->oa, objcount,
                        ioo, npages, pp_rnb, local_nb, oti);
        ost_stime_record(req, &start, 1, 0);
        if (rc != 0)
                GOTO(out_bulk, rc);

        /* NB Having prepped, we must commit... */

        for (i = 0; i < npages; i++)
                ptlrpc_prep_bulk_page(desc, local_nb[i].page, 
                                      pp_rnb[i].offset & (PAGE_SIZE - 1),
                                      pp_rnb[i].len);

        rc = ptlrpc_start_bulk_transfer (desc);
        if (rc == 0) {
                lwi = LWI_TIMEOUT(obd_timeout * HZ / 4,
                                  ost_bulk_timeout, desc);
                rc = l_wait_event(desc->bd_waitq, !ptlrpc_bulk_active(desc), 
                                  &lwi);
                LASSERT(rc == 0 || rc == -ETIMEDOUT);
                if (rc == -ETIMEDOUT) {
                        DEBUG_REQ(D_ERROR, req, "timeout on bulk GET");
                        ptlrpc_abort_bulk(desc);
                } else if (!desc->bd_success ||
                           desc->bd_nob_transferred != desc->bd_nob) {
                        DEBUG_REQ(D_ERROR, req, "%s bulk GET %d(%d)",
                                  desc->bd_success ? 
                                  "truncated" : "network error on",
                                  desc->bd_nob_transferred, desc->bd_nob);
                        /* XXX should this be a different errno? */
                        rc = -ETIMEDOUT;
                }
        } else {
                DEBUG_REQ(D_ERROR, req, "ptlrpc_bulk_get failed: rc %d\n", rc);
        }
        comms_error = rc != 0;

        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(repbody->oa));

#if CHECKSUM_BULK
        if (rc == 0 && (body->oa.o_valid & OBD_MD_FLCKSUM) != 0) {
                static int cksum_counter;
                obd_count client_cksum = body->oa.o_cksum;
                obd_count cksum = ost_checksum_bulk(desc);

                if (client_cksum != cksum) {
                        CERROR("Bad checksum: client %x, server %x id %s\n",
                               client_cksum, cksum,
                               req->rq_peerstr);
                        cksum_counter = 1;
                        repbody->oa.o_cksum = cksum;
                } else {
                        cksum_counter++;
                        if ((cksum_counter & (-cksum_counter)) == cksum_counter)
                                CWARN("Checksum %u from NID %s: %x OK\n",         
                                      cksum_counter, req->rq_peerstr, cksum);
                }
        }
#endif
        ost_stime_record(req, &start, 1, 1);
        /* Must commit after prep above in all cases */
        rc = obd_commitrw(OBD_BRW_WRITE, req->rq_export, &repbody->oa,
                          objcount, ioo, npages, local_nb, oti, rc);

        ost_stime_record(req, &start, 1, 2);
        if (rc == 0) {
#if CHECKSUM_BULK
                repbody->oa.o_cksum = ost_checksum_bulk(desc);
                repbody->oa.o_valid |= OBD_MD_FLCKSUM;
#endif
                /* set per-requested niobuf return codes */
                for (i = j = 0; i < niocount; i++) {
                        int nob = remote_nb[i].len;

                        rcs[i] = 0;
                        do {
                                LASSERT(j < npages);
                                if (local_nb[j].rc < 0)
                                        rcs[i] = local_nb[j].rc;
                                nob -= pp_rnb[j].len;
                                j++;
                        } while (nob > 0);
                        LASSERT(nob == 0);
                }
                LASSERT(j == npages);
        }
        /*XXX This write extents only for write-back cache extents*/
        rc = obd_write_extents(req->rq_export, ioo, objcount, niocount, 
                               local_nb, rc);
 out_bulk:
        ptlrpc_free_bulk(desc);
 out_local:
        OBD_FREE(local_nb, sizeof(*local_nb) * npages);
 out_pp_rnb:
        free_per_page_niobufs(npages, pp_rnb, remote_nb);
 out:
        if (rc == 0) {
                oti_to_request(oti, req);
                rc = ptlrpc_reply(req);
        } else if (!comms_error) {
                /* Only reply if there was no comms problem with bulk */
                req->rq_status = rc;
                ptlrpc_error(req);
        } else {
                if (req->rq_reply_state != NULL) {
                        /* reply out callback would free */
                        lustre_free_reply_state (req->rq_reply_state);
                }
                if (req->rq_reqmsg->conn_cnt == req->rq_export->exp_conn_cnt) {
                        CERROR("%s: bulk IO comm error evicting %s@%s id %s\n",
                               req->rq_export->exp_obd->obd_name,
                               req->rq_export->exp_client_uuid.uuid,
                               req->rq_export->exp_connection->c_remote_uuid.uuid,
                               req->rq_peerstr);
                        ptlrpc_fail_export(req->rq_export);
                } else {
                        CERROR("ignoring bulk IO comms error: "
                               "client reconnected %s@%s id %s\n",
                               req->rq_export->exp_client_uuid.uuid,
                               req->rq_export->exp_connection->c_remote_uuid.uuid,
                               req->rq_peerstr);
                }
        }
        RETURN(rc);
}
EXPORT_SYMBOL(ost_brw_write);

static int ost_san_brw(struct ptlrpc_request *req, int cmd)
{
        struct niobuf_remote *remote_nb, *res_nb, *pp_rnb;
        struct obd_ioobj *ioo;
        struct ost_body *body, *repbody;
        int rc, i, objcount, niocount, size[2] = {sizeof(*body)}, npages;
        int swab;
        ENTRY;

        /* XXX not set to use latest protocol */

        swab = lustre_msg_swabbed(req->rq_reqmsg);
        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL) {
                CERROR("Missing/short ost_body\n");
                GOTO(out, rc = -EFAULT);
        }

        ioo = lustre_swab_reqbuf(req, 1, sizeof(*ioo), lustre_swab_obd_ioobj);
        if (ioo == NULL) {
                CERROR("Missing/short ioobj\n");
                GOTO(out, rc = -EFAULT);
        }
        objcount = req->rq_reqmsg->buflens[1] / sizeof(*ioo);
        niocount = ioo[0].ioo_bufcnt;
        for (i = 1; i < objcount; i++) {
                if (swab)
                        lustre_swab_obd_ioobj (&ioo[i]);
                niocount += ioo[i].ioo_bufcnt;
        }

        remote_nb = lustre_swab_reqbuf(req, 2, niocount * sizeof(*remote_nb),
                                       lustre_swab_niobuf_remote);
        if (remote_nb == NULL) {
                CERROR("Missing/short niobuf\n");
                GOTO(out, rc = -EFAULT);
        }
        if (swab) {                             /* swab the remaining niobufs */
                for (i = 1; i < niocount; i++)
                        lustre_swab_niobuf_remote (&remote_nb[i]);
        }

        /* CAVEAT EMPTOR this sets ioo->ioo_bufcnt to # pages */
        npages = get_per_page_niobufs(ioo, objcount,remote_nb,niocount,&pp_rnb);
        if (npages < 0)
                GOTO (out, rc = npages);
 
        size[1] = npages * sizeof(*pp_rnb);
        rc = lustre_pack_reply(req, 2, size, NULL);
        if (rc)
                GOTO(out_pp_rnb, rc);

        req->rq_status = obd_san_preprw(cmd, req->rq_export, &body->oa,
                                        objcount, ioo, npages, pp_rnb);

        if (req->rq_status)
                GOTO(out_pp_rnb, rc = 0);

        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));
        memcpy(&repbody->oa, &body->oa, sizeof(body->oa));

        res_nb = lustre_msg_buf(req->rq_repmsg, 1, size[1]);
        memcpy(res_nb, remote_nb, size[1]);
        rc = 0;
out_pp_rnb:
        free_per_page_niobufs(npages, pp_rnb, remote_nb);
out:
        if (rc) {
                req->rq_status = rc;
                ptlrpc_error(req);
        } else
                ptlrpc_reply(req);

        return rc;
}

static int ost_set_info(struct obd_export *exp, struct ptlrpc_request *req)
{
        char *key, *val;
        int keylen, rc = 0;
        ENTRY;

        key = lustre_msg_buf(req->rq_reqmsg, 0, 1);
        if (key == NULL) {
                DEBUG_REQ(D_HA, req, "no set_info key");
                RETURN(-EFAULT);
        }
        keylen = req->rq_reqmsg->buflens[0];

        rc = lustre_pack_reply(req, 0, NULL, NULL);
        if (rc)
                RETURN(rc);

        val = lustre_msg_buf(req->rq_reqmsg, 1, 0);
        
        if (keylen == 8 && memcmp(key, "auditlog", 8) == 0) {
                lustre_swab_reqbuf(req, 1, sizeof(struct audit_msg),
                                   lustre_swab_audit_msg);
        }
        else if (keylen == 5 && strcmp(key, "audit") == 0) {
                lustre_swab_reqbuf(req, 1, sizeof(struct audit_attr_msg),
                                   lustre_swab_audit_attr);
        }
        else if (keylen == 9 && strcmp(key, "audit_obj") == 0) {
                lustre_swab_reqbuf(req, 1, sizeof(struct obdo),
                                   lustre_swab_obdo);
        }

        rc = obd_set_info(exp, keylen, key, req->rq_reqmsg->buflens[1], val);
        req->rq_repmsg->status = 0;
        RETURN(rc);
}

static int ost_get_info(struct obd_export *exp, struct ptlrpc_request *req)
{
        char *key;
        int keylen, rc = 0, size = sizeof(obd_id);
        obd_id *reply;
        ENTRY;

        key = lustre_msg_buf(req->rq_reqmsg, 0, 1);
        if (key == NULL) {
                DEBUG_REQ(D_HA, req, "no get_info key");
                RETURN(-EFAULT);
        }
        keylen = req->rq_reqmsg->buflens[0];

        if (keylen < strlen("last_id") || memcmp(key, "last_id", 7) != 0)
                RETURN(-EPROTO);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        reply = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*reply));
        rc = obd_get_info(exp, keylen, key, (__u32 *)&size, reply);
        req->rq_repmsg->status = 0;
        RETURN(rc);
}

static int ost_llog_handle_connect(struct obd_export *exp,
            			   struct ptlrpc_request *req)
{
        struct llogd_conn_body *body;
        int rc;
        ENTRY;

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        rc = obd_llog_connect(exp, body);
        RETURN(rc);
}

static int ost_filter_recovery_request(struct ptlrpc_request *req,
                                       struct obd_device *obd, int *process)
{
        switch (req->rq_reqmsg->opc) {
        case OST_CONNECT: /* This will never get here, but for completeness. */
        case OST_DISCONNECT:
               *process = 1;
               RETURN(0);

        case OBD_PING:
        case OST_CREATE:
        case OST_DESTROY:
        case OST_PUNCH:
        case OST_SETATTR:
        case OST_SYNC:
        case OST_WRITE:
        case OBD_LOG_CANCEL:
        case LDLM_ENQUEUE:
                *process = target_queue_recovery_request(req, obd);
                RETURN(0);

        default:
                DEBUG_REQ(D_ERROR, req, "not permitted during recovery");
                *process = 0;
                /* XXX what should we set rq_status to here? */
                req->rq_status = -EAGAIN;
                RETURN(ptlrpc_error(req));
        }
}

int ost_msg_check_version(struct lustre_msg *msg)
{
        int rc;

        switch(msg->opc) {
        case OST_CONNECT:
        case OST_DISCONNECT:
        case OBD_PING:
        case OST_CREATE:
        case OST_DESTROY:
        case OST_GETATTR:
        case OST_SETATTR:
        case OST_WRITE:
        case OST_READ:
        case OST_SAN_READ:
        case OST_SAN_WRITE:
        case OST_PUNCH:
        case OST_STATFS:
        case OST_SYNC:
        case OST_SET_INFO:
        case OST_GET_INFO:
                rc = lustre_msg_check_version(msg, LUSTRE_OBD_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_OBD_VERSION);
                break;
        case LDLM_ENQUEUE:
        case LDLM_CONVERT:
        case LDLM_CANCEL:
        case LDLM_BL_CALLBACK:
        case LDLM_CP_CALLBACK:
                rc = lustre_msg_check_version(msg, LUSTRE_DLM_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_DLM_VERSION);
                break;
        case OBD_LOG_CANCEL:
        case LLOG_ORIGIN_CONNECT:
                rc = lustre_msg_check_version(msg, LUSTRE_LOG_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_LOG_VERSION);
                break;
        case SEC_INIT:
        case SEC_INIT_CONTINUE:
        case SEC_FINI:
                rc = 0;
                break;
        default:
                CERROR("OST unexpected opcode %d\n", msg->opc);
                rc = -ENOTSUPP;
                break;
        }
        return rc;
}

int ost_handle(struct ptlrpc_request *req)
{
        int should_process, fail = OBD_FAIL_OST_ALL_REPLY_NET, rc = 0;
        struct obd_trans_info *oti = NULL;
        struct obd_device *obd = NULL;
        ENTRY;

        LASSERT(current->journal_info == NULL);

        rc = ost_msg_check_version(req->rq_reqmsg);
        if (rc) {
                CERROR("OST drop mal-formed request\n");
                RETURN(rc);
        }

        /* Security opc should NOT trigger any recovery events */
        if (req->rq_reqmsg->opc == SEC_INIT ||
            req->rq_reqmsg->opc == SEC_INIT_CONTINUE ||
            req->rq_reqmsg->opc == SEC_FINI) {
                GOTO(out_check_req, rc = 0);
        }

        /* XXX identical to MDS */
        if (req->rq_reqmsg->opc != OST_CONNECT) {
                int recovering;

                if (req->rq_export == NULL) {
                        CDEBUG(D_HA,"operation %d on unconnected OST from %s\n",
                               req->rq_reqmsg->opc,
                               req->rq_peerstr);
                        req->rq_status = -ENOTCONN;
                        GOTO(out_check_req, rc = -ENOTCONN);
                }

                obd = req->rq_export->exp_obd;

                /* Check for aborted recovery. */
                spin_lock_bh(&obd->obd_processing_task_lock);
                recovering = obd->obd_recovering;
                spin_unlock_bh(&obd->obd_processing_task_lock);
		if (recovering) {
                        rc = ost_filter_recovery_request(req, obd,
                                                         &should_process);
                        if (rc || !should_process)
                                RETURN(rc);
                        if (should_process < 0) {
                                req->rq_status = should_process;
                                rc = ptlrpc_error(req);
                                RETURN(rc);
                        }
                }
        }

	OBD_ALLOC(oti, sizeof(*oti));
	if (oti == NULL)
		RETURN(-ENOMEM);
		
        oti_init(oti, req);

        switch (req->rq_reqmsg->opc) {
        case OST_CONNECT: {
                CDEBUG(D_INODE, "connect\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_CONNECT_NET, out_free_oti, rc = 0);
                rc = target_handle_connect(req);
                if (!rc)
                        obd = req->rq_export->exp_obd;
                break;
        }
        case OST_DISCONNECT:
                CDEBUG(D_INODE, "disconnect\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_DISCONNECT_NET, out_free_oti, rc = 0);
                rc = target_handle_disconnect(req);
                break;
        case OST_CREATE:
                CDEBUG(D_INODE, "create\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_ENOSPC, out_check_req, rc = -ENOSPC);
                OBD_FAIL_GOTO(OBD_FAIL_OST_EROFS, out_check_req, rc = -EROFS);
                OBD_FAIL_GOTO(OBD_FAIL_OST_CREATE_NET, out_free_oti, rc = 0);
                rc = ost_create(req->rq_export, req, oti);
                break;
        case OST_DESTROY:
                CDEBUG(D_INODE, "destroy\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_DESTROY_NET, out_free_oti, rc = 0);
                OBD_FAIL_GOTO(OBD_FAIL_OST_EROFS, out_check_req, rc = -EROFS);
                rc = ost_destroy(req->rq_export, req, oti);
                break;
        case OST_GETATTR:
                CDEBUG(D_INODE, "getattr\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_GETATTR_NET, out_free_oti, rc = 0);
                rc = ost_getattr(req->rq_export, req);
                break;
        case OST_SETATTR:
                CDEBUG(D_INODE, "setattr\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_SETATTR_NET, out_free_oti, rc = 0);
                rc = ost_setattr(req->rq_export, req, oti);
                break;
        case OST_WRITE:
                CDEBUG(D_INODE, "write\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_BRW_NET, out_free_oti, rc = 0);
                OBD_FAIL_GOTO(OBD_FAIL_OST_ENOSPC, out_check_req, rc = -ENOSPC);
                OBD_FAIL_GOTO(OBD_FAIL_OST_EROFS, out_check_req, rc = -EROFS);
                rc = ost_brw_write(req, oti);
                LASSERT(current->journal_info == NULL);
                /* ost_brw sends its own replies */
                GOTO(out_free_oti, rc);
        case OST_READ:
                CDEBUG(D_INODE, "read\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_BRW_NET, out_free_oti, rc = 0);
                rc = ost_brw_read(req);
                LASSERT(current->journal_info == NULL);
                /* ost_brw sends its own replies */
                GOTO(out_free_oti, rc);
        case OST_SAN_READ:
                CDEBUG(D_INODE, "san read\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_BRW_NET, out_free_oti, rc = 0);
                rc = ost_san_brw(req, OBD_BRW_READ);
                /* ost_san_brw sends its own replies */
                GOTO(out_free_oti, rc);
        case OST_SAN_WRITE:
                CDEBUG(D_INODE, "san write\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_BRW_NET, out_free_oti, rc = 0);
                rc = ost_san_brw(req, OBD_BRW_WRITE);
                /* ost_san_brw sends its own replies */
                GOTO(out_free_oti, rc);
        case OST_PUNCH:
                CDEBUG(D_INODE, "punch\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_PUNCH_NET, out_free_oti, rc = 0);
                OBD_FAIL_GOTO(OBD_FAIL_OST_EROFS, out_check_req, rc = -EROFS);
                rc = ost_punch(req->rq_export, req, oti);
                break;
        case OST_STATFS:
                CDEBUG(D_INODE, "statfs\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_STATFS_NET, out_free_oti, rc = 0);
                rc = ost_statfs(req);
                break;
        case OST_SYNC:
                CDEBUG(D_INODE, "sync\n");
                OBD_FAIL_GOTO(OBD_FAIL_OST_SYNC_NET, out_free_oti, rc = 0);
                rc = ost_sync(req->rq_export, req);
                break;
        case OST_SET_INFO:
                DEBUG_REQ(D_INODE, req, "set_info");
                rc = ost_set_info(req->rq_export, req);
                break;
        case OST_GET_INFO:
                DEBUG_REQ(D_INODE, req, "get_info");
                rc = ost_get_info(req->rq_export, req);
                break;
        case OBD_PING:
                DEBUG_REQ(D_INODE, req, "ping");
                rc = target_handle_ping(req);
                break;
        /* FIXME - just reply status */
        case LLOG_ORIGIN_CONNECT:
                DEBUG_REQ(D_INODE, req, "log connect\n");
                rc = ost_llog_handle_connect(req->rq_export, req); 
                req->rq_status = rc;
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc)
                        GOTO(out_free_oti, rc);
                GOTO(out_free_oti, rc = ptlrpc_reply(req));
        case OBD_LOG_CANCEL:
                CDEBUG(D_INODE, "log cancel\n");
                OBD_FAIL_GOTO(OBD_FAIL_OBD_LOG_CANCEL_NET, out_free_oti, rc = 0);
                rc = llog_origin_handle_cancel(req);
                req->rq_status = rc;
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc)
                        GOTO(out_free_oti, rc);
                GOTO(out_free_oti, rc = ptlrpc_reply(req));
        case LDLM_ENQUEUE:
                CDEBUG(D_INODE, "enqueue\n");
                OBD_FAIL_GOTO(OBD_FAIL_LDLM_ENQUEUE, out_free_oti, rc = 0);
                rc = ldlm_handle_enqueue(req, ldlm_server_completion_ast,
                                         ldlm_server_blocking_ast,
                                         ldlm_server_glimpse_ast);
                fail = OBD_FAIL_OST_LDLM_REPLY_NET;
                break;
        case LDLM_CONVERT:
                CDEBUG(D_INODE, "convert\n");
                OBD_FAIL_GOTO(OBD_FAIL_LDLM_CONVERT, out_free_oti, rc = 0);
                rc = ldlm_handle_convert(req);
                break;
        case LDLM_CANCEL:
                CDEBUG(D_INODE, "cancel\n");
                OBD_FAIL_GOTO(OBD_FAIL_LDLM_CANCEL, out_free_oti, rc = 0);
                rc = ldlm_handle_cancel(req);
                break;
        case LDLM_BL_CALLBACK:
        case LDLM_CP_CALLBACK:
                CDEBUG(D_INODE, "callback\n");
                CERROR("callbacks should not happen on OST\n");
                /* fall through */
        default:
                CERROR("Unexpected opcode %d\n", req->rq_reqmsg->opc);
                req->rq_status = -ENOTSUPP;
                rc = ptlrpc_error(req);
                GOTO(out_free_oti, rc);
        }

        LASSERT(current->journal_info == NULL);

        EXIT;
        /* If we're DISCONNECTing, the export_data is already freed */
        if (!rc && req->rq_reqmsg->opc != OST_DISCONNECT) {
                if (!obd->obd_no_transno) {
                        req->rq_repmsg->last_committed =
                                obd->obd_last_committed;
                } else {
                        DEBUG_REQ(D_IOCTL, req,
                                  "not sending last_committed update");
                }
                CDEBUG(D_INFO, "last_committed "LPU64", xid "LPX64"\n",
                       obd->obd_last_committed, req->rq_xid);
        }

out_check_req:

        if (!rc)
                oti_to_request(oti, req);
        target_send_reply(req, rc, fail);
        rc = 0;
        
out_free_oti:
        if (oti)
                OBD_FREE(oti, sizeof(*oti));
        return rc;
}
EXPORT_SYMBOL(ost_handle);

int ost_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(ost,&lvars);
        return lprocfs_obd_attach(dev, lvars.obd_vars);
}

int ost_detach(struct obd_device *dev)
{
        return lprocfs_obd_detach(dev);
}

extern struct file_operations ost_stimes_fops;

static int ost_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct ost_obd *ost = &obd->u.ost;
        int rc;
        ENTRY;

        rc = cleanup_group_info();
        if (rc)
                RETURN(rc);

        rc = llog_start_commit_thread();
        if (rc < 0)
                RETURN(rc);

        lprocfs_obd_seq_create(obd, "service_times", 0444, &ost_stimes_fops,
                               obd);

        ost->ost_service =
                ptlrpc_init_svc(OST_NBUFS, OST_BUFSIZE, OST_MAXREQSIZE,
                                OST_REQUEST_PORTAL, OSC_REPLY_PORTAL, 30000,
                                ost_handle, "ost",
                                obd->obd_proc_entry);
        if (ost->ost_service == NULL) {
                CERROR("failed to start service\n");
                RETURN(-ENOMEM);
        }

        rc = ptlrpc_start_n_threads(obd, ost->ost_service, OST_NUM_THREADS,
                                    "ll_ost");
        if (rc)
                GOTO(out_service, rc = -EINVAL);

        ost->ost_create_service =
                ptlrpc_init_svc(OST_NBUFS, OST_BUFSIZE, OST_MAXREQSIZE,
                                OST_CREATE_PORTAL, OSC_REPLY_PORTAL, 30000,
                                ost_handle, "ost_create",
                                obd->obd_proc_entry);
        if (ost->ost_create_service == NULL) {
                CERROR("failed to start OST create service\n");
                GOTO(out_service, rc = -ENOMEM);
        }


        spin_lock_init(&ost->ost_lock);
        ost->ost_service->srv_obddev = obd;
        
        rc = ptlrpc_start_n_threads(obd, ost->ost_create_service, 1,
                                    "ll_ost_creat");
        if (rc)
                GOTO(out_create, rc = -EINVAL);

        RETURN(0);

out_create:
        ptlrpc_unregister_service(ost->ost_create_service);
out_service:
        ptlrpc_unregister_service(ost->ost_service);
        RETURN(rc);
}

extern void lgss_svc_cache_purge_all(void);
static int ost_cleanup(struct obd_device *obd, int flags)
{
        struct ost_obd *ost = &obd->u.ost;
        int err = 0;
        ENTRY;

        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_recovering) {
                target_cancel_recovery_timer(obd);
                obd->obd_recovering = 0;
        }
        spin_unlock_bh(&obd->obd_processing_task_lock);

        ptlrpc_stop_all_threads(ost->ost_service);
        ptlrpc_unregister_service(ost->ost_service);

        ptlrpc_stop_all_threads(ost->ost_create_service);
        ptlrpc_unregister_service(ost->ost_create_service);

#ifdef ENABLE_GSS
        /* XXX */
        lgss_svc_cache_purge_all();
#endif
        RETURN(err);
}

/* use obd ops to offer management infrastructure */
static struct obd_ops ost_obd_ops = {
        .o_owner        = THIS_MODULE,
        .o_attach       = ost_attach,
        .o_detach       = ost_detach,
        .o_setup        = ost_setup,
        .o_cleanup      = ost_cleanup,
};

static int __init ost_init(void)
{
        struct lprocfs_static_vars lvars;
        ENTRY;

        lprocfs_init_vars(ost,&lvars);
        RETURN(class_register_type(&ost_obd_ops, NULL, lvars.module_vars,
                                   OBD_OST_DEVICENAME));
}

static void /*__exit*/ ost_exit(void)
{
        class_unregister_type(OBD_OST_DEVICENAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Object Storage Target (OST) v0.01");
MODULE_LICENSE("GPL");

module_init(ost_init);
module_exit(ost_exit);
