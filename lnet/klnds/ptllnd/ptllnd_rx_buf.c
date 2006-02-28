/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2005 Cluster File Systems, Inc. All rights reserved.
 *   Author: PJ Kirner <pjkirner@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   This file is confidential source code owned by Cluster File Systems.
 *   No viewing, modification, compilation, redistribution, or any other
 *   form of use is permitted except through a signed license agreement.
 *
 *   If you have not signed such an agreement, then you have no rights to
 *   this file.  Please destroy it immediately and contact CFS.
 *
 */

 #include "ptllnd.h"

void
kptllnd_rx_buffer_pool_init(kptl_rx_buffer_pool_t *rxbp)
{
        memset(rxbp, 0, sizeof(*rxbp));
        spin_lock_init(&rxbp->rxbp_lock);
        INIT_LIST_HEAD(&rxbp->rxbp_list);
}

void
kptllnd_rx_buffer_destroy(kptl_rx_buffer_t *rxb)
{
        kptl_rx_buffer_pool_t *rxbp = rxb->rxb_pool;

        LASSERT(rxb->rxb_refcount == 0);
        LASSERT(PtlHandleIsEqual(rxb->rxb_mdh, PTL_INVALID_HANDLE));
        LASSERT(!rxb->rxb_posted);
        LASSERT(rxb->rxb_idle);

        list_del(&rxb->rxb_list);
        rxbp->rxbp_count--;

        LIBCFS_FREE(rxb->rxb_buffer, kptllnd_rx_buffer_size());
        LIBCFS_FREE(rxb, sizeof(*rxb));
}

int
kptllnd_rx_buffer_pool_reserve(kptl_rx_buffer_pool_t *rxbp, int count)
{
        int               bufsize;
        int               msgs_per_buffer;
        int               rc;
        kptl_rx_buffer_t *rxb;
        char             *buffer;
        unsigned long     flags;

        bufsize = kptllnd_rx_buffer_size();
        msgs_per_buffer = bufsize / (*kptllnd_tunables.kptl_max_msg_size);

        CDEBUG(D_NET, "kptllnd_rx_buffer_pool_reserve(%d)\n", count);

        spin_lock_irqsave(&rxbp->rxbp_lock, flags);

        for (;;) {
                if (rxbp->rxbp_shutdown) {
                        rc = -ESHUTDOWN;
                        break;
                }
                
                if (rxbp->rxbp_reserved + count <= 
                    rxbp->rxbp_count * msgs_per_buffer) {
                        rc = 0;
                        break;
                }
                
                spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
                
                LIBCFS_ALLOC(rxb, sizeof(*rxb));
                LIBCFS_ALLOC(buffer, bufsize);

                if (rxb == NULL || buffer == NULL) {
                        CERROR("Failed to allocate rx buffer\n");

                        if (rxb != NULL)
                                LIBCFS_FREE(rxb, sizeof(*rxb));
                        if (buffer != NULL)
                                LIBCFS_FREE(buffer, bufsize);
                        
                        spin_lock_irqsave(&rxbp->rxbp_lock, flags);
                        rc = -ENOMEM;
                        break;
                }

                memset(rxb, 0, sizeof(*rxb));

                rxb->rxb_eventarg.eva_type = PTLLND_EVENTARG_TYPE_BUF;
                rxb->rxb_refcount = 0;
                rxb->rxb_pool = rxbp;
                rxb->rxb_idle = 0;
                rxb->rxb_posted = 0;
                rxb->rxb_buffer = buffer;
                rxb->rxb_mdh = PTL_INVALID_HANDLE;

                spin_lock_irqsave(&rxbp->rxbp_lock, flags);
                
                if (rxbp->rxbp_shutdown) {
                        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
                        
                        LIBCFS_FREE(rxb, sizeof(*rxb));
                        LIBCFS_FREE(buffer, bufsize);

                        spin_lock_irqsave(&rxbp->rxbp_lock, flags);
                        rc = -ESHUTDOWN;
                        break;
                }
                
                list_add_tail(&rxb->rxb_list, &rxbp->rxbp_list);
                rxbp->rxbp_count++;

                spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
                
                kptllnd_rx_buffer_post(rxb);

                spin_lock_irqsave(&rxbp->rxbp_lock, flags);
        }

        if (rc == 0)
                rxbp->rxbp_reserved += count;

        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);

        return rc;
}

