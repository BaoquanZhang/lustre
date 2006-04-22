/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/quotaops.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <lustre_lite.h>
#include <lustre_dlm.h>
#include <linux/lustre_version.h>
#include "llite_internal.h"

/* methods */

/* called from iget{4,5_locked}->find_inode() under inode_lock spinlock */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
static int ll_test_inode(struct inode *inode, unsigned long ino, void *opaque)
#else
static int ll_test_inode(struct inode *inode, void *opaque)
#endif
{
        static int last_ino, last_gen, last_count;
        struct lustre_md *md = opaque;

        if (!(md->body->valid & (OBD_MD_FLGENER | OBD_MD_FLID))) {
                CERROR("MDS body missing inum or generation\n");
                return 0;
        }

        if (last_ino == md->body->ino && last_gen == md->body->generation &&
            last_count < 500) {
                last_count++;
        } else {
                if (last_count > 1)
                        CDEBUG(D_VFSTRACE, "compared %u/%u %u times\n",
                               last_ino, last_gen, last_count);
                last_count = 0;
                last_ino = md->body->ino;
                last_gen = md->body->generation;
                CDEBUG(D_VFSTRACE,
                       "comparing inode %p ino %lu/%u to body "LPU64"/%u\n",
                       inode, inode->i_ino, inode->i_generation,
                       md->body->ino, md->body->generation);
        }

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
        if (inode->i_ino != md->body->ino)
                return 0;
#endif
        if (inode->i_generation != md->body->generation) {
                struct ll_sb_info *sbi = ll_i2sbi(inode);
                struct ll_inode_info *lli = ll_i2info(inode);

                if (inode->i_state & (I_FREEING | I_CLEAR))
                        return 0;

                atomic_inc(&inode->i_count);
                inode->i_nlink = 0;
                inode->i_state |= I_FREEING;
                LASSERT(list_empty(&lli->lli_dead_list));
                /* add "duplicate" inode into deathrow for destroy */
                spin_lock(&sbi->ll_deathrow_lock);
                list_add(&lli->lli_dead_list, &sbi->ll_deathrow);
                spin_unlock(&sbi->ll_deathrow_lock);

                /* remove inode from dirty/io lists */
                list_del_init(&inode->i_list);
                
                return 0;
        }

        /* Apply the attributes in 'opaque' to this inode */
        if (!(inode->i_state & (I_FREEING | I_CLEAR)))
                ll_update_inode(inode, md);
        return 1;
}

extern struct dentry_operations ll_d_ops;

int ll_unlock(__u32 mode, struct lustre_handle *lockh)
{
        ENTRY;

        ldlm_lock_decref(lockh, mode);

        RETURN(0);
}

