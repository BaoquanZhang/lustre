/*
 * Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 * Copyright (C) 2002, Lawrence Livermore National Labs (LLNL)
 * W. Marcus Miller - Based on ksocknal
 *
 * This file is part of Portals, http://www.sf.net/projects/lustre/
 *
 * Portals is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * Portals is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Portals; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "qswnal.h"

nal_t			kqswnal_api;
kqswnal_data_t		kqswnal_data;
ptl_handle_ni_t         kqswnal_ni;
kqswnal_tunables_t      kqswnal_tunables;

kpr_nal_interface_t kqswnal_router_interface = {
	kprni_nalid:	QSWNAL,
	kprni_arg:	NULL,
	kprni_fwd:	kqswnal_fwd_packet,
	kprni_notify:   NULL,			/* we're connectionless */
};

#if CONFIG_SYSCTL
#define QSWNAL_SYSCTL  201

#define QSWNAL_SYSCTL_OPTIMIZED_GETS     1
#define QSWNAL_SYSCTL_COPY_SMALL_FWD     2

static ctl_table kqswnal_ctl_table[] = {
	{QSWNAL_SYSCTL_OPTIMIZED_GETS, "optimized_puts",
	 &kqswnal_tunables.kqn_optimized_puts, sizeof (int),
	 0644, NULL, &proc_dointvec},
	{QSWNAL_SYSCTL_OPTIMIZED_GETS, "optimized_gets",
	 &kqswnal_tunables.kqn_optimized_gets, sizeof (int),
	 0644, NULL, &proc_dointvec},
	{0}
};

static ctl_table kqswnal_top_ctl_table[] = {
	{QSWNAL_SYSCTL, "qswnal", NULL, 0, 0555, kqswnal_ctl_table},
	{0}
};
#endif

int
kqswnal_get_tx_desc (struct portals_cfg *pcfg)
{
	unsigned long      flags;
	struct list_head  *tmp;
	kqswnal_tx_t      *ktx;
	int                index = pcfg->pcfg_count;
	int                rc = -ENOENT;

	spin_lock_irqsave (&kqswnal_data.kqn_idletxd_lock, flags);

	list_for_each (tmp, &kqswnal_data.kqn_activetxds) {
		if (index-- != 0)
			continue;

		ktx = list_entry (tmp, kqswnal_tx_t, ktx_list);

		pcfg->pcfg_pbuf1 = (char *)ktx;
		pcfg->pcfg_count = NTOH__u32(ktx->ktx_wire_hdr->type);
		pcfg->pcfg_size  = NTOH__u32(ktx->ktx_wire_hdr->payload_length);
		pcfg->pcfg_nid   = NTOH__u64(ktx->ktx_wire_hdr->dest_nid);
		pcfg->pcfg_nid2  = ktx->ktx_nid;
		pcfg->pcfg_misc  = ktx->ktx_launcher;
		pcfg->pcfg_flags = (list_empty (&ktx->ktx_delayed_list) ? 0 : 1) |
				  (!ktx->ktx_isnblk                    ? 0 : 2) |
				  (ktx->ktx_state << 2);
		rc = 0;
		break;
	}
	
	spin_unlock_irqrestore (&kqswnal_data.kqn_idletxd_lock, flags);
	return (rc);
}

int
kqswnal_cmd (struct portals_cfg *pcfg, void *private)
{
	LASSERT (pcfg != NULL);
	
	switch (pcfg->pcfg_command) {
	case NAL_CMD_GET_TXDESC:
		return (kqswnal_get_tx_desc (pcfg));

	case NAL_CMD_REGISTER_MYNID:
		CDEBUG (D_IOCTL, "setting NID offset to "LPX64" (was "LPX64")\n",
			pcfg->pcfg_nid - kqswnal_data.kqn_elanid,
			kqswnal_data.kqn_nid_offset);
		kqswnal_data.kqn_nid_offset =
			pcfg->pcfg_nid - kqswnal_data.kqn_elanid;
		kqswnal_lib.libnal_ni.ni_pid.nid = pcfg->pcfg_nid;
		return (0);
		
	default:
		return (-EINVAL);
	}
}

