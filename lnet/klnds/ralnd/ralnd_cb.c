/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
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

#include "ranal.h"

void
kranal_device_callback(RAP_INT32 devid, RAP_PVOID arg)
{
        kra_device_t *dev;
        int           i;
        unsigned long flags;

        CDEBUG(D_NET, "callback for device %d\n", devid);

        for (i = 0; i < kranal_data.kra_ndevs; i++) {

                dev = &kranal_data.kra_devices[i];
                if (dev->rad_id != devid)
                        continue;

                spin_lock_irqsave(&dev->rad_lock, flags);

                if (!dev->rad_ready) {
                        dev->rad_ready = 1;
                        wake_up(&dev->rad_waitq);
                }

                spin_unlock_irqrestore(&dev->rad_lock, flags);
                return;
        }

        CWARN("callback for unknown device %d\n", devid);
}

void
kranal_schedule_conn(kra_conn_t *conn)
{
        kra_device_t    *dev = conn->rac_device;
        unsigned long    flags;

        spin_lock_irqsave(&dev->rad_lock, flags);

        if (!conn->rac_scheduled) {
                kranal_conn_addref(conn);       /* +1 ref for scheduler */
                conn->rac_scheduled = 1;
                list_add_tail(&conn->rac_schedlist, &dev->rad_ready_conns);
                wake_up(&dev->rad_waitq);
        }

        spin_unlock_irqrestore(&dev->rad_lock, flags);
}

kra_tx_t *
kranal_get_idle_tx (int may_block)
{
        unsigned long  flags;
        kra_tx_t      *tx = NULL;

        for (;;) {
                spin_lock_irqsave(&kranal_data.kra_tx_lock, flags);

                /* "normal" descriptor is free */
                if (!list_empty(&kranal_data.kra_idle_txs)) {
                        tx = list_entry(kranal_data.kra_idle_txs.next,
                                        kra_tx_t, tx_list);
                        break;
                }

                if (!may_block) {
                        /* may dip into reserve pool */
                        if (list_empty(&kranal_data.kra_idle_nblk_txs)) {
                                CERROR("reserved tx desc pool exhausted\n");
                                break;
                        }

                        tx = list_entry(kranal_data.kra_idle_nblk_txs.next,
                                        kra_tx_t, tx_list);
                        break;
                }

                /* block for idle tx */
                spin_unlock_irqrestore(&kranal_data.kra_tx_lock, flags);

                wait_event(kranal_data.kra_idle_tx_waitq,
                           !list_empty(&kranal_data.kra_idle_txs));
        }

        if (tx != NULL) {
                list_del(&tx->tx_list);

                /* Allocate a new completion cookie.  It might not be
                 * needed, but we've got a lock right now... */
                tx->tx_cookie = kranal_data.kra_next_tx_cookie++;

                LASSERT (tx->tx_buftype == RANAL_BUF_NONE);
                LASSERT (tx->tx_msg.ram_type == RANAL_MSG_NONE);
                LASSERT (tx->tx_conn == NULL);
                LASSERT (tx->tx_ptlmsg[0] == NULL);
                LASSERT (tx->tx_ptlmsg[1] == NULL);
        }

        spin_unlock_irqrestore(&kranal_data.kra_tx_lock, flags);

        return tx;
}

void
kranal_init_msg(kra_msg_t *msg, int type)
{
        msg->ram_magic = RANAL_MSG_MAGIC;
        msg->ram_version = RANAL_MSG_VERSION;
        msg->ram_type = type;
        msg->ram_srcnid = kranal_data.kra_ni->ni_nid;
        /* ram_connstamp gets set when FMA is sent */
}

kra_tx_t *
kranal_new_tx_msg (int may_block, int type)
{
        kra_tx_t *tx = kranal_get_idle_tx(may_block);

        if (tx == NULL)
                return NULL;

        kranal_init_msg(&tx->tx_msg, type);
        return tx;
}

int
kranal_setup_immediate_buffer (kra_tx_t *tx, int niov, struct iovec *iov,
                               int offset, int nob)

{
        /* For now this is almost identical to kranal_setup_virt_buffer, but we
         * could "flatten" the payload into a single contiguous buffer ready
         * for sending direct over an FMA if we ever needed to. */

        LASSERT (tx->tx_buftype == RANAL_BUF_NONE);
        LASSERT (nob >= 0);

        if (nob == 0) {
                tx->tx_buffer = NULL;
        } else {
                LASSERT (niov > 0);

                while (offset >= iov->iov_len) {
                        offset -= iov->iov_len;
                        niov--;
                        iov++;
                        LASSERT (niov > 0);
                }

                if (nob > iov->iov_len - offset) {
                        CERROR("Can't handle multiple vaddr fragments\n");
                        return -EMSGSIZE;
                }

                tx->tx_buffer = (void *)(((unsigned long)iov->iov_base) + offset);
        }

        tx->tx_buftype = RANAL_BUF_IMMEDIATE;
        tx->tx_nob = nob;
        return 0;
}

int
kranal_setup_virt_buffer (kra_tx_t *tx, int niov, struct iovec *iov,
                          int offset, int nob)

{
        LASSERT (nob > 0);
        LASSERT (niov > 0);
        LASSERT (tx->tx_buftype == RANAL_BUF_NONE);

        while (offset >= iov->iov_len) {
                offset -= iov->iov_len;
                niov--;
                iov++;
                LASSERT (niov > 0);
        }

        if (nob > iov->iov_len - offset) {
                CERROR("Can't handle multiple vaddr fragments\n");
                return -EMSGSIZE;
        }

        tx->tx_buftype = RANAL_BUF_VIRT_UNMAPPED;
        tx->tx_nob = nob;
        tx->tx_buffer = (void *)(((unsigned long)iov->iov_base) + offset);
        return 0;
}

int
kranal_setup_phys_buffer (kra_tx_t *tx, int nkiov, ptl_kiov_t *kiov,
                          int offset, int nob)
{
        RAP_PHYS_REGION *phys = tx->tx_phys;
        int              resid;

        CDEBUG(D_NET, "niov %d offset %d nob %d\n", nkiov, offset, nob);

        LASSERT (nob > 0);
        LASSERT (nkiov > 0);
        LASSERT (tx->tx_buftype == RANAL_BUF_NONE);

        while (offset >= kiov->kiov_len) {
                offset -= kiov->kiov_len;
                nkiov--;
                kiov++;
                LASSERT (nkiov > 0);
        }

        tx->tx_buftype = RANAL_BUF_PHYS_UNMAPPED;
        tx->tx_nob = nob;
        tx->tx_buffer = (void *)((unsigned long)(kiov->kiov_offset + offset));

        phys->Address = kranal_page2phys(kiov->kiov_page);
        phys++;

        resid = nob - (kiov->kiov_len - offset);
        while (resid > 0) {
                kiov++;
                nkiov--;
                LASSERT (nkiov > 0);

                if (kiov->kiov_offset != 0 ||
                    ((resid > PAGE_SIZE) &&
                     kiov->kiov_len < PAGE_SIZE)) {
                        /* Can't have gaps */
                        CERROR("Can't make payload contiguous in I/O VM:"
                               "page %d, offset %d, len %d \n",
                               (int)(phys - tx->tx_phys),
                               kiov->kiov_offset, kiov->kiov_len);
                        return -EINVAL;
                }

                if ((phys - tx->tx_phys) == PTL_MD_MAX_IOV) {
                        CERROR ("payload too big (%d)\n", (int)(phys - tx->tx_phys));
                        return -EMSGSIZE;
                }

                phys->Address = kranal_page2phys(kiov->kiov_page);
                phys++;

                resid -= PAGE_SIZE;
        }

        tx->tx_phys_npages = phys - tx->tx_phys;
        return 0;
}

static inline int
kranal_setup_rdma_buffer (kra_tx_t *tx, int niov,
                          struct iovec *iov, ptl_kiov_t *kiov,
                          int offset, int nob)
{
        LASSERT ((iov == NULL) != (kiov == NULL));

        if (kiov != NULL)
                return kranal_setup_phys_buffer(tx, niov, kiov, offset, nob);

        return kranal_setup_virt_buffer(tx, niov, iov, offset, nob);
}

