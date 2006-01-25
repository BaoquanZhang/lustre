/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre curproc API implementation for XNU kernel
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 * Author: Nikita Danilov <nikita@clusterfs.com>
 *
 * This file is part of Lustre, http://www.lustre.org.
 *
 * Lustre is free software; you can redistribute it and/or modify it under the
 * terms of version 2 of the GNU General Public License as published by the
 * Free Software Foundation. Lustre is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details. You should have received a copy of the GNU
 * General Public License along with Lustre; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/libcfs.h>
#include <libcfs/kp30.h>

/*
 * Implementation of cfs_curproc API (see lnet/include/libcfs/curproc.h)
 * for XNU kernel.
 */

static inline struct ucred *curproc_ucred(void)
{
#ifdef __DARWIN8__
        return proc_ucred(current_proc());
#else
        return current_proc()->p_cred->pc_ucred;
#endif
}

uid_t  cfs_curproc_uid(void)
{
        return curproc_ucred()->cr_uid;
}

gid_t  cfs_curproc_gid(void)
{
        LASSERT(curproc_ucred()->cr_ngroups > 0);
        return curproc_ucred()->cr_groups[0];
}

uid_t  cfs_curproc_fsuid(void)
{
#ifdef __DARWIN8__
        return curproc_ucred()->cr_ruid;
#else
        return current_proc()->p_cred->p_ruid;
#endif
}

gid_t  cfs_curproc_fsgid(void)
{
#ifdef __DARWIN8__
        return curproc_ucred()->cr_rgid;
#else
        return current_proc()->p_cred->p_rgid;
#endif
}

pid_t  cfs_curproc_pid(void)
{
#ifdef __DARWIN8__
        return proc_pid(current_proc());
#else
        return current_proc()->p_pid;
#endif
}

int    cfs_curproc_groups_nr(void)
{
        LASSERT(curproc_ucred()->cr_ngroups > 0);
        return curproc_ucred()->cr_ngroups - 1;
}

int    cfs_curproc_is_in_groups(gid_t gid)
{
        int i;
        struct ucred *cr;

        cr = curproc_ucred();
        LASSERT(cr != NULL);

        for (i = 0; i < cr->cr_ngroups; ++ i) {
                if (cr->cr_groups[i] == gid)
                        return 1;
        }
        return 0;
}

void   cfs_curproc_groups_dump(gid_t *array, int size)
{
        struct ucred *cr;

        cr = curproc_ucred();
        LASSERT(cr != NULL);
        CLASSERT(sizeof array[0] == sizeof (__u32));

        size = min_t(int, size, cr->cr_ngroups);
        memcpy(array, &cr->cr_groups[1], size * sizeof(gid_t));
}

mode_t cfs_curproc_umask(void)
{
#ifdef __DARWIN8__
        /*
         * XXX Liang:
         *
         * fd_cmask is not available in kexts, so we just assume 
         * verything is permited.
         */
        return -1;
#else
        return current_proc()->p_fd->fd_cmask;
#endif
}

char  *cfs_curproc_comm(void)
{
#ifdef __DARWIN8__
        /*
         * Writing to proc->p_comm is not permited in Darwin8,
         * because proc_selfname() only return a copy of proc->p_comm,
         * so this function is not really working.
         */
        static char     pcomm[MAXCOMLEN+1];

        proc_selfname(pcomm, MAXCOMLEN+1);
        return pcomm;
#else
        return current_proc()->p_comm;
#endif
}

cfs_kernel_cap_t cfs_curproc_cap_get(void)
{
        return -1;
}

void cfs_curproc_cap_set(cfs_kernel_cap_t cap)
{
        return;
}


/*
 * Local variables:
 * c-indentation-style: "K&R"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 80
 * scroll-step: 1
 * End:
 */
