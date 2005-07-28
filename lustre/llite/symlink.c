/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002 Cluster File Systems, Inc.
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

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/stat.h>
#include <linux/smp_lock.h>
#include <linux/version.h>
#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/lustre_lite.h>
#include "llite_internal.h"

static int ll_readlink_internal(struct inode *inode,
                                struct ptlrpc_request **request,
                                char **symname)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct lustre_id id;
        struct mds_body *body;
        int rc, symlen = inode->i_size + 1;
        ENTRY;

        *request = NULL;

        if (lli->lli_symlink_name) {
                *symname = lli->lli_symlink_name;
                CDEBUG(D_INODE, "using cached symlink %s\n", *symname);
                RETURN(0);
        }

        ll_inode2id(&id, inode);
        rc = md_getattr(sbi->ll_md_exp, &id, OBD_MD_LINKNAME, NULL, symlen,
                        request);

        if (rc) {
                if (rc != -ENOENT)
                        CERROR("inode %lu: rc = %d\n", inode->i_ino, rc);
                GOTO(failed, rc);
        }

        body = lustre_msg_buf ((*request)->rq_repmsg, 0, sizeof (*body));
        LASSERT (body != NULL);
        LASSERT_REPSWABBED (*request, 0);

        if ((body->valid & OBD_MD_LINKNAME) == 0) {
                CERROR ("OBD_MD_LINKNAME not set on reply\n");
                GOTO (failed, rc = -EPROTO);
        }
        
        LASSERT (symlen != 0);
        if (body->eadatasize != symlen) {
                CERROR ("inode %lu: symlink length %d not expected %d\n",
                        inode->i_ino, body->eadatasize - 1, symlen - 1);
                GOTO (failed, rc = -EPROTO);
        }

        *symname = lustre_msg_buf ((*request)->rq_repmsg, 1, symlen);
        if (*symname == NULL ||
            strnlen (*symname, symlen) != symlen - 1) {
                /* not full/NULL terminated */
                CERROR ("inode %lu: symlink not NULL terminated string"
                        "of length %d\n", inode->i_ino, symlen - 1);
                GOTO (failed, rc = -EPROTO);
        }

        OBD_ALLOC(lli->lli_symlink_name, symlen);
        /* do not return an error if we cannot cache the symlink locally */
        if (lli->lli_symlink_name)
                memcpy(lli->lli_symlink_name, *symname, symlen);

        RETURN(0);

 failed:
        ptlrpc_req_finished (*request);
        RETURN(rc);
}

static int ll_readlink(struct dentry *dentry, char *buffer, int buflen)
{
        struct inode *inode = dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ptlrpc_request *request;
        char *symname;
        int rc;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op\n");
        /* on symlinks lli_open_sem protects lli_symlink_name allocation/data */
        down(&lli->lli_open_sem);
        rc = ll_readlink_internal(inode, &request, &symname);
        if (rc)
                GOTO(out, rc);

        rc = vfs_readlink(dentry, buffer, buflen, symname);
        ptlrpc_req_finished(request);
 out:
        up(&lli->lli_open_sem);
        RETURN(rc);
}

static int ll_follow_link(struct dentry *dentry, struct nameidata *nd)
{
        struct inode *inode = dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lookup_intent *it = ll_nd2it(nd);
        struct ptlrpc_request *request;
        int rc;
        char *symname;
        ENTRY;

        if (it != NULL) {
                int op = it->it_op;
                int mode = it->it_create_mode;

                ll_intent_release(it);
                it->it_op = op;
                it->it_create_mode = mode;
        }

        CDEBUG(D_VFSTRACE, "VFS Op\n");
        down(&lli->lli_open_sem);
        rc = ll_readlink_internal(inode, &request, &symname);
        up(&lli->lli_open_sem);
        if (rc) {
                path_release(nd); /* Kernel assumes that ->follow_link()
                                     releases nameidata on error */
                GOTO(out, rc);
        }

        rc = vfs_follow_link(nd, symname);
        ptlrpc_req_finished(request);
 out:
        RETURN(rc);
}

struct inode_operations ll_fast_symlink_inode_operations = {
        .readlink       = ll_readlink,
        .setattr        = ll_setattr,
        .follow_link    = ll_follow_link,
        .setxattr       = ll_setxattr,
        .getxattr       = ll_getxattr,
        .listxattr      = ll_listxattr,
        .removexattr    = ll_removexattr,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        .revalidate_it  = ll_inode_revalidate_it
#else 
        .getattr        = ll_getattr
#endif
};