/* Get an inode by inode number (already instantiated by the intent lookup).
 * Returns inode or NULL
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
int ll_set_inode(struct inode *inode, void *opaque)
{
        ll_read_inode2(inode, opaque);
        return 0;
}

struct inode *ll_iget(struct super_block *sb, ino_t hash,
                      struct lustre_md *md)
{
        struct inode *inode;

        LASSERT(hash != 0);
        inode = iget5_locked(sb, hash, ll_test_inode, ll_set_inode, md);

        if (inode) {
                if (inode->i_state & I_NEW)
                        unlock_new_inode(inode);
                CDEBUG(D_VFSTRACE, "inode: %lu/%u(%p)\n", inode->i_ino,
                       inode->i_generation, inode);
        }

        return inode;
}
#else
struct inode *ll_iget(struct super_block *sb, ino_t hash,
                      struct lustre_md *md)
{
        struct inode *inode;
        LASSERT(hash != 0);
        inode = iget4(sb, hash, ll_test_inode, md);
        if (inode)
                CDEBUG(D_VFSTRACE, "inode: %lu/%u(%p)\n", inode->i_ino,
                       inode->i_generation, inode);
        return inode;
}
#endif

int ll_mdc_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                        void *data, int flag)
{
        int rc;
        struct lustre_handle lockh;
        ENTRY;

        switch (flag) {
        case LDLM_CB_BLOCKING:
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                if (rc < 0) {
                        CDEBUG(D_INODE, "ldlm_cli_cancel: %d\n", rc);
                        RETURN(rc);
                }
                break;
        case LDLM_CB_CANCELING: {
                struct inode *inode = ll_inode_from_lock(lock);
                __u64 bits = lock->l_policy_data.l_inodebits.bits;

                /* Invalidate all dentries associated with this inode */
                if (inode == NULL)
                        break;

                if (lock->l_resource->lr_name.name[0] != inode->i_ino ||
                    lock->l_resource->lr_name.name[1] != inode->i_generation) {
                        LDLM_ERROR(lock, "data mismatch with ino %lu/%u (%p)",
                                   inode->i_ino, inode->i_generation, inode);
                }

                if (bits & MDS_INODELOCK_UPDATE)
                        clear_bit(LLI_F_HAVE_MDS_SIZE_LOCK,
                                  &(ll_i2info(inode)->lli_flags));

                
                if (S_ISDIR(inode->i_mode) &&
                     (bits & MDS_INODELOCK_UPDATE))  {
                        CDEBUG(D_INODE, "invalidating inode %lu\n",
                               inode->i_ino);
                        truncate_inode_pages(inode->i_mapping, 0);
                }

                if (inode->i_sb->s_root &&
                    inode != inode->i_sb->s_root->d_inode &&
                    (bits & MDS_INODELOCK_LOOKUP))
                        ll_unhash_aliases(inode);
                iput(inode);
                break;
        }
        default:
                LBUG();
        }

        RETURN(0);
}

int ll_mdc_cancel_unused(struct lustre_handle *conn, struct inode *inode,
                         int flags, void *opaque)
{
        struct ldlm_res_id res_id =
                { .name = {inode->i_ino, inode->i_generation} };
        struct obd_device *obddev = class_conn2obd(conn);
        ENTRY;

        RETURN(ldlm_cli_cancel_unused(obddev->obd_namespace, &res_id, flags,
                                      opaque));
}

/* Pack the required supplementary groups into the supplied groups array.
 * If we don't need to use the groups from the target inode(s) then we
 * instead pack one or more groups from the user's supplementary group
 * array in case it might be useful.  Not needed if doing an MDS-side upcall. */
void ll_i2gids(__u32 *suppgids, struct inode *i1, struct inode *i2)
{
        int i;

        LASSERT(i1 != NULL);
        LASSERT(suppgids != NULL);

        if (in_group_p(i1->i_gid))
                suppgids[0] = i1->i_gid;
        else
                suppgids[0] = -1;

        if (i2) {
                if (in_group_p(i2->i_gid))
                        suppgids[1] = i2->i_gid;
                else
                        suppgids[1] = -1;
        } else {
                suppgids[1] = -1;
        }

        for (i = 0; i < current_ngroups; i++) {
                if (suppgids[0] == -1) {
                        if (current_groups[i] != suppgids[1])
                                suppgids[0] = current_groups[i];
                        continue;
                }
                if (suppgids[1] == -1) {
                        if (current_groups[i] != suppgids[0])
                                suppgids[1] = current_groups[i];
                        continue;
                }
                break;
        }
}

void ll_prepare_mdc_op_data(struct mdc_op_data *data, struct inode *i1,
                            struct inode *i2, const char *name, int namelen,
                            int mode)
{
        LASSERT(i1);

        ll_i2gids(data->suppgids, i1, i2);
        ll_inode2fid(&data->fid1, i1);

        if (i2)
                ll_inode2fid(&data->fid2, i2);
        else
                memset(&data->fid2, 0, sizeof(data->fid2));

        data->name = name;
        data->namelen = namelen;
        data->create_mode = mode;
        data->mod_time = CURRENT_SECONDS;
}