void
kptllnd_rx_buffer_pool_unreserve(kptl_rx_buffer_pool_t *rxbp,
                                 int count)
{
        unsigned long flags;

        spin_lock_irqsave(&rxbp->rxbp_lock, flags);

        CDEBUG(D_NET, "kptllnd_rx_buffer_pool_unreserve(%d)\n", count);
        rxbp->rxbp_reserved -= count;

        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
}

void
kptllnd_rx_buffer_pool_fini(kptl_rx_buffer_pool_t *rxbp)
{
        kptl_rx_buffer_t       *rxb;
        int                     rc;
        int                     i;
        unsigned long           flags;
        struct list_head       *tmp;
        struct list_head       *nxt;
        ptl_handle_md_t         mdh;

        spin_lock_irqsave(&rxbp->rxbp_lock, flags);
        rxbp->rxbp_shutdown = 1;
        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);

        /* CAVEAT EMPTOR: I'm racing with everything here!!!  
         *
         * Buffers can still be posted after I set rxbp_shutdown because I
         * can't hold rxbp_lock while I'm posting them.
         *
         * Calling PtlMDUnlink() here races with auto-unlinks; i.e. a buffer's
         * MD handle could become invalid under me.  I am vulnerable to portals
         * re-using handles (i.e. make the same handle valid again, but for a
         * different MD) from when the MD is actually unlinked, to when the
         * event callback tells me it has been unlinked. */

        for (i = 3;; i++) {
                spin_lock_irqsave(&rxbp->rxbp_lock, flags);

                list_for_each_safe(tmp, nxt, &rxbp->rxbp_list) {
                        rxb = list_entry (tmp, kptl_rx_buffer_t, rxb_list);
                
                        if (rxb->rxb_idle) {
                                spin_unlock_irqrestore(&rxbp->rxbp_lock, 
                                                       flags);
                                kptllnd_rx_buffer_destroy(rxb);
                                spin_lock_irqsave(&rxbp->rxbp_lock, 
                                                  flags);
                                continue;
                        }

                        mdh = rxb->rxb_mdh;
                        if (PtlHandleIsEqual(mdh, PTL_INVALID_HANDLE))
                                continue;
                        
                        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);

                        rc = PtlMDUnlink(mdh);

                        spin_lock_irqsave(&rxbp->rxbp_lock, flags);
                        
#ifdef LUSTRE_PORTALS_UNLINK_SEMANTICS
                        /* callback clears rxb_mdh and drops net's ref
                         * (which causes repost, but since I set
                         * shutdown, it will just set the buffer
                         * idle) */
#else
                        if (rc == PTL_OK) {
                                rxb->rxb_posted = 0;
                                rxb->rxb_mdh = PTL_INVALID_HANDLE;
                                kptllnd_rx_buffer_decref_locked(rxb);
                        }
#endif
                }

                if (list_empty(&rxbp->rxbp_list))
                        break;

                /* Wait a bit for references to be dropped */
                CDEBUG(((i & (-i)) == i) ? D_NET : D_NET, /* power of 2? */
                       "Waiting for %d Busy RX Buffers\n",
                       rxbp->rxbp_count);

                cfs_pause(cfs_time_seconds(1));
        }
}