void
kranal_map_buffer (kra_tx_t *tx)
{
        kra_conn_t     *conn = tx->tx_conn;
        kra_device_t   *dev = conn->rac_device;
        RAP_RETURN      rrc;

        LASSERT (current == dev->rad_scheduler);

        switch (tx->tx_buftype) {
        default:
                LBUG();

        case RANAL_BUF_NONE:
        case RANAL_BUF_IMMEDIATE:
        case RANAL_BUF_PHYS_MAPPED:
        case RANAL_BUF_VIRT_MAPPED:
                break;

        case RANAL_BUF_PHYS_UNMAPPED:
                rrc = RapkRegisterPhys(dev->rad_handle,
                                       tx->tx_phys, tx->tx_phys_npages,
                                       &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                tx->tx_buftype = RANAL_BUF_PHYS_MAPPED;
                break;

        case RANAL_BUF_VIRT_UNMAPPED:
                rrc = RapkRegisterMemory(dev->rad_handle,
                                         tx->tx_buffer, tx->tx_nob,
                                         &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                tx->tx_buftype = RANAL_BUF_VIRT_MAPPED;
                break;
        }
}

void
kranal_unmap_buffer (kra_tx_t *tx)
{
        kra_device_t   *dev;
        RAP_RETURN      rrc;

        switch (tx->tx_buftype) {
        default:
                LBUG();

        case RANAL_BUF_NONE:
        case RANAL_BUF_IMMEDIATE:
        case RANAL_BUF_PHYS_UNMAPPED:
        case RANAL_BUF_VIRT_UNMAPPED:
                break;

        case RANAL_BUF_PHYS_MAPPED:
                LASSERT (tx->tx_conn != NULL);
                dev = tx->tx_conn->rac_device;
                LASSERT (current == dev->rad_scheduler);
                rrc = RapkDeregisterMemory(dev->rad_handle, NULL,
                                           &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                tx->tx_buftype = RANAL_BUF_PHYS_UNMAPPED;
                break;

        case RANAL_BUF_VIRT_MAPPED:
                LASSERT (tx->tx_conn != NULL);
                dev = tx->tx_conn->rac_device;
                LASSERT (current == dev->rad_scheduler);
                rrc = RapkDeregisterMemory(dev->rad_handle, tx->tx_buffer,
                                           &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                tx->tx_buftype = RANAL_BUF_VIRT_UNMAPPED;
                break;
        }
}

void
kranal_tx_done (kra_tx_t *tx, int completion)
{
        ptl_err_t        ptlrc = (completion == 0) ? PTL_OK : PTL_FAIL;
        unsigned long    flags;
        int              i;

        LASSERT (!in_interrupt());

        kranal_unmap_buffer(tx);

        for (i = 0; i < 2; i++) {
                /* tx may have up to 2 ptlmsgs to finalise */
                if (tx->tx_ptlmsg[i] == NULL)
                        continue;

                ptl_finalize(kranal_data.kra_ni, NULL, tx->tx_ptlmsg[i], ptlrc);
                tx->tx_ptlmsg[i] = NULL;
        }

        tx->tx_buftype = RANAL_BUF_NONE;
        tx->tx_msg.ram_type = RANAL_MSG_NONE;
        tx->tx_conn = NULL;

        spin_lock_irqsave(&kranal_data.kra_tx_lock, flags);

        if (tx->tx_isnblk) {
                list_add_tail(&tx->tx_list, &kranal_data.kra_idle_nblk_txs);
        } else {
                list_add_tail(&tx->tx_list, &kranal_data.kra_idle_txs);
                wake_up(&kranal_data.kra_idle_tx_waitq);
        }

        spin_unlock_irqrestore(&kranal_data.kra_tx_lock, flags);
}

kra_conn_t *
kranal_find_conn_locked (kra_peer_t *peer)
{
        struct list_head *tmp;

        /* just return the first connection */
        list_for_each (tmp, &peer->rap_conns) {
                return list_entry(tmp, kra_conn_t, rac_list);
        }

        return NULL;
}

void
kranal_post_fma (kra_conn_t *conn, kra_tx_t *tx)
{
        unsigned long    flags;

        tx->tx_conn = conn;

        spin_lock_irqsave(&conn->rac_lock, flags);
        list_add_tail(&tx->tx_list, &conn->rac_fmaq);
        tx->tx_qtime = jiffies;
        spin_unlock_irqrestore(&conn->rac_lock, flags);

        kranal_schedule_conn(conn);
}

void
kranal_launch_tx (kra_tx_t *tx, ptl_nid_t nid)
{
        unsigned long    flags;
        kra_peer_t      *peer;
        kra_conn_t      *conn;
        unsigned long    now;
        rwlock_t        *g_lock = &kranal_data.kra_global_lock;

        /* If I get here, I've committed to send, so I complete the tx with
         * failure on any problems */

        LASSERT (tx->tx_conn == NULL);          /* only set when assigned a conn */

        read_lock(g_lock);

        peer = kranal_find_peer_locked(nid);
        if (peer == NULL) {
                read_unlock(g_lock);
                kranal_tx_done(tx, -EHOSTUNREACH);
                return;
        }

        conn = kranal_find_conn_locked(peer);
        if (conn != NULL) {
                kranal_post_fma(conn, tx);
                read_unlock(g_lock);
                return;
        }

        /* Making one or more connections; I'll need a write lock... */
        read_unlock(g_lock);
        write_lock_irqsave(g_lock, flags);

        peer = kranal_find_peer_locked(nid);
        if (peer == NULL) {
                write_unlock_irqrestore(g_lock, flags);
                kranal_tx_done(tx, -EHOSTUNREACH);
                return;
        }

        conn = kranal_find_conn_locked(peer);
        if (conn != NULL) {
                /* Connection exists; queue message on it */
                kranal_post_fma(conn, tx);
                write_unlock_irqrestore(g_lock, flags);
                return;
        }

        LASSERT (peer->rap_persistence > 0);

        if (!peer->rap_connecting) {
                LASSERT (list_empty(&peer->rap_tx_queue));

                now = CURRENT_SECONDS;
                if (now < peer->rap_reconnect_time) {
                        write_unlock_irqrestore(g_lock, flags);
                        kranal_tx_done(tx, -EHOSTUNREACH);
                        return;
                }

                peer->rap_connecting = 1;
                kranal_peer_addref(peer); /* extra ref for connd */

                spin_lock(&kranal_data.kra_connd_lock);

                list_add_tail(&peer->rap_connd_list,
                              &kranal_data.kra_connd_peers);
                wake_up(&kranal_data.kra_connd_waitq);

                spin_unlock(&kranal_data.kra_connd_lock);
        }

        /* A connection is being established; queue the message... */
        list_add_tail(&tx->tx_list, &peer->rap_tx_queue);

        write_unlock_irqrestore(g_lock, flags);
}

void
kranal_rdma(kra_tx_t *tx, int type,
            kra_rdma_desc_t *sink, int nob, __u64 cookie)
{
        kra_conn_t   *conn = tx->tx_conn;
        RAP_RETURN    rrc;
        unsigned long flags;

        LASSERT (kranal_tx_mapped(tx));
        LASSERT (nob <= sink->rard_nob);
        LASSERT (nob <= tx->tx_nob);

        /* No actual race with scheduler sending CLOSE (I'm she!) */
        LASSERT (current == conn->rac_device->rad_scheduler);

        memset(&tx->tx_rdma_desc, 0, sizeof(tx->tx_rdma_desc));
        tx->tx_rdma_desc.SrcPtr.AddressBits = (__u64)((unsigned long)tx->tx_buffer);
        tx->tx_rdma_desc.SrcKey = tx->tx_map_key;
        tx->tx_rdma_desc.DstPtr = sink->rard_addr;
        tx->tx_rdma_desc.DstKey = sink->rard_key;
        tx->tx_rdma_desc.Length = nob;
        tx->tx_rdma_desc.AppPtr = tx;

        /* prep final completion message */
        kranal_init_msg(&tx->tx_msg, type);
        tx->tx_msg.ram_u.completion.racm_cookie = cookie;

        if (nob == 0) { /* Immediate completion */
                kranal_post_fma(conn, tx);
                return;
        }

        LASSERT (!conn->rac_close_sent); /* Don't lie (CLOSE == RDMA idle) */

        rrc = RapkPostRdma(conn->rac_rihandle, &tx->tx_rdma_desc);
        LASSERT (rrc == RAP_SUCCESS);

        spin_lock_irqsave(&conn->rac_lock, flags);
        list_add_tail(&tx->tx_list, &conn->rac_rdmaq);
        tx->tx_qtime = jiffies;
        spin_unlock_irqrestore(&conn->rac_lock, flags);
}

int
kranal_consume_rxmsg (kra_conn_t *conn, void *buffer, int nob)
{
        __u32      nob_received = nob;
        RAP_RETURN rrc;

        LASSERT (conn->rac_rxmsg != NULL);
        CDEBUG(D_NET, "Consuming %p\n", conn);

        rrc = RapkFmaCopyOut(conn->rac_rihandle, buffer,
                             &nob_received, sizeof(kra_msg_t));
        LASSERT (rrc == RAP_SUCCESS);

        conn->rac_rxmsg = NULL;

        if (nob_received < nob) {
                CWARN("Incomplete immediate msg from "LPX64
                      ": expected %d, got %d\n",
                      conn->rac_peer->rap_nid, nob, nob_received);
                return -EPROTO;
        }

        return 0;
}

ptl_err_t
kranal_do_send (ptl_ni_t     *ni,
                void         *private,
                ptl_msg_t    *ptlmsg,
                ptl_hdr_t    *hdr,
                int           type,
                ptl_nid_t     nid,
                ptl_pid_t     pid,
                unsigned int  niov,
                struct iovec *iov,
                ptl_kiov_t   *kiov,
                int           offset,
                int           nob)
{
        kra_conn_t *conn;
        kra_tx_t   *tx;
        int         rc;

        /* NB 'private' is different depending on what we're sending.... */

        CDEBUG(D_NET, "sending %d bytes in %d frags to nid:"LPX64" pid %d\n",
               nob, niov, nid, pid);

        LASSERT (nob == 0 || niov > 0);
        LASSERT (niov <= PTL_MD_MAX_IOV);

        LASSERT (!in_interrupt());
        /* payload is either all vaddrs or all pages */
        LASSERT (!(kiov != NULL && iov != NULL));

        switch(type) {
        default:
                LBUG();

        case PTL_MSG_REPLY: {
                /* reply's 'private' is the conn that received the GET_REQ */
                conn = private;
                LASSERT (conn->rac_rxmsg != NULL);

                if (conn->rac_rxmsg->ram_type == RANAL_MSG_IMMEDIATE) {
                        if (nob > RANAL_FMA_MAX_DATA) {
                                CERROR("Can't REPLY IMMEDIATE %d to "LPX64"\n",
                                       nob, nid);
                                return PTL_FAIL;
                        }
                        break;                  /* RDMA not expected */
                }

                /* Incoming message consistent with RDMA? */
                if (conn->rac_rxmsg->ram_type != RANAL_MSG_GET_REQ) {
                        CERROR("REPLY to "LPX64" bad msg type %x!!!\n",
                               nid, conn->rac_rxmsg->ram_type);
                        return PTL_FAIL;
                }

                tx = kranal_get_idle_tx(0);
                if (tx == NULL)
                        return PTL_FAIL;

                rc = kranal_setup_rdma_buffer(tx, niov, iov, kiov, offset, nob);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_conn = conn;
                tx->tx_ptlmsg[0] = ptlmsg;

                kranal_map_buffer(tx);
                kranal_rdma(tx, RANAL_MSG_GET_DONE,
                            &conn->rac_rxmsg->ram_u.get.ragm_desc, nob,
                            conn->rac_rxmsg->ram_u.get.ragm_cookie);

                /* flag matched by consuming rx message */
                kranal_consume_rxmsg(conn, NULL, 0);
                return PTL_OK;
        }

        case PTL_MSG_GET:
                LASSERT (niov == 0);
                LASSERT (nob == 0);
                /* We have to consider the eventual sink buffer rather than any
                 * payload passed here (there isn't any, and strictly, looking
                 * inside ptlmsg is a layering violation).  We send a simple
                 * IMMEDIATE GET if the sink buffer is mapped already and small
                 * enough for FMA */

                if ((ptlmsg->msg_md->md_options & PTL_MD_KIOV) == 0 &&
                    ptlmsg->msg_md->md_length <= RANAL_FMA_MAX_DATA &&
                    ptlmsg->msg_md->md_length <= kranal_tunables.kra_max_immediate)
                        break;

                tx = kranal_new_tx_msg(!in_interrupt(), RANAL_MSG_GET_REQ);
                if (tx == NULL)
                        return PTL_NO_SPACE;

                if ((ptlmsg->msg_md->md_options & PTL_MD_KIOV) == 0)
                        rc = kranal_setup_virt_buffer(tx, ptlmsg->msg_md->md_niov,
                                                      ptlmsg->msg_md->md_iov.iov,
                                                      0, ptlmsg->msg_md->md_length);
                else
                        rc = kranal_setup_phys_buffer(tx, ptlmsg->msg_md->md_niov,
                                                      ptlmsg->msg_md->md_iov.kiov,
                                                      0, ptlmsg->msg_md->md_length);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_ptlmsg[1] = ptl_create_reply_msg(kranal_data.kra_ni, 
                                                        nid, ptlmsg);
                if (tx->tx_ptlmsg[1] == NULL) {
                        CERROR("Can't create reply for GET to "LPX64"\n", nid);
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_ptlmsg[0] = ptlmsg;
                tx->tx_msg.ram_u.get.ragm_hdr = *hdr;
                /* rest of tx_msg is setup just before it is sent */
                kranal_launch_tx(tx, nid);
                return PTL_OK;

        case PTL_MSG_ACK:
                LASSERT (nob == 0);
                break;

        case PTL_MSG_PUT:
                if (kiov == NULL &&             /* not paged */
                    nob <= RANAL_FMA_MAX_DATA && /* small enough */
                    nob <= kranal_tunables.kra_max_immediate)
                        break;                  /* send IMMEDIATE */

                tx = kranal_new_tx_msg(!in_interrupt(), RANAL_MSG_PUT_REQ);
                if (tx == NULL)
                        return PTL_NO_SPACE;

                rc = kranal_setup_rdma_buffer(tx, niov, iov, kiov, offset, nob);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_ptlmsg[0] = ptlmsg;
                tx->tx_msg.ram_u.putreq.raprm_hdr = *hdr;
                /* rest of tx_msg is setup just before it is sent */
                kranal_launch_tx(tx, nid);
                return PTL_OK;
        }

        LASSERT (kiov == NULL);
        LASSERT (nob <= RANAL_FMA_MAX_DATA);

        tx = kranal_new_tx_msg(!(type == PTL_MSG_ACK ||
                                 type == PTL_MSG_REPLY ||
                                 in_interrupt()),
                               RANAL_MSG_IMMEDIATE);
        if (tx == NULL)
                return PTL_NO_SPACE;

        rc = kranal_setup_immediate_buffer(tx, niov, iov, offset, nob);
        if (rc != 0) {
                kranal_tx_done(tx, rc);
                return PTL_FAIL;
        }

        tx->tx_msg.ram_u.immediate.raim_hdr = *hdr;
        tx->tx_ptlmsg[0] = ptlmsg;
        kranal_launch_tx(tx, nid);
        return PTL_OK;
}

ptl_err_t
kranal_send (ptl_ni_t *ni, void *private, ptl_msg_t *cookie,
             ptl_hdr_t *hdr, int type, ptl_nid_t nid, ptl_pid_t pid,
             unsigned int niov, struct iovec *iov,
             size_t offset, size_t len)
{
        return kranal_do_send(ni, private, cookie,
                              hdr, type, nid, pid,
                              niov, iov, NULL,
                              offset, len);
}

ptl_err_t
kranal_send_pages (ptl_ni_t *ni, void *private, ptl_msg_t *cookie,
                   ptl_hdr_t *hdr, int type, ptl_nid_t nid, ptl_pid_t pid,
                   unsigned int niov, ptl_kiov_t *kiov,
                   size_t offset, size_t len)
{
        return kranal_do_send(ni, private, cookie,
                              hdr, type, nid, pid,
                              niov, NULL, kiov,
                              offset, len);
}

ptl_err_t
kranal_do_recv (ptl_ni_t *ni, void *private, ptl_msg_t *ptlmsg,
                unsigned int niov, struct iovec *iov, ptl_kiov_t *kiov,
                int offset, int mlen, int rlen)
{
        kra_conn_t  *conn = private;
        kra_msg_t   *rxmsg = conn->rac_rxmsg;
        kra_tx_t    *tx;
        void        *buffer;
        int          rc;

        LASSERT (mlen <= rlen);
        LASSERT (!in_interrupt());
        /* Either all pages or all vaddrs */
        LASSERT (!(kiov != NULL && iov != NULL));

        CDEBUG(D_NET, "conn %p, rxmsg %p, ptlmsg %p\n", conn, rxmsg, ptlmsg);

        if (ptlmsg == NULL) {
                /* GET or ACK or portals is discarding */
                LASSERT (mlen == 0);
                ptl_finalize(ni, NULL, ptlmsg, PTL_OK);
                return PTL_OK;
        }

        switch(rxmsg->ram_type) {
        default:
                LBUG();
                return PTL_FAIL;

        case RANAL_MSG_IMMEDIATE:
                if (mlen == 0) {
                        buffer = NULL;
                } else if (kiov != NULL) {
                        CERROR("Can't recv immediate into paged buffer\n");
                        return PTL_FAIL;
                } else {
                        LASSERT (niov > 0);
                        while (offset >= iov->iov_len) {
                                offset -= iov->iov_len;
                                iov++;
                                niov--;
                                LASSERT (niov > 0);
                        }
                        if (mlen > iov->iov_len - offset) {
                                CERROR("Can't handle immediate frags\n");
                                return PTL_FAIL;
                        }
                        buffer = ((char *)iov->iov_base) + offset;
                }
                rc = kranal_consume_rxmsg(conn, buffer, mlen);
                ptl_finalize(ni, NULL, ptlmsg, (rc == 0) ? PTL_OK : PTL_FAIL);
                return PTL_OK;

        case RANAL_MSG_PUT_REQ:
                tx = kranal_new_tx_msg(0, RANAL_MSG_PUT_ACK);
                if (tx == NULL)
                        return PTL_NO_SPACE;

                rc = kranal_setup_rdma_buffer(tx, niov, iov, kiov, offset, mlen);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_conn = conn;
                kranal_map_buffer(tx);

                tx->tx_msg.ram_u.putack.rapam_src_cookie =
                        conn->rac_rxmsg->ram_u.putreq.raprm_cookie;
                tx->tx_msg.ram_u.putack.rapam_dst_cookie = tx->tx_cookie;
                tx->tx_msg.ram_u.putack.rapam_desc.rard_key = tx->tx_map_key;
                tx->tx_msg.ram_u.putack.rapam_desc.rard_addr.AddressBits =
                        (__u64)((unsigned long)tx->tx_buffer);
                tx->tx_msg.ram_u.putack.rapam_desc.rard_nob = mlen;

                tx->tx_ptlmsg[0] = ptlmsg; /* finalize this on RDMA_DONE */

                kranal_post_fma(conn, tx);

                /* flag matched by consuming rx message */
                kranal_consume_rxmsg(conn, NULL, 0);
                return PTL_OK;
        }
}

ptl_err_t
kranal_recv (ptl_ni_t *ni, void *private, ptl_msg_t *msg,
             unsigned int niov, struct iovec *iov,
             size_t offset, size_t mlen, size_t rlen)
{
        return kranal_do_recv(ni, private, msg, niov, iov, NULL,
                              offset, mlen, rlen);
}

ptl_err_t
kranal_recv_pages (ptl_ni_t *ni, void *private, ptl_msg_t *msg,
                   unsigned int niov, ptl_kiov_t *kiov,
                   size_t offset, size_t mlen, size_t rlen)
{
        return kranal_do_recv(ni, private, msg, niov, NULL, kiov,
                              offset, mlen, rlen);
}

int
kranal_thread_start (int(*fn)(void *arg), void *arg)
{
        long    pid = kernel_thread(fn, arg, 0);

        if (pid < 0)
                return(int)pid;

        atomic_inc(&kranal_data.kra_nthreads);
        return 0;
}

void
kranal_thread_fini (void)
{
        atomic_dec(&kranal_data.kra_nthreads);
}

int
kranal_check_conn_timeouts (kra_conn_t *conn)
{
        kra_tx_t          *tx;
        struct list_head  *ttmp;
        unsigned long      flags;
        long               timeout;
        unsigned long      now = jiffies;

        LASSERT (conn->rac_state == RANAL_CONN_ESTABLISHED ||
                 conn->rac_state == RANAL_CONN_CLOSING);

        if (!conn->rac_close_sent &&
            time_after_eq(now, conn->rac_last_tx + conn->rac_keepalive * HZ)) {
                /* not sent in a while; schedule conn so scheduler sends a keepalive */
                CDEBUG(D_NET, "Scheduling keepalive %p->"LPX64"\n",
                       conn, conn->rac_peer->rap_nid);
                kranal_schedule_conn(conn);
        }

        timeout = conn->rac_timeout * HZ;

        if (!conn->rac_close_recvd &&
            time_after_eq(now, conn->rac_last_rx + timeout)) {
                CERROR("%s received from "LPX64" within %lu seconds\n",
                       (conn->rac_state == RANAL_CONN_ESTABLISHED) ?
                       "Nothing" : "CLOSE not",
                       conn->rac_peer->rap_nid, (now - conn->rac_last_rx)/HZ);
                return -ETIMEDOUT;
        }

        if (conn->rac_state != RANAL_CONN_ESTABLISHED)
                return 0;

        /* Check the conn's queues are moving.  These are "belt+braces" checks,
         * in case of hardware/software errors that make this conn seem
         * responsive even though it isn't progressing its message queues. */

        spin_lock_irqsave(&conn->rac_lock, flags);

        list_for_each (ttmp, &conn->rac_fmaq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);

                if (time_after_eq(now, tx->tx_qtime + timeout)) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CERROR("tx on fmaq for "LPX64" blocked %lu seconds\n",
                               conn->rac_peer->rap_nid, (now - tx->tx_qtime)/HZ);
                        return -ETIMEDOUT;
                }
        }

        list_for_each (ttmp, &conn->rac_rdmaq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);

                if (time_after_eq(now, tx->tx_qtime + timeout)) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CERROR("tx on rdmaq for "LPX64" blocked %lu seconds\n",
                               conn->rac_peer->rap_nid, (now - tx->tx_qtime)/HZ);
                        return -ETIMEDOUT;
                }
        }

        list_for_each (ttmp, &conn->rac_replyq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);

                if (time_after_eq(now, tx->tx_qtime + timeout)) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CERROR("tx on replyq for "LPX64" blocked %lu seconds\n",
                               conn->rac_peer->rap_nid, (now - tx->tx_qtime)/HZ);
                        return -ETIMEDOUT;
                }
        }

        spin_unlock_irqrestore(&conn->rac_lock, flags);
        return 0;
}

