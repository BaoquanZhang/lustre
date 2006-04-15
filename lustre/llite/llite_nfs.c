/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   NFS export of Lustre Light File System 
 *
 *   Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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

#define DEBUG_SUBSYSTEM S_LLITE
#include <linux/lustre_lite.h>
#include "llite_internal.h"

__u32 get_uuid2int(const char *name, int len)
{
        __u32 key0 = 0x12a3fe2d, key1 = 0x37abe8f9;
        while (len--) {
                __u32 key = key1 + (key0 ^ (*name++ * 7152373));
                if (key & 0x80000000) key -= 0x7fffffff;
                key1 = key0;
                key0 = key;
        }
        return (key0 << 1);
}

struct ll_ino {
        unsigned long ino;
        unsigned long gen;
};

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
static int ll_nfs_test_inode(struct inode *inode, unsigned long ino, void *opaque)
#else
static int ll_nfs_test_inode(struct inode *inode, void *opaque)
#endif
{
        struct lu_fid *ifid = &ll_i2info(inode)->lli_fid;
        struct lu_fid *lfid = opaque;

        if (memcmp(ifid, lfid, sizeof(struct lu_fid)) == 0)
                return 1;

        return 0;
}

static struct inode *search_inode_for_lustre(struct super_block *sb,
                                             struct lu_fid *fid,
                                             int mode)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct ptlrpc_request *req = NULL;
        struct inode *inode = NULL;
        unsigned long valid = 0;
        int eadatalen = 0, rc;

        inode = ILOOKUP(sb, ll_fid_build_ino(sbi, fid),
                        ll_nfs_test_inode, fid);
        if (inode)
                return inode;

        if (S_ISREG(mode)) {
                rc = ll_get_max_mdsize(sbi, &eadatalen);
                if (rc) 
                        return ERR_PTR(rc); 
                valid |= OBD_MD_FLEASIZE;
        }

        rc = mdc_getattr(sbi->ll_md_exp, fid, valid, eadatalen, &req);
        if (rc) {
                CERROR("can't get object attrs, fid "DFID3", rc %d\n",
                       PFID3(fid), rc);
                return ERR_PTR(rc);
        }

        rc = ll_prep_inode(sbi->ll_dt_exp, &inode, req, 0, sb);
        if (rc) {
                ptlrpc_req_finished(req);
                return ERR_PTR(rc);
        }
        ptlrpc_req_finished(req);

        return inode;
}

extern struct dentry_operations ll_d_ops;

static struct dentry *ll_iget_for_nfs(struct super_block *sb,
                                      struct lu_fid *fid, umode_t mode)
{
        struct inode *inode;
        struct dentry *result;
        struct list_head *lp;

        if (fid_num(fid) == 0)
                return ERR_PTR(-ESTALE);

        inode = search_inode_for_lustre(sb, fid, mode);
        if (IS_ERR(inode))
                return ERR_PTR(PTR_ERR(inode));

        if (is_bad_inode(inode)) {
                /* we didn't find the right inode.. */
                CERROR("can't get inode by fid "DFID3"\n",
                       PFID3(fid));
                iput(inode);
                return ERR_PTR(-ESTALE);
        }

        /* now to find a dentry.
         * If possible, get a well-connected one
         */
        spin_lock(&dcache_lock);
        for (lp = inode->i_dentry.next; lp != &inode->i_dentry ; lp=lp->next) {
                result = list_entry(lp,struct dentry, d_alias);
                lock_dentry(result);
                if (!(result->d_flags & DCACHE_DISCONNECTED)) {
                        dget_locked(result);
                        ll_set_dflags(result, DCACHE_REFERENCED);
                        unlock_dentry(result);
                        spin_unlock(&dcache_lock);
                        iput(inode);
                        return result;
                }
                unlock_dentry(result);
        }
        spin_unlock(&dcache_lock);
        result = d_alloc_root(inode);
        if (result == NULL) {
                iput(inode);
                return ERR_PTR(-ENOMEM);
        }
        result->d_flags |= DCACHE_DISCONNECTED;

        ll_set_dd(result);
        result->d_op = &ll_d_ops;
        return result;
}

static void ll_fh_to_fid(struct lu_fid *fid, __u32 *mode, __u32 *datap)
{
        /* unpacking ->f_seq */
        fid->f_seq = datap[0];
        fid->f_seq = (fid->f_seq << 32) | datap[1];

        /* unpacking ->f_num */
        fid->f_ver = datap[2];
        fid->f_oid = datap[3];

        *mode = datap[4];
}

static void ll_fid_to_fh(struct lu_fid *fid, __u32 *mode, __u32 *datap)
{
        /* packing ->f_seq */
        *datap++ = (__u32)(fid_seq(fid) >> 32);
        *datap++ = (__u32)(fid_seq(fid) & 0x00000000ffffffff);

        /* packing ->f_num */
        *datap++ = fid_ver(fid);
        *datap++ = fid_oid(fid);

        /* packing inode mode */
        *datap++ = (__u32)(S_IFMT & *mode);
}

struct dentry *ll_fh_to_dentry(struct super_block *sb, __u32 *data, int len,
                               int fhtype, int parent)
{
        struct lu_fid fid;
        __u32 mode;
        
        switch (fhtype) {
                case 2:
                        if (len < 10)
                                break;
                        if (parent) {
                                /* getting fid from parent's patr of @fh. That
                                 * is (data + 5) */
                                ll_fh_to_fid(&fid, &mode, data + 5);
                                return ll_iget_for_nfs(sb, &fid, mode);
                        }
                case 1:
                        if (len < 5)
                                break;
                        if (parent)
                                break;
                        ll_fh_to_fid(&fid, &mode, data);
                        return ll_iget_for_nfs(sb, &fid, mode);
                default: break;
        }
        return ERR_PTR(-EINVAL);
}

int ll_dentry_to_fh(struct dentry *dentry, __u32 *datap, int *lenp,
                    int need_parent)
{
        struct inode *child = dentry->d_inode;
        
        if (*lenp < 5)
                return 255;

        /* XXX: there is suspection that @datap is 5*4 bytes max long, so that
         * 10*4 bytes (two fids + two times mode) does not fit into it. Not sure
         * how to fix it though. */
        ll_fid_to_fh(&ll_i2info(child)->lli_fid, 
                     (__u32 *)&child->i_mode, datap);

        if (*lenp == 5 || S_ISDIR(child->i_mode)) {
                *lenp = 5;
                return 1;
        }
        if (dentry->d_parent) {
                struct inode *parent = dentry->d_parent->d_inode;
                
                ll_fid_to_fh(&ll_i2info(parent)->lli_fid,
                             (__u32 *)&parent->i_mode, datap);
                *lenp = 10;
                return 2;
        }
        *lenp = 5;
        return 1;
}