void
kptllnd_rx_buffer_post(kptl_rx_buffer_t *rxb)
{
        int                     rc;
        ptl_md_t                md;
        ptl_handle_me_t         meh;
        ptl_handle_md_t         mdh;
        ptl_process_id_t        any;
        kptl_rx_buffer_pool_t  *rxbp = rxb->rxb_pool;
        unsigned long           flags;

        LASSERT (!in_interrupt());
        LASSERT (rxb->rxb_refcount == 0);
        LASSERT (!rxb->rxb_idle);
        LASSERT (!rxb->rxb_posted);
        LASSERT (PtlHandleIsEqual(rxb->rxb_mdh, PTL_INVALID_HANDLE));

        any.nid = PTL_NID_ANY;
        any.pid = PTL_PID_ANY;

        spin_lock_irqsave(&rxbp->rxbp_lock, flags);

        if (rxbp->rxbp_shutdown) {
                rxb->rxb_idle = 1;
                spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
                return;
        }

        rxb->rxb_refcount = 1;                  /* net's ref */
        rxb->rxb_posted = 1;                    /* I'm posting */
        
        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);

        rc = PtlMEAttach(kptllnd_data.kptl_nih,
                         *kptllnd_tunables.kptl_portal,
                         any,
                         LNET_MSG_MATCHBITS,
                         0, /* all matchbits are valid - ignore none */
                         PTL_UNLINK,
                         PTL_INS_AFTER,
                         &meh);
        if (rc != PTL_OK) {
                CERROR("PtlMeAttach rxb failed %d\n", rc);
                goto failed;
        }

        /*
         * Setup MD
         */
        md.start = rxb->rxb_buffer;
        md.length = PAGE_SIZE * *kptllnd_tunables.kptl_rxb_npages;
        md.threshold = PTL_MD_THRESH_INF;
        md.options = PTL_MD_OP_PUT;
        md.options |= PTL_MD_LUSTRE_COMPLETION_SEMANTICS;
        md.options |= PTL_MD_EVENT_START_DISABLE;
        md.options |= PTL_MD_MAX_SIZE;
        md.user_ptr = &rxb->rxb_eventarg;
        md.max_size = *kptllnd_tunables.kptl_max_msg_size;
        md.eq_handle = kptllnd_data.kptl_eqh;

        rc = PtlMDAttach(meh, md, PTL_UNLINK, &mdh);
        if (rc == PTL_OK) {
                spin_lock_irqsave(&rxbp->rxbp_lock, flags);
                if (rxb->rxb_posted)            /* Not auto-unlinked yet!!! */
                        rxb->rxb_mdh = mdh;
                spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
                return;
        }
        
        CERROR("PtlMDAttach rxb failed %d\n", rc);
        rc = PtlMEUnlink(meh);
        LASSERT(rc == PTL_OK);

 failed:
        spin_lock_irqsave(&rxbp->rxbp_lock, flags);
        rxb->rxb_posted = 0;
        /* XXX this will just try again immediately */
        kptllnd_rx_buffer_decref_locked(rxb);
        spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
}

kptl_rx_t *
kptllnd_rx_alloc(void)
{
        kptl_rx_t* rx;

        if (IS_SIMULATION_ENABLED(FAIL_RX_ALLOC)) {
                CERROR ("FAIL_RX_ALLOC SIMULATION triggered\n");
                return NULL;
        }

        rx = cfs_mem_cache_alloc(kptllnd_data.kptl_rx_cache, CFS_ALLOC_ATOMIC);
        if (rx == NULL) {
                CERROR("Failed to allocate rx\n");
                return NULL;
        }

        memset(rx, 0, sizeof(*rx));
        return rx;
}

void
kptllnd_rx_done(kptl_rx_t *rx)
{
        kptl_rx_buffer_t *rxb = rx->rx_rxb;
        kptl_peer_t      *peer = rx->rx_peer;
        unsigned long     flags;

        CDEBUG(D_NET, "rx=%p rxb %p peer %p\n", rx, rxb, peer);

        kptllnd_rx_buffer_decref(rxb);

        if (peer != NULL) {
                /* Update credits (after I've decref-ed the buffer) */
                spin_lock_irqsave(&peer->peer_lock, flags);

                peer->peer_outstanding_credits++;
                LASSERT (peer->peer_outstanding_credits <=
                         *kptllnd_tunables.kptl_peercredits);

                spin_unlock_irqrestore(&peer->peer_lock, flags);

                CDEBUG(D_NET, "Peer=%s Credits=%d Outstanding=%d\n", 
                       libcfs_nid2str(peer->peer_nid), 
                       peer->peer_credits, peer->peer_outstanding_credits);

                /* I might have to send back credits */
                kptllnd_peer_check_sends(peer);
                kptllnd_peer_decref(peer);
        }

        cfs_mem_cache_free(kptllnd_data.kptl_rx_cache, rx);
}