static void ll_d_add(struct dentry *de, struct inode *inode)
{
        CDEBUG(D_DENTRY, "adding inode %p to dentry %p\n", inode, de);
        /* d_instantiate */
        if (!list_empty(&de->d_alias)) {
                spin_unlock(&dcache_lock);
                CERROR("dentry %.*s %p alias next %p, prev %p\n",
                       de->d_name.len, de->d_name.name, de,
                       de->d_alias.next, de->d_alias.prev);
                LBUG();
        }
        if (inode)
                list_add(&de->d_alias, &inode->i_dentry);
        de->d_inode = inode;

        /* d_rehash */
        if (!d_unhashed(de)) {
                spin_unlock(&dcache_lock);
                CERROR("dentry %.*s %p hash next %p\n",
                       de->d_name.len, de->d_name.name, de, de->d_hash.next);
                LBUG();
        }
        __d_rehash(de, 0);
}

/* 2.6.15 and prior versions have buggy d_instantiate_unique that leaks an inode
 * if suitable alias is found. But we are not going to fix it by just freeing
 * such inode, because if some vendor's kernel contains this bugfix already,
 * we will break everything then. We will use our own reimplementation
 * instead. */
#if !defined(HAVE_D_ADD_UNIQUE) || (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16))
/* Search "inode"'s alias list for a dentry that has the same name and parent as
 * de.  If found, return it.  If not found, return de. */
struct dentry *ll_find_alias(struct inode *inode, struct dentry *de)
{
        struct list_head *tmp;

        spin_lock(&dcache_lock);
        list_for_each(tmp, &inode->i_dentry) {
                struct dentry *dentry = list_entry(tmp, struct dentry, d_alias);

                /* We are called here with 'de' already on the aliases list. */
                if (dentry == de) {
                        CERROR("whoops\n");
                        continue;
                }

                if (dentry->d_parent != de->d_parent)
                        continue;

                if (dentry->d_name.len != de->d_name.len)
                        continue;

                if (memcmp(dentry->d_name.name, de->d_name.name,
                           de->d_name.len) != 0)
                        continue;

                dget_locked(dentry);
                lock_dentry(dentry);
                __d_drop(dentry);
                dentry->d_flags &= ~DCACHE_LUSTRE_INVALID;
                unlock_dentry(dentry);
                __d_rehash(dentry, 0); /* avoid taking dcache_lock inside */
                spin_unlock(&dcache_lock);
                iput(inode);
                CDEBUG(D_DENTRY, "alias dentry %.*s (%p) parent %p inode %p "
                       "refc %d\n", de->d_name.len, de->d_name.name, de,
                       de->d_parent, de->d_inode, atomic_read(&de->d_count));
                return dentry;
        }

        ll_d_add(de, inode);

        spin_unlock(&dcache_lock);

        return de;
}
#else
struct dentry *ll_find_alias(struct inode *inode, struct dentry *de)
{
        struct dentry *dentry;

        dentry = d_add_unique(de, inode);
        if (dentry) {
                lock_dentry(dentry);
                dentry->d_flags &= ~DCACHE_LUSTRE_INVALID;
                unlock_dentry(dentry);
        }

        return dentry?dentry:de;
}
#endif

static int lookup_it_finish(struct ptlrpc_request *request, int offset,
                            struct lookup_intent *it, void *data)
{
        struct it_cb_data *icbd = data;
        struct dentry **de = icbd->icbd_childp;
        struct inode *parent = icbd->icbd_parent;
        struct ll_sb_info *sbi = ll_i2sbi(parent);
        struct inode *inode = NULL;
        int rc;

        /* NB 1 request reference will be taken away by ll_intent_lock()
         * when I return */
        if (!it_disposition(it, DISP_LOOKUP_NEG)) {
                ENTRY;

                rc = ll_prep_inode(sbi->ll_osc_exp, &inode, request, offset,
                                   (*de)->d_sb);
                if (rc)
                        RETURN(rc);

                CDEBUG(D_DLMTRACE, "setting l_data to inode %p (%lu/%u)\n",
                       inode, inode->i_ino, inode->i_generation);
                mdc_set_lock_data(&it->d.lustre.it_lock_handle, inode);

                /* We used to query real size from OSTs here, but actually
                   this is not needed. For stat() calls size would be updated
                   from subsequent do_revalidate()->ll_inode_revalidate_it() in
                   2.4 and
                   vfs_getattr_it->ll_getattr()->ll_inode_revalidate_it() in 2.6
                   Everybody else who needs correct file size would call
                   ll_glimpse_size or some equivalent themselves anyway.
                   Also see bug 7198. */

                *de = ll_find_alias(inode, *de);
        } else {
                ENTRY;
                spin_lock(&dcache_lock);
                ll_d_add(*de, inode);
                spin_unlock(&dcache_lock);
        }