static void
kqswnal_shutdown(nal_t *nal)
{
	unsigned long flags;
	int           do_lib_fini = 0;

	/* NB The first ref was this module! */
	if (nal->nal_refct != 0) {
		PORTAL_MODULE_UNUSE;
		return;
	}

	CDEBUG (D_NET, "shutdown\n");
	LASSERT (nal == &kqswnal_api);

	switch (kqswnal_data.kqn_init)
	{
	default:
		LASSERT (0);

	case KQN_INIT_ALL:
                libcfs_nal_cmd_unregister(QSWNAL);
		/* fall through */

	case KQN_INIT_LIB:
		do_lib_fini = 1;
		/* fall through */

	case KQN_INIT_DATA:
		break;

	case KQN_INIT_NOTHING:
		return;
	}

	/**********************************************************************/
	/* Tell router we're shutting down.  Any router calls my threads
	 * make will now fail immediately and the router will stop calling
	 * into me. */
	kpr_shutdown (&kqswnal_data.kqn_router);
	
	/**********************************************************************/
	/* Signal the start of shutdown... */
	spin_lock_irqsave(&kqswnal_data.kqn_idletxd_lock, flags);
	kqswnal_data.kqn_shuttingdown = 1;
	spin_unlock_irqrestore(&kqswnal_data.kqn_idletxd_lock, flags);

	wake_up_all(&kqswnal_data.kqn_idletxd_waitq);

	/**********************************************************************/
	/* wait for sends that have allocated a tx desc to launch or give up */
	while (atomic_read (&kqswnal_data.kqn_pending_txs) != 0) {
		CDEBUG(D_NET, "waiting for %d pending sends\n",
		       atomic_read (&kqswnal_data.kqn_pending_txs));
		set_current_state (TASK_UNINTERRUPTIBLE);
		schedule_timeout (HZ);
	}

	/**********************************************************************/
	/* close elan comms */
#if MULTIRAIL_EKC
	/* Shut down receivers first; rx callbacks might try sending... */
	if (kqswnal_data.kqn_eprx_small != NULL)
		ep_free_rcvr (kqswnal_data.kqn_eprx_small);

	if (kqswnal_data.kqn_eprx_large != NULL)
		ep_free_rcvr (kqswnal_data.kqn_eprx_large);

	/* NB ep_free_rcvr() returns only after we've freed off all receive
	 * buffers (see shutdown handling in kqswnal_requeue_rx()).  This
	 * means we must have completed any messages we passed to
	 * lib_parse() or kpr_fwd_start(). */

	if (kqswnal_data.kqn_eptx != NULL)
		ep_free_xmtr (kqswnal_data.kqn_eptx);

	/* NB ep_free_xmtr() returns only after all outstanding transmits
	 * have called their callback... */
	LASSERT(list_empty(&kqswnal_data.kqn_activetxds));
#else
	/* "Old" EKC just pretends to shutdown cleanly but actually
	 * provides no guarantees */
	if (kqswnal_data.kqn_eprx_small != NULL)
		ep_remove_large_rcvr (kqswnal_data.kqn_eprx_small);

	if (kqswnal_data.kqn_eprx_large != NULL)
		ep_remove_large_rcvr (kqswnal_data.kqn_eprx_large);

	/* wait for transmits to complete */
	while (!list_empty(&kqswnal_data.kqn_activetxds)) {
		CWARN("waiting for active transmits to complete\n");
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ);
	}

	if (kqswnal_data.kqn_eptx != NULL)
		ep_free_large_xmtr (kqswnal_data.kqn_eptx);
#endif
	/**********************************************************************/
	/* flag threads to terminate, wake them and wait for them to die */
	kqswnal_data.kqn_shuttingdown = 2;
	wake_up_all (&kqswnal_data.kqn_sched_waitq);

	while (atomic_read (&kqswnal_data.kqn_nthreads) != 0) {
		CDEBUG(D_NET, "waiting for %d threads to terminate\n",
		       atomic_read (&kqswnal_data.kqn_nthreads));
		set_current_state (TASK_UNINTERRUPTIBLE);
		schedule_timeout (HZ);
	}

	/**********************************************************************/
	/* No more threads.  No more portals, router or comms callbacks!
	 * I control the horizontals and the verticals...
	 */