void
kranal_reaper_check (int idx, unsigned long *min_timeoutp)
{
        struct list_head  *conns = &kranal_data.kra_conns[idx];
        struct list_head  *ctmp;
        kra_conn_t        *conn;
        unsigned long      flags;
        int                rc;

 again:
        /* NB. We expect to check all the conns and not find any problems, so
         * we just use a shared lock while we take a look... */
        read_lock(&kranal_data.kra_global_lock);

        list_for_each (ctmp, conns) {
                conn = list_entry(ctmp, kra_conn_t, rac_hashlist);

                if (conn->rac_timeout < *min_timeoutp )
                        *min_timeoutp = conn->rac_timeout;
                if (conn->rac_keepalive < *min_timeoutp )
                        *min_timeoutp = conn->rac_keepalive;

                rc = kranal_check_conn_timeouts(conn);
                if (rc == 0)
                        continue;

                kranal_conn_addref(conn);
                read_unlock(&kranal_data.kra_global_lock);

                CERROR("Conn to "LPX64", cqid %d timed out\n",
                       conn->rac_peer->rap_nid, conn->rac_cqid);

                write_lock_irqsave(&kranal_data.kra_global_lock, flags);

                switch (conn->rac_state) {
                default:
                        LBUG();

                case RANAL_CONN_ESTABLISHED:
                        kranal_close_conn_locked(conn, -ETIMEDOUT);
                        break;

                case RANAL_CONN_CLOSING:
                        kranal_terminate_conn_locked(conn);
                        break;
                }

                write_unlock_irqrestore(&kranal_data.kra_global_lock, flags);

                kranal_conn_decref(conn);

                /* start again now I've dropped the lock */
                goto again;
        }

        read_unlock(&kranal_data.kra_global_lock);
}