        ll_set_dd(*de);
        (*de)->d_op = &ll_d_ops;

        RETURN(0);
}

static struct dentry *ll_lookup_it(struct inode *parent, struct dentry *dentry,
                                   struct lookup_intent *it, int lookup_flags)
{
        struct dentry *save = dentry, *retval;
        struct mdc_op_data op_data;
        struct it_cb_data icbd;
        struct ptlrpc_request *req = NULL;
        struct lookup_intent lookup_it = { .it_op = IT_LOOKUP };
        int rc;
        ENTRY;

        if (dentry->d_name.len > ll_i2sbi(parent)->ll_namelen)
                RETURN(ERR_PTR(-ENAMETOOLONG));

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p),intent=%s\n",
               dentry->d_name.len, dentry->d_name.name, parent->i_ino,
               parent->i_generation, parent, LL_IT2STR(it));

        if (d_mountpoint(dentry))
                CERROR("Tell Peter, lookup on mtpt, it %s\n", LL_IT2STR(it));

        ll_frob_intent(&it, &lookup_it);

        icbd.icbd_childp = &dentry;
        icbd.icbd_parent = parent;

        ll_prepare_mdc_op_data(&op_data, parent, NULL, dentry->d_name.name,
                               dentry->d_name.len, lookup_flags);

        it->it_create_mode &= ~current->fs->umask;

        rc = mdc_intent_lock(ll_i2mdcexp(parent), &op_data, NULL, 0, it,
                             lookup_flags, &req, ll_mdc_blocking_ast, 0);

        if (rc < 0)
                GOTO(out, retval = ERR_PTR(rc));

        rc = lookup_it_finish(req, 1, it, &icbd);
        if (rc != 0) {
                ll_intent_release(it);
                GOTO(out, retval = ERR_PTR(rc));
        }

        if ((it->it_op & IT_OPEN) && dentry->d_inode &&
            !S_ISREG(dentry->d_inode->i_mode) &&
            !S_ISDIR(dentry->d_inode->i_mode)) {
                ll_release_openhandle(dentry, it);
        }
        ll_lookup_finish_locks(it, dentry);

        if (dentry == save)
                GOTO(out, retval = NULL);
        else
                GOTO(out, retval = dentry);
 out:
        if (req)
                ptlrpc_req_finished(req);
        return retval;
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
static struct dentry *ll_lookup_nd(struct inode *parent, struct dentry *dentry,
                                   struct nameidata *nd)
{
        struct dentry *de;
        ENTRY;

        if (nd && nd->flags & LOOKUP_LAST && !(nd->flags & LOOKUP_LINK_NOTLAST))
                de = ll_lookup_it(parent, dentry, &nd->intent, nd->flags);
        else
                de = ll_lookup_it(parent, dentry, NULL, 0);

        RETURN(de);
}
#endif

/* We depend on "mode" being set with the proper file type/umask by now */
static struct inode *ll_create_node(struct inode *dir, const char *name,
                                    int namelen, const void *data, int datalen,
                                    int mode, __u64 extra,
                                    struct lookup_intent *it)
{
        struct inode *inode = NULL;
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        int rc;
        ENTRY;

        LASSERT(it && it->d.lustre.it_disposition);

        request = it->d.lustre.it_data;
        rc = ll_prep_inode(sbi->ll_osc_exp, &inode, request, 1, dir->i_sb);
        if (rc)
                GOTO(out, inode = ERR_PTR(rc));

        LASSERT(list_empty(&inode->i_dentry));

        /* We asked for a lock on the directory, but were granted a
         * lock on the inode.  Since we finally have an inode pointer,
         * stuff it in the lock. */
        CDEBUG(D_DLMTRACE, "setting l_ast_data to inode %p (%lu/%u)\n",
               inode, inode->i_ino, inode->i_generation);
        mdc_set_lock_data(&it->d.lustre.it_lock_handle, inode);
        EXIT;
 out:
        ptlrpc_req_finished(request);
        return inode;
}