#if MULTIRAIL_EKC
	LASSERT (list_empty (&kqswnal_data.kqn_readyrxds));
	LASSERT (list_empty (&kqswnal_data.kqn_delayedtxds));
	LASSERT (list_empty (&kqswnal_data.kqn_delayedfwds));
#endif

	/**********************************************************************/
	/* Complete any blocked forwarding packets, with error
	 */

	while (!list_empty (&kqswnal_data.kqn_idletxd_fwdq))
	{
		kpr_fwd_desc_t *fwd = list_entry (kqswnal_data.kqn_idletxd_fwdq.next,
						  kpr_fwd_desc_t, kprfd_list);
		list_del (&fwd->kprfd_list);
		kpr_fwd_done (&kqswnal_data.kqn_router, fwd, -ESHUTDOWN);
	}

	/**********************************************************************/
	/* finalise router and portals lib */

	kpr_deregister (&kqswnal_data.kqn_router);

	if (do_lib_fini)
		lib_fini (&kqswnal_lib);

	/**********************************************************************/
	/* Unmap message buffers and free all descriptors and buffers
	 */

#if MULTIRAIL_EKC
	/* FTTB, we need to unmap any remaining mapped memory.  When
	 * ep_dvma_release() get fixed (and releases any mappings in the
	 * region), we can delete all the code from here -------->  */

	if (kqswnal_data.kqn_txds != NULL) {
	        int  i;

		for (i = 0; i < KQSW_NTXMSGS + KQSW_NNBLK_TXMSGS; i++) {
			kqswnal_tx_t *ktx = &kqswnal_data.kqn_txds[i];

			/* If ktx has a buffer, it got mapped; unmap now.
			 * NB only the pre-mapped stuff is still mapped
			 * since all tx descs must be idle */

			if (ktx->ktx_buffer != NULL)
				ep_dvma_unload(kqswnal_data.kqn_ep,
					       kqswnal_data.kqn_ep_tx_nmh,
					       &ktx->ktx_ebuffer);
		}
	}

	if (kqswnal_data.kqn_rxds != NULL) {
	        int   i;

		for (i = 0; i < KQSW_NRXMSGS_SMALL + KQSW_NRXMSGS_LARGE; i++) {
			kqswnal_rx_t *krx = &kqswnal_data.kqn_rxds[i];

			/* If krx_kiov[0].kiov_page got allocated, it got mapped.  
			 * NB subsequent pages get merged */

			if (krx->krx_kiov[0].kiov_page != NULL)
				ep_dvma_unload(kqswnal_data.kqn_ep,
					       kqswnal_data.kqn_ep_rx_nmh,
					       &krx->krx_elanbuffer);
		}
	}
	/* <----------- to here */

	if (kqswnal_data.kqn_ep_rx_nmh != NULL)
		ep_dvma_release(kqswnal_data.kqn_ep, kqswnal_data.kqn_ep_rx_nmh);

	if (kqswnal_data.kqn_ep_tx_nmh != NULL)
		ep_dvma_release(kqswnal_data.kqn_ep, kqswnal_data.kqn_ep_tx_nmh);
#else
	if (kqswnal_data.kqn_eprxdmahandle != NULL)
	{
		elan3_dvma_unload(kqswnal_data.kqn_ep->DmaState,
				  kqswnal_data.kqn_eprxdmahandle, 0,
				  KQSW_NRXMSGPAGES_SMALL * KQSW_NRXMSGS_SMALL +
				  KQSW_NRXMSGPAGES_LARGE * KQSW_NRXMSGS_LARGE);

		elan3_dma_release(kqswnal_data.kqn_ep->DmaState,
				  kqswnal_data.kqn_eprxdmahandle);
	}

	if (kqswnal_data.kqn_eptxdmahandle != NULL)
	{
		elan3_dvma_unload(kqswnal_data.kqn_ep->DmaState,
				  kqswnal_data.kqn_eptxdmahandle, 0,
				  KQSW_NTXMSGPAGES * (KQSW_NTXMSGS +
						      KQSW_NNBLK_TXMSGS));

		elan3_dma_release(kqswnal_data.kqn_ep->DmaState,
				  kqswnal_data.kqn_eptxdmahandle);
	}