int
kranal_connd (void *arg)
{
        long               id = (long)arg;
        char               name[16];
        wait_queue_t       wait;
        unsigned long      flags;
        kra_peer_t        *peer;
        kra_acceptsock_t  *ras;
        int                did_something;

        snprintf(name, sizeof(name), "kranal_connd_%02ld", id);
        kportal_daemonize(name);
        kportal_blockallsigs();

        init_waitqueue_entry(&wait, current);

        spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);

        while (!kranal_data.kra_shutdown) {
                did_something = 0;

                if (!list_empty(&kranal_data.kra_connd_acceptq)) {
                        ras = list_entry(kranal_data.kra_connd_acceptq.next,
                                         kra_acceptsock_t, ras_list);
                        list_del(&ras->ras_list);

                        spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

                        CDEBUG(D_NET,"About to handshake someone\n");

                        kranal_conn_handshake(ras->ras_sock, NULL);
                        kranal_free_acceptsock(ras);

                        CDEBUG(D_NET,"Finished handshaking someone\n");

                        spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);
                        did_something = 1;
                }

                if (!list_empty(&kranal_data.kra_connd_peers)) {
                        peer = list_entry(kranal_data.kra_connd_peers.next,
                                          kra_peer_t, rap_connd_list);

                        list_del_init(&peer->rap_connd_list);
                        spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

                        kranal_connect(peer);
                        kranal_peer_decref(peer);

                        spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);
                        did_something = 1;
                }

                if (did_something)
                        continue;

                set_current_state(TASK_INTERRUPTIBLE);
                add_wait_queue(&kranal_data.kra_connd_waitq, &wait);

                spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

                schedule ();

                set_current_state(TASK_RUNNING);
                remove_wait_queue(&kranal_data.kra_connd_waitq, &wait);

                spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);
        }

        spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

        kranal_thread_fini();
        return 0;
}