/*
 * By the time this is called, we already have created the directory cache
 * entry for the new file, but it is so far negative - it has no inode.
 *
 * We defer creating the OBD object(s) until open, to keep the intent and
 * non-intent code paths similar, and also because we do not have the MDS
 * inode number before calling ll_create_node() (which is needed for LOV),
 * so we would need to do yet another RPC to the MDS to store the LOV EA
 * data on the MDS.  If needed, we would pass the PACKED lmm as data and
 * lmm_size in datalen (the MDS still has code which will handle that).
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int ll_create_it(struct inode *dir, struct dentry *dentry, int mode,
                        struct lookup_intent *it)
{
        struct inode *inode;
        struct ptlrpc_request *request = it->d.lustre.it_data;
        int rc = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p),intent=%s\n",
               dentry->d_name.len, dentry->d_name.name, dir->i_ino,
               dir->i_generation, dir, LL_IT2STR(it));

        rc = it_open_error(DISP_OPEN_CREATE, it);
        if (rc)
                RETURN(rc);

        mdc_store_inode_generation(request, MDS_REQ_INTENT_REC_OFF, 1);
        inode = ll_create_node(dir, dentry->d_name.name, dentry->d_name.len,
                               NULL, 0, mode, 0, it);
        if (IS_ERR(inode)) {
                RETURN(PTR_ERR(inode));
        }

        d_instantiate(dentry, inode);
        RETURN(0);
}

static void ll_update_times(struct ptlrpc_request *request, int offset,
                            struct inode *inode)
{
        struct mds_body *body = lustre_msg_buf(request->rq_repmsg, offset,
                                               sizeof(*body));
        LASSERT(body);

        if (body->valid & OBD_MD_FLMTIME &&
            body->mtime > LTIME_S(inode->i_mtime)) {
                CDEBUG(D_INODE, "setting ino %lu mtime from %lu to "LPU64"\n",
                       inode->i_ino, LTIME_S(inode->i_mtime), body->mtime);
                LTIME_S(inode->i_mtime) = body->mtime;
        }
        if (body->valid & OBD_MD_FLCTIME &&
            body->ctime > LTIME_S(inode->i_ctime))
                LTIME_S(inode->i_ctime) = body->ctime;
}

static int ll_mknod_generic(struct inode *dir, struct qstr *name, int mode,
                            unsigned rdev, struct dentry *dchild)
{
        struct ptlrpc_request *request = NULL;
        struct inode *inode = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct mdc_op_data op_data;
        int err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p) mode %o dev %x\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir,
               mode, rdev);

        mode &= ~current->fs->umask;

        switch (mode & S_IFMT) {
        case 0:
        case S_IFREG:
                mode |= S_IFREG; /* for mode = 0 case, fallthrough */
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:
                ll_prepare_mdc_op_data(&op_data, dir, NULL, name->name,
                                       name->len, 0);
                err = mdc_create(sbi->ll_mdc_exp, &op_data, NULL, 0, mode,
                                 current->fsuid, current->fsgid,
                                 current->cap_effective, rdev, &request);
                if (err)
                        break;
                ll_update_times(request, 0, dir);

                if (dchild) {
                        err = ll_prep_inode(sbi->ll_osc_exp, &inode, request, 0,
                                            dchild->d_sb);
                        if (err)
                                break;

                        d_instantiate(dchild, inode);
                }
                break;
        case S_IFDIR:
                err = -EPERM;
                break;
        default:
                err = -EINVAL;
        }
        ptlrpc_req_finished(request);
        RETURN(err);
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
static int ll_create_nd(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{

        if (!nd || !nd->intent.d.lustre.it_disposition) {
                /* No saved request? Just mknod the file */
                return ll_mknod_generic(dir, &dentry->d_name, mode, 0, dentry);
        }

        return ll_create_it(dir, dentry, mode, &nd->intent);
}
#endif

static int ll_symlink_generic(struct inode *dir, struct qstr *name,
                              const char *tgt)
{
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct mdc_op_data op_data;
        int err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p),target=%s\n",
               name->len, name->name, dir->i_ino, dir->i_generation,
               dir, tgt);

        ll_prepare_mdc_op_data(&op_data, dir, NULL, name->name,
                               name->len, 0);
        err = mdc_create(sbi->ll_mdc_exp, &op_data,
                         tgt, strlen(tgt) + 1, S_IFLNK | S_IRWXUGO,
                         current->fsuid, current->fsgid, current->cap_effective,
                         0, &request);
        if (err == 0)
                ll_update_times(request, 0, dir);

        ptlrpc_req_finished(request);
        RETURN(err);
}