void
kptllnd_rx_buffer_callback (ptl_event_t *ev)
{
        kptl_eventarg_t        *eva = ev->md.user_ptr;
        kptl_rx_buffer_t       *rxb = kptllnd_eventarg2obj(eva);
        kptl_rx_buffer_pool_t  *rxbp = rxb->rxb_pool;
        kptl_rx_t              *rx;
        int                     unlinked;
        unsigned long           flags;

#ifdef LUSTRE_PORTALS_UNLINK_SEMANTICS
        unlinked = ev->unlinked;
#else
        unlinked = ev->type == PTL_EVENT_UNLINK;
#endif

        CDEBUG(D_NET, "RXB Callback %s(%d) rxb=%p id="FMT_NID
               " unlink=%d rc %d\n",
               kptllnd_evtype2str(ev->type), ev->type,
               rxb, ev->initiator.nid, unlinked, ev->ni_fail_type);

        LASSERT (!rxb->rxb_idle);
        LASSERT (ev->md.start == rxb->rxb_buffer);
        LASSERT (ev->offset + ev->mlength <= 
                 PAGE_SIZE * *kptllnd_tunables.kptl_rxb_npages);
        LASSERT (ev->type == PTL_EVENT_PUT_END || 
                 ev->type == PTL_EVENT_UNLINK);
        LASSERT (ev->type == PTL_EVENT_UNLINK ||
                 ev->match_bits == LNET_MSG_MATCHBITS);

        if (ev->ni_fail_type != PTL_NI_OK)
                CERROR("event type %d, status %d from "FMT_NID"\n",
                       ev->type, ev->ni_fail_type, ev->initiator.nid);

        if (ev->type == PTL_EVENT_PUT_END &&
            ev->ni_fail_type == PTL_NI_OK &&
            !rxbp->rxbp_shutdown) {

                /* rxbp_shutdown sampled without locking!  I only treat it as a
                 * hint since shutdown can start while rx's are queued on
                 * kptl_sched_rxq. */

                rx = kptllnd_rx_alloc();
                if (rx == NULL) {
                        CERROR("Message from "FMT_NID" dropped: ENOMEM",
                               ev->initiator.nid);
                } else {
                        kptllnd_rx_buffer_addref(rxb);

                        rx->rx_rxb = rxb;
                        rx->rx_nob = ev->mlength;
                        rx->rx_msg = (kptl_msg_t *)(rxb->rxb_buffer + ev->offset);
                        rx->rx_initiator = ev->initiator;
#if CRAY_XT3
                        rx->rx_uid = ev->uid;
#endif
                        /* Queue for attention */
                        spin_lock_irqsave(&kptllnd_data.kptl_sched_lock, 
                                          flags);

                        list_add_tail(&rx->rx_list, 
                                      &kptllnd_data.kptl_sched_rxq);
                        wake_up(&kptllnd_data.kptl_sched_waitq);

                        spin_unlock_irqrestore(&kptllnd_data.kptl_sched_lock, 
                                               flags);
                }
        }

        if (unlinked) {
                spin_lock_irqsave(&rxbp->rxbp_lock, flags);

                rxb->rxb_posted = 0;
                rxb->rxb_mdh = PTL_INVALID_HANDLE;
                kptllnd_rx_buffer_decref_locked(rxb);

                spin_unlock_irqrestore(&rxbp->rxbp_lock, flags);
        }
}