void
kranal_update_reaper_timeout(long timeout)
{
        unsigned long   flags;

        LASSERT (timeout > 0);

        spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);

        if (timeout < kranal_data.kra_new_min_timeout)
                kranal_data.kra_new_min_timeout = timeout;

        spin_unlock_irqrestore(&kranal_data.kra_reaper_lock, flags);
}

int
kranal_reaper (void *arg)
{
        wait_queue_t       wait;
        unsigned long      flags;
        long               timeout;
        int                i;
        int                conn_entries = kranal_data.kra_conn_hash_size;
        int                conn_index = 0;
        int                base_index = conn_entries - 1;
        unsigned long      next_check_time = jiffies;
        long               next_min_timeout = MAX_SCHEDULE_TIMEOUT;
        long               current_min_timeout = 1;

        kportal_daemonize("kranal_reaper");
        kportal_blockallsigs();

        init_waitqueue_entry(&wait, current);

        spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);

        while (!kranal_data.kra_shutdown) {
                /* I wake up every 'p' seconds to check for timeouts on some
                 * more peers.  I try to check every connection 'n' times
                 * within the global minimum of all keepalive and timeout
                 * intervals, to ensure I attend to every connection within
                 * (n+1)/n times its timeout intervals. */
                const int     p = 1;
                const int     n = 3;
                unsigned long min_timeout;
                int           chunk;

                /* careful with the jiffy wrap... */
                timeout = (long)(next_check_time - jiffies);
                if (timeout > 0) {
                        set_current_state(TASK_INTERRUPTIBLE);
                        add_wait_queue(&kranal_data.kra_reaper_waitq, &wait);

                        spin_unlock_irqrestore(&kranal_data.kra_reaper_lock, flags);

                        schedule_timeout(timeout);

                        spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);

                        set_current_state(TASK_RUNNING);
                        remove_wait_queue(&kranal_data.kra_reaper_waitq, &wait);
                        continue;
                }

                if (kranal_data.kra_new_min_timeout != MAX_SCHEDULE_TIMEOUT) {
                        /* new min timeout set: restart min timeout scan */
                        next_min_timeout = MAX_SCHEDULE_TIMEOUT;
                        base_index = conn_index - 1;
                        if (base_index < 0)
                                base_index = conn_entries - 1;

                        if (kranal_data.kra_new_min_timeout < current_min_timeout) {
                                current_min_timeout = kranal_data.kra_new_min_timeout;
                                CDEBUG(D_NET, "Set new min timeout %ld\n",
                                       current_min_timeout);
                        }

                        kranal_data.kra_new_min_timeout = MAX_SCHEDULE_TIMEOUT;
                }
                min_timeout = current_min_timeout;

                spin_unlock_irqrestore(&kranal_data.kra_reaper_lock, flags);

                LASSERT (min_timeout > 0);

                /* Compute how many table entries to check now so I get round
                 * the whole table fast enough given that I do this at fixed
                 * intervals of 'p' seconds) */
                chunk = conn_entries;
                if (min_timeout > n * p)
                        chunk = (chunk * n * p) / min_timeout;
                if (chunk == 0)
                        chunk = 1;

                for (i = 0; i < chunk; i++) {
                        kranal_reaper_check(conn_index,
                                            &next_min_timeout);
                        conn_index = (conn_index + 1) % conn_entries;
                }

                next_check_time += p * HZ;

                spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);

                if (((conn_index - chunk <= base_index &&
                      base_index < conn_index) ||
                     (conn_index - conn_entries - chunk <= base_index &&
                      base_index < conn_index - conn_entries))) {

                        /* Scanned all conns: set current_min_timeout... */
                        if (current_min_timeout != next_min_timeout) {
                                current_min_timeout = next_min_timeout;
                                CDEBUG(D_NET, "Set new min timeout %ld\n",
                                       current_min_timeout);
                        }

                        /* ...and restart min timeout scan */
                        next_min_timeout = MAX_SCHEDULE_TIMEOUT;
                        base_index = conn_index - 1;
                        if (base_index < 0)
                                base_index = conn_entries - 1;
                }
        }

        kranal_thread_fini();
        return 0;
}

void
kranal_check_rdma_cq (kra_device_t *dev)
{
        kra_conn_t          *conn;
        kra_tx_t            *tx;
        RAP_RETURN           rrc;
        unsigned long        flags;
        RAP_RDMA_DESCRIPTOR *desc;
        __u32                cqid;
        __u32                event_type;

        for (;;) {
                rrc = RapkCQDone(dev->rad_rdma_cqh, &cqid, &event_type);
                if (rrc == RAP_NOT_DONE) {
                        CDEBUG(D_NET, "RDMA CQ %d empty\n", dev->rad_id);
                        return;
                }

                LASSERT (rrc == RAP_SUCCESS);
                LASSERT ((event_type & RAPK_CQ_EVENT_OVERRUN) == 0);

                read_lock(&kranal_data.kra_global_lock);

                conn = kranal_cqid2conn_locked(cqid);
                if (conn == NULL) {
                        /* Conn was destroyed? */
                        CDEBUG(D_NET, "RDMA CQID lookup %d failed\n", cqid);
                        read_unlock(&kranal_data.kra_global_lock);
                        continue;
                }

                rrc = RapkRdmaDone(conn->rac_rihandle, &desc);
                LASSERT (rrc == RAP_SUCCESS);

                CDEBUG(D_NET, "Completed %p\n",
                       list_entry(conn->rac_rdmaq.next, kra_tx_t, tx_list));

                spin_lock_irqsave(&conn->rac_lock, flags);

                LASSERT (!list_empty(&conn->rac_rdmaq));
                tx = list_entry(conn->rac_rdmaq.next, kra_tx_t, tx_list);
                list_del(&tx->tx_list);

                LASSERT(desc->AppPtr == (void *)tx);
                LASSERT(tx->tx_msg.ram_type == RANAL_MSG_PUT_DONE ||
                        tx->tx_msg.ram_type == RANAL_MSG_GET_DONE);

                list_add_tail(&tx->tx_list, &conn->rac_fmaq);
                tx->tx_qtime = jiffies;

                spin_unlock_irqrestore(&conn->rac_lock, flags);

                /* Get conn's fmaq processed, now I've just put something
                 * there */
                kranal_schedule_conn(conn);

                read_unlock(&kranal_data.kra_global_lock);
        }
}

