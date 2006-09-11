/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2004 Cluster File Systems, Inc.
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
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>

#include <fcntl.h>
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef _AIX
#include "syscall_AIX.h"
#else
#include <syscall.h>
#endif
#include <sys/utsname.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#include <sys/socket.h>
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_CATAMOUNT_DATA_H
#include <catamount/data.h>
#endif

#include "lutil.h"


unsigned int libcfs_subsystem_debug = ~0 - (S_LNET | S_LND);
unsigned int libcfs_debug = 0;

struct task_struct     *current;

void *inter_module_get(char *arg)
{
        if (!strcmp(arg, "ldlm_cli_cancel_unused"))
                return ldlm_cli_cancel_unused;
        else if (!strcmp(arg, "ldlm_namespace_cleanup"))
                return ldlm_namespace_cleanup;
        else if (!strcmp(arg, "ldlm_replay_locks"))
                return ldlm_replay_locks;
#ifdef HAVE_QUOTA_SUPPORT
        else if (!strcmp(arg, "osc_quota_interface"))
                return &osc_quota_interface;
        else if (!strcmp(arg, "mdc_quota_interface"))
                return &mdc_quota_interface;
        else if (!strcmp(arg, "lov_quota_interface"))
                return &lov_quota_interface;
#endif
        else
                return NULL;
}

/*
 * random number generator stuff
 */

#ifdef HAVE_GETHOSTBYNAME
static int get_ipv4_addr()
{
        struct utsname myname;
        struct hostent *hptr;
        int ip;

        if (uname(&myname) < 0)
                return 0;

        hptr = gethostbyname(myname.nodename);
        if (hptr == NULL ||
            hptr->h_addrtype != AF_INET ||
            *hptr->h_addr_list == NULL) {
                CWARN("Warning: fail to get local IPv4 address\n");
                return 0;
        }

        ip = ntohl(*((int *) *hptr->h_addr_list));

        return ip;
}
#endif

void liblustre_init_random()
{
        int _rand_dev_fd;
        int seed[2];
        struct timeval tv;

#ifdef LIBLUSTRE_USE_URANDOM
        _rand_dev_fd = syscall(SYS_open, "/dev/urandom", O_RDONLY);
        if (_rand_dev_fd >= 0) {
                if (syscall(SYS_read, _rand_dev_fd,
                            &seed, sizeof(seed)) == sizeof(seed)) {
                        ll_srand(seed[0], seed[1]);
                        return;
                }
                syscall(SYS_close, _rand_dev_fd);
        }
#endif /* LIBLUSTRE_USE_URANDOM */

#ifdef HAVE_GETHOSTBYNAME
        seed[0] = get_ipv4_addr();
#else
        seed[0] = _my_pnid;
#endif
        gettimeofday(&tv, NULL);
        ll_srand(tv.tv_sec ^ __swab32(seed[0]), tv.tv_usec ^__swab32(getpid()));
}

void get_random_bytes(void *buf, int size)
{
        int *p = buf;
        int rem;
        LASSERT(size >= 0);

        rem = min((unsigned long)buf & (sizeof(int) - 1), size);
        if (rem) {
                int val = ll_rand();
                memcpy(buf, &val, rem);
                p = buf + rem;
                size -= rem;
        }

        while (size >= sizeof(int)) {
                *p = ll_rand();
                size -= sizeof(int);
                p++;
        }
        buf = p;
        if (size) {
                int val = ll_rand();
                memcpy(buf, &val, size);
        }
}
 
static void init_capability(int *res)
{
#ifdef HAVE_LIBCAP
        cap_t syscap;
        cap_flag_value_t capval;
        int i;

        *res = 0;

        syscap = cap_get_proc();
        if (!syscap) {
                CWARN("Warning: failed to get system capability, "
                      "set to minimal\n");
                return;
        }

        for (i = 0; i < sizeof(cap_value_t) * 8; i++) {
                if (!cap_get_flag(syscap, i, CAP_EFFECTIVE, &capval)) {
                        if (capval == CAP_SET) {
                                *res |= 1 << i;
                        }
                }
        }
#else
	/*
	 * set fake cap flags to ship to linux server
	 * from client platforms that have none (eg. catamount)
	 *  full capability for root
	 *  no capability for anybody else
	 */
#define FAKE_ROOT_CAP 0x1ffffeff
#define FAKE_USER_CAP 0

	*res = (current->fsuid == 0) ? FAKE_ROOT_CAP: FAKE_USER_CAP;
#endif
}

int in_group_p(gid_t gid)
{
        int i;

        if (gid == current->fsgid)
                return 1;

        for (i = 0; i < current->ngroups; i++) {
                if (gid == current->groups[i])
                        return 1;
        }

        return 0;
}

int liblustre_init_current(char *comm)
{
        current = malloc(sizeof(*current));
        if (!current) {
                CERROR("Not enough memory\n");
                return -ENOMEM;
        }

        strncpy(current->comm, comm, sizeof(current->comm));
        current->pid = getpid();
        current->gid = getgid();
        current->fsuid = geteuid();
        current->fsgid = getegid();
        memset(&current->pending, 0, sizeof(current->pending));

        current->max_groups = sysconf(_SC_NGROUPS_MAX);
        current->groups = malloc(sizeof(gid_t) * current->max_groups);
        if (!current->groups) {
                CERROR("Not enough memory\n");
                return -ENOMEM;
        }
        current->ngroups = getgroups(current->max_groups, current->groups);
        if (current->ngroups < 0) {
                perror("Error getgroups");
                return -EINVAL;
        }

        init_capability(&current->cap_effective);

        return 0;
}

void generate_random_uuid(unsigned char uuid_out[16])
{
        get_random_bytes(uuid_out, sizeof(uuid_out));
}

int init_lib_portals()
{
        int rc;
        ENTRY;

        rc = libcfs_debug_init(5 * 1024 * 1024);
        if (rc != 0) {
                CERROR("libcfs_debug_init() failed: %d\n", rc);
                RETURN (-ENXIO);
        }

        rc = LNetInit();
        if (rc != 0) {
                CERROR("LNetInit() failed: %d\n", rc);
                RETURN (-ENXIO);
        }
        RETURN(0);
}

extern void ptlrpc_exit_portals(void);
void cleanup_lib_portals()
{
        libcfs_debug_cleanup();
        ptlrpc_exit_portals();
}