void
kptllnd_version_nak (kptl_rx_t *rx)
{
        /* Fire-and-forget a stub message that will let the peer know my
         * protocol magic/version */
        static struct {
                __u32      magic;
                __u16      version;
        } version_reply = {
                .magic       = PTLLND_MSG_MAGIC,
                .version     = PTLLND_MSG_VERSION};

        static ptl_md_t md = {
                .start        = &version_reply,
                .length       = sizeof(version_reply),
                .threshold    = 1,
                .options      = 0,
                .user_ptr     = NULL,
                .eq_handle    = PTL_EQ_NONE};

        ptl_handle_md_t   mdh;
        int               rc;

        rc = PtlMDBind(kptllnd_data.kptl_nih, md, PTL_UNLINK, &mdh);
        if (rc != PTL_OK) {
                CERROR("Can't version NAK "FMT_NID"/%d: bind failed %d\n",
                       rx->rx_initiator.nid, rx->rx_initiator.pid, rc);
                return;
        }

        rc = PtlPut(mdh, PTL_NOACK_REQ, rx->rx_initiator,
                    *kptllnd_tunables.kptl_portal, 0,
                    LNET_MSG_MATCHBITS, 0, 0);

        if (rc != PTL_OK)
                CERROR("Can't version NAK "FMT_NID"/%d: put failed %d\n",
                       rx->rx_initiator.nid, rx->rx_initiator.pid, rc);
}