static int ll_link_generic(struct inode *src,  struct inode *dir,
                           struct qstr *name)
{
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int err;
        struct ll_sb_info *sbi = ll_i2sbi(dir);

        ENTRY;
        CDEBUG(D_VFSTRACE,
               "VFS Op: inode=%lu/%u(%p), dir=%lu/%u(%p), target=%.*s\n",
               src->i_ino, src->i_generation, src, dir->i_ino,
               dir->i_generation, dir, name->len, name->name);

        ll_prepare_mdc_op_data(&op_data, src, dir, name->name,
                               name->len, 0);
        err = mdc_link(sbi->ll_mdc_exp, &op_data, &request);
        if (err == 0)
                ll_update_times(request, 0, dir);

        ptlrpc_req_finished(request);

        RETURN(err);
}

static int ll_mkdir_generic(struct inode *dir, struct qstr *name, int mode,
                            struct dentry *dchild)

{
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct mdc_op_data op_data;
        struct inode *inode = NULL;
        int err;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p)\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir);

        mode = (mode & (S_IRWXUGO|S_ISVTX) & ~current->fs->umask) | S_IFDIR;
        ll_prepare_mdc_op_data(&op_data, dir, NULL, name->name,
                               name->len, 0);
        err = mdc_create(sbi->ll_mdc_exp, &op_data, NULL, 0, mode,
                         current->fsuid, current->fsgid, current->cap_effective,
                         0, &request);
        if (err)
                GOTO(out, err);

        ll_update_times(request, 0, dir);
        if (dchild) {
                err = ll_prep_inode(sbi->ll_osc_exp, &inode, request, 0,
                                    dchild->d_sb);
                if (err)
                        GOTO(out, err);
                d_instantiate(dchild, inode);
        }
        EXIT;
out:
        ptlrpc_req_finished(request);
        return err;
}

static int ll_rmdir_generic(struct inode *dir, struct dentry *dparent,
                            struct qstr *name)
{
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        struct dentry *dentry;
        int rc;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p)\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir);

        /* Check if we have something mounted at the dir we are going to delete
         * In such a case there would always be dentry present. */
        if (dparent) {
                dentry = d_lookup(dparent, name);
                if (dentry) {
                        int mounted = d_mountpoint(dentry);
                        dput(dentry);
                        if (mounted)
                                RETURN(-EBUSY);
                }
        }
                
        ll_prepare_mdc_op_data(&op_data, dir, NULL, name->name,
                               name->len, S_IFDIR);
        rc = mdc_unlink(ll_i2sbi(dir)->ll_mdc_exp, &op_data, &request);
        if (rc == 0)
                ll_update_times(request, 0, dir);
        ptlrpc_req_finished(request);
        RETURN(rc);
}

