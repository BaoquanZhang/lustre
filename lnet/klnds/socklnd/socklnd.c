/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *   Author: Zach Brown <zab@zabbo.net>
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 *   This file is part of Portals, http://www.sf.net/projects/sandiaportals/
 *
 *   Portals is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Portals is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Portals; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "socklnd.h"

lnd_t the_ksocklnd = {
        .lnd_type       = SOCKLND,
        .lnd_startup    = ksocknal_startup,
        .lnd_shutdown   = ksocknal_shutdown,
        .lnd_ctl        = ksocknal_ctl,
        .lnd_send       = ksocknal_send,
        .lnd_recv       = ksocknal_recv,
        .lnd_notify     = ksocknal_notify,
        .lnd_accept     = ksocknal_accept,
};

ksock_nal_data_t        ksocknal_data;

ksock_interface_t *
ksocknal_ip2iface(lnet_ni_t *ni, __u32 ip)
{
        ksock_net_t       *net = ni->ni_data;
        int                i;
        ksock_interface_t *iface;

        for (i = 0; i < net->ksnn_ninterfaces; i++) {
                LASSERT(i < LNET_MAX_INTERFACES);
                iface = &net->ksnn_interfaces[i];

                if (iface->ksni_ipaddr == ip)
                        return (iface);
        }

        return (NULL);
}

ksock_route_t *
ksocknal_create_route (__u32 ipaddr, int port)
{
        ksock_route_t *route;

        LIBCFS_ALLOC (route, sizeof (*route));
        if (route == NULL)
                return (NULL);

        atomic_set (&route->ksnr_refcount, 1);
        route->ksnr_peer = NULL;
        route->ksnr_retry_interval = 0;         /* OK to connect at any time */
        route->ksnr_ipaddr = ipaddr;
        route->ksnr_port = port;
        route->ksnr_connecting = 0;
        route->ksnr_connected = 0;
        route->ksnr_deleted = 0;
        route->ksnr_conn_count = 0;
        route->ksnr_share_count = 0;

        return (route);
}

void
ksocknal_destroy_route (ksock_route_t *route)
{
        LASSERT (atomic_read(&route->ksnr_refcount) == 0);

        if (route->ksnr_peer != NULL)
                ksocknal_peer_decref(route->ksnr_peer);

        LIBCFS_FREE (route, sizeof (*route));
}

int
ksocknal_create_peer (ksock_peer_t **peerp, lnet_ni_t *ni, lnet_process_id_t id)
{
        ksock_net_t   *net = ni->ni_data;
        ksock_peer_t  *peer;
        unsigned long  flags;

        LASSERT (id.nid != LNET_NID_ANY);
        LASSERT (id.pid != LNET_PID_ANY);
        LASSERT (!in_interrupt());

        LIBCFS_ALLOC (peer, sizeof (*peer));
        if (peer == NULL)
                return -ENOMEM;

        memset (peer, 0, sizeof (*peer));       /* NULL pointers/clear flags etc */

        peer->ksnp_ni = ni;
        peer->ksnp_id = id;
        atomic_set (&peer->ksnp_refcount, 1);   /* 1 ref for caller */
        peer->ksnp_closing = 0;
        CFS_INIT_LIST_HEAD (&peer->ksnp_conns);
        CFS_INIT_LIST_HEAD (&peer->ksnp_routes);
        CFS_INIT_LIST_HEAD (&peer->ksnp_tx_queue);

        spin_lock_irqsave(&net->ksnn_lock, flags);

        if (net->ksnn_shutdown) {
                spin_unlock_irqrestore(&net->ksnn_lock, flags);
                
                LIBCFS_FREE(peer, sizeof(*peer));
                CERROR("Can't create peer: network shutdown\n");
                return -ESHUTDOWN;
        }

        net->ksnn_npeers++;

        spin_unlock_irqrestore(&net->ksnn_lock, flags);

        *peerp = peer;
        return 0;
}

void
ksocknal_destroy_peer (ksock_peer_t *peer)
{
        ksock_net_t    *net = peer->ksnp_ni->ni_data;
        unsigned long   flags;

        CDEBUG (D_NET, "peer %s %p deleted\n", 
                libcfs_id2str(peer->ksnp_id), peer);

        LASSERT (atomic_read (&peer->ksnp_refcount) == 0);
        LASSERT (list_empty (&peer->ksnp_conns));
        LASSERT (list_empty (&peer->ksnp_routes));
        LASSERT (list_empty (&peer->ksnp_tx_queue));

        LIBCFS_FREE (peer, sizeof (*peer));

        /* NB a peer's connections and routes keep a reference on their peer
         * until they are destroyed, so we can be assured that _all_ state to
         * do with this peer has been cleaned up when its refcount drops to
         * zero. */
        spin_lock_irqsave(&net->ksnn_lock, flags);
        net->ksnn_npeers--;
        spin_unlock_irqrestore(&net->ksnn_lock, flags);
}

ksock_peer_t *
ksocknal_find_peer_locked (lnet_ni_t *ni, lnet_process_id_t id)
{
        struct list_head *peer_list = ksocknal_nid2peerlist(id.nid);
        struct list_head *tmp;
        ksock_peer_t     *peer;

        list_for_each (tmp, peer_list) {

                peer = list_entry (tmp, ksock_peer_t, ksnp_list);

                LASSERT (!peer->ksnp_closing);

                if (peer->ksnp_ni != ni)
                        continue;

                if (peer->ksnp_id.nid != id.nid ||
                    peer->ksnp_id.pid != id.pid)
                        continue;

                CDEBUG(D_NET, "got peer [%p] -> %s (%d)\n",
                       peer, libcfs_id2str(id), 
                       atomic_read(&peer->ksnp_refcount));
                return (peer);
        }
        return (NULL);
}

ksock_peer_t *
ksocknal_find_peer (lnet_ni_t *ni, lnet_process_id_t id)
{
        ksock_peer_t     *peer;

        read_lock (&ksocknal_data.ksnd_global_lock);
        peer = ksocknal_find_peer_locked (ni, id);
        if (peer != NULL)                       /* +1 ref for caller? */
                ksocknal_peer_addref(peer);
        read_unlock (&ksocknal_data.ksnd_global_lock);

        return (peer);
}

void
ksocknal_unlink_peer_locked (ksock_peer_t *peer)
{
        int                i;
        __u32              ip;

        for (i = 0; i < peer->ksnp_n_passive_ips; i++) {
                LASSERT (i < LNET_MAX_INTERFACES);
                ip = peer->ksnp_passive_ips[i];

                ksocknal_ip2iface(peer->ksnp_ni, ip)->ksni_npeers--;
        }

        LASSERT (list_empty(&peer->ksnp_conns));
        LASSERT (list_empty(&peer->ksnp_routes));
        LASSERT (!peer->ksnp_closing);
        peer->ksnp_closing = 1;
        list_del (&peer->ksnp_list);
        /* lose peerlist's ref */
        ksocknal_peer_decref(peer);
}

int
ksocknal_get_peer_info (lnet_ni_t *ni, int index, 
                        lnet_process_id_t *id, __u32 *myip, __u32 *peer_ip, int *port,
                        int *conn_count, int *share_count)
{
        ksock_peer_t      *peer;
        struct list_head  *ptmp;
        ksock_route_t     *route;
        struct list_head  *rtmp;
        int                i;
        int                j;
        int                rc = -ENOENT;

        read_lock (&ksocknal_data.ksnd_global_lock);

        for (i = 0; i < ksocknal_data.ksnd_peer_hash_size; i++) {

                list_for_each (ptmp, &ksocknal_data.ksnd_peers[i]) {
                        peer = list_entry (ptmp, ksock_peer_t, ksnp_list);

                        if (peer->ksnp_ni != ni)
                                continue;

                        if (peer->ksnp_n_passive_ips == 0 &&
                            list_empty(&peer->ksnp_routes)) {
                                if (index-- > 0)
                                        continue;

                                *id = peer->ksnp_id;
                                *myip = 0;
                                *peer_ip = 0;
                                *port = 0;
                                *conn_count = 0;
                                *share_count = 0;
                                rc = 0;
                                goto out;
                        }

                        for (j = 0; j < peer->ksnp_n_passive_ips; j++) {
                                if (index-- > 0)
                                        continue;

                                *id = peer->ksnp_id;
                                *myip = peer->ksnp_passive_ips[j];
                                *peer_ip = 0;
                                *port = 0;
                                *conn_count = 0;
                                *share_count = 0;
                                rc = 0;
                                goto out;
                        }

                        list_for_each (rtmp, &peer->ksnp_routes) {
                                if (index-- > 0)
                                        continue;

                                route = list_entry(rtmp, ksock_route_t,
                                                   ksnr_list);

                                *id = peer->ksnp_id;
                                *myip = route->ksnr_myipaddr;
                                *peer_ip = route->ksnr_ipaddr;
                                *port = route->ksnr_port;
                                *conn_count = route->ksnr_conn_count;
                                *share_count = route->ksnr_share_count;
                                rc = 0;
                                goto out;
                        }
                }
        }
 out:
        read_unlock (&ksocknal_data.ksnd_global_lock);
        return (rc);
}