#endif

	if (kqswnal_data.kqn_txds != NULL)
	{
		int   i;

		for (i = 0; i < KQSW_NTXMSGS + KQSW_NNBLK_TXMSGS; i++)
		{
			kqswnal_tx_t *ktx = &kqswnal_data.kqn_txds[i];

			if (ktx->ktx_buffer != NULL)
				PORTAL_FREE(ktx->ktx_buffer,
					    KQSW_TX_BUFFER_SIZE);
		}

		PORTAL_FREE(kqswnal_data.kqn_txds,
			    sizeof (kqswnal_tx_t) * (KQSW_NTXMSGS +
						     KQSW_NNBLK_TXMSGS));
	}

	if (kqswnal_data.kqn_rxds != NULL)
	{
		int   i;
		int   j;

		for (i = 0; i < KQSW_NRXMSGS_SMALL + KQSW_NRXMSGS_LARGE; i++)
		{
			kqswnal_rx_t *krx = &kqswnal_data.kqn_rxds[i];

			for (j = 0; j < krx->krx_npages; j++)
				if (krx->krx_kiov[j].kiov_page != NULL)
					__free_page (krx->krx_kiov[j].kiov_page);
		}

		PORTAL_FREE(kqswnal_data.kqn_rxds,
			    sizeof(kqswnal_rx_t) * (KQSW_NRXMSGS_SMALL +
						    KQSW_NRXMSGS_LARGE));
	}

	/* resets flags, pointers to NULL etc */
	memset(&kqswnal_data, 0, sizeof (kqswnal_data));

	CDEBUG (D_MALLOC, "done kmem %d\n", atomic_read(&portal_kmemory));

	printk (KERN_INFO "Lustre: Routing QSW NAL unloaded (final mem %d)\n",
                atomic_read(&portal_kmemory));
}