int ll_objects_destroy(struct ptlrpc_request *request, struct inode *dir)
{
        struct mds_body *body;
        struct lov_mds_md *eadata;
        struct lov_stripe_md *lsm = NULL;
        struct obd_trans_info oti = { 0 };
        struct obdo *oa;
        int rc;
        ENTRY;

        /* req is swabbed so this is safe */
        body = lustre_msg_buf(request->rq_repmsg, 0, sizeof(*body));

        if (!(body->valid & OBD_MD_FLEASIZE))
                RETURN(0);

        if (body->eadatasize == 0) {
                CERROR("OBD_MD_FLEASIZE set but eadatasize zero\n");
                GOTO(out, rc = -EPROTO);
        }

        /* The MDS sent back the EA because we unlinked the last reference
         * to this file. Use this EA to unlink the objects on the OST.
         * It's opaque so we don't swab here; we leave it to obd_unpackmd() to
         * check it is complete and sensible. */
        eadata = lustre_swab_repbuf(request, 1, body->eadatasize, NULL);
        LASSERT(eadata != NULL);
        if (eadata == NULL) {
                CERROR("Can't unpack MDS EA data\n");
                GOTO(out, rc = -EPROTO);
        }

        rc = obd_unpackmd(ll_i2obdexp(dir), &lsm, eadata, body->eadatasize);
        if (rc < 0) {
                CERROR("obd_unpackmd: %d\n", rc);
                GOTO(out, rc);
        }
        LASSERT(rc >= sizeof(*lsm));

        rc = obd_checkmd(ll_i2obdexp(dir), ll_i2mdcexp(dir), lsm);
        if (rc)
                GOTO(out_free_memmd, rc);

        oa = obdo_alloc();
        if (oa == NULL)
                GOTO(out_free_memmd, rc = -ENOMEM);

        oa->o_id = lsm->lsm_object_id;
        oa->o_mode = body->mode & S_IFMT;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE;

        if (body->valid & OBD_MD_FLCOOKIE) {
                oa->o_valid |= OBD_MD_FLCOOKIE;
                oti.oti_logcookies =
                        lustre_msg_buf(request->rq_repmsg, 2,
                                       sizeof(struct llog_cookie) *
                                       lsm->lsm_stripe_count);
                if (oti.oti_logcookies == NULL) {
                        oa->o_valid &= ~OBD_MD_FLCOOKIE;
                        body->valid &= ~OBD_MD_FLCOOKIE;
                }
        }

        rc = obd_destroy(ll_i2obdexp(dir), oa, lsm, &oti, ll_i2mdcexp(dir));
        obdo_free(oa);
        if (rc)
                CERROR("obd destroy objid "LPX64" error %d\n",
                       lsm->lsm_object_id, rc);
 out_free_memmd:
        obd_free_memmd(ll_i2obdexp(dir), &lsm);
 out:
        return rc;
}

static int ll_unlink_generic(struct inode * dir, struct qstr *name)
{
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int rc;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p)\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir);

        ll_prepare_mdc_op_data(&op_data, dir, NULL, name->name,
                               name->len, 0);
        rc = mdc_unlink(ll_i2sbi(dir)->ll_mdc_exp, &op_data, &request);
        if (rc)
                GOTO(out, rc);

        ll_update_times(request, 0, dir);

        rc = ll_objects_destroy(request, dir);
 out:
        ptlrpc_req_finished(request);
        RETURN(rc);
}

static int ll_rename_generic(struct inode *src, struct qstr *src_name,
                             struct inode *tgt, struct qstr *tgt_name)
{
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(src);
        struct mdc_op_data op_data;
        int err;
        ENTRY;
        CDEBUG(D_VFSTRACE,"VFS Op:oldname=%.*s,src_dir=%lu/%u(%p),newname=%.*s,"
               "tgt_dir=%lu/%u(%p)\n", src_name->len, src_name->name,
               src->i_ino, src->i_generation, src, tgt_name->len,
               tgt_name->name, tgt->i_ino, tgt->i_generation, tgt);

        ll_prepare_mdc_op_data(&op_data, src, tgt, NULL, 0, 0);
        err = mdc_rename(sbi->ll_mdc_exp, &op_data,
                         src_name->name, src_name->len,
                         tgt_name->name, tgt_name->len, &request);
        if (!err) {
                ll_update_times(request, 0, src);
                ll_update_times(request, 0, tgt);
                err = ll_objects_destroy(request, src);
        }

        ptlrpc_req_finished(request);

        RETURN(err);
}