void
ksocknal_associate_route_conn_locked(ksock_route_t *route, ksock_conn_t *conn)
{
        ksock_peer_t      *peer = route->ksnr_peer;
        int                type = conn->ksnc_type;
        ksock_interface_t *iface;

        conn->ksnc_route = route;
        ksocknal_route_addref(route);

        if (route->ksnr_myipaddr != conn->ksnc_myipaddr) {
                if (route->ksnr_myipaddr == 0) {
                        /* route wasn't bound locally yet (the initial route) */
                        CDEBUG(D_NET, "Binding %s %u.%u.%u.%u to %u.%u.%u.%u\n",
                               libcfs_id2str(peer->ksnp_id),
                               HIPQUAD(route->ksnr_ipaddr),
                               HIPQUAD(conn->ksnc_myipaddr));
                } else {
                        CDEBUG(D_NET, "Rebinding %s %u.%u.%u.%u from "
                               "%u.%u.%u.%u to %u.%u.%u.%u\n",
                               libcfs_id2str(peer->ksnp_id),
                               HIPQUAD(route->ksnr_ipaddr),
                               HIPQUAD(route->ksnr_myipaddr),
                               HIPQUAD(conn->ksnc_myipaddr));

                        iface = ksocknal_ip2iface(route->ksnr_peer->ksnp_ni,
                                                  route->ksnr_myipaddr);
                        if (iface != NULL)
                                iface->ksni_nroutes--;
                }
                route->ksnr_myipaddr = conn->ksnc_myipaddr;
                iface = ksocknal_ip2iface(route->ksnr_peer->ksnp_ni,
                                          route->ksnr_myipaddr);
                if (iface != NULL)
                        iface->ksni_nroutes++;
        }

        route->ksnr_connected |= (1<<type);
        route->ksnr_conn_count++;

        /* Successful connection => further attempts can
         * proceed immediately */
        route->ksnr_retry_interval = 0;
}

void
ksocknal_add_route_locked (ksock_peer_t *peer, ksock_route_t *route)
{
        struct list_head  *tmp;
        ksock_conn_t      *conn;
        int                type;
        ksock_route_t     *route2;

        LASSERT (route->ksnr_peer == NULL);
        LASSERT (!route->ksnr_connecting);
        LASSERT (route->ksnr_connected == 0);

        /* LASSERT(unique) */
        list_for_each(tmp, &peer->ksnp_routes) {
                route2 = list_entry(tmp, ksock_route_t, ksnr_list);

                if (route2->ksnr_ipaddr == route->ksnr_ipaddr) {
                        CERROR ("Duplicate route %s %u.%u.%u.%u\n",
                                libcfs_id2str(peer->ksnp_id), 
                                HIPQUAD(route->ksnr_ipaddr));
                        LBUG();
                }
        }

        route->ksnr_peer = peer;
        ksocknal_peer_addref(peer);
        /* peer's routelist takes over my ref on 'route' */
        list_add_tail(&route->ksnr_list, &peer->ksnp_routes);

        list_for_each(tmp, &peer->ksnp_conns) {
                conn = list_entry(tmp, ksock_conn_t, ksnc_list);
                type = conn->ksnc_type;

                if (conn->ksnc_ipaddr != route->ksnr_ipaddr)
                        continue;

                ksocknal_associate_route_conn_locked(route, conn);
                /* keep going (typed routes) */
        }
}

void
ksocknal_del_route_locked (ksock_route_t *route)
{
        ksock_peer_t      *peer = route->ksnr_peer;
        ksock_interface_t *iface;
        ksock_conn_t      *conn;
        struct list_head  *ctmp;
        struct list_head  *cnxt;

        LASSERT (!route->ksnr_deleted);

        /* Close associated conns */
        list_for_each_safe (ctmp, cnxt, &peer->ksnp_conns) {
                conn = list_entry(ctmp, ksock_conn_t, ksnc_list);

                if (conn->ksnc_route != route)
                        continue;

                ksocknal_close_conn_locked (conn, 0);
        }

        if (route->ksnr_myipaddr != 0) {
                iface = ksocknal_ip2iface(route->ksnr_peer->ksnp_ni,
                                          route->ksnr_myipaddr);
                if (iface != NULL)
                        iface->ksni_nroutes--;
        }

        route->ksnr_deleted = 1;
        list_del (&route->ksnr_list);
        ksocknal_route_decref(route);             /* drop peer's ref */

        if (list_empty (&peer->ksnp_routes) &&
            list_empty (&peer->ksnp_conns)) {
                /* I've just removed the last route to a peer with no active
                 * connections */
                ksocknal_unlink_peer_locked (peer);
        }
}

int
ksocknal_add_peer (lnet_ni_t *ni, lnet_process_id_t id, __u32 ipaddr, int port)
{
        unsigned long      flags;
        struct list_head  *tmp;
        ksock_peer_t      *peer;
        ksock_peer_t      *peer2;
        ksock_route_t     *route;
        ksock_route_t     *route2;
        int                rc;

        if (id.nid == LNET_NID_ANY ||
            id.pid == LNET_PID_ANY)
                return (-EINVAL);

        /* Have a brand new peer ready... */
        rc = ksocknal_create_peer(&peer, ni, id);
        if (rc != 0)
                return rc;

        route = ksocknal_create_route (ipaddr, port);
        if (route == NULL) {
                ksocknal_peer_decref(peer);
                return (-ENOMEM);
        }

        write_lock_irqsave (&ksocknal_data.ksnd_global_lock, flags);

        peer2 = ksocknal_find_peer_locked (ni, id);
        if (peer2 != NULL) {
                ksocknal_peer_decref(peer);
                peer = peer2;
        } else {
                /* peer table takes my ref on peer */
                list_add_tail (&peer->ksnp_list,
                               ksocknal_nid2peerlist (id.nid));
        }

        route2 = NULL;
        list_for_each (tmp, &peer->ksnp_routes) {
                route2 = list_entry(tmp, ksock_route_t, ksnr_list);

                if (route2->ksnr_ipaddr == ipaddr)
                        break;

                route2 = NULL;
        }
        if (route2 == NULL) {
                ksocknal_add_route_locked(peer, route);
                route->ksnr_share_count++;
        } else {
                ksocknal_route_decref(route);
                route2->ksnr_share_count++;
        }

        write_unlock_irqrestore (&ksocknal_data.ksnd_global_lock, flags);

        return (0);
}

void
ksocknal_del_peer_locked (ksock_peer_t *peer, __u32 ip)
{
        ksock_conn_t     *conn;
        ksock_route_t    *route;
        struct list_head *tmp;
        struct list_head *nxt;
        int               nshared;

        LASSERT (!peer->ksnp_closing);

        /* Extra ref prevents peer disappearing until I'm done with it */
        ksocknal_peer_addref(peer);

        list_for_each_safe (tmp, nxt, &peer->ksnp_routes) {
                route = list_entry(tmp, ksock_route_t, ksnr_list);

                /* no match */
                if (!(ip == 0 || route->ksnr_ipaddr == ip))
                        continue;

                route->ksnr_share_count = 0;
                /* This deletes associated conns too */
                ksocknal_del_route_locked (route);
        }

        nshared = 0;
        list_for_each_safe (tmp, nxt, &peer->ksnp_routes) {
                route = list_entry(tmp, ksock_route_t, ksnr_list);
                nshared += route->ksnr_share_count;
        }

        if (nshared == 0) {
                /* remove everything else if there are no explicit entries
                 * left */

                list_for_each_safe (tmp, nxt, &peer->ksnp_routes) {
                        route = list_entry(tmp, ksock_route_t, ksnr_list);

                        /* we should only be removing auto-entries */
                        LASSERT(route->ksnr_share_count == 0);
                        ksocknal_del_route_locked (route);
                }

                list_for_each_safe (tmp, nxt, &peer->ksnp_conns) {
                        conn = list_entry(tmp, ksock_conn_t, ksnc_list);

                        ksocknal_close_conn_locked(conn, 0);
                }
        }

        ksocknal_peer_decref(peer);
        /* NB peer unlinks itself when last conn/route is removed */
}

int
ksocknal_del_peer (lnet_ni_t *ni, lnet_process_id_t id, __u32 ip)
{
        unsigned long      flags;
        struct list_head  *ptmp;
        struct list_head  *pnxt;
        ksock_peer_t      *peer;
        int                lo;
        int                hi;
        int                i;
        int                rc = -ENOENT;

        write_lock_irqsave (&ksocknal_data.ksnd_global_lock, flags);

        if (id.nid != LNET_NID_ANY)
                lo = hi = ksocknal_nid2peerlist(id.nid) - ksocknal_data.ksnd_peers;
        else {
                lo = 0;
                hi = ksocknal_data.ksnd_peer_hash_size - 1;
        }

        for (i = lo; i <= hi; i++) {
                list_for_each_safe (ptmp, pnxt, &ksocknal_data.ksnd_peers[i]) {
                        peer = list_entry (ptmp, ksock_peer_t, ksnp_list);

                        if (peer->ksnp_ni != ni)
                                continue;

                        if (!((id.nid == LNET_NID_ANY || peer->ksnp_id.nid == id.nid) &&
                              (id.pid == LNET_PID_ANY || peer->ksnp_id.pid == id.pid)))
                                continue;

                        ksocknal_del_peer_locked (peer, ip);
                        rc = 0;                 /* matched! */
                }
        }

        write_unlock_irqrestore (&ksocknal_data.ksnd_global_lock, flags);

        return (rc);
}

ksock_conn_t *
ksocknal_get_conn_by_idx (lnet_ni_t *ni, int index)
{
        ksock_peer_t      *peer;
        struct list_head  *ptmp;
        ksock_conn_t      *conn;
        struct list_head  *ctmp;
        int                i;

        read_lock (&ksocknal_data.ksnd_global_lock);

        for (i = 0; i < ksocknal_data.ksnd_peer_hash_size; i++) {
                list_for_each (ptmp, &ksocknal_data.ksnd_peers[i]) {
                        peer = list_entry (ptmp, ksock_peer_t, ksnp_list);

                        LASSERT (!peer->ksnp_closing);

                        if (peer->ksnp_ni != ni)
                                continue;

                        list_for_each (ctmp, &peer->ksnp_conns) {
                                if (index-- > 0)
                                        continue;

                                conn = list_entry (ctmp, ksock_conn_t, ksnc_list);
                                ksocknal_conn_addref(conn);
                                read_unlock (&ksocknal_data.ksnd_global_lock);
                                return (conn);
                        }
                }
        }

        read_unlock (&ksocknal_data.ksnd_global_lock);
        return (NULL);
}