static int __init
kqswnal_startup (nal_t *nal, ptl_pid_t requested_pid,
		 ptl_ni_limits_t *requested_limits, 
		 ptl_ni_limits_t *actual_limits)
{
#if MULTIRAIL_EKC
	EP_RAILMASK       all_rails = EP_RAILMASK_ALL;
#else
	ELAN3_DMA_REQUEST dmareq;
#endif
	int               rc;
	int               i;
	int               elan_page_idx;
	ptl_process_id_t  my_process_id;
	int               pkmem = atomic_read(&portal_kmemory);

	LASSERT (nal == &kqswnal_api);

	if (nal->nal_refct != 0) {
		if (actual_limits != NULL)
			*actual_limits = kqswnal_lib.libnal_ni.ni_actual_limits;
		/* This module got the first ref */
		PORTAL_MODULE_USE;
		return (PTL_OK);
	}

	LASSERT (kqswnal_data.kqn_init == KQN_INIT_NOTHING);

	CDEBUG (D_MALLOC, "start kmem %d\n", atomic_read(&portal_kmemory));

	/* ensure all pointers NULL etc */
	memset (&kqswnal_data, 0, sizeof (kqswnal_data));

	INIT_LIST_HEAD (&kqswnal_data.kqn_idletxds);
	INIT_LIST_HEAD (&kqswnal_data.kqn_nblk_idletxds);
	INIT_LIST_HEAD (&kqswnal_data.kqn_activetxds);
	spin_lock_init (&kqswnal_data.kqn_idletxd_lock);
	init_waitqueue_head (&kqswnal_data.kqn_idletxd_waitq);
	INIT_LIST_HEAD (&kqswnal_data.kqn_idletxd_fwdq);

	INIT_LIST_HEAD (&kqswnal_data.kqn_delayedfwds);
	INIT_LIST_HEAD (&kqswnal_data.kqn_delayedtxds);
	INIT_LIST_HEAD (&kqswnal_data.kqn_readyrxds);

	spin_lock_init (&kqswnal_data.kqn_sched_lock);
	init_waitqueue_head (&kqswnal_data.kqn_sched_waitq);

	/* Leave kqn_rpc_success zeroed */
#if MULTIRAIL_EKC
	kqswnal_data.kqn_rpc_failed.Data[0] = -ECONNREFUSED;
#else
	kqswnal_data.kqn_rpc_failed.Status = -ECONNREFUSED;
#endif

	/* pointers/lists/locks initialised */
	kqswnal_data.kqn_init = KQN_INIT_DATA;
	
#if MULTIRAIL_EKC
	kqswnal_data.kqn_ep = ep_system();
	if (kqswnal_data.kqn_ep == NULL) {
		CERROR("Can't initialise EKC\n");
		kqswnal_shutdown(nal);
		return (PTL_IFACE_INVALID);
	}

	if (ep_waitfor_nodeid(kqswnal_data.kqn_ep) == ELAN_INVALID_NODE) {
		CERROR("Can't get elan ID\n");
		kqswnal_shutdown(nal);
		return (PTL_IFACE_INVALID);
	}
#else
	/**********************************************************************/
	/* Find the first Elan device */

	kqswnal_data.kqn_ep = ep_device (0);
	if (kqswnal_data.kqn_ep == NULL)
	{
		CERROR ("Can't get elan device 0\n");
		kqswnal_shutdown(nal);
		return (PTL_IFACE_INVALID);
	}
#endif

	kqswnal_data.kqn_nid_offset = 0;
	kqswnal_data.kqn_nnodes     = ep_numnodes (kqswnal_data.kqn_ep);
	kqswnal_data.kqn_elanid     = ep_nodeid (kqswnal_data.kqn_ep);
	
	/**********************************************************************/
	/* Get the transmitter */

	kqswnal_data.kqn_eptx = ep_alloc_xmtr (kqswnal_data.kqn_ep);
	if (kqswnal_data.kqn_eptx == NULL)
	{
		CERROR ("Can't allocate transmitter\n");
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}

	/**********************************************************************/
	/* Get the receivers */

	kqswnal_data.kqn_eprx_small = ep_alloc_rcvr (kqswnal_data.kqn_ep,
						     EP_MSG_SVC_PORTALS_SMALL,
						     KQSW_EP_ENVELOPES_SMALL);
	if (kqswnal_data.kqn_eprx_small == NULL)
	{
		CERROR ("Can't install small msg receiver\n");
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}

	kqswnal_data.kqn_eprx_large = ep_alloc_rcvr (kqswnal_data.kqn_ep,
						     EP_MSG_SVC_PORTALS_LARGE,
						     KQSW_EP_ENVELOPES_LARGE);
	if (kqswnal_data.kqn_eprx_large == NULL)
	{
		CERROR ("Can't install large msg receiver\n");
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}

	/**********************************************************************/
	/* Reserve Elan address space for transmit descriptors NB we may
	 * either send the contents of associated buffers immediately, or
	 * map them for the peer to suck/blow... */
#if MULTIRAIL_EKC
	kqswnal_data.kqn_ep_tx_nmh = 
		ep_dvma_reserve(kqswnal_data.kqn_ep,
				KQSW_NTXMSGPAGES*(KQSW_NTXMSGS+KQSW_NNBLK_TXMSGS),
				EP_PERM_WRITE);
	if (kqswnal_data.kqn_ep_tx_nmh == NULL) {
		CERROR("Can't reserve tx dma space\n");
		kqswnal_shutdown(nal);
		return (PTL_NO_SPACE);
	}
#else
        dmareq.Waitfn   = DDI_DMA_SLEEP;
        dmareq.ElanAddr = (E3_Addr) 0;
        dmareq.Attr     = PTE_LOAD_LITTLE_ENDIAN;
        dmareq.Perm     = ELAN_PERM_REMOTEWRITE;

	rc = elan3_dma_reserve(kqswnal_data.kqn_ep->DmaState,
			      KQSW_NTXMSGPAGES*(KQSW_NTXMSGS+KQSW_NNBLK_TXMSGS),
			      &dmareq, &kqswnal_data.kqn_eptxdmahandle);
	if (rc != DDI_SUCCESS)
	{
		CERROR ("Can't reserve rx dma space\n");
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}
#endif
	/**********************************************************************/
	/* Reserve Elan address space for receive buffers */
#if MULTIRAIL_EKC
	kqswnal_data.kqn_ep_rx_nmh =
		ep_dvma_reserve(kqswnal_data.kqn_ep,
				KQSW_NRXMSGPAGES_SMALL * KQSW_NRXMSGS_SMALL +
				KQSW_NRXMSGPAGES_LARGE * KQSW_NRXMSGS_LARGE,
				EP_PERM_WRITE);
	if (kqswnal_data.kqn_ep_tx_nmh == NULL) {
		CERROR("Can't reserve rx dma space\n");
		kqswnal_shutdown(nal);
		return (PTL_NO_SPACE);
	}
#else
        dmareq.Waitfn   = DDI_DMA_SLEEP;
        dmareq.ElanAddr = (E3_Addr) 0;
        dmareq.Attr     = PTE_LOAD_LITTLE_ENDIAN;
        dmareq.Perm     = ELAN_PERM_REMOTEWRITE;

	rc = elan3_dma_reserve (kqswnal_data.kqn_ep->DmaState,
				KQSW_NRXMSGPAGES_SMALL * KQSW_NRXMSGS_SMALL +
				KQSW_NRXMSGPAGES_LARGE * KQSW_NRXMSGS_LARGE,
				&dmareq, &kqswnal_data.kqn_eprxdmahandle);
	if (rc != DDI_SUCCESS)
	{
		CERROR ("Can't reserve rx dma space\n");
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}
#endif
	/**********************************************************************/
	/* Allocate/Initialise transmit descriptors */

	PORTAL_ALLOC(kqswnal_data.kqn_txds,
		     sizeof(kqswnal_tx_t) * (KQSW_NTXMSGS + KQSW_NNBLK_TXMSGS));
	if (kqswnal_data.kqn_txds == NULL)
	{
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}

	/* clear flags, null pointers etc */
	memset(kqswnal_data.kqn_txds, 0,
	       sizeof(kqswnal_tx_t) * (KQSW_NTXMSGS + KQSW_NNBLK_TXMSGS));
	for (i = 0; i < (KQSW_NTXMSGS + KQSW_NNBLK_TXMSGS); i++)
	{
		int           premapped_pages;
		kqswnal_tx_t *ktx = &kqswnal_data.kqn_txds[i];
		int           basepage = i * KQSW_NTXMSGPAGES;

		PORTAL_ALLOC (ktx->ktx_buffer, KQSW_TX_BUFFER_SIZE);
		if (ktx->ktx_buffer == NULL)
		{
			kqswnal_shutdown (nal);
			return (PTL_NO_SPACE);
		}

		/* Map pre-allocated buffer NOW, to save latency on transmit */
		premapped_pages = kqswnal_pages_spanned(ktx->ktx_buffer,
							KQSW_TX_BUFFER_SIZE);
#if MULTIRAIL_EKC
		ep_dvma_load(kqswnal_data.kqn_ep, NULL, 
			     ktx->ktx_buffer, KQSW_TX_BUFFER_SIZE, 
			     kqswnal_data.kqn_ep_tx_nmh, basepage,
			     &all_rails, &ktx->ktx_ebuffer);
#else
		elan3_dvma_kaddr_load (kqswnal_data.kqn_ep->DmaState,
				       kqswnal_data.kqn_eptxdmahandle,
				       ktx->ktx_buffer, KQSW_TX_BUFFER_SIZE,
				       basepage, &ktx->ktx_ebuffer);
#endif
		ktx->ktx_basepage = basepage + premapped_pages; /* message mapping starts here */
		ktx->ktx_npages = KQSW_NTXMSGPAGES - premapped_pages; /* for this many pages */

		INIT_LIST_HEAD (&ktx->ktx_delayed_list);

		ktx->ktx_state = KTX_IDLE;
		ktx->ktx_isnblk = (i >= KQSW_NTXMSGS);
		list_add_tail (&ktx->ktx_list, 
			       ktx->ktx_isnblk ? &kqswnal_data.kqn_nblk_idletxds :
			                         &kqswnal_data.kqn_idletxds);
	}

	/**********************************************************************/
	/* Allocate/Initialise receive descriptors */

	PORTAL_ALLOC (kqswnal_data.kqn_rxds,
		      sizeof (kqswnal_rx_t) * (KQSW_NRXMSGS_SMALL + KQSW_NRXMSGS_LARGE));
	if (kqswnal_data.kqn_rxds == NULL)
	{
		kqswnal_shutdown (nal);
		return (PTL_NO_SPACE);
	}

	memset(kqswnal_data.kqn_rxds, 0, /* clear flags, null pointers etc */
	       sizeof(kqswnal_rx_t) * (KQSW_NRXMSGS_SMALL+KQSW_NRXMSGS_LARGE));

	elan_page_idx = 0;
	for (i = 0; i < KQSW_NRXMSGS_SMALL + KQSW_NRXMSGS_LARGE; i++)
	{
#if MULTIRAIL_EKC
		EP_NMD        elanbuffer;
#else
		E3_Addr       elanbuffer;
#endif
		int           j;
		kqswnal_rx_t *krx = &kqswnal_data.kqn_rxds[i];

		if (i < KQSW_NRXMSGS_SMALL)
		{
			krx->krx_npages = KQSW_NRXMSGPAGES_SMALL;
			krx->krx_eprx   = kqswnal_data.kqn_eprx_small;
		}
		else
		{
			krx->krx_npages = KQSW_NRXMSGPAGES_LARGE;
			krx->krx_eprx   = kqswnal_data.kqn_eprx_large;
		}

		LASSERT (krx->krx_npages > 0);
		for (j = 0; j < krx->krx_npages; j++)
		{
			struct page *page = alloc_page(GFP_KERNEL);
			
			if (page == NULL) {
				kqswnal_shutdown (nal);
				return (PTL_NO_SPACE);
			}

			krx->krx_kiov[j].kiov_page = page;
			LASSERT(page_address(page) != NULL);

#if MULTIRAIL_EKC
			ep_dvma_load(kqswnal_data.kqn_ep, NULL,
				     page_address(page),
				     PAGE_SIZE, kqswnal_data.kqn_ep_rx_nmh,
				     elan_page_idx, &all_rails, &elanbuffer);
			
			if (j == 0) {
				krx->krx_elanbuffer = elanbuffer;
			} else {
				rc = ep_nmd_merge(&krx->krx_elanbuffer,
						  &krx->krx_elanbuffer, 
						  &elanbuffer);
				/* NB contiguous mapping */
				LASSERT(rc);
			}
#else
			elan3_dvma_kaddr_load(kqswnal_data.kqn_ep->DmaState,
					      kqswnal_data.kqn_eprxdmahandle,
					      page_address(page),
					      PAGE_SIZE, elan_page_idx,
					      &elanbuffer);
			if (j == 0)
				krx->krx_elanbuffer = elanbuffer;

			/* NB contiguous mapping */
			LASSERT (elanbuffer == krx->krx_elanbuffer + j * PAGE_SIZE);
#endif
			elan_page_idx++;

		}
	}
	LASSERT (elan_page_idx ==
		 (KQSW_NRXMSGS_SMALL * KQSW_NRXMSGPAGES_SMALL) +
		 (KQSW_NRXMSGS_LARGE * KQSW_NRXMSGPAGES_LARGE));

	/**********************************************************************/
	/* Network interface ready to initialise */

	my_process_id.nid = kqswnal_elanid2nid(kqswnal_data.kqn_elanid);
	my_process_id.pid = requested_pid;

	rc = lib_init(&kqswnal_lib, nal, my_process_id,
		      requested_limits, actual_limits);
        if (rc != PTL_OK)
	{
		CERROR ("lib_init failed %d\n", rc);
		kqswnal_shutdown (nal);
		return (rc);
	}

	kqswnal_data.kqn_init = KQN_INIT_LIB;

	/**********************************************************************/
	/* Queue receives, now that it's OK to run their completion callbacks */

	for (i = 0; i < KQSW_NRXMSGS_SMALL + KQSW_NRXMSGS_LARGE; i++)
	{
		kqswnal_rx_t *krx = &kqswnal_data.kqn_rxds[i];

		/* NB this enqueue can allocate/sleep (attr == 0) */
		krx->krx_state = KRX_POSTED;
#if MULTIRAIL_EKC
		rc = ep_queue_receive(krx->krx_eprx, kqswnal_rxhandler, krx,
				      &krx->krx_elanbuffer, 0);
#else
		rc = ep_queue_receive(krx->krx_eprx, kqswnal_rxhandler, krx,
				      krx->krx_elanbuffer,
				      krx->krx_npages * PAGE_SIZE, 0);
#endif
		if (rc != EP_SUCCESS)
		{
			CERROR ("failed ep_queue_receive %d\n", rc);
			kqswnal_shutdown (nal);
			return (PTL_FAIL);
		}
	}

	/**********************************************************************/
	/* Spawn scheduling threads */
	for (i = 0; i < num_online_cpus(); i++) {
		rc = kqswnal_thread_start (kqswnal_scheduler, NULL);
		if (rc != 0)
		{
			CERROR ("failed to spawn scheduling thread: %d\n", rc);
			kqswnal_shutdown (nal);
			return (PTL_FAIL);
		}
	}

	/**********************************************************************/
	/* Connect to the router */
	rc = kpr_register (&kqswnal_data.kqn_router, &kqswnal_router_interface);
	CDEBUG(D_NET, "Can't initialise routing interface (rc = %d): not routing\n",rc);

	rc = libcfs_nal_cmd_register (QSWNAL, &kqswnal_cmd, NULL);
	if (rc != 0) {
		CERROR ("Can't initialise command interface (rc = %d)\n", rc);
		kqswnal_shutdown (nal);
		return (PTL_FAIL);
	}

	kqswnal_data.kqn_init = KQN_INIT_ALL;

	printk(KERN_INFO "Lustre: Routing QSW NAL loaded on node %d of %d "
	       "(Routing %s, initial mem %d)\n", 
	       kqswnal_data.kqn_elanid, kqswnal_data.kqn_nnodes,
	       kpr_routing (&kqswnal_data.kqn_router) ? "enabled" : "disabled",
	       pkmem);

	return (PTL_OK);
}