static int ll_mknod_raw(struct nameidata *nd, int mode, dev_t rdev)
{
        return ll_mknod_generic(nd->dentry->d_inode, &nd->last, mode,rdev,NULL);
}
static int ll_rename_raw(struct nameidata *srcnd, struct nameidata *tgtnd)
{
        return ll_rename_generic(srcnd->dentry->d_inode, &srcnd->last,
                                 tgtnd->dentry->d_inode, &tgtnd->last);
}
static int ll_link_raw(struct nameidata *srcnd, struct nameidata *tgtnd)
{
        return ll_link_generic(srcnd->dentry->d_inode, tgtnd->dentry->d_inode,
                               &tgtnd->last);
}
static int ll_symlink_raw(struct nameidata *nd, const char *tgt)
{
        return ll_symlink_generic(nd->dentry->d_inode, &nd->last, tgt);
}
static int ll_rmdir_raw(struct nameidata *nd)
{
        return ll_rmdir_generic(nd->dentry->d_inode, nd->dentry, &nd->last);
}
static int ll_mkdir_raw(struct nameidata *nd, int mode)
{
        return ll_mkdir_generic(nd->dentry->d_inode, &nd->last, mode, NULL);
}
static int ll_unlink_raw(struct nameidata *nd)
{
        return ll_unlink_generic(nd->dentry->d_inode, &nd->last);
}

static int ll_mknod(struct inode *dir, struct dentry *dchild, int mode,
                    ll_dev_t rdev)
{
        return ll_mknod_generic(dir, &dchild->d_name, mode,
                                old_encode_dev(rdev), dchild);
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
static int ll_unlink(struct inode * dir, struct dentry *dentry)
{
        return ll_unlink_generic(dir, &dentry->d_name);
}
static int ll_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
        return ll_mkdir_generic(dir, &dentry->d_name, mode, dentry);
}
static int ll_rmdir(struct inode *dir, struct dentry *dentry)
{
        return ll_rmdir_generic(dir, NULL, &dentry->d_name);
}
static int ll_symlink(struct inode *dir, struct dentry *dentry,
                      const char *oldname)
{
        return ll_symlink_generic(dir, &dentry->d_name, oldname);
}
static int ll_link(struct dentry *old_dentry, struct inode *dir, 
                   struct dentry *new_dentry)
{
        return ll_link_generic(old_dentry->d_inode, dir, &new_dentry->d_name);
}
static int ll_rename(struct inode *old_dir, struct dentry *old_dentry,
                     struct inode *new_dir, struct dentry *new_dentry)
{
        return ll_rename_generic(old_dir, &old_dentry->d_name, new_dir, 
                               &new_dentry->d_name);
}
#endif

struct inode_operations ll_dir_inode_operations = {
        .link_raw           = ll_link_raw,
        .unlink_raw         = ll_unlink_raw,
        .symlink_raw        = ll_symlink_raw,
        .mkdir_raw          = ll_mkdir_raw,
        .rmdir_raw          = ll_rmdir_raw,
        .mknod_raw          = ll_mknod_raw,
        .mknod              = ll_mknod,
        .rename_raw         = ll_rename_raw,
        .setattr            = ll_setattr,
        .setattr_raw        = ll_setattr_raw,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        .create_it          = ll_create_it,
        .lookup_it          = ll_lookup_it,
        .revalidate_it      = ll_inode_revalidate_it,
#else
        .lookup             = ll_lookup_nd,
        .create             = ll_create_nd,
        .getattr_it         = ll_getattr_it,
        /* We need all these non-raw things for NFSD, to not patch it. */
        .unlink             = ll_unlink,
        .mkdir              = ll_mkdir,
        .rmdir              = ll_rmdir,
        .symlink            = ll_symlink,
        .link               = ll_link,
        .rename             = ll_rename,
        .setattr            = ll_setattr,
        .getattr            = ll_getattr,
#endif
        .permission         = ll_inode_permission,
        .setxattr           = ll_setxattr,
        .getxattr           = ll_getxattr,
        .listxattr          = ll_listxattr,
        .removexattr        = ll_removexattr,
};

struct inode_operations ll_special_inode_operations = {
        .setattr_raw    = ll_setattr_raw,
        .setattr        = ll_setattr,
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
        .getattr_it     = ll_getattr_it,
#else   
        .revalidate_it  = ll_inode_revalidate_it,
#endif
        .permission     = ll_inode_permission,
        .setxattr       = ll_setxattr,
        .getxattr       = ll_getxattr,
        .listxattr      = ll_listxattr,
        .removexattr    = ll_removexattr,
};