ksock_sched_t *
ksocknal_choose_scheduler_locked (unsigned int irq)
{
        ksock_sched_t    *sched;
        ksock_irqinfo_t  *info;
        int               i;

        LASSERT (irq < NR_IRQS);
        info = &ksocknal_data.ksnd_irqinfo[irq];

        if (irq != 0 &&                         /* hardware NIC */
            info->ksni_valid) {                 /* already set up */
                return (&ksocknal_data.ksnd_schedulers[info->ksni_sched]);
        }

        /* software NIC (irq == 0) || not associated with a scheduler yet.
         * Choose the CPU with the fewest connections... */
        sched = &ksocknal_data.ksnd_schedulers[0];
        for (i = 1; i < ksocknal_data.ksnd_nschedulers; i++)
                if (sched->kss_nconns >
                    ksocknal_data.ksnd_schedulers[i].kss_nconns)
                        sched = &ksocknal_data.ksnd_schedulers[i];

        if (irq != 0) {                         /* Hardware NIC */
                info->ksni_valid = 1;
                info->ksni_sched = sched - ksocknal_data.ksnd_schedulers;

                /* no overflow... */
                LASSERT (info->ksni_sched == sched - ksocknal_data.ksnd_schedulers);
        }

        return (sched);
}

int
ksocknal_local_ipvec (lnet_ni_t *ni, __u32 *ipaddrs)
{
        ksock_net_t       *net = ni->ni_data;
        int                i;
        int                nip;

        read_lock (&ksocknal_data.ksnd_global_lock);

        nip = net->ksnn_ninterfaces;
        LASSERT (nip < LNET_MAX_INTERFACES);

        for (i = 0; i < nip; i++) {
                ipaddrs[i] = net->ksnn_interfaces[i].ksni_ipaddr;
                LASSERT (ipaddrs[i] != 0);
        }

        read_unlock (&ksocknal_data.ksnd_global_lock);
        return (nip);
}

int
ksocknal_match_peerip (ksock_interface_t *iface, __u32 *ips, int nips)
{
        int   best_netmatch = 0;
        int   best_xor      = 0;
        int   best          = -1;
        int   this_xor;
        int   this_netmatch;
        int   i;

        for (i = 0; i < nips; i++) {
                if (ips[i] == 0)
                        continue;

                this_xor = (ips[i] ^ iface->ksni_ipaddr);
                this_netmatch = ((this_xor & iface->ksni_netmask) == 0) ? 1 : 0;

                if (!(best < 0 ||
                      best_netmatch < this_netmatch ||
                      (best_netmatch == this_netmatch &&
                       best_xor > this_xor)))
                        continue;

                best = i;
                best_netmatch = this_netmatch;
                best_xor = this_xor;
        }

        LASSERT (best >= 0);
        return (best);
}

int
ksocknal_select_ips(ksock_peer_t *peer, __u32 *peerips, int n_peerips)
{
        rwlock_t           *global_lock = &ksocknal_data.ksnd_global_lock;
        ksock_net_t        *net = peer->ksnp_ni->ni_data;
        unsigned long       flags;
        ksock_interface_t  *iface;
        ksock_interface_t  *best_iface;
        int                 n_ips;
        int                 i;
        int                 j;
        int                 k;
        __u32               ip;
        __u32               xor;
        int                 this_netmatch;
        int                 best_netmatch;
        int                 best_npeers;

        /* CAVEAT EMPTOR: We do all our interface matching with an
         * exclusive hold of global lock at IRQ priority.  We're only
         * expecting to be dealing with small numbers of interfaces, so the
         * O(n**3)-ness shouldn't matter */

        /* Also note that I'm not going to return more than n_peerips
         * interfaces, even if I have more myself */

        write_lock_irqsave(global_lock, flags);

        LASSERT (n_peerips <= LNET_MAX_INTERFACES);
        LASSERT (net->ksnn_ninterfaces <= LNET_MAX_INTERFACES);

        n_ips = MIN(n_peerips, net->ksnn_ninterfaces);

        for (i = 0; peer->ksnp_n_passive_ips < n_ips; i++) {
                /*              ^ yes really... */

                /* If we have any new interfaces, first tick off all the
                 * peer IPs that match old interfaces, then choose new
                 * interfaces to match the remaining peer IPS.
                 * We don't forget interfaces we've stopped using; we might
                 * start using them again... */

                if (i < peer->ksnp_n_passive_ips) {
                        /* Old interface. */
                        ip = peer->ksnp_passive_ips[i];
                        best_iface = ksocknal_ip2iface(peer->ksnp_ni, ip);

                        /* peer passive ips are kept up to date */
                        LASSERT(best_iface != NULL);
                } else {
                        /* choose a new interface */
                        LASSERT (i == peer->ksnp_n_passive_ips);

                        best_iface = NULL;
                        best_netmatch = 0;
                        best_npeers = 0;

                        for (j = 0; j < net->ksnn_ninterfaces; j++) {
                                iface = &net->ksnn_interfaces[j];
                                ip = iface->ksni_ipaddr;

                                for (k = 0; k < peer->ksnp_n_passive_ips; k++)
                                        if (peer->ksnp_passive_ips[k] == ip)
                                                break;

                                if (k < peer->ksnp_n_passive_ips) /* using it already */
                                        continue;

                                k = ksocknal_match_peerip(iface, peerips, n_peerips);
                                xor = (ip ^ peerips[k]);
                                this_netmatch = ((xor & iface->ksni_netmask) == 0) ? 1 : 0;

                                if (!(best_iface == NULL ||
                                      best_netmatch < this_netmatch ||
                                      (best_netmatch == this_netmatch &&
                                       best_npeers > iface->ksni_npeers)))
                                        continue;

                                best_iface = iface;
                                best_netmatch = this_netmatch;
                                best_npeers = iface->ksni_npeers;
                        }

                        best_iface->ksni_npeers++;
                        ip = best_iface->ksni_ipaddr;
                        peer->ksnp_passive_ips[i] = ip;
                        peer->ksnp_n_passive_ips = i+1;
                }

                LASSERT (best_iface != NULL);

                /* mark the best matching peer IP used */
                j = ksocknal_match_peerip(best_iface, peerips, n_peerips);
                peerips[j] = 0;
        }

        /* Overwrite input peer IP addresses */
        memcpy(peerips, peer->ksnp_passive_ips, n_ips * sizeof(*peerips));

        write_unlock_irqrestore(global_lock, flags);

        return (n_ips);
}

void
ksocknal_create_routes(ksock_peer_t *peer, int port,
                       __u32 *peer_ipaddrs, int npeer_ipaddrs)
{
        ksock_route_t      *newroute = NULL;
        rwlock_t           *global_lock = &ksocknal_data.ksnd_global_lock;
        lnet_ni_t          *ni = peer->ksnp_ni;
        ksock_net_t        *net = ni->ni_data;
        unsigned long       flags;
        struct list_head   *rtmp;
        ksock_route_t      *route;
        ksock_interface_t  *iface;
        ksock_interface_t  *best_iface;
        int                 best_netmatch;
        int                 this_netmatch;
        int                 best_nroutes;
        int                 i;
        int                 j;

        /* CAVEAT EMPTOR: We do all our interface matching with an
         * exclusive hold of global lock at IRQ priority.  We're only
         * expecting to be dealing with small numbers of interfaces, so the
         * O(n**3)-ness here shouldn't matter */

        write_lock_irqsave(global_lock, flags);

        LASSERT (npeer_ipaddrs <= LNET_MAX_INTERFACES);

        for (i = 0; i < npeer_ipaddrs; i++) {
                if (newroute != NULL) {
                        newroute->ksnr_ipaddr = peer_ipaddrs[i];
                } else {
                        write_unlock_irqrestore(global_lock, flags);

                        newroute = ksocknal_create_route(peer_ipaddrs[i], port);
                        if (newroute == NULL)
                                return;

                        write_lock_irqsave(global_lock, flags);
                }

                /* Already got a route? */
                route = NULL;
                list_for_each(rtmp, &peer->ksnp_routes) {
                        route = list_entry(rtmp, ksock_route_t, ksnr_list);

                        if (route->ksnr_ipaddr == newroute->ksnr_ipaddr)
                                break;

                        route = NULL;
                }
                if (route != NULL)
                        continue;

                best_iface = NULL;
                best_nroutes = 0;
                best_netmatch = 0;

                LASSERT (net->ksnn_ninterfaces <= LNET_MAX_INTERFACES);

                /* Select interface to connect from */
                for (j = 0; j < net->ksnn_ninterfaces; j++) {
                        iface = &net->ksnn_interfaces[j];

                        /* Using this interface already? */
                        list_for_each(rtmp, &peer->ksnp_routes) {
                                route = list_entry(rtmp, ksock_route_t, ksnr_list);

                                if (route->ksnr_myipaddr == iface->ksni_ipaddr)
                                        break;

                                route = NULL;
                        }
                        if (route != NULL)
                                continue;

                        this_netmatch = (((iface->ksni_ipaddr ^
                                           newroute->ksnr_ipaddr) &
                                           iface->ksni_netmask) == 0) ? 1 : 0;

                        if (!(best_iface == NULL ||
                              best_netmatch < this_netmatch ||
                              (best_netmatch == this_netmatch &&
                               best_nroutes > iface->ksni_nroutes)))
                                continue;

                        best_iface = iface;
                        best_netmatch = this_netmatch;
                        best_nroutes = iface->ksni_nroutes;
                }

                if (best_iface == NULL)
                        continue;

                newroute->ksnr_myipaddr = best_iface->ksni_ipaddr;
                best_iface->ksni_nroutes++;

                ksocknal_add_route_locked(peer, newroute);
                newroute = NULL;
        }

        write_unlock_irqrestore(global_lock, flags);
        if (newroute != NULL)
                ksocknal_route_decref(newroute);
}

