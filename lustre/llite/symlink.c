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

#include <lustre_lite.h>
#include "llite_internal.h"

static int ll_readlink_internal(struct inode *inode,
                                struct ptlrpc_request **request, char **symname)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        int rc, symlen = i_size_read(inode) + 1;
        struct mdt_body *body;
        struct obd_capa *oc;
        ENTRY;

        *request = NULL;

        if (lli->lli_symlink_name) {
                *symname = lli->lli_symlink_name;
                CDEBUG(D_INODE, "using cached symlink %s\n", *symname);
                RETURN(0);
        }

        oc = ll_mdscapa_get(inode);
        rc = md_getattr(sbi->ll_md_exp, ll_inode2fid(inode), oc,
                        OBD_MD_LINKNAME, symlen, request);
        capa_put(oc);
        if (rc) {
                if (rc != -ENOENT)
                        CERROR("inode %lu: rc = %d\n", inode->i_ino, rc);
                GOTO (failed, rc);
        }

        body = lustre_msg_buf((*request)->rq_repmsg, REPLY_REC_OFF,
                              sizeof(*body));
        LASSERT(body != NULL);
        LASSERT_REPSWABBED(*request, REPLY_REC_OFF);

        if ((body->valid & OBD_MD_LINKNAME) == 0) {
                CERROR("OBD_MD_LINKNAME not set on reply\n");
                GOTO(failed, rc = -EPROTO);
        }
        
        LASSERT(symlen != 0);
        if (body->eadatasize != symlen) {
                CERROR("inode %lu: symlink length %d not expected %d\n",
                        inode->i_ino, body->eadatasize - 1, symlen - 1);
                GOTO(failed, rc = -EPROTO);
        }

        *symname = lustre_msg_buf((*request)->rq_repmsg, REPLY_REC_OFF + 1,
                                  symlen);
        if (*symname == NULL ||
            strnlen (*symname, symlen) != symlen - 1) {
                /* not full/NULL terminated */
                CERROR("inode %lu: symlink not NULL terminated string"
                        "of length %d\n", inode->i_ino, symlen - 1);
                GOTO(failed, rc = -EPROTO);
        }

        OBD_ALLOC(lli->lli_symlink_name, symlen);
        /* do not return an error if we cannot cache the symlink locally */
        if (lli->lli_symlink_name) {
                memcpy(lli->lli_symlink_name, *symname, symlen);
                ptlrpc_req_finished (*request);
                *request = NULL;
                *symname = lli->lli_symlink_name;
        }

        RETURN(0);

 failed:
        ptlrpc_req_finished (*request);
        RETURN (rc);
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
        down(&lli->lli_size_sem);
        rc = ll_readlink_internal(inode, &request, &symname);
        if (rc)
                GOTO(out, rc);

        rc = vfs_readlink(dentry, buffer, buflen, symname);
        ptlrpc_req_finished(request);
 out:
        up(&lli->lli_size_sem);
        RETURN(rc);
}

#ifdef HAVE_COOKIE_FOLLOW_LINK
# define LL_FOLLOW_LINK_RETURN_TYPE void *
#else
# define LL_FOLLOW_LINK_RETURN_TYPE int
#endif

static LL_FOLLOW_LINK_RETURN_TYPE ll_follow_link(struct dentry *dentry,
                                                 struct nameidata *nd)
{
        struct inode *inode = dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
#ifdef HAVE_VFS_INTENT_PATCHES
        struct lookup_intent *it = ll_nd2it(nd);
#endif
        struct ptlrpc_request *request;
        int rc;
        char *symname;
        ENTRY;

#ifdef HAVE_VFS_INTENT_PATCHES
        if (it != NULL) {
                int op = it->it_op;
                int mode = it->it_create_mode;

                ll_intent_release(it);
                it->it_op = op;
                it->it_create_mode = mode;
        }
#endif

        CDEBUG(D_VFSTRACE, "VFS Op\n");
        down(&lli->lli_size_sem);
        rc = ll_readlink_internal(inode, &request, &symname);
        up(&lli->lli_size_sem);
        if (rc) {
                path_release(nd); /* Kernel assumes that ->follow_link()
                                     releases nameidata on error */
                GOTO(out, rc);
        }

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,8))
        rc = vfs_follow_link(nd, symname);
#else
#ifdef HAVE_COOKIE_FOLLOW_LINK
        nd_set_link(nd, symname);
        /* @symname may contain a pointer to the request message buffer,
           we delay request releasing until ll_put_link then. */
        RETURN(request);
#else
        if (request != NULL) {
                /* falling back to recursive follow link if the request
                 * needs to be cleaned up still. */
        rc = vfs_follow_link(nd, symname);
                GOTO(out, rc);
        }
        nd_set_link(nd, symname);
        RETURN(0);
#endif
#endif
out:
        ptlrpc_req_finished(request);
#ifdef HAVE_COOKIE_FOLLOW_LINK
        RETURN(ERR_PTR(rc));
#else
        RETURN(rc);
#endif
}

#ifdef HAVE_COOKIE_FOLLOW_LINK
static void ll_put_link(struct dentry *dentry, struct nameidata *nd, void *cookie)
{
        ptlrpc_req_finished(cookie);
}
#endif

struct inode_operations ll_fast_symlink_inode_operations = {
        .readlink       = ll_readlink,
        .setattr        = ll_setattr,
#ifdef HAVE_VFS_INTENT_PATCHES
        .setattr_raw    = ll_setattr_raw,
#endif
        .follow_link    = ll_follow_link,
#ifdef HAVE_COOKIE_FOLLOW_LINK
        .put_link       = ll_put_link,
#endif
        .getattr        = ll_getattr,
        .permission     = ll_inode_permission,
        .setxattr       = ll_setxattr,
        .getxattr       = ll_getxattr,
        .listxattr      = ll_listxattr,
        .removexattr    = ll_removexattr,
};