void
kranal_check_fma_cq (kra_device_t *dev)
{
        kra_conn_t         *conn;
        RAP_RETURN          rrc;
        __u32               cqid;
        __u32               event_type;
        struct list_head   *conns;
        struct list_head   *tmp;
        int                 i;

        for (;;) {
                rrc = RapkCQDone(dev->rad_fma_cqh, &cqid, &event_type);
                if (rrc == RAP_NOT_DONE) {
                        CDEBUG(D_NET, "FMA CQ %d empty\n", dev->rad_id);
                        return;
                }

                LASSERT (rrc == RAP_SUCCESS);

                if ((event_type & RAPK_CQ_EVENT_OVERRUN) == 0) {

                        read_lock(&kranal_data.kra_global_lock);

                        conn = kranal_cqid2conn_locked(cqid);
                        if (conn == NULL) {
                                CDEBUG(D_NET, "FMA CQID lookup %d failed\n",
                                       cqid);
                        } else {
                                CDEBUG(D_NET, "FMA completed: %p CQID %d\n",
                                       conn, cqid);
                                kranal_schedule_conn(conn);
                        }

                        read_unlock(&kranal_data.kra_global_lock);
                        continue;
                }

                /* FMA CQ has overflowed: check ALL conns */
                CWARN("Scheduling ALL conns on device %d\n", dev->rad_id);

                for (i = 0; i < kranal_data.kra_conn_hash_size; i++) {

                        read_lock(&kranal_data.kra_global_lock);

                        conns = &kranal_data.kra_conns[i];

                        list_for_each (tmp, conns) {
                                conn = list_entry(tmp, kra_conn_t,
                                                  rac_hashlist);

                                if (conn->rac_device == dev)
                                        kranal_schedule_conn(conn);
                        }

                        /* don't block write lockers for too long... */
                        read_unlock(&kranal_data.kra_global_lock);
                }
        }
}

int
kranal_sendmsg(kra_conn_t *conn, kra_msg_t *msg,
               void *immediate, int immediatenob)
{
        int        sync = (msg->ram_type & RANAL_MSG_FENCE) != 0;
        RAP_RETURN rrc;

        CDEBUG(D_NET,"%p sending msg %p %02x%s [%p for %d]\n",
               conn, msg, msg->ram_type, sync ? "(sync)" : "",
               immediate, immediatenob);

        LASSERT (sizeof(*msg) <= RANAL_FMA_MAX_PREFIX);
        LASSERT ((msg->ram_type == RANAL_MSG_IMMEDIATE) ?
                 immediatenob <= RANAL_FMA_MAX_DATA :
                 immediatenob == 0);

        msg->ram_connstamp = conn->rac_my_connstamp;
        msg->ram_seq = conn->rac_tx_seq;

        if (sync)
                rrc = RapkFmaSyncSend(conn->rac_rihandle,
                                      immediate, immediatenob,
                                      msg, sizeof(*msg));
        else
                rrc = RapkFmaSend(conn->rac_rihandle,
                                  immediate, immediatenob,
                                  msg, sizeof(*msg));

        switch (rrc) {
        default:
                LBUG();

        case RAP_SUCCESS:
                conn->rac_last_tx = jiffies;
                conn->rac_tx_seq++;
                return 0;

        case RAP_NOT_DONE:
                if (time_after_eq(jiffies,
                                  conn->rac_last_tx + conn->rac_keepalive*HZ))
                        CDEBUG(D_WARNING, "EAGAIN sending %02x (idle %lu secs)\n",
                               msg->ram_type, (jiffies - conn->rac_last_tx)/HZ);
                return -EAGAIN;
        }
}

void
kranal_process_fmaq (kra_conn_t *conn)
{
        unsigned long flags;
        int           more_to_do;
        kra_tx_t     *tx;
        int           rc;
        int           expect_reply;

        /* NB 1. kranal_sendmsg() may fail if I'm out of credits right now.
         *       However I will be rescheduled some by an FMA completion event
         *       when I eventually get some.
         * NB 2. Sampling rac_state here races with setting it elsewhere.
         *       But it doesn't matter if I try to send a "real" message just
         *       as I start closing because I'll get scheduled to send the
         *       close anyway. */

        /* Not racing with incoming message processing! */
        LASSERT (current == conn->rac_device->rad_scheduler);

        if (conn->rac_state != RANAL_CONN_ESTABLISHED) {
                if (!list_empty(&conn->rac_rdmaq)) {
                        /* RDMAs in progress */
                        LASSERT (!conn->rac_close_sent);

                        if (time_after_eq(jiffies,
                                          conn->rac_last_tx +
                                          conn->rac_keepalive * HZ)) {
                                CDEBUG(D_NET, "sending NOOP (rdma in progress)\n");
                                kranal_init_msg(&conn->rac_msg, RANAL_MSG_NOOP);
                                kranal_sendmsg(conn, &conn->rac_msg, NULL, 0);
                        }
                        return;
                }

                if (conn->rac_close_sent)
                        return;

                CWARN("sending CLOSE to "LPX64"\n", conn->rac_peer->rap_nid);
                kranal_init_msg(&conn->rac_msg, RANAL_MSG_CLOSE);
                rc = kranal_sendmsg(conn, &conn->rac_msg, NULL, 0);
                if (rc != 0)
                        return;

                conn->rac_close_sent = 1;
                if (!conn->rac_close_recvd)
                        return;

                write_lock_irqsave(&kranal_data.kra_global_lock, flags);

                if (conn->rac_state == RANAL_CONN_CLOSING)
                        kranal_terminate_conn_locked(conn);

                write_unlock_irqrestore(&kranal_data.kra_global_lock, flags);
                return;
        }

        spin_lock_irqsave(&conn->rac_lock, flags);

        if (list_empty(&conn->rac_fmaq)) {

                spin_unlock_irqrestore(&conn->rac_lock, flags);

                if (time_after_eq(jiffies,
                                  conn->rac_last_tx + conn->rac_keepalive * HZ)) {
                        CDEBUG(D_NET, "sending NOOP -> "LPX64" (%p idle %lu(%ld))\n",
                               conn->rac_peer->rap_nid, conn,
                               (jiffies - conn->rac_last_tx)/HZ, conn->rac_keepalive);
                        kranal_init_msg(&conn->rac_msg, RANAL_MSG_NOOP);
                        kranal_sendmsg(conn, &conn->rac_msg, NULL, 0);
                }
                return;
        }

        tx = list_entry(conn->rac_fmaq.next, kra_tx_t, tx_list);
        list_del(&tx->tx_list);
        more_to_do = !list_empty(&conn->rac_fmaq);

        spin_unlock_irqrestore(&conn->rac_lock, flags);

        expect_reply = 0;
        CDEBUG(D_NET, "sending regular msg: %p, type %02x, cookie "LPX64"\n",
               tx, tx->tx_msg.ram_type, tx->tx_cookie);
        switch (tx->tx_msg.ram_type) {
        default:
                LBUG();

        case RANAL_MSG_IMMEDIATE:
                rc = kranal_sendmsg(conn, &tx->tx_msg,
                                    tx->tx_buffer, tx->tx_nob);
                expect_reply = 0;
                break;

        case RANAL_MSG_PUT_NAK:
        case RANAL_MSG_PUT_DONE:
        case RANAL_MSG_GET_NAK:
        case RANAL_MSG_GET_DONE:
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                expect_reply = 0;
                break;

        case RANAL_MSG_PUT_REQ:
                tx->tx_msg.ram_u.putreq.raprm_cookie = tx->tx_cookie;
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                kranal_map_buffer(tx);
                expect_reply = 1;
                break;

        case RANAL_MSG_PUT_ACK:
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                expect_reply = 1;
                break;

        case RANAL_MSG_GET_REQ:
                kranal_map_buffer(tx);
                tx->tx_msg.ram_u.get.ragm_cookie = tx->tx_cookie;
                tx->tx_msg.ram_u.get.ragm_desc.rard_key = tx->tx_map_key;
                tx->tx_msg.ram_u.get.ragm_desc.rard_addr.AddressBits =
                        (__u64)((unsigned long)tx->tx_buffer);
                tx->tx_msg.ram_u.get.ragm_desc.rard_nob = tx->tx_nob;
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                expect_reply = 1;
                break;
        }

        if (rc == -EAGAIN) {
                /* I need credits to send this.  Replace tx at the head of the
                 * fmaq and I'll get rescheduled when credits appear */
                CDEBUG(D_NET, "EAGAIN on %p\n", conn);
                spin_lock_irqsave(&conn->rac_lock, flags);
                list_add(&tx->tx_list, &conn->rac_fmaq);
                spin_unlock_irqrestore(&conn->rac_lock, flags);
                return;
        }

        LASSERT (rc == 0);

        if (!expect_reply) {
                kranal_tx_done(tx, 0);
        } else {
                /* LASSERT(current) above ensures this doesn't race with reply
                 * processing */
                spin_lock_irqsave(&conn->rac_lock, flags);
                list_add_tail(&tx->tx_list, &conn->rac_replyq);
                tx->tx_qtime = jiffies;
                spin_unlock_irqrestore(&conn->rac_lock, flags);
        }

        if (more_to_do) {
                CDEBUG(D_NET, "Rescheduling %p (more to do)\n", conn);
                kranal_schedule_conn(conn);
        }
}