int
ksocknal_accept (lnet_ni_t *ni, struct socket *sock)
{
        ksock_connreq_t    *cr;
        int                 rc;
        __u32               peer_ip;
        int                 peer_port;
        unsigned long       flags;

        rc = libcfs_sock_getaddr(sock, 1, &peer_ip, &peer_port);
        LASSERT (rc == 0);                      /* we succeeded before */

        LIBCFS_ALLOC(cr, sizeof(*cr));
        if (cr == NULL) {
                LCONSOLE_ERROR("Dropping connection request from "
                               "%u.%u.%u.%u: memory exhausted\n",
                               HIPQUAD(peer_ip));
                return -ENOMEM;
        }

        lnet_ni_addref(ni);
        cr->ksncr_ni   = ni;
        cr->ksncr_sock = sock;

        spin_lock_irqsave(&ksocknal_data.ksnd_connd_lock, flags);

        list_add_tail(&cr->ksncr_list, &ksocknal_data.ksnd_connd_connreqs);
        wake_up(&ksocknal_data.ksnd_connd_waitq);
                        
        spin_unlock_irqrestore(&ksocknal_data.ksnd_connd_lock, flags);
        return 0;
}

int
ksocknal_create_conn (lnet_ni_t *ni, ksock_route_t *route, 
                      struct socket *sock, int type)
{
        rwlock_t          *global_lock = &ksocknal_data.ksnd_global_lock;
        __u32              ipaddrs[LNET_MAX_INTERFACES];
        int                nipaddrs;
        lnet_process_id_t   peerid;
        struct list_head  *tmp;
        __u64              incarnation;
        unsigned long      flags;
        ksock_conn_t      *conn;
        ksock_conn_t      *conn2;
        ksock_peer_t      *peer = NULL;
        ksock_peer_t      *peer2;
        ksock_sched_t     *sched;
        unsigned int       irq;
        ksock_tx_t        *tx;
        int                rc;

        LASSERT (route == NULL == (type == SOCKLND_CONN_NONE));

        rc = ksocknal_lib_setup_sock (sock);
        if (rc != 0)
                return (rc);

        irq = ksocknal_lib_sock_irq (sock);

        rc = -ENOMEM;
        LIBCFS_ALLOC(conn, sizeof(*conn));
        if (conn == NULL)
                goto failed_0;

        memset (conn, 0, sizeof (*conn));
        conn->ksnc_peer = NULL;
        conn->ksnc_route = NULL;
        conn->ksnc_sock = sock;
        atomic_set (&conn->ksnc_sock_refcount, 1); /* 1 ref for conn */
        conn->ksnc_type = type;
        ksocknal_lib_save_callback(sock, conn);
        atomic_set (&conn->ksnc_conn_refcount, 1); /* 1 ref for me */

        conn->ksnc_rx_ready = 0;
        conn->ksnc_rx_scheduled = 0;
        ksocknal_new_packet (conn, 0);

        CFS_INIT_LIST_HEAD (&conn->ksnc_tx_queue);
        conn->ksnc_tx_ready = 0;
        conn->ksnc_tx_scheduled = 0;
        atomic_set (&conn->ksnc_tx_nob, 0);

        /* stash conn's local and remote addrs */
        rc = ksocknal_lib_get_conn_addrs (conn);
        if (rc != 0)
                goto failed_1;

        /* Find out/confirm peer's NID and connection type and get the
         * vector of interfaces she's willing to let me connect to.
         * Passive connections use the listener timeout since the peer sends
         * eagerly */

        if (route != NULL) {
                LASSERT(ni == route->ksnr_peer->ksnp_ni);

                /* Active connection sends HELLO eagerly */
                nipaddrs = ksocknal_local_ipvec(ni, ipaddrs);
                peerid = route->ksnr_peer->ksnp_id;

                rc = ksocknal_send_hello (ni, conn, peerid.nid,
                                          ipaddrs, nipaddrs);
                if (rc != 0)
                        goto failed_1;
        } else {
                peerid.nid = LNET_NID_ANY;
                peerid.pid = LNET_PID_ANY;
        }

        rc = ksocknal_recv_hello (ni, conn, &peerid, &incarnation, ipaddrs);
        if (rc < 0)
                goto failed_1;
        nipaddrs = rc;
        LASSERT (peerid.nid != LNET_NID_ANY);

        if (route != NULL) {
                peer = route->ksnr_peer;
                ksocknal_peer_addref(peer);

                /* additional routes after interface exchange? */
                ksocknal_create_routes(peer, conn->ksnc_port,
                                       ipaddrs, nipaddrs);
                rc = 0;
        } else {
                rc = ksocknal_create_peer(&peer, ni, peerid);
                if (rc != 0)
                        goto failed_1;

                write_lock_irqsave(global_lock, flags);

                peer2 = ksocknal_find_peer_locked(ni, peerid);
                if (peer2 == NULL) {
                        /* NB this puts an "empty" peer in the peer
                         * table (which takes my ref) */
                        list_add_tail(&peer->ksnp_list,
                                      ksocknal_nid2peerlist(peerid.nid));
                } else  {
                        ksocknal_peer_decref(peer);
                        peer = peer2;
                }
                /* +1 ref for me */
                ksocknal_peer_addref(peer);

                write_unlock_irqrestore(global_lock, flags);

                nipaddrs = ksocknal_select_ips(peer, ipaddrs, nipaddrs);
                rc = ksocknal_send_hello (ni, conn, peerid.nid,
                                          ipaddrs, nipaddrs);
                if (rc < 0)
                        goto failed_2;
        }

        write_lock_irqsave (global_lock, flags);

        if (peer->ksnp_closing ||
            (route != NULL && route->ksnr_deleted)) {
                /* route/peer got closed under me */
                rc = -ESTALE;
                goto failed_3;
        }

        /* Refuse to duplicate an existing connection (both sides might
         * connect at once), unless this is a loopback connection */
        if (conn->ksnc_ipaddr != conn->ksnc_myipaddr) {
                list_for_each(tmp, &peer->ksnp_conns) {
                        conn2 = list_entry(tmp, ksock_conn_t, ksnc_list);

                        if (conn2->ksnc_ipaddr != conn->ksnc_ipaddr ||
                            conn2->ksnc_myipaddr != conn->ksnc_myipaddr ||
                            conn2->ksnc_type != conn->ksnc_type ||
                            conn2->ksnc_incarnation != incarnation)
                                continue;

                        CWARN("Not creating duplicate connection to "
                              "%u.%u.%u.%u type %d\n",
                              HIPQUAD(conn->ksnc_ipaddr), conn->ksnc_type);
                        rc = -EALREADY;
                        goto failed_3;
                }
        }

        /* If the connection created by this route didn't bind to the IP
         * address the route connected to, the connection/route matching
         * code below probably isn't going to work. */
        if (route != NULL &&
            route->ksnr_ipaddr != conn->ksnc_ipaddr) {
                CERROR("Route %s %u.%u.%u.%u connected to %u.%u.%u.%u\n",
                       libcfs_id2str(peer->ksnp_id),
                       HIPQUAD(route->ksnr_ipaddr),
                       HIPQUAD(conn->ksnc_ipaddr));
        }

        /* Search for a route corresponding to the new connection and
         * create an association.  This allows incoming connections created
         * by routes in my peer to match my own route entries so I don't
         * continually create duplicate routes. */
        list_for_each (tmp, &peer->ksnp_routes) {
                route = list_entry(tmp, ksock_route_t, ksnr_list);

                if (route->ksnr_ipaddr != conn->ksnc_ipaddr)
                        continue;

                ksocknal_associate_route_conn_locked(route, conn);
                break;
        }

        conn->ksnc_peer = peer;                 /* conn takes my ref on peer */
        conn->ksnc_incarnation = incarnation;
        peer->ksnp_last_alive = cfs_time_current();
        peer->ksnp_error = 0;

        sched = ksocknal_choose_scheduler_locked (irq);
        sched->kss_nconns++;
        conn->ksnc_scheduler = sched;

        /* Set the deadline for the outgoing HELLO to drain */
        conn->ksnc_tx_bufnob = SOCK_WMEM_QUEUED(sock);
        conn->ksnc_tx_deadline = cfs_time_shift(*ksocknal_tunables.ksnd_timeout);
        mb();       /* order with adding to peer's conn list */

        list_add (&conn->ksnc_list, &peer->ksnp_conns);
        ksocknal_conn_addref(conn);

        /* NB my callbacks block while I hold ksnd_global_lock */
        ksocknal_lib_set_callback(sock, conn);

        /* Take all the packets blocking for a connection.
         * NB, it might be nicer to share these blocked packets among any
         * other connections that are becoming established. */
        while (!list_empty (&peer->ksnp_tx_queue)) {
                tx = list_entry (peer->ksnp_tx_queue.next,
                                 ksock_tx_t, tx_list);

                list_del (&tx->tx_list);
                ksocknal_queue_tx_locked (tx, conn);
        }

        rc = ksocknal_close_stale_conns_locked(peer, incarnation);
        if (rc != 0)
                CERROR ("Closed %d stale conns to %s ip %d.%d.%d.%d\n",
                        rc, libcfs_id2str(conn->ksnc_peer->ksnp_id),
                        HIPQUAD(conn->ksnc_ipaddr));

        write_unlock_irqrestore (global_lock, flags);

        ksocknal_lib_bind_irq (irq);

        /* Call the callbacks right now to get things going. */
        if (ksocknal_connsock_addref(conn) == 0) {
                ksocknal_lib_act_callback(sock, conn);
                ksocknal_connsock_decref(conn);
        }