void
kptllnd_rx_parse(kptl_rx_t *rx)
{
        kptl_msg_t             *msg = rx->rx_msg;
        kptl_peer_t            *peer;
        int                     rc;
        unsigned long           flags;

        LASSERT (rx->rx_peer == NULL);

        CDEBUG (D_NET, "rx=%p nob=%d "FMT_NID"/%d\n",
                rx, rx->rx_nob, rx->rx_initiator.nid, rx->rx_initiator.pid);

        if ((rx->rx_nob >= 4 &&
             (msg->ptlm_magic == LNET_PROTO_MAGIC ||
              msg->ptlm_magic == __swab32(LNET_PROTO_MAGIC))) ||
            (rx->rx_nob >= 6 &&
             ((msg->ptlm_magic == PTLLND_MSG_MAGIC &&
               msg->ptlm_version != PTLLND_MSG_VERSION) ||
              (msg->ptlm_magic == __swab32(PTLLND_MSG_MAGIC) &&
               msg->ptlm_version != __swab16(PTLLND_MSG_VERSION))))) {
                /* Future protocol compatibility support!
                 * When LNET unifies protocols over all LNDs, or if the
                 * ptllnd-specific protocol changes, it will expect "old" peers
                 * to reply with a stub message containing their
                 * magic/version. */
                kptllnd_version_nak(rx);
                goto rx_done;
        }
        
        rc = kptllnd_msg_unpack(msg, rx->rx_nob);
        if (rc != 0) {
                CERROR ("Error %d unpacking rx from "FMT_NID"/%d\n",
                        rc, rx->rx_initiator.nid, rx->rx_initiator.pid);
                goto rx_done;
        }

        CDEBUG(D_NET, "rx=%p type=%s(%d) nob %d cred %d seq "LPX64"\n",
               rx, kptllnd_msgtype2str(msg->ptlm_type), msg->ptlm_type,
               msg->ptlm_nob, msg->ptlm_credits, msg->ptlm_seq);

        if (msg->ptlm_type == PTLLND_MSG_TYPE_HELLO) {

                peer = kptllnd_peer_handle_hello(rx->rx_initiator, msg);
                if (peer == NULL) {
                        CERROR ("Failed to create peer for "FMT_NID"/%d\n",
                                rx->rx_initiator.nid, rx->rx_initiator.pid);
                        goto rx_done;
                }

                rx->rx_peer = peer;             /* rx takes my ref on peer */

                if (!(msg->ptlm_dststamp == kptllnd_data.kptl_incarnation ||
                      msg->ptlm_dststamp == 0)) {
                        CERROR("Stale rx from %s dststamp "LPX64" expected "LPX64"\n",
                               libcfs_nid2str(peer->peer_nid),
                               msg->ptlm_dststamp,
                               kptllnd_data.kptl_incarnation);
                        goto failed;
                }
        } else {
                peer = kptllnd_ptlnid2peer(rx->rx_initiator.nid);
                if (peer == NULL) {
                        CERROR("No connection with "FMT_NID"/%d\n",
                               rx->rx_initiator.nid, rx->rx_initiator.pid);
                        goto rx_done;
                }

                rx->rx_peer = peer;             /* rx takes my ref on peer */

                if (msg->ptlm_dststamp != kptllnd_data.kptl_incarnation) {
                        CERROR("Stale rx from %s dststamp "LPX64" expected "LPX64"\n",
                               libcfs_nid2str(peer->peer_nid), msg->ptlm_dststamp,
                               kptllnd_data.kptl_incarnation);
                        goto failed;
                }
        }

        if (msg->ptlm_srcnid != peer->peer_nid) {
                CERROR("Bad rx srcnid %s expected %s\n",
                       libcfs_nid2str(msg->ptlm_srcnid),
                       libcfs_nid2str(peer->peer_nid));
                goto failed;
        }

        if (msg->ptlm_srcstamp != peer->peer_incarnation) {
                CERROR ("Stale rx from %s srcstamp "LPX64" expected "LPX64"\n",
                        libcfs_nid2str(peer->peer_nid),
                        msg->ptlm_srcstamp,
                        peer->peer_incarnation);
                goto failed;
        }

        if (msg->ptlm_dstnid != kptllnd_data.kptl_ni->ni_nid) {
                CERROR ("Bad rx from %s dstnid %s expected %s\n",
                        libcfs_nid2str(peer->peer_nid),
                        libcfs_nid2str(msg->ptlm_dstnid),
                        libcfs_nid2str(kptllnd_data.kptl_ni->ni_nid));
                goto failed;
        }

        /* NB msg->ptlm_seq is ignored; it's only a debugging aid */

        if (msg->ptlm_credits != 0) {
                spin_lock_irqsave(&peer->peer_lock, flags);

                peer->peer_credits += msg->ptlm_credits;
                LASSERT (peer->peer_credits <=
                         *kptllnd_tunables.kptl_peercredits);

                spin_unlock_irqrestore(&peer->peer_lock, flags);

                kptllnd_peer_check_sends(peer);
        }

        switch (msg->ptlm_type) {
        default:
                CERROR("Bad PTL message type %x from %s\n",
                       msg->ptlm_type, libcfs_nid2str(rx->rx_peer->peer_nid));
                goto failed;

        case PTLLND_MSG_TYPE_HELLO:
                CDEBUG(D_NET, "PTLLND_MSG_TYPE_HELLO\n");
                goto rx_done;

        case PTLLND_MSG_TYPE_NOOP:
                CDEBUG(D_NET, "PTLLND_MSG_TYPE_NOOP\n");
                goto rx_done;

        case PTLLND_MSG_TYPE_IMMEDIATE:
                CDEBUG(D_NET, "PTLLND_MSG_TYPE_IMMEDIATE\n");
                rc = lnet_parse(kptllnd_data.kptl_ni,
                                &msg->ptlm_u.immediate.kptlim_hdr,
                                msg->ptlm_srcnid,
                                rx, 0);
                if (rc >= 0)                    /* kptllnd_recv owns 'rx' now */
                        return;
                goto failed;
                
        case PTLLND_MSG_TYPE_PUT:
        case PTLLND_MSG_TYPE_GET:
                CDEBUG(D_NET, "PTLLND_MSG_TYPE_%s\n",
                        msg->ptlm_type == PTLLND_MSG_TYPE_PUT ?
                        "PUT" : "GET");
                /* Update last match bits seen */
                spin_lock_irqsave(&rx->rx_peer->peer_lock, flags);

                if (msg->ptlm_u.rdma.kptlrm_matchbits >
                    rx->rx_peer->peer_last_matchbits_seen)
                        rx->rx_peer->peer_last_matchbits_seen =
                                msg->ptlm_u.rdma.kptlrm_matchbits;

                spin_unlock_irqrestore(&rx->rx_peer->peer_lock, flags);

                rc = lnet_parse(kptllnd_data.kptl_ni,
                                &msg->ptlm_u.rdma.kptlrm_hdr,
                                msg->ptlm_srcnid,
                                rx, 1);
                if (rc >= 0)                    /* kptllnd_recv owns 'rx' now */
                        return;
                goto failed;
         }

 failed:
        kptllnd_peer_close(peer);
rx_done:
        kptllnd_rx_done(rx);
}
