/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Portals
 *   http://sourceforge.net/projects/sandiaportals/
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
 *
 */

#ifndef _KPTLROUTER_H
#define _KPTLROUTER_H
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/init.h>

#define DEBUG_SUBSYSTEM S_PTLROUTER

#include <linux/kp30.h>
#include <linux/kpr.h>
#include <portals/p30.h>
#include <portals/lib-p30.h>

typedef struct
{
	struct list_head	kpne_list;
	kpr_nal_interface_t     kpne_interface;
	atomic_t                kpne_refcount;
	int                     kpne_shutdown;
} kpr_nal_entry_t;

typedef struct
{
        struct list_head        kpge_list;
        atomic_t                kpge_weight;
        time_t                  kpge_timestamp;
        int                     kpge_alive;
        int                     kpge_nalid;
        int                     kpge_refcount;
        ptl_nid_t               kpge_nid;
} kpr_gateway_entry_t;

typedef struct
{
	struct list_head   	kpre_list;
        kpr_gateway_entry_t    *kpre_gateway;
	ptl_nid_t           	kpre_lo_nid;
        ptl_nid_t               kpre_hi_nid;
} kpr_route_entry_t;

typedef struct
{
        work_struct_t           kpru_tq;
        int                     kpru_nal_id;
        ptl_nid_t               kpru_nid;
        int                     kpru_alive;
        time_t                  kpru_when;
} kpr_upcall_t;

extern int kpr_register_nal (kpr_nal_interface_t *nalif, void **argp);
extern int kpr_lookup_target (void *arg, ptl_nid_t target_nid, int nob, 
                              ptl_nid_t *gateway_nidp);
extern kpr_nal_entry_t *kpr_find_nal_entry_locked (int nal_id);
extern void kpr_forward_packet (void *arg, kpr_fwd_desc_t *fwd);
extern void kpr_complete_packet (void *arg, kpr_fwd_desc_t *fwd, int error);
extern void kpr_nal_notify (void *arg, ptl_nid_t peer,
                            int alive, time_t when);
extern void kpr_shutdown_nal (void *arg);
extern void kpr_deregister_nal (void *arg);

extern void kpr_proc_init (void);
extern void kpr_proc_fini (void);

extern int kpr_add_route (int gateway_nal, ptl_nid_t gateway_nid, 
                          ptl_nid_t lo_nid, ptl_nid_t hi_nid);
extern int kpr_del_route (int gw_nal, ptl_nid_t gw_nid,
                          ptl_nid_t lo, ptl_nid_t hi);
extern int kpr_get_route (int idx, int *gateway_nal, ptl_nid_t *gateway_nid, 
                          ptl_nid_t *lo_nid, ptl_nid_t *hi_nid, int *alive);
extern int kpr_sys_notify (int gw_nalid, ptl_nid_t gw_nid,
                           int alive, time_t when);

extern unsigned int       kpr_routes_generation;
extern unsigned long long kpr_fwd_bytes;
extern unsigned long      kpr_fwd_packets;
extern unsigned long      kpr_fwd_errors;
extern atomic_t           kpr_queue_depth;
extern struct list_head   kpr_routes;
extern rwlock_t           kpr_rwlock;

#endif /* _KPLROUTER_H */