        CDEBUG(D_NET, "New conn %s %u.%u.%u.%u -> %u.%u.%u.%u/%d"
               " incarnation:"LPD64" sched[%d]/%d\n",
               libcfs_id2str(peerid), HIPQUAD(conn->ksnc_myipaddr),
               HIPQUAD(conn->ksnc_ipaddr), conn->ksnc_port, incarnation,
               (int)(conn->ksnc_scheduler - ksocknal_data.ksnd_schedulers), irq);

        ksocknal_conn_decref(conn);
        return (0);

 failed_3:
        if (!peer->ksnp_closing &&
            list_empty (&peer->ksnp_conns) &&
            list_empty (&peer->ksnp_routes))
                ksocknal_unlink_peer_locked(peer);
        write_unlock_irqrestore(global_lock, flags);

 failed_2:
        ksocknal_peer_decref(peer);

 failed_1:
        LIBCFS_FREE (conn, sizeof(*conn));

 failed_0:
        libcfs_sock_release(sock);

        LASSERT (rc != 0);
        return (rc);
}

void
ksocknal_close_conn_locked (ksock_conn_t *conn, int error)
{
        /* This just does the immmediate housekeeping, and queues the
         * connection for the reaper to terminate.
         * Caller holds ksnd_global_lock exclusively in irq context */
        ksock_peer_t      *peer = conn->ksnc_peer;
        ksock_route_t     *route;
        ksock_conn_t      *conn2;
        struct list_head  *tmp;

        LASSERT (peer->ksnp_error == 0);
        LASSERT (!conn->ksnc_closing);
        conn->ksnc_closing = 1;

        /* ksnd_deathrow_conns takes over peer's ref */
        list_del (&conn->ksnc_list);

        route = conn->ksnc_route;
        if (route != NULL) {
                /* dissociate conn from route... */
                LASSERT (!route->ksnr_deleted);
                LASSERT ((route->ksnr_connected & (1 << conn->ksnc_type)) != 0);

                conn2 = NULL;
                list_for_each(tmp, &peer->ksnp_conns) {
                        conn2 = list_entry(tmp, ksock_conn_t, ksnc_list);

                        if (conn2->ksnc_route == route &&
                            conn2->ksnc_type == conn->ksnc_type)
                                break;

                        conn2 = NULL;
                }
                if (conn2 == NULL)
                        route->ksnr_connected &= ~(1 << conn->ksnc_type);

                conn->ksnc_route = NULL;

#if 0           /* irrelevent with only eager routes */
                list_del (&route->ksnr_list);   /* make route least favourite */
                list_add_tail (&route->ksnr_list, &peer->ksnp_routes);
#endif
                ksocknal_route_decref(route);     /* drop conn's ref on route */
        }

        if (list_empty (&peer->ksnp_conns)) {
                /* No more connections to this peer */

                peer->ksnp_error = error;       /* stash last conn close reason */

                if (list_empty (&peer->ksnp_routes)) {
                        /* I've just closed last conn belonging to a
                         * peer with no routes to it */
                        ksocknal_unlink_peer_locked (peer);
                }
        }

        spin_lock (&ksocknal_data.ksnd_reaper_lock);

        list_add_tail (&conn->ksnc_list, &ksocknal_data.ksnd_deathrow_conns);
        cfs_waitq_signal (&ksocknal_data.ksnd_reaper_waitq);

        spin_unlock (&ksocknal_data.ksnd_reaper_lock);
}

void
ksocknal_terminate_conn (ksock_conn_t *conn)
{
        /* This gets called by the reaper (guaranteed thread context) to
         * disengage the socket from its callbacks and close it.
         * ksnc_refcount will eventually hit zero, and then the reaper will
         * destroy it. */
        unsigned long   flags;
        ksock_peer_t   *peer = conn->ksnc_peer;
        ksock_sched_t  *sched = conn->ksnc_scheduler;
        struct timeval  now;
        time_t          then = 0;
        int             notify = 0;

        LASSERT(conn->ksnc_closing);

        /* wake up the scheduler to "send" all remaining packets to /dev/null */
        spin_lock_irqsave(&sched->kss_lock, flags);

        if (!conn->ksnc_tx_scheduled &&
            !list_empty(&conn->ksnc_tx_queue)){
                list_add_tail (&conn->ksnc_tx_list,
                               &sched->kss_tx_conns);
                /* a closing conn is always ready to tx */
                conn->ksnc_tx_ready = 1;
                conn->ksnc_tx_scheduled = 1;
                /* extra ref for scheduler */
                ksocknal_conn_addref(conn);

                cfs_waitq_signal (&sched->kss_waitq);
        }

        spin_unlock_irqrestore (&sched->kss_lock, flags);

        /* serialise with callbacks */
        write_lock_irqsave (&ksocknal_data.ksnd_global_lock, flags);

        ksocknal_lib_reset_callback(conn->ksnc_sock, conn);

        /* OK, so this conn may not be completely disengaged from its
         * scheduler yet, but it _has_ committed to terminate... */
        conn->ksnc_scheduler->kss_nconns--;

        if (peer->ksnp_error != 0) {
                /* peer's last conn closed in error */
                LASSERT (list_empty (&peer->ksnp_conns));

                /* convert peer's last-known-alive timestamp from jiffies */
                do_gettimeofday (&now);
                then = now.tv_sec - cfs_duration_sec(cfs_time_sub(cfs_time_current(),
                                                                  peer->ksnp_last_alive));
                notify = 1;
        }

        write_unlock_irqrestore (&ksocknal_data.ksnd_global_lock, flags);

        /* The socket is closed on the final put; either here, or in
         * ksocknal_{send,recv}msg().  Since we set up the linger2 option
         * when the connection was established, this will close the socket
         * immediately, aborting anything buffered in it. Any hung
         * zero-copy transmits will therefore complete in finite time. */
        ksocknal_connsock_decref(conn);

        /* no auto-down for now */
        //        if (notify)
        //                lnet_notify (peer->ksnp_ni, peer->ksnp_id.nid, 0, then);
}

void
ksocknal_queue_zombie_conn (ksock_conn_t *conn)
{
        /* Queue the conn for the reaper to destroy */
        unsigned long flags;

        LASSERT (atomic_read(&conn->ksnc_conn_refcount) == 0);
        spin_lock_irqsave(&ksocknal_data.ksnd_reaper_lock, flags);

        list_add_tail(&conn->ksnc_list, &ksocknal_data.ksnd_zombie_conns);
        cfs_waitq_signal(&ksocknal_data.ksnd_reaper_waitq);
        
        spin_unlock_irqrestore(&ksocknal_data.ksnd_reaper_lock, flags);
}

void
ksocknal_destroy_conn (ksock_conn_t *conn)
{
        /* Final coup-de-grace of the reaper */
        CDEBUG (D_NET, "connection %p\n", conn);

        LASSERT (atomic_read (&conn->ksnc_conn_refcount) == 0);
        LASSERT (atomic_read (&conn->ksnc_sock_refcount) == 0);
        LASSERT (conn->ksnc_sock == NULL);
        LASSERT (conn->ksnc_route == NULL);
        LASSERT (!conn->ksnc_tx_scheduled);
        LASSERT (!conn->ksnc_rx_scheduled);
        LASSERT (list_empty(&conn->ksnc_tx_queue));

        /* complete current receive if any */
        switch (conn->ksnc_rx_state) {
        case SOCKNAL_RX_BODY:
                CERROR("Completing partial receive from %s"
                       ", ip %d.%d.%d.%d:%d, with error\n",
                       libcfs_id2str(conn->ksnc_peer->ksnp_id),
                       HIPQUAD(conn->ksnc_ipaddr), conn->ksnc_port);
                lnet_finalize (conn->ksnc_peer->ksnp_ni, 
                               conn->ksnc_cookie, -EIO);
                break;
        case SOCKNAL_RX_HEADER:
        case SOCKNAL_RX_SLOP:
                break;
        default:
                LBUG ();
                break;
        }

        ksocknal_peer_decref(conn->ksnc_peer);

        LIBCFS_FREE (conn, sizeof (*conn));
}

int
ksocknal_close_peer_conns_locked (ksock_peer_t *peer, __u32 ipaddr, int why)
{
        ksock_conn_t       *conn;
        struct list_head   *ctmp;
        struct list_head   *cnxt;
        int                 count = 0;

        list_for_each_safe (ctmp, cnxt, &peer->ksnp_conns) {
                conn = list_entry (ctmp, ksock_conn_t, ksnc_list);

                if (ipaddr == 0 ||
                    conn->ksnc_ipaddr == ipaddr) {
                        count++;
                        ksocknal_close_conn_locked (conn, why);
                }
        }

        return (count);
}

int
ksocknal_close_stale_conns_locked (ksock_peer_t *peer, __u64 incarnation)
{
        ksock_conn_t       *conn;
        struct list_head   *ctmp;
        struct list_head   *cnxt;
        int                 count = 0;

        list_for_each_safe (ctmp, cnxt, &peer->ksnp_conns) {
                conn = list_entry (ctmp, ksock_conn_t, ksnc_list);

                if (conn->ksnc_incarnation == incarnation)
                        continue;

                CDEBUG(D_NET, "Closing stale conn %s ip:%08x/%d "
                       "incarnation:"LPD64"("LPD64")\n",
                       libcfs_id2str(peer->ksnp_id), 
                       conn->ksnc_ipaddr, conn->ksnc_port,
                       conn->ksnc_incarnation, incarnation);

                count++;
                ksocknal_close_conn_locked (conn, -ESTALE);
        }

        return (count);
}