void __exit
kqswnal_finalise (void)
{
#if CONFIG_SYSCTL
	if (kqswnal_tunables.kqn_sysctl != NULL)
		unregister_sysctl_table (kqswnal_tunables.kqn_sysctl);
#endif
	PtlNIFini(kqswnal_ni);

	ptl_unregister_nal(QSWNAL);
}

static int __init
kqswnal_initialise (void)
{
	int   rc;

	kqswnal_api.nal_ni_init = kqswnal_startup;
	kqswnal_api.nal_ni_fini = kqswnal_shutdown;

	/* Initialise dynamic tunables to defaults once only */
	kqswnal_tunables.kqn_optimized_puts = KQSW_OPTIMIZED_PUTS;
	kqswnal_tunables.kqn_optimized_gets = KQSW_OPTIMIZED_GETS;
	
	rc = ptl_register_nal(QSWNAL, &kqswnal_api);
	if (rc != PTL_OK) {
		CERROR("Can't register QSWNAL: %d\n", rc);
		return (-ENOMEM);		/* or something... */
	}

	/* Pure gateways, and the workaround for 'EKC blocks forever until
	 * the service is active' want the NAL started up at module load
	 * time... */
	rc = PtlNIInit(QSWNAL, LUSTRE_SRV_PTL_PID, NULL, NULL, &kqswnal_ni);
	if (rc != PTL_OK && rc != PTL_IFACE_DUP) {
		ptl_unregister_nal(QSWNAL);
		return (-ENODEV);
	}

#if CONFIG_SYSCTL
        /* Press on regardless even if registering sysctl doesn't work */
        kqswnal_tunables.kqn_sysctl = 
		register_sysctl_table (kqswnal_top_ctl_table, 0);
#endif
	return (0);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Kernel Quadrics/Elan NAL v1.01");
MODULE_LICENSE("GPL");

module_init (kqswnal_initialise);
module_exit (kqswnal_finalise);
