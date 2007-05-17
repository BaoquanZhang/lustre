/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2006 Cluster File Systems, Inc.
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

#include "o2iblnd.h"

static int service = 987;
CFS_MODULE_PARM(service, "i", int, 0444,
                "service number (within RDMA_PS_TCP)");

static int cksum = 0;
CFS_MODULE_PARM(cksum, "i", int, 0644,
		"set non-zero to enable message (not RDMA) checksums");

static int timeout = 50;
CFS_MODULE_PARM(timeout, "i", int, 0644,
		"timeout (seconds)");

static int ntx = 256;
CFS_MODULE_PARM(ntx, "i", int, 0444,
		"# of message descriptors");

static int credits = 64;
CFS_MODULE_PARM(credits, "i", int, 0444,
		"# concurrent sends");

static int peer_credits = 8;
CFS_MODULE_PARM(peer_credits, "i", int, 0444,
		"# concurrent sends to 1 peer");

static char *ipif_name = "ib0";
CFS_MODULE_PARM(ipif_name, "s", charp, 0444,
                "IPoIB interface name");

static int retry_count = 5;
CFS_MODULE_PARM(retry_count, "i", int, 0644,
                "Retransmissions when no ACK received");

static int rnr_retry_count = 6;
CFS_MODULE_PARM(rnr_retry_count, "i", int, 0644,
                "RNR retransmissions");

static int keepalive = 100;
CFS_MODULE_PARM(keepalive, "i", int, 0644,
                "Idle time in seconds before sending a keepalive");

static int ib_mtu = 0;
CFS_MODULE_PARM(ib_mtu, "i", int, 0444,
                "IB MTU 256/512/1024/2048/4096");

#if IBLND_MAP_ON_DEMAND
static int concurrent_sends = IBLND_RX_MSGS;
#else
static int concurrent_sends = IBLND_MSG_QUEUE_SIZE;
#endif
CFS_MODULE_PARM(concurrent_sends, "i", int, 0444,
                "send work-queue sizing");

#if IBLND_MAP_ON_DEMAND
static int fmr_pool_size = 512;
CFS_MODULE_PARM(fmr_pool_size, "i", int, 0444,
                "size of the fmr pool (>= ntx)");

static int fmr_flush_trigger = 384;
CFS_MODULE_PARM(fmr_flush_trigger, "i", int, 0444,
                "# dirty FMRs that triggers pool flush");

static int fmr_cache = 1;
CFS_MODULE_PARM(fmr_cache, "i", int, 0444,
                "non-zero to enable FMR caching");
#endif

kib_tunables_t kiblnd_tunables = {
        .kib_service                = &service,
        .kib_cksum                  = &cksum,
        .kib_timeout                = &timeout,
        .kib_keepalive              = &keepalive,
        .kib_ntx                    = &ntx,
        .kib_credits                = &credits,
        .kib_peercredits            = &peer_credits,
        .kib_default_ipif           = &ipif_name,
        .kib_retry_count            = &retry_count,
        .kib_rnr_retry_count        = &rnr_retry_count,
        .kib_concurrent_sends       = &concurrent_sends,
        .kib_ib_mtu                 = &ib_mtu,
#if IBLND_MAP_ON_DEMAND
        .kib_fmr_pool_size          = &fmr_pool_size,
        .kib_fmr_flush_trigger      = &fmr_flush_trigger,
        .kib_fmr_cache              = &fmr_cache,
#endif
};

#if CONFIG_SYSCTL && !CFS_SYSFS_MODULE_PARM

static char ipif_basename_space[32];

static ctl_table kiblnd_ctl_table[] = {
	{1, "service", &service, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{2, "cksum", &cksum, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{3, "timeout", &timeout, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{4, "ntx", &ntx, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{5, "credits", &credits, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{6, "peer_credits", &peer_credits, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{7, "ipif_name", ipif_basename_space, 
	 sizeof(ipif_basename_space), 0444, NULL, &proc_dostring},
	{8, "retry_count", &retry_count, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{9, "rnr_retry_count", &rnr_retry_count, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{10, "keepalive", &keepalive, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{11, "concurrent_sends", &concurrent_sends, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{12, "ib_mtu", &ib_mtu, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
#if IBLND_MAP_ON_DEMAND
	{12, "fmr_pool_size", &fmr_pool_size, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{13, "fmr_flush_trigger", &fmr_flush_trigger, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{14, "fmr_cache", &fmr_cache, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
#endif
	{0}
};

static ctl_table kiblnd_top_ctl_table[] = {
	{203, "o2iblnd", NULL, 0, 0555, kiblnd_ctl_table},
	{0}
};

void
kiblnd_initstrtunable(char *space, char *str, int size)
{
        strncpy(space, str, size);
        space[size-1] = 0;
}

void
kiblnd_sysctl_init (void)
{
        kiblnd_initstrtunable(ipif_basename_space, ipif_name,
                              sizeof(ipif_basename_space));

	kiblnd_tunables.kib_sysctl =
		cfs_register_sysctl_table(kiblnd_top_ctl_table, 0);

	if (kiblnd_tunables.kib_sysctl == NULL)
		CWARN("Can't setup /proc tunables\n");
}

void
kiblnd_sysctl_fini (void)
{
	if (kiblnd_tunables.kib_sysctl != NULL)
		cfs_unregister_sysctl_table(kiblnd_tunables.kib_sysctl);
}

#else

void
kiblnd_sysctl_init (void)
{
}

void
kiblnd_sysctl_fini (void)
{
}

#endif

int
kiblnd_tunables_init (void)
{
        kiblnd_sysctl_init();

        if (*kiblnd_tunables.kib_concurrent_sends > IBLND_RX_MSGS)
                *kiblnd_tunables.kib_concurrent_sends = IBLND_RX_MSGS;
        if (*kiblnd_tunables.kib_concurrent_sends < IBLND_MSG_QUEUE_SIZE)
                *kiblnd_tunables.kib_concurrent_sends = IBLND_MSG_QUEUE_SIZE;

	return 0;
}

void
kiblnd_tunables_fini (void)
{
        kiblnd_sysctl_fini();
}



