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

#include "openibnal.h"

static int n_connd = IBNAL_N_CONND;
CFS_MODULE_PARM(n_connd, "i", int, 0444,
                "# of connection daemons");

static int min_reconnect_interval = IBNAL_MIN_RECONNECT_INTERVAL;
CFS_MODULE_PARM(min_reconnect_interval, "i", int, 0644,
		"minimum connection retry interval (seconds)");

static int max_reconnect_interval = IBNAL_MAX_RECONNECT_INTERVAL;
CFS_MODULE_PARM(max_reconnect_interval, "i", int, 0644,
		"maximum connection retry interval (seconds)");

static int concurrent_peers = IBNAL_CONCURRENT_PEERS;
CFS_MODULE_PARM(concurrent_peers, "i", int, 0444,
		"maximum number of peers that may connect");

static int cksum = IBNAL_CKSUM;
CFS_MODULE_PARM(cksum, "i", int, 0644,
		"set non-zero to enable message (not RDMA) checksums");

static int timeout = IBNAL_TIMEOUT;
CFS_MODULE_PARM(timeout, "i", int, 0644,
		"timeout (seconds)");

static int listener_timeout = IBNAL_LISTENER_TIMEOUT;
CFS_MODULE_PARM(listener_timeout, "i", int, 0644,
		"passive connection timeout (seconds)");

static int backlog = IBNAL_BACKLOG;
CFS_MODULE_PARM(backlog, "i", int, 0444,
		"passive connection (listen) backlog");

static int port = IBNAL_PORT;
CFS_MODULE_PARM(port, "i", int, 0444,
		"connection request TCP/IP port");

static int ntx = IBNAL_NTX;
CFS_MODULE_PARM(ntx, "i", int, 0444,
		"# of 'normal' message descriptors");

static int ntx_nblk = IBNAL_NTX_NBLK;
CFS_MODULE_PARM(ntx_nblk, "i", int, 0444,
		"# of 'reserved' message descriptors");

kib_tunables_t kibnal_tunables = {
	.kib_n_connd                = &n_connd,
        .kib_min_reconnect_interval = &min_reconnect_interval,
        .kib_max_reconnect_interval = &max_reconnect_interval,
        .kib_concurrent_peers       = &concurrent_peers,
	.kib_cksum                  = &cksum,
        .kib_timeout                = &timeout,
        .kib_listener_timeout       = &listener_timeout,
	.kib_backlog                = &backlog,
	.kib_port                   = &port,
        .kib_ntx                    = &ntx,
        .kib_ntx_nblk               = &ntx_nblk,
};

#if CONFIG_SYSCTL && !CFS_SYSFS_MODULE_PARM

static ctl_table kibnal_ctl_table[] = {
	{1, "n_connd", &n_connd, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{2, "min_reconnect_interval", &min_reconnect_interval, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{3, "max_reconnect_interval", &max_reconnect_interval, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{4, "concurrent_peers", &concurrent_peers, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{5, "cksum", &cksum, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{6, "timeout", &timeout, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{7, "listener_timeout", &listener_timeout, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{8, "backlog", &backlog, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{9, "port", &port, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{10, "ntx", &ntx, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{11, "ntx_nblk", &ntx_nblk, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{0}
};

static ctl_table kibnal_top_ctl_table[] = {
	{203, "openibnal", NULL, 0, 0555, kibnal_ctl_table},
	{0}
};

int
kibnal_tunables_init ()
{
	kibnal_tunables.kib_sysctl =
		register_sysctl_table(kibnal_top_ctl_table, 0);
	
	if (kibnal_tunables.kib_sysctl == NULL)
		CWARN("Can't setup /proc tunables\n");

	return 0;
}

void
kibnal_tunables_fini ()
{
	if (kibnal_tunables.kib_sysctl != NULL)
		unregister_sysctl_table(kibnal_tunables.kib_sysctl);
}

#else

int
kibnal_tunables_init ()
{
	return 0;
}

void
kibnal_tunables_fini ()
{
}

#endif