static inline void
kranal_swab_rdma_desc (kra_rdma_desc_t *d)
{
        __swab64s(&d->rard_key.Key);
        __swab16s(&d->rard_key.Cookie);
        __swab16s(&d->rard_key.MdHandle);
        __swab32s(&d->rard_key.Flags);
        __swab64s(&d->rard_addr.AddressBits);
        __swab32s(&d->rard_nob);
}

kra_tx_t *
kranal_match_reply(kra_conn_t *conn, int type, __u64 cookie)
{
        struct list_head *ttmp;
        kra_tx_t         *tx;
        unsigned long     flags;

        spin_lock_irqsave(&conn->rac_lock, flags);

        list_for_each(ttmp, &conn->rac_replyq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);

                CDEBUG(D_NET,"Checking %p %02x/"LPX64"\n",
                       tx, tx->tx_msg.ram_type, tx->tx_cookie);

                if (tx->tx_cookie != cookie)
                        continue;

                if (tx->tx_msg.ram_type != type) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CWARN("Unexpected type %x (%x expected) "
                              "matched reply from "LPX64"\n",
                              tx->tx_msg.ram_type, type,
                              conn->rac_peer->rap_nid);
                        return NULL;
                }

                list_del(&tx->tx_list);
                spin_unlock_irqrestore(&conn->rac_lock, flags);
                return tx;
        }

        spin_unlock_irqrestore(&conn->rac_lock, flags);
        CWARN("Unmatched reply %02x/"LPX64" from "LPX64"\n",
              type, cookie, conn->rac_peer->rap_nid);
        return NULL;
}

void
kranal_check_fma_rx (kra_conn_t *conn)
{
        unsigned long flags;
        __u32         seq;
        kra_tx_t     *tx;
        kra_msg_t    *msg;
        void         *prefix;
        RAP_RETURN    rrc = RapkFmaGetPrefix(conn->rac_rihandle, &prefix);
        kra_peer_t   *peer = conn->rac_peer;

        if (rrc == RAP_NOT_DONE)
                return;

        CDEBUG(D_NET, "RX on %p\n", conn);

        LASSERT (rrc == RAP_SUCCESS);
        conn->rac_last_rx = jiffies;
        seq = conn->rac_rx_seq++;
        msg = (kra_msg_t *)prefix;

        /* stash message for portals callbacks they'll NULL
         * rac_rxmsg if they consume it */
        LASSERT (conn->rac_rxmsg == NULL);
        conn->rac_rxmsg = msg;

        if (msg->ram_magic != RANAL_MSG_MAGIC) {
                if (__swab32(msg->ram_magic) != RANAL_MSG_MAGIC) {
                        CERROR("Unexpected magic %08x from "LPX64"\n",
                               msg->ram_magic, peer->rap_nid);
                        goto out;
                }

                __swab32s(&msg->ram_magic);
                __swab16s(&msg->ram_version);
                __swab16s(&msg->ram_type);
                __swab64s(&msg->ram_srcnid);
                __swab64s(&msg->ram_connstamp);
                __swab32s(&msg->ram_seq);

                /* NB message type checked below; NOT here... */
                switch (msg->ram_type) {
                case RANAL_MSG_PUT_ACK:
                        kranal_swab_rdma_desc(&msg->ram_u.putack.rapam_desc);
                        break;

                case RANAL_MSG_GET_REQ:
                        kranal_swab_rdma_desc(&msg->ram_u.get.ragm_desc);
                        break;

                default:
                        break;
                }
        }

        if (msg->ram_version != RANAL_MSG_VERSION) {
                CERROR("Unexpected protocol version %d from "LPX64"\n",
                       msg->ram_version, peer->rap_nid);
                goto out;
        }

        if (msg->ram_srcnid != peer->rap_nid) {
                CERROR("Unexpected peer "LPX64" from "LPX64"\n",
                       msg->ram_srcnid, peer->rap_nid);
                goto out;
        }

        if (msg->ram_connstamp != conn->rac_peer_connstamp) {
                CERROR("Unexpected connstamp "LPX64"("LPX64
                       " expected) from "LPX64"\n",
                       msg->ram_connstamp, conn->rac_peer_connstamp,
                       peer->rap_nid);
                goto out;
        }

        if (msg->ram_seq != seq) {
                CERROR("Unexpected sequence number %d(%d expected) from "
                       LPX64"\n", msg->ram_seq, seq, peer->rap_nid);
                goto out;
        }

        if ((msg->ram_type & RANAL_MSG_FENCE) != 0) {
                /* This message signals RDMA completion... */
                rrc = RapkFmaSyncWait(conn->rac_rihandle);
                LASSERT (rrc == RAP_SUCCESS);
        }

        if (conn->rac_close_recvd) {
                CERROR("Unexpected message %d after CLOSE from "LPX64"\n",
                       msg->ram_type, conn->rac_peer->rap_nid);
                goto out;
        }

        if (msg->ram_type == RANAL_MSG_CLOSE) {
                CWARN("RX CLOSE from "LPX64"\n", conn->rac_peer->rap_nid);
                conn->rac_close_recvd = 1;
                write_lock_irqsave(&kranal_data.kra_global_lock, flags);

                if (conn->rac_state == RANAL_CONN_ESTABLISHED)
                        kranal_close_conn_locked(conn, 0);
                else if (conn->rac_state == RANAL_CONN_CLOSING &&
                         conn->rac_close_sent)
                        kranal_terminate_conn_locked(conn);

                write_unlock_irqrestore(&kranal_data.kra_global_lock, flags);
                goto out;
        }

        if (conn->rac_state != RANAL_CONN_ESTABLISHED)
                goto out;

        switch (msg->ram_type) {
        case RANAL_MSG_NOOP:
                /* Nothing to do; just a keepalive */
                CDEBUG(D_NET, "RX NOOP on %p\n", conn);
                break;

        case RANAL_MSG_IMMEDIATE:
                CDEBUG(D_NET, "RX IMMEDIATE on %p\n", conn);
                ptl_parse(kranal_data.kra_ni, &msg->ram_u.immediate.raim_hdr, conn);
                break;

        case RANAL_MSG_PUT_REQ:
                CDEBUG(D_NET, "RX PUT_REQ on %p\n", conn);
                ptl_parse(kranal_data.kra_ni, &msg->ram_u.putreq.raprm_hdr, conn);

                if (conn->rac_rxmsg == NULL)    /* ptl_parse matched something */
                        break;

                tx = kranal_new_tx_msg(0, RANAL_MSG_PUT_NAK);
                if (tx == NULL)
                        break;

                tx->tx_msg.ram_u.completion.racm_cookie =
                        msg->ram_u.putreq.raprm_cookie;
                kranal_post_fma(conn, tx);
                break;

        case RANAL_MSG_PUT_NAK:
                CDEBUG(D_NET, "RX PUT_NAK on %p\n", conn);
                tx = kranal_match_reply(conn, RANAL_MSG_PUT_REQ,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;

                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, -ENOENT);    /* no match */
                break;

        case RANAL_MSG_PUT_ACK:
                CDEBUG(D_NET, "RX PUT_ACK on %p\n", conn);
                tx = kranal_match_reply(conn, RANAL_MSG_PUT_REQ,
                                        msg->ram_u.putack.rapam_src_cookie);
                if (tx == NULL)
                        break;

                kranal_rdma(tx, RANAL_MSG_PUT_DONE,
                            &msg->ram_u.putack.rapam_desc,
                            msg->ram_u.putack.rapam_desc.rard_nob,
                            msg->ram_u.putack.rapam_dst_cookie);
                break;

        case RANAL_MSG_PUT_DONE:
                CDEBUG(D_NET, "RX PUT_DONE on %p\n", conn);
                tx = kranal_match_reply(conn, RANAL_MSG_PUT_ACK,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;

                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, 0);
                break;

        case RANAL_MSG_GET_REQ:
                CDEBUG(D_NET, "RX GET_REQ on %p\n", conn);
                ptl_parse(kranal_data.kra_ni, &msg->ram_u.get.ragm_hdr, conn);

                if (conn->rac_rxmsg == NULL)    /* ptl_parse matched something */
                        break;

                tx = kranal_new_tx_msg(0, RANAL_MSG_GET_NAK);
                if (tx == NULL)
                        break;

                tx->tx_msg.ram_u.completion.racm_cookie = msg->ram_u.get.ragm_cookie;
                kranal_post_fma(conn, tx);
                break;

        case RANAL_MSG_GET_NAK:
                CDEBUG(D_NET, "RX GET_NAK on %p\n", conn);
                tx = kranal_match_reply(conn, RANAL_MSG_GET_REQ,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;

                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, -ENOENT);    /* no match */
                break;

        case RANAL_MSG_GET_DONE:
                CDEBUG(D_NET, "RX GET_DONE on %p\n", conn);
                tx = kranal_match_reply(conn, RANAL_MSG_GET_REQ,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;

                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, 0);
                break;
        }

 out:
        if (conn->rac_rxmsg != NULL)
                kranal_consume_rxmsg(conn, NULL, 0);

        /* check again later */
        kranal_schedule_conn(conn);
}