int
ksocknal_close_conn_and_siblings (ksock_conn_t *conn, int why)
{
        ksock_peer_t     *peer = conn->ksnc_peer;
        __u32             ipaddr = conn->ksnc_ipaddr;
        unsigned long     flags;
        int               count;

        write_lock_irqsave (&ksocknal_data.ksnd_global_lock, flags);

        count = ksocknal_close_peer_conns_locked (peer, ipaddr, why);

        write_unlock_irqrestore (&ksocknal_data.ksnd_global_lock, flags);

        return (count);
}

int
ksocknal_close_matching_conns (lnet_process_id_t id, __u32 ipaddr)
{
        unsigned long       flags;
        ksock_peer_t       *peer;
        struct list_head   *ptmp;
        struct list_head   *pnxt;
        int                 lo;
        int                 hi;
        int                 i;
        int                 count = 0;

        write_lock_irqsave (&ksocknal_data.ksnd_global_lock, flags);

        if (id.nid != LNET_NID_ANY)
                lo = hi = ksocknal_nid2peerlist(id.nid) - ksocknal_data.ksnd_peers;
        else {
                lo = 0;
                hi = ksocknal_data.ksnd_peer_hash_size - 1;
        }

        for (i = lo; i <= hi; i++) {
                list_for_each_safe (ptmp, pnxt, &ksocknal_data.ksnd_peers[i]) {

                        peer = list_entry (ptmp, ksock_peer_t, ksnp_list);

                        if (!((id.nid == LNET_NID_ANY || id.nid == peer->ksnp_id.nid) &&
                              (id.pid == LNET_PID_ANY || id.pid == peer->ksnp_id.pid)))
                                continue;

                        count += ksocknal_close_peer_conns_locked (peer, ipaddr, 0);
                }
        }

        write_unlock_irqrestore (&ksocknal_data.ksnd_global_lock, flags);

        /* wildcards always succeed */
        if (id.nid == LNET_NID_ANY || id.pid == LNET_PID_ANY || ipaddr == 0)
                return (0);

        return (count == 0 ? -ENOENT : 0);
}

void
ksocknal_notify (lnet_ni_t *ni, lnet_nid_t gw_nid, int alive)
{
        /* The router is telling me she's been notified of a change in
         * gateway state.... */
        lnet_process_id_t  id = {.nid = gw_nid, .pid = LNET_PID_ANY};

        CDEBUG (D_NET, "gw %s %s\n", libcfs_nid2str(gw_nid), 
                alive ? "up" : "down");

        if (!alive) {
                /* If the gateway crashed, close all open connections... */
                ksocknal_close_matching_conns (id, 0);
                return;
        }

        /* ...otherwise do nothing.  We can only establish new connections
         * if we have autroutes, and these connect on demand. */
}

void
ksocknal_push_peer (ksock_peer_t *peer)
{
        int               index;
        int               i;
        struct list_head *tmp;
        ksock_conn_t     *conn;

        for (index = 0; ; index++) {
                read_lock (&ksocknal_data.ksnd_global_lock);

                i = 0;
                conn = NULL;

                list_for_each (tmp, &peer->ksnp_conns) {
                        if (i++ == index) {
                                conn = list_entry (tmp, ksock_conn_t, ksnc_list);
                                ksocknal_conn_addref(conn);
                                break;
                        }
                }

                read_unlock (&ksocknal_data.ksnd_global_lock);

                if (conn == NULL)
                        break;

                ksocknal_lib_push_conn (conn);
                ksocknal_conn_decref(conn);
        }
}

int
ksocknal_push (lnet_ni_t *ni, lnet_process_id_t id)
{
        ksock_peer_t      *peer;
        struct list_head  *tmp;
        int                index;
        int                i;
        int                j;
        int                rc = -ENOENT;

        for (i = 0; i < ksocknal_data.ksnd_peer_hash_size; i++) {
                for (j = 0; ; j++) {
                        read_lock (&ksocknal_data.ksnd_global_lock);

                        index = 0;
                        peer = NULL;

                        list_for_each (tmp, &ksocknal_data.ksnd_peers[i]) {
                                peer = list_entry(tmp, ksock_peer_t,
                                                  ksnp_list);

                                if (!((id.nid == LNET_NID_ANY ||
                                       id.nid == peer->ksnp_id.nid) &&
                                      (id.pid == LNET_PID_ANY ||
                                       id.pid == peer->ksnp_id.pid))) {
                                        peer = NULL;
                                        continue;
                                }

                                if (index++ == j) {
                                        ksocknal_peer_addref(peer);
                                        break;
                                }
                        }

                        read_unlock (&ksocknal_data.ksnd_global_lock);

                        if (peer != NULL) {
                                rc = 0;
                                ksocknal_push_peer (peer);
                                ksocknal_peer_decref(peer);
                        }
                }

        }

        return (rc);
}

int
ksocknal_add_interface(lnet_ni_t *ni, __u32 ipaddress, __u32 netmask)
{
        ksock_net_t       *net = ni->ni_data;
        unsigned long      flags;
        ksock_interface_t *iface;
        int                rc;
        int                i;
        int                j;
        struct list_head  *ptmp;
        ksock_peer_t      *peer;
        struct list_head  *rtmp;
        ksock_route_t     *route;

        if (ipaddress == 0 ||
            netmask == 0)
                return (-EINVAL);

        write_lock_irqsave(&ksocknal_data.ksnd_global_lock, flags);

        iface = ksocknal_ip2iface(ni, ipaddress);
        if (iface != NULL) {
                /* silently ignore dups */
                rc = 0;
        } else if (net->ksnn_ninterfaces == LNET_MAX_INTERFACES) {
                rc = -ENOSPC;
        } else {
                iface = &net->ksnn_interfaces[net->ksnn_ninterfaces++];

                iface->ksni_ipaddr = ipaddress;
                iface->ksni_netmask = netmask;
                iface->ksni_nroutes = 0;
                iface->ksni_npeers = 0;

                for (i = 0; i < ksocknal_data.ksnd_peer_hash_size; i++) {
                        list_for_each(ptmp, &ksocknal_data.ksnd_peers[i]) {
                                peer = list_entry(ptmp, ksock_peer_t, ksnp_list);

                                for (j = 0; i < peer->ksnp_n_passive_ips; j++)
                                        if (peer->ksnp_passive_ips[j] == ipaddress)
                                                iface->ksni_npeers++;

                                list_for_each(rtmp, &peer->ksnp_routes) {
                                        route = list_entry(rtmp, ksock_route_t, ksnr_list);

                                        if (route->ksnr_myipaddr == ipaddress)
                                                iface->ksni_nroutes++;
                                }
                        }
                }

                rc = 0;
                /* NB only new connections will pay attention to the new interface! */
        }

        write_unlock_irqrestore(&ksocknal_data.ksnd_global_lock, flags);

        return (rc);
}

void
ksocknal_peer_del_interface_locked(ksock_peer_t *peer, __u32 ipaddr)
{
        struct list_head   *tmp;
        struct list_head   *nxt;
        ksock_route_t      *route;
        ksock_conn_t       *conn;
        int                 i;
        int                 j;

        for (i = 0; i < peer->ksnp_n_passive_ips; i++)
                if (peer->ksnp_passive_ips[i] == ipaddr) {
                        for (j = i+1; j < peer->ksnp_n_passive_ips; j++)
                                peer->ksnp_passive_ips[j-1] =
                                        peer->ksnp_passive_ips[j];
                        peer->ksnp_n_passive_ips--;
                        break;
                }

        list_for_each_safe(tmp, nxt, &peer->ksnp_routes) {
                route = list_entry (tmp, ksock_route_t, ksnr_list);

                if (route->ksnr_myipaddr != ipaddr)
                        continue;

                if (route->ksnr_share_count != 0) {
                        /* Manually created; keep, but unbind */
                        route->ksnr_myipaddr = 0;
                } else {
                        ksocknal_del_route_locked(route);
                }
        }

        list_for_each_safe(tmp, nxt, &peer->ksnp_conns) {
                conn = list_entry(tmp, ksock_conn_t, ksnc_list);

                if (conn->ksnc_myipaddr == ipaddr)
                        ksocknal_close_conn_locked (conn, 0);
        }
}

int
ksocknal_del_interface(lnet_ni_t *ni, __u32 ipaddress)
{
        ksock_net_t       *net = ni->ni_data;
        int                rc = -ENOENT;
        unsigned long      flags;
        struct list_head  *tmp;
        struct list_head  *nxt;
        ksock_peer_t      *peer;
        __u32              this_ip;
        int                i;
        int                j;

        write_lock_irqsave(&ksocknal_data.ksnd_global_lock, flags);

        for (i = 0; i < net->ksnn_ninterfaces; i++) {
                this_ip = net->ksnn_interfaces[i].ksni_ipaddr;

                if (!(ipaddress == 0 ||
                      ipaddress == this_ip))
                        continue;

                rc = 0;

                for (j = i+1; j < net->ksnn_ninterfaces; j++)
                        net->ksnn_interfaces[j-1] =
                                net->ksnn_interfaces[j];

                net->ksnn_ninterfaces--;

                for (j = 0; j < ksocknal_data.ksnd_peer_hash_size; j++) {
                        list_for_each_safe(tmp, nxt, &ksocknal_data.ksnd_peers[j]) {
                                peer = list_entry(tmp, ksock_peer_t, ksnp_list);

                                if (peer->ksnp_ni != ni)
                                        continue;

                                ksocknal_peer_del_interface_locked(peer, this_ip);
                        }
                }
        }

        write_unlock_irqrestore(&ksocknal_data.ksnd_global_lock, flags);

        return (rc);
}