void
kranal_complete_closed_conn (kra_conn_t *conn)
{
        kra_tx_t   *tx;
        int         nfma;
        int         nreplies;

        LASSERT (conn->rac_state == RANAL_CONN_CLOSED);
        LASSERT (list_empty(&conn->rac_list));
        LASSERT (list_empty(&conn->rac_hashlist));

        for (nfma = 0; !list_empty(&conn->rac_fmaq); nfma++) {
                tx = list_entry(conn->rac_fmaq.next, kra_tx_t, tx_list);

                list_del(&tx->tx_list);
                kranal_tx_done(tx, -ECONNABORTED);
        }

        LASSERT (list_empty(&conn->rac_rdmaq));

        for (nreplies = 0; !list_empty(&conn->rac_replyq); nreplies++) {
                tx = list_entry(conn->rac_replyq.next, kra_tx_t, tx_list);

                list_del(&tx->tx_list);
                kranal_tx_done(tx, -ECONNABORTED);
        }

        CDEBUG(D_WARNING, "Closed conn %p -> "LPX64": nmsg %d nreplies %d\n",
               conn, conn->rac_peer->rap_nid, nfma, nreplies);
}

int
kranal_process_new_conn (kra_conn_t *conn)
{
        RAP_RETURN   rrc;
        
        rrc = RapkCompleteSync(conn->rac_rihandle, 1);
        if (rrc == RAP_SUCCESS)
                return 0;

        LASSERT (rrc == RAP_NOT_DONE);
        if (!time_after_eq(jiffies, conn->rac_last_tx + 
                           conn->rac_timeout * HZ))
                return -EAGAIN;

        /* Too late */
        rrc = RapkCompleteSync(conn->rac_rihandle, 0);
        LASSERT (rrc == RAP_SUCCESS);
        return -ETIMEDOUT;
}

int
kranal_scheduler (void *arg)
{
        kra_device_t     *dev = (kra_device_t *)arg;
        wait_queue_t      wait;
        char              name[16];
        kra_conn_t       *conn;
        unsigned long     flags;
        unsigned long     deadline;
        unsigned long     soonest;
        int               nsoonest;
        long              timeout;
        struct list_head *tmp;
        struct list_head *nxt;
        int               rc;
        int               dropped_lock;
        int               busy_loops = 0;

        snprintf(name, sizeof(name), "kranal_sd_%02d", dev->rad_idx);
        kportal_daemonize(name);
        kportal_blockallsigs();

        dev->rad_scheduler = current;
        init_waitqueue_entry(&wait, current);

        spin_lock_irqsave(&dev->rad_lock, flags);

        while (!kranal_data.kra_shutdown) {
                /* Safe: kra_shutdown only set when quiescent */

                if (busy_loops++ >= RANAL_RESCHED) {
                        spin_unlock_irqrestore(&dev->rad_lock, flags);

                        our_cond_resched();
                        busy_loops = 0;

                        spin_lock_irqsave(&dev->rad_lock, flags);
                }

                dropped_lock = 0;

                if (dev->rad_ready) {
                        /* Device callback fired since I last checked it */
                        dev->rad_ready = 0;
                        spin_unlock_irqrestore(&dev->rad_lock, flags);
                        dropped_lock = 1;

                        kranal_check_rdma_cq(dev);
                        kranal_check_fma_cq(dev);

                        spin_lock_irqsave(&dev->rad_lock, flags);
                }

                list_for_each_safe(tmp, nxt, &dev->rad_ready_conns) {
                        conn = list_entry(tmp, kra_conn_t, rac_schedlist);

                        list_del_init(&conn->rac_schedlist);
                        LASSERT (conn->rac_scheduled);
                        conn->rac_scheduled = 0;
                        spin_unlock_irqrestore(&dev->rad_lock, flags);
                        dropped_lock = 1;

                        kranal_check_fma_rx(conn);
                        kranal_process_fmaq(conn);

                        if (conn->rac_state == RANAL_CONN_CLOSED)
                                kranal_complete_closed_conn(conn);

                        kranal_conn_decref(conn);
                        spin_lock_irqsave(&dev->rad_lock, flags);
                }

                nsoonest = 0;
                soonest = jiffies;

                list_for_each_safe(tmp, nxt, &dev->rad_new_conns) {
                        conn = list_entry(tmp, kra_conn_t, rac_schedlist);
                        
                        deadline = conn->rac_last_tx + conn->rac_keepalive;
                        if (time_after_eq(jiffies, deadline)) {
                                /* Time to process this new conn */
                                spin_unlock_irqrestore(&dev->rad_lock, flags);
                                dropped_lock = 1;

                                rc = kranal_process_new_conn(conn);
                                if (rc != -EAGAIN) {
                                        /* All done with this conn */
                                        spin_lock_irqsave(&dev->rad_lock, flags);
                                        list_del_init(&conn->rac_schedlist);
                                        spin_unlock_irqrestore(&dev->rad_lock, flags);

                                        kranal_conn_decref(conn);
                                        spin_lock_irqsave(&dev->rad_lock, flags);
                                        continue;
                                }

                                /* retry with exponential backoff until HZ */
                                if (conn->rac_keepalive == 0)
                                        conn->rac_keepalive = 1;
                                else if (conn->rac_keepalive <= HZ)
                                        conn->rac_keepalive *= 2;
                                else
                                        conn->rac_keepalive += HZ;
                                
                                deadline = conn->rac_last_tx + conn->rac_keepalive;
                                spin_lock_irqsave(&dev->rad_lock, flags);
                        }

                        /* Does this conn need attention soonest? */
                        if (nsoonest++ == 0 ||
                            !time_after_eq(deadline, soonest))
                                soonest = deadline;
                }

                if (dropped_lock)               /* may sleep iff I didn't drop the lock */
                        continue;

                set_current_state(TASK_INTERRUPTIBLE);
                add_wait_queue(&dev->rad_waitq, &wait);
                spin_unlock_irqrestore(&dev->rad_lock, flags);

                if (nsoonest == 0) {
                        busy_loops = 0;
                        schedule();
                } else {
                        timeout = (long)(soonest - jiffies);
                        if (timeout > 0) {
                                busy_loops = 0;
                                schedule_timeout(timeout);
                        }
                }

                remove_wait_queue(&dev->rad_waitq, &wait);
                set_current_state(TASK_RUNNING);
                spin_lock_irqsave(&dev->rad_lock, flags);
        }

        spin_unlock_irqrestore(&dev->rad_lock, flags);

        dev->rad_scheduler = NULL;
        kranal_thread_fini();
        return 0;
}