int
ksocknal_ctl(lnet_ni_t *ni, unsigned int cmd, void *arg)
{
        struct libcfs_ioctl_data *data = arg;
        int rc;

        switch(cmd) {
        case IOC_LIBCFS_GET_INTERFACE: {
                ksock_net_t       *net = ni->ni_data;
                ksock_interface_t *iface;

                read_lock (&ksocknal_data.ksnd_global_lock);

                if (data->ioc_count < 0 ||
                    data->ioc_count >= net->ksnn_ninterfaces) {
                        rc = -ENOENT;
                } else {
                        rc = 0;
                        iface = &net->ksnn_interfaces[data->ioc_count];

                        data->ioc_u32[0] = iface->ksni_ipaddr;
                        data->ioc_u32[1] = iface->ksni_netmask;
                        data->ioc_u32[2] = iface->ksni_npeers;
                        data->ioc_u32[3] = iface->ksni_nroutes;
                }

                read_unlock (&ksocknal_data.ksnd_global_lock);
                return rc;
        }

        case IOC_LIBCFS_ADD_INTERFACE:
                return ksocknal_add_interface(ni,
                                              data->ioc_u32[0], /* IP address */
                                              data->ioc_u32[1]); /* net mask */

        case IOC_LIBCFS_DEL_INTERFACE:
                return ksocknal_del_interface(ni, 
                                              data->ioc_u32[0]); /* IP address */

        case IOC_LIBCFS_GET_PEER: {
                lnet_process_id_t id = {0,};
                __u32            myip = 0;
                __u32            ip = 0;
                int              port = 0;
                int              conn_count = 0;
                int              share_count = 0;

                rc = ksocknal_get_peer_info(ni, data->ioc_count,
                                            &id, &myip, &ip, &port,
                                            &conn_count,  &share_count);
                if (rc != 0)
                        return rc;
                        
                data->ioc_nid    = id.nid;
                data->ioc_count  = share_count;
                data->ioc_u32[0] = ip;
                data->ioc_u32[1] = port;
                data->ioc_u32[2] = myip;
                data->ioc_u32[3] = conn_count;
                data->ioc_u32[4] = id.pid;
                return 0;
        }

        case IOC_LIBCFS_ADD_PEER: {
                lnet_process_id_t  id = {.nid = data->ioc_nid,
                                        .pid = LUSTRE_SRV_LNET_PID};
                return ksocknal_add_peer (ni, id,
                                          data->ioc_u32[0], /* IP */
                                          data->ioc_u32[1]); /* port */
        }
        case IOC_LIBCFS_DEL_PEER: {
                lnet_process_id_t  id = {.nid = data->ioc_nid,
                                        .pid = LNET_PID_ANY};
                return ksocknal_del_peer (ni, id,
                                          data->ioc_u32[0]); /* IP */
        }
        case IOC_LIBCFS_GET_CONN: {
                int           txmem;
                int           rxmem;
                int           nagle;
                ksock_conn_t *conn = ksocknal_get_conn_by_idx (ni, data->ioc_count);

                if (conn == NULL)
                        return -ENOENT;

                ksocknal_lib_get_conn_tunables(conn, &txmem, &rxmem, &nagle);

                data->ioc_count  = txmem;
                data->ioc_nid    = conn->ksnc_peer->ksnp_id.nid;
                data->ioc_flags  = nagle;
                data->ioc_u32[0] = conn->ksnc_ipaddr;
                data->ioc_u32[1] = conn->ksnc_port;
                data->ioc_u32[2] = conn->ksnc_myipaddr;
                data->ioc_u32[3] = conn->ksnc_type;
                data->ioc_u32[4] = conn->ksnc_scheduler -
                                   ksocknal_data.ksnd_schedulers;
                data->ioc_u32[5] = rxmem;
                data->ioc_u32[6] = conn->ksnc_peer->ksnp_id.pid;
                ksocknal_conn_decref(conn);
                return 0;
        }

        case IOC_LIBCFS_CLOSE_CONNECTION: {
                lnet_process_id_t  id = {.nid = data->ioc_nid,
                                        .pid = LNET_PID_ANY};

                return ksocknal_close_matching_conns (id,
                                                      data->ioc_u32[0]);
        }
        case IOC_LIBCFS_REGISTER_MYNID:
                /* Ignore if this is a noop */
		if (data->ioc_nid == ni->ni_nid)
			return 0;

		CERROR("obsolete IOC_LIBCFS_REGISTER_MYNID: %s(%s)\n",
		       libcfs_nid2str(data->ioc_nid),
		       libcfs_nid2str(ni->ni_nid));
		return -EINVAL;

        case IOC_LIBCFS_PUSH_CONNECTION: {
                lnet_process_id_t  id = {.nid = data->ioc_nid,
                                        .pid = LNET_PID_ANY};
                
                return ksocknal_push(ni, id);
        }
        default:
                return -EINVAL;
        }
        /* not reached */
}

void
ksocknal_free_buffers (void)
{
        LASSERT (atomic_read(&ksocknal_data.ksnd_nactive_txs) == 0);

        if (ksocknal_data.ksnd_schedulers != NULL)
                LIBCFS_FREE (ksocknal_data.ksnd_schedulers,
                             sizeof (ksock_sched_t) * ksocknal_data.ksnd_nschedulers);

        LIBCFS_FREE (ksocknal_data.ksnd_peers,
                     sizeof (struct list_head) *
                     ksocknal_data.ksnd_peer_hash_size);
}

void
ksocknal_base_shutdown (void)
{
        ksock_sched_t *sched;
        int            i;
        unsigned long  flags;

        CDEBUG(D_MALLOC, "before NAL cleanup: kmem %d\n",
               atomic_read (&libcfs_kmemory));
        LASSERT (ksocknal_data.ksnd_nnets == 0);

        switch (ksocknal_data.ksnd_init) {
        default:
                LASSERT (0);

        case SOCKNAL_INIT_ALL:
                /* Wait for queued connreqs to clean up */
                i = 2;
                spin_lock_irqsave(&ksocknal_data.ksnd_connd_lock, flags);
                while (!list_empty(&ksocknal_data.ksnd_connd_connreqs)) {
                        spin_unlock_irqrestore(&ksocknal_data.ksnd_connd_lock,
                                               flags);
                        i++;
                        CDEBUG(((i & (-i)) == i) ? D_WARNING : D_NET, /* power of 2? */
                               "waiting for connreqs to clean up\n");
                        cfs_pause(cfs_time_seconds(1));

                        spin_lock_irqsave(&ksocknal_data.ksnd_connd_lock, flags);
                }
                spin_unlock_irqrestore(&ksocknal_data.ksnd_connd_lock, 
                                       flags);

                /* fall through */

        case SOCKNAL_INIT_DATA:
                LASSERT (ksocknal_data.ksnd_peers != NULL);
                for (i = 0; i < ksocknal_data.ksnd_peer_hash_size; i++) {
                        LASSERT (list_empty (&ksocknal_data.ksnd_peers[i]));
                }
                LASSERT (list_empty (&ksocknal_data.ksnd_enomem_conns));
                LASSERT (list_empty (&ksocknal_data.ksnd_zombie_conns));
                LASSERT (list_empty (&ksocknal_data.ksnd_connd_connreqs));
                LASSERT (list_empty (&ksocknal_data.ksnd_connd_routes));

                if (ksocknal_data.ksnd_schedulers != NULL)
                        for (i = 0; i < ksocknal_data.ksnd_nschedulers; i++) {
                                ksock_sched_t *kss =
                                        &ksocknal_data.ksnd_schedulers[i];

                                LASSERT (list_empty (&kss->kss_tx_conns));
                                LASSERT (list_empty (&kss->kss_rx_conns));
                                LASSERT (kss->kss_nconns == 0);
                        }

                /* flag threads to terminate; wake and wait for them to die */
                ksocknal_data.ksnd_shuttingdown = 1;
                cfs_waitq_broadcast (&ksocknal_data.ksnd_connd_waitq);
                cfs_waitq_broadcast (&ksocknal_data.ksnd_reaper_waitq);

                if (ksocknal_data.ksnd_schedulers != NULL)
                        for (i = 0; i < ksocknal_data.ksnd_nschedulers; i++) {
                                sched = &ksocknal_data.ksnd_schedulers[i];
                                cfs_waitq_broadcast(&sched->kss_waitq);
                        }

                i = 4;
                read_lock(&ksocknal_data.ksnd_global_lock);
                while (ksocknal_data.ksnd_nthreads != 0) {
                        i++;
                        CDEBUG(((i & (-i)) == i) ? D_WARNING : D_NET, /* power of 2? */
                               "waiting for %d threads to terminate\n",
                                ksocknal_data.ksnd_nthreads);
                        read_unlock(&ksocknal_data.ksnd_global_lock);
                        cfs_pause(cfs_time_seconds(1));
                        read_lock(&ksocknal_data.ksnd_global_lock);
                }
                read_unlock(&ksocknal_data.ksnd_global_lock);

                ksocknal_free_buffers();

                ksocknal_data.ksnd_init = SOCKNAL_INIT_NOTHING;
                break;
        }

        CDEBUG(D_MALLOC, "after NAL cleanup: kmem %d\n",
               atomic_read (&libcfs_kmemory));

        PORTAL_MODULE_UNUSE;
}


__u64
ksocknal_new_incarnation (void)
{
        struct timeval tv;

        /* The incarnation number is the time this module loaded and it
         * identifies this particular instance of the socknal.  Hopefully
         * we won't be able to reboot more frequently than 1MHz for the
         * forseeable future :) */

        do_gettimeofday(&tv);

        return (((__u64)tv.tv_sec) * 1000000) + tv.tv_usec;
}

int
ksocknal_base_startup (void)
{
        int               rc;
        int               i;

        LASSERT (ksocknal_data.ksnd_init == SOCKNAL_INIT_NOTHING);
        LASSERT (ksocknal_data.ksnd_nnets == 0);

        memset (&ksocknal_data, 0, sizeof (ksocknal_data)); /* zero pointers */

        ksocknal_data.ksnd_peer_hash_size = SOCKNAL_PEER_HASH_SIZE;
        LIBCFS_ALLOC (ksocknal_data.ksnd_peers,
                      sizeof (struct list_head) * ksocknal_data.ksnd_peer_hash_size);
        if (ksocknal_data.ksnd_peers == NULL)
                return -ENOMEM;

        for (i = 0; i < ksocknal_data.ksnd_peer_hash_size; i++)
                CFS_INIT_LIST_HEAD(&ksocknal_data.ksnd_peers[i]);

        rwlock_init(&ksocknal_data.ksnd_global_lock);

        spin_lock_init (&ksocknal_data.ksnd_reaper_lock);
        CFS_INIT_LIST_HEAD (&ksocknal_data.ksnd_enomem_conns);
        CFS_INIT_LIST_HEAD (&ksocknal_data.ksnd_zombie_conns);
        CFS_INIT_LIST_HEAD (&ksocknal_data.ksnd_deathrow_conns);
        cfs_waitq_init(&ksocknal_data.ksnd_reaper_waitq);

        spin_lock_init (&ksocknal_data.ksnd_connd_lock);
        CFS_INIT_LIST_HEAD (&ksocknal_data.ksnd_connd_connreqs);
        CFS_INIT_LIST_HEAD (&ksocknal_data.ksnd_connd_routes);
        cfs_waitq_init(&ksocknal_data.ksnd_connd_waitq);

        /* NB memset above zeros whole of ksocknal_data, including
         * ksocknal_data.ksnd_irqinfo[all].ksni_valid */

        /* flag lists/ptrs/locks initialised */
        ksocknal_data.ksnd_init = SOCKNAL_INIT_DATA;
        PORTAL_MODULE_USE;

        ksocknal_data.ksnd_nschedulers = ksocknal_nsched();
        LIBCFS_ALLOC(ksocknal_data.ksnd_schedulers,
                     sizeof(ksock_sched_t) * ksocknal_data.ksnd_nschedulers);
        if (ksocknal_data.ksnd_schedulers == NULL)
                goto failed;

        for (i = 0; i < ksocknal_data.ksnd_nschedulers; i++) {
                ksock_sched_t *kss = &ksocknal_data.ksnd_schedulers[i];

                spin_lock_init (&kss->kss_lock);
                CFS_INIT_LIST_HEAD (&kss->kss_rx_conns);
                CFS_INIT_LIST_HEAD (&kss->kss_tx_conns);
#if SOCKNAL_ZC
                CFS_INIT_LIST_HEAD (&kss->kss_zctxdone_list);
#endif
                cfs_waitq_init (&kss->kss_waitq);
        }

        for (i = 0; i < ksocknal_data.ksnd_nschedulers; i++) {
                rc = ksocknal_thread_start (ksocknal_scheduler,
                                            &ksocknal_data.ksnd_schedulers[i]);
                if (rc != 0) {
                        CERROR("Can't spawn socknal scheduler[%d]: %d\n",
                               i, rc);
                        goto failed;
                }
        }

        for (i = 0; i < *ksocknal_tunables.ksnd_nconnds; i++) {
                rc = ksocknal_thread_start (ksocknal_connd, (void *)((long)i));
                if (rc != 0) {
                        CERROR("Can't spawn socknal connd: %d\n", rc);
                        goto failed;
                }
        }

        rc = ksocknal_thread_start (ksocknal_reaper, NULL);
        if (rc != 0) {
                CERROR ("Can't spawn socknal reaper: %d\n", rc);
                goto failed;
        }

        /* flag everything initialised */
        ksocknal_data.ksnd_init = SOCKNAL_INIT_ALL;

        return 0;

 failed:
        ksocknal_base_shutdown();
        return -ENETDOWN;
}

void
ksocknal_shutdown (lnet_ni_t *ni)
{
        ksock_net_t      *net = ni->ni_data;
        int               i;
        unsigned long     flags;
        lnet_process_id_t  anyid = {.nid = LNET_NID_ANY,
                                   .pid = LNET_PID_ANY};

        LASSERT(ksocknal_data.ksnd_init == SOCKNAL_INIT_ALL);
        LASSERT(ksocknal_data.ksnd_nnets > 0);

        spin_lock_irqsave(&net->ksnn_lock, flags);
        net->ksnn_shutdown = 1;                 /* prevent new peers */
        spin_unlock_irqrestore(&net->ksnn_lock, flags);

        /* Delete all peers */
        ksocknal_del_peer(ni, anyid, 0);

        /* Wait for all peer state to clean up */
        i = 2;
        spin_lock_irqsave(&net->ksnn_lock, flags);
        while (net->ksnn_npeers != 0) {
                spin_unlock_irqrestore(&net->ksnn_lock, flags);

                i++;
                CDEBUG(((i & (-i)) == i) ? D_WARNING : D_NET, /* power of 2? */
                       "waiting for %d peers to disconnect\n",
                       net->ksnn_npeers);
                cfs_pause(cfs_time_seconds(1));

                spin_lock_irqsave(&net->ksnn_lock, flags);
        }
        spin_unlock_irqrestore(&net->ksnn_lock, flags);

        for (i = 0; i < net->ksnn_ninterfaces; i++) {
                LASSERT (net->ksnn_interfaces[i].ksni_npeers == 0);
                LASSERT (net->ksnn_interfaces[i].ksni_nroutes == 0);
        }

        LIBCFS_FREE(net, sizeof(*net));
        
        ksocknal_data.ksnd_nnets--;
        if (ksocknal_data.ksnd_nnets == 0)
                ksocknal_base_shutdown();
}

int
ksocknal_enumerate_interfaces(ksock_net_t *net)
{
        char      **names;
        int         i;
        int         j;
        int         rc;
        int         n;
                
        n = libcfs_ipif_enumerate(&names);
        if (n <= 0) {
                CERROR("Can't enumerate interfaces: %d\n", n);
                return n;
        }

        for (i = j = 0; i < n; i++) {
                int        up;
                __u32      ip;
                __u32      mask;
                if (!strcmp(names[i], "lo")) /* skip the loopback IF */
                        continue;
                rc = libcfs_ipif_query(names[i], &up, &ip, &mask);
                if (rc != 0) {
                        CWARN("Can't get interface %s info: %d\n",
                              names[i], rc);
                        continue;
                }
                
                if (!up) {
                        CWARN("Ignoring interface %s (down)\n",
                              names[i]);
                        continue;
                }

                if (j == LNET_MAX_INTERFACES) {
                        CWARN("Ignoring interface %s (too many interfaces)\n",
                              names[i]);
                        continue;
                }

                net->ksnn_interfaces[j].ksni_ipaddr = ip;
                net->ksnn_interfaces[j].ksni_netmask = mask;
                j++;
        }

        libcfs_ipif_free_enumeration(names, n);
        
        if (j == 0)
                CERROR("Can't find any usable interfaces\n");
        
        return j;
}

int
ksocknal_startup (lnet_ni_t *ni)
{
        ksock_net_t  *net;
        int           rc;
        int           i;

        LASSERT (ni->ni_lnd == &the_ksocklnd);

        if (ksocknal_data.ksnd_init == SOCKNAL_INIT_NOTHING) {
                rc = ksocknal_base_startup();
                if (rc != 0)
                        return rc;
        }
        
        LIBCFS_ALLOC(net, sizeof(*net));
        if (net == NULL)
                goto fail_0;
                
        memset(net, 0, sizeof(*net));
        spin_lock_init(&net->ksnn_lock);
        net->ksnn_incarnation = ksocknal_new_incarnation();
        ni->ni_data = net;
        ni->ni_maxtxcredits = *ksocknal_tunables.ksnd_credits;
        ni->ni_peertxcredits = *ksocknal_tunables.ksnd_peercredits;
        
        if (ni->ni_interfaces[0] == NULL) {
                rc = ksocknal_enumerate_interfaces(net);
                if (rc <= 0)
                        goto fail_1;

                net->ksnn_ninterfaces = rc;
        } else {
                for (i = 0; i < LNET_MAX_INTERFACES; i++) {
                        int    up;

                        if (ni->ni_interfaces[i] == NULL)
                                break;

                        rc = libcfs_ipif_query(
                                ni->ni_interfaces[i], &up,
                                &net->ksnn_interfaces[i].ksni_ipaddr,
                                &net->ksnn_interfaces[i].ksni_netmask);
                        
                        if (rc != 0) {
                                CERROR("Can't get interface %s info: %d\n",
                                       ni->ni_interfaces[i], rc);
                                goto fail_1;
                        }
                        
                        if (!up) {
                                CERROR("Interface %s is down\n",
                                       ni->ni_interfaces[i]);
                                goto fail_1;
                        }
                }
                net->ksnn_ninterfaces = i;
        }

        ni->ni_nid = LNET_MKNID(LNET_NIDNET(ni->ni_nid),
                                net->ksnn_interfaces[0].ksni_ipaddr);

        ksocknal_data.ksnd_nnets++;

        return 0;
        
 fail_1:
        LIBCFS_FREE(net, sizeof(*net));
 fail_0:
        if (ksocknal_data.ksnd_nnets == 0)
                ksocknal_base_shutdown();

        return -ENETDOWN;
}


void __exit
ksocknal_module_fini (void)
{
        lnet_unregister_lnd(&the_ksocklnd);
        ksocknal_lib_tunables_fini();
}

int __init
ksocknal_module_init (void)
{
        int    rc;

        /* check ksnr_connected/connecting field large enough */
        CLASSERT(SOCKLND_CONN_NTYPES <= 4);
        
        rc = ksocknal_lib_tunables_init();
        if (rc != 0)
                return rc;

        lnet_register_lnd(&the_ksocklnd);

        return 0;
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Kernel TCP Socket NAL v1.0.0");
MODULE_LICENSE("GPL");

cfs_module(ksocknal, "1.0.0", ksocknal_module_init, ksocknal_module_fini);
