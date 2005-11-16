/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/dir.c
 *  linux/fs/ext2/dir.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 directory handling functions
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 *  All code that works with directory layout had been switched to pagecache
 *  and moved here. AV
 *
 *  Adapted for Lustre Light
 *  Copyright (C) 2002-2003, Cluster File Systems, Inc.
 *
 */

#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
# include <linux/locks.h>   // for wait_on_buffer
#else
# include <linux/buffer_head.h>   // for wait_on_buffer
#endif

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_lite.h>
#include <linux/lustre_dlm.h>
#include "llite_internal.h"

typedef struct ext2_dir_entry_2 ext2_dirent;

#define PageChecked(page)        test_bit(PG_checked, &(page)->flags)
#define SetPageChecked(page)     set_bit(PG_checked, &(page)->flags)

/* returns the page unlocked, but with a reference */
static int ll_dir_readpage(struct file *file, struct page *page)
{
        struct inode *inode = page->mapping->host;
        struct ll_fid mdc_fid;
        __u64 offset;
        struct ptlrpc_request *request;
        struct mds_body *body;
        int rc = 0;
        ENTRY;

        offset = (__u64)page->index << PAGE_SHIFT;
        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p) off "LPU64"\n",
               inode->i_ino, inode->i_generation, inode, offset);

        mdc_pack_fid(&mdc_fid, inode->i_ino, inode->i_generation, S_IFDIR);

        rc = mdc_readpage(ll_i2sbi(inode)->ll_mdc_exp, &mdc_fid,
                          offset, page, &request);
        if (!rc) {
                body = lustre_msg_buf(request->rq_repmsg, 0, sizeof (*body));
                LASSERT (body != NULL);         /* checked by mdc_readpage() */
                LASSERT_REPSWABBED (request, 0); /* swabbed by mdc_readpage() */

                inode->i_size = body->size;
                SetPageUptodate(page);
        }
        ptlrpc_req_finished(request);

        unlock_page(page);
        EXIT;
        return rc;
}

struct address_space_operations ll_dir_aops = {
        .readpage  = ll_dir_readpage,
};

/*
 * ext2 uses block-sized chunks. Arguably, sector-sized ones would be
 * more robust, but we have what we have
 */
static inline unsigned ext2_chunk_size(struct inode *inode)
{
        return inode->i_sb->s_blocksize;
}

static inline void ext2_put_page(struct page *page)
{
        kunmap(page);
        page_cache_release(page);
}

static inline unsigned long dir_pages(struct inode *inode)
{
        return (inode->i_size+PAGE_CACHE_SIZE-1)>>PAGE_CACHE_SHIFT;
}


static void ext2_check_page(struct page *page)
{
        struct inode *dir = page->mapping->host;
        unsigned chunk_size = ext2_chunk_size(dir);
        char *kaddr = page_address(page);
        //      u32 max_inumber = le32_to_cpu(sb->u.ext2_sb.s_es->s_inodes_count);
        unsigned offs, rec_len;
        unsigned limit = PAGE_CACHE_SIZE;
        ext2_dirent *p;
        char *error;

        if ((dir->i_size >> PAGE_CACHE_SHIFT) == page->index) {
                limit = dir->i_size & ~PAGE_CACHE_MASK;
                if (limit & (chunk_size - 1)) {
                        CERROR("limit %d dir size %lld index %ld\n",
                               limit, dir->i_size, page->index);
                        goto Ebadsize;
                }
                for (offs = limit; offs<PAGE_CACHE_SIZE; offs += chunk_size) {
                        ext2_dirent *p = (ext2_dirent*)(kaddr + offs);
                        p->rec_len = cpu_to_le16(chunk_size);
                        p->name_len = 0;
                        p->inode = 0;
                }
                if (!limit)
                        goto out;
        }
        for (offs = 0; offs <= limit - EXT2_DIR_REC_LEN(1); offs += rec_len) {
                p = (ext2_dirent *)(kaddr + offs);
                rec_len = le16_to_cpu(p->rec_len);

                if (rec_len < EXT2_DIR_REC_LEN(1))
                        goto Eshort;
                if (rec_len & 3)
                        goto Ealign;
                if (rec_len < EXT2_DIR_REC_LEN(p->name_len))
                        goto Enamelen;
                if (((offs + rec_len - 1) ^ offs) & ~(chunk_size-1))
                        goto Espan;
                //              if (le32_to_cpu(p->inode) > max_inumber)
                //goto Einumber;
        }
        if (offs != limit)
                goto Eend;
out:
        SetPageChecked(page);
        return;

        /* Too bad, we had an error */

Ebadsize:
        CERROR("ext2_check_page"
                "size of directory #%lu is not a multiple of chunk size\n",
                dir->i_ino
        );
        goto fail;
Eshort:
        error = "rec_len is smaller than minimal";
        goto bad_entry;
Ealign:
        error = "unaligned directory entry";
        goto bad_entry;
Enamelen:
        error = "rec_len is too small for name_len";
        goto bad_entry;
Espan:
        error = "directory entry across blocks";
        goto bad_entry;
        //Einumber:
        // error = "inode out of bounds";
bad_entry:
        CERROR("ext2_check_page: bad entry in directory #%lu: %s - "
                "offset=%lu+%u, inode=%lu, rec_len=%d, name_len=%d",
                dir->i_ino, error, (page->index<<PAGE_CACHE_SHIFT), offs,
                (unsigned long) le32_to_cpu(p->inode),
                rec_len, p->name_len);
        goto fail;
Eend:
        p = (ext2_dirent *)(kaddr + offs);
        CERROR("ext2_check_page"
                "entry in directory #%lu spans the page boundary"
                "offset=%lu, inode=%lu",
                dir->i_ino, (page->index<<PAGE_CACHE_SHIFT)+offs,
                (unsigned long) le32_to_cpu(p->inode));
fail:
        SetPageChecked(page);
        SetPageError(page);
}

static struct page *ll_get_dir_page(struct inode *dir, unsigned long n)
{
        struct ldlm_res_id res_id =
                { .name = { dir->i_ino, (__u64)dir->i_generation} };
        struct lustre_handle lockh;
        struct obd_device *obddev = class_exp2obd(ll_i2sbi(dir)->ll_mdc_exp);
        struct address_space *mapping = dir->i_mapping;
        struct page *page;
        ldlm_policy_data_t policy = {.l_inodebits = {MDS_INODELOCK_UPDATE} };
        int rc;

        rc = ldlm_lock_match(obddev->obd_namespace, LDLM_FL_BLOCK_GRANTED,
                             &res_id, LDLM_IBITS, &policy, LCK_CR, &lockh);
        if (!rc) {
                struct lookup_intent it = { .it_op = IT_READDIR };
                struct ptlrpc_request *request;
                struct mdc_op_data data;

                ll_prepare_mdc_op_data(&data, dir, NULL, NULL, 0, 0);

                rc = mdc_enqueue(ll_i2sbi(dir)->ll_mdc_exp, LDLM_IBITS, &it,
                                 LCK_CR, &data, &lockh, NULL, 0,
                                 ldlm_completion_ast, ll_mdc_blocking_ast, dir,
                                 0);

                request = (struct ptlrpc_request *)it.d.lustre.it_data;
                if (request)
                        ptlrpc_req_finished(request);
                if (rc < 0) {
                        CERROR("lock enqueue: rc: %d\n", rc);
                        return ERR_PTR(rc);
                }
        }
        ldlm_lock_dump_handle(D_OTHER, &lockh);

        page = read_cache_page(mapping, n,
                               (filler_t*)mapping->a_ops->readpage, NULL);
        if (!IS_ERR(page)) {
                wait_on_page(page);
                (void)kmap(page);
                if (!PageUptodate(page))
                        goto fail;
                if (!PageChecked(page))
                        ext2_check_page(page);
                if (PageError(page))
                        goto fail;
        }

out_unlock:
        ldlm_lock_decref(&lockh, LCK_CR);
        return page;

fail:
        ext2_put_page(page);
        page = ERR_PTR(-EIO);
        goto out_unlock;
}

/*
 * p is at least 6 bytes before the end of page
 */
static inline ext2_dirent *ext2_next_entry(ext2_dirent *p)
{
        return (ext2_dirent *)((char*)p + le16_to_cpu(p->rec_len));
}

static inline unsigned
ext2_validate_entry(char *base, unsigned offset, unsigned mask)
{
        ext2_dirent *de = (ext2_dirent*)(base + offset);
        ext2_dirent *p = (ext2_dirent*)(base + (offset&mask));
        while ((char*)p < (char*)de)
                p = ext2_next_entry(p);
        return (char *)p - base;
}

static unsigned char ext2_filetype_table[EXT2_FT_MAX] = {
        [EXT2_FT_UNKNOWN]       DT_UNKNOWN,
        [EXT2_FT_REG_FILE]      DT_REG,
        [EXT2_FT_DIR]           DT_DIR,
        [EXT2_FT_CHRDEV]        DT_CHR,
        [EXT2_FT_BLKDEV]        DT_BLK,
        [EXT2_FT_FIFO]          DT_FIFO,
        [EXT2_FT_SOCK]          DT_SOCK,
        [EXT2_FT_SYMLINK]       DT_LNK,
};


int ll_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
        struct inode *inode = filp->f_dentry->d_inode;
        loff_t pos = filp->f_pos;
        // XXX struct super_block *sb = inode->i_sb;
        unsigned offset = pos & ~PAGE_CACHE_MASK;
        unsigned long n = pos >> PAGE_CACHE_SHIFT;
        unsigned long npages = dir_pages(inode);
        unsigned chunk_mask = ~(ext2_chunk_size(inode)-1);
        unsigned char *types = ext2_filetype_table;
        int need_revalidate = (filp->f_version != inode->i_version);
        int rc = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p) pos %llu/%llu\n",
               inode->i_ino, inode->i_generation, inode, pos, inode->i_size);

        if (pos > inode->i_size - EXT2_DIR_REC_LEN(1))
                RETURN(0);

        for ( ; n < npages; n++, offset = 0) {
                char *kaddr, *limit;
                ext2_dirent *de;
                struct page *page;

                CDEBUG(D_EXT2,"read %lu of dir %lu/%u page %lu/%lu size %llu\n",
                       PAGE_CACHE_SIZE, inode->i_ino, inode->i_generation,
                       n, npages, inode->i_size);
                page = ll_get_dir_page(inode, n);

                /* size might have been updated by mdc_readpage */
                npages = dir_pages(inode);

                if (IS_ERR(page)) {
                        rc = PTR_ERR(page);
                        CERROR("error reading dir %lu/%u page %lu: rc %d\n",
                               inode->i_ino, inode->i_generation, n, rc);
                        continue;
                }

                kaddr = page_address(page);
                if (need_revalidate) {
                        offset = ext2_validate_entry(kaddr, offset, chunk_mask);
                        need_revalidate = 0;
                }
                de = (ext2_dirent *)(kaddr+offset);
                limit = kaddr + PAGE_CACHE_SIZE - EXT2_DIR_REC_LEN(1);
                for ( ;(char*)de <= limit; de = ext2_next_entry(de)) {
                        if (de->inode) {
                                int over;

                                rc = 0; /* no error if we return something */

                                offset = (char *)de - kaddr;
                                over = filldir(dirent, de->name, de->name_len,
                                               (n<<PAGE_CACHE_SHIFT) | offset,
                                               le32_to_cpu(de->inode),
                                               types[de->file_type &
                                                     (EXT2_FT_MAX - 1)]);
                                if (over) {
                                        ext2_put_page(page);
                                        GOTO(done, rc);
                                }
                        }
                }
                ext2_put_page(page);
        }

done:
        filp->f_pos = (n << PAGE_CACHE_SHIFT) | offset;
        filp->f_version = inode->i_version;
        update_atime(inode);
        RETURN(rc);
}

#define QCTL_COPY(out, in)              \
do {                                    \
        Q_COPY(out, in, qc_cmd);        \
        Q_COPY(out, in, qc_type);       \
        Q_COPY(out, in, qc_id);         \
        Q_COPY(out, in, qc_stat);       \
        Q_COPY(out, in, qc_dqinfo);     \
        Q_COPY(out, in, qc_dqblk);      \
} while (0)

static int ll_dir_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct obd_ioctl_data *data;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), cmd=%#x\n",
               inode->i_ino, inode->i_generation, inode, cmd);

        /* asm-ppc{,64} declares TCGETS, et. al. as type 't' not 'T' */
        if (_IOC_TYPE(cmd) == 'T' || _IOC_TYPE(cmd) == 't') /* tty ioctls */
                return -ENOTTY;

        lprocfs_counter_incr(ll_i2sbi(inode)->ll_stats, LPROC_LL_IOCTL);
        switch(cmd) {
        case EXT3_IOC_GETFLAGS:
        case EXT3_IOC_SETFLAGS:
                RETURN(ll_iocontrol(inode, file, cmd, arg));
        case EXT3_IOC_GETVERSION_OLD:
        case EXT3_IOC_GETVERSION:
                RETURN(put_user(inode->i_generation, (int *)arg));
        /* We need to special case any other ioctls we want to handle,
         * to send them to the MDS/OST as appropriate and to properly
         * network encode the arg field.
        case EXT3_IOC_SETVERSION_OLD:
        case EXT3_IOC_SETVERSION:
        */
        case IOC_MDC_LOOKUP: {
                struct ptlrpc_request *request = NULL;
                struct ll_fid fid;
                char *buf = NULL;
                char *filename;
                int namelen, rc, len = 0;

                rc = obd_ioctl_getdata(&buf, &len, (void *)arg);
                if (rc)
                        RETURN(rc);
                data = (void *)buf;

                filename = data->ioc_inlbuf1;
                namelen = data->ioc_inllen1;

                if (namelen < 1) {
                        CDEBUG(D_INFO, "IOC_MDC_LOOKUP missing filename\n");
                        GOTO(out, rc = -EINVAL);
                }

                ll_inode2fid(&fid, inode);
                rc = mdc_getattr_name(sbi->ll_mdc_exp, &fid, filename, namelen,
                                      OBD_MD_FLID, 0, &request);
                if (rc < 0) {
                        CDEBUG(D_INFO, "mdc_getattr_name: %d\n", rc);
                        GOTO(out, rc);
                }

                ptlrpc_req_finished(request);

                EXIT;
        out:
                obd_ioctl_freedata(buf, len);
                return rc;
        }
        case LL_IOC_LOV_SETSTRIPE: {
                struct ptlrpc_request *request = NULL;
                struct mdc_op_data op_data;
                struct iattr attr = { 0 };
                struct lov_user_md lum, *lump = (struct lov_user_md *)arg;
                int rc = 0;

                ll_prepare_mdc_op_data(&op_data, inode, NULL, NULL, 0, 0);

                LASSERT(sizeof(lum) == sizeof(*lump));
                LASSERT(sizeof(lum.lmm_objects[0]) ==
                        sizeof(lump->lmm_objects[0]));
                rc = copy_from_user(&lum, lump, sizeof(lum));
                if (rc)
                        return(-EFAULT);

                /*
                 * This is coming from userspace, so should be in
                 * local endian.  But the MDS would like it in little
                 * endian, so we swab it before we send it.
                 */
                if (lum.lmm_magic != LOV_USER_MAGIC)
                        RETURN(-EINVAL);

                if (lum.lmm_magic != cpu_to_le32(LOV_USER_MAGIC))
                        lustre_swab_lov_user_md(&lum);

                /* swabbing is done in lov_setstripe() on server side */
                rc = mdc_setattr(sbi->ll_mdc_exp, &op_data,
                                 &attr, &lum, sizeof(lum), NULL, 0, &request);
                if (rc) {
                        ptlrpc_req_finished(request);
                        if (rc != -EPERM && rc != -EACCES)
                                CERROR("mdc_setattr fails: rc = %d\n", rc);
                        return rc;
                }
                ptlrpc_req_finished(request);

                return rc;
        }
        case LL_IOC_LOV_GETSTRIPE: {
                struct ptlrpc_request *request = NULL;
                struct lov_user_md *lump = (struct lov_user_md *)arg;
                struct lov_mds_md *lmm;
                struct ll_fid fid;
                struct mds_body *body;
                int rc, lmmsize;

                ll_inode2fid(&fid, inode);
                rc = mdc_getattr(sbi->ll_mdc_exp, &fid, OBD_MD_FLDIREA,
                                 obd_size_diskmd(sbi->ll_osc_exp, NULL),
                                 &request);
                if (rc < 0) {
                        CDEBUG(D_INFO, "mdc_getattr failed: rc = %d\n", rc);
                        RETURN(rc);
                }

                body = lustre_msg_buf(request->rq_repmsg, 0, sizeof(*body));
                LASSERT(body != NULL);         /* checked by mdc_getattr_name */
                LASSERT_REPSWABBED(request, 0);/* swabbed by mdc_getattr_name */

                lmmsize = body->eadatasize;
                if (lmmsize == 0)
                        GOTO(out_get, rc = -ENODATA);

                lmm = lustre_msg_buf(request->rq_repmsg, 1, lmmsize);
                LASSERT(lmm != NULL);
                LASSERT_REPSWABBED(request, 1);

                /*
                 * This is coming from the MDS, so is probably in
                 * little endian.  We convert it to host endian before
                 * passing it to userspace.
                 */
                if (lmm->lmm_magic == __swab32(LOV_MAGIC)) {
                        lustre_swab_lov_user_md((struct lov_user_md *)lmm);
                        lustre_swab_lov_user_md_objects((struct lov_user_md *)lmm);
                }

                rc = copy_to_user(lump, lmm, lmmsize);
                if (rc)
                        GOTO(out_get, rc = -EFAULT);

                EXIT;
        out_get:
                ptlrpc_req_finished(request);
                return rc;
        }
        case IOC_MDC_GETFILEINFO:
        case IOC_MDC_GETSTRIPE: {
                struct ptlrpc_request *request = NULL;
                struct ll_fid fid;
                struct mds_body *body;
                struct lov_user_md *lump;
                struct lov_mds_md *lmm;
                char *filename;
                int rc, lmmsize;

                filename = getname((const char *)arg);
                if (IS_ERR(filename))
                        RETURN(PTR_ERR(filename));

                ll_inode2fid(&fid, inode);
                rc = mdc_getattr_name(sbi->ll_mdc_exp, &fid, filename,
                                      strlen(filename) + 1, OBD_MD_FLEASIZE,
                                      obd_size_diskmd(sbi->ll_osc_exp, NULL),
                                      &request);
                if (rc < 0) {
                        CDEBUG(D_INFO, "mdc_getattr_name failed on %s: rc %d\n",
                               filename, rc);
                        GOTO(out_name, rc);
                }

                body = lustre_msg_buf(request->rq_repmsg, 0, sizeof (*body));
                LASSERT(body != NULL);         /* checked by mdc_getattr_name */
                LASSERT_REPSWABBED(request, 0);/* swabbed by mdc_getattr_name */

                lmmsize = body->eadatasize;

                if (!(body->valid & OBD_MD_FLEASIZE) || lmmsize == 0)
                        GOTO(out_req, rc = -ENODATA);

                if (lmmsize > 4096)
                        GOTO(out_req, rc = -EFBIG);

                lmm = lustre_msg_buf(request->rq_repmsg, 1, lmmsize);
                LASSERT(lmm != NULL);
                LASSERT_REPSWABBED(request, 1);

                /*
                 * This is coming from the MDS, so is probably in
                 * little endian.  We convert it to host endian before
                 * passing it to userspace.
                 */
                if (lmm->lmm_magic == __swab32(LOV_MAGIC)) {
                        lustre_swab_lov_user_md((struct lov_user_md *)lmm);
                        lustre_swab_lov_user_md_objects((struct lov_user_md *)lmm);
                }

                if (cmd == IOC_MDC_GETFILEINFO) {
                        struct lov_user_mds_data *lmdp;
                        lstat_t st = { 0 };

                        st.st_dev     = 0;
                        st.st_mode    = body->mode;
                        st.st_nlink   = body->nlink;
                        st.st_uid     = body->uid;
                        st.st_gid     = body->gid;
                        st.st_rdev    = body->rdev;
                        st.st_size    = body->size;
                        st.st_blksize = PAGE_SIZE;
                        st.st_blocks  = body->blocks;
                        st.st_atime   = body->atime;
                        st.st_mtime   = body->mtime;
                        st.st_ctime   = body->ctime;
                        st.st_ino     = body->ino;

                        lmdp = (struct lov_user_mds_data *)arg;
                        rc = copy_to_user(&lmdp->lmd_st, &st, sizeof(st));
                        if (rc)
                                GOTO(out_req, rc = -EFAULT);
                        lump = &lmdp->lmd_lmm;
                } else {
                        lump = (struct lov_user_md *)arg;
                }

                rc = copy_to_user(lump, lmm, lmmsize);
                if (rc)
                        GOTO(out_req, rc = -EFAULT);

                EXIT;
        out_req:
                ptlrpc_req_finished(request);
        out_name:
                putname(filename);
                return rc;
        }
        case OBD_IOC_LLOG_CATINFO: {
                struct ptlrpc_request *req = NULL;
                char *buf = NULL;
                int rc, len = 0;
                char *bufs[2], *str;
                int lens[2], size;

                rc = obd_ioctl_getdata(&buf, &len, (void *)arg);
                if (rc)
                        RETURN(rc);
                data = (void *)buf;

                if (!data->ioc_inlbuf1) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                lens[0] = data->ioc_inllen1;
                bufs[0] = data->ioc_inlbuf1;
                if (data->ioc_inllen2) {
                        lens[1] = data->ioc_inllen2;
                        bufs[1] = data->ioc_inlbuf2;
                } else {
                        lens[1] = 0;
                        bufs[1] = NULL;
                }
                size = data->ioc_plen1;
                req = ptlrpc_prep_req(sbi2mdc(sbi)->cl_import, LLOG_CATINFO,
                                      2, lens, bufs);
                if (!req)
                        GOTO(out_catinfo, rc = -ENOMEM);
                req->rq_replen = lustre_msg_size(1, &size);

                rc = ptlrpc_queue_wait(req);
                str = lustre_msg_string(req->rq_repmsg, 0, data->ioc_plen1);
                if (!rc)
                        rc = copy_to_user(data->ioc_pbuf1, str,
                                          data->ioc_plen1);
                ptlrpc_req_finished(req);
        out_catinfo:
                obd_ioctl_freedata(buf, len);
                RETURN(rc);
        }
        case OBD_IOC_QUOTACHECK: {
                struct obd_quotactl *oqctl;
                int rc, error = 0;

                if (!capable(CAP_SYS_ADMIN))
                        RETURN(-EPERM);

                OBD_ALLOC_PTR(oqctl);
                if (!oqctl)
                        RETURN(-ENOMEM);
                oqctl->qc_type = arg;
                rc = obd_quotacheck(sbi->ll_mdc_exp, oqctl);
                if (rc < 0) {
                        CDEBUG(D_INFO, "mdc_quotacheck failed: rc %d\n", rc);
                        error = rc;
                }

                rc = obd_quotacheck(sbi->ll_osc_exp, oqctl);
                if (rc < 0)
                        CDEBUG(D_INFO, "osc_quotacheck failed: rc %d\n", rc);

                OBD_FREE_PTR(oqctl);
                return error ?: rc;
        }
        case OBD_IOC_POLL_QUOTACHECK: {
                struct if_quotacheck *check;
                int rc;

                if (!capable(CAP_SYS_ADMIN))
                        RETURN(-EPERM);

                OBD_ALLOC_PTR(check);
                if (!check)
                        RETURN(-ENOMEM);

                rc = obd_iocontrol(cmd, sbi->ll_mdc_exp, 0, (void *)check,
                                   NULL);
                if (rc) {
                        CDEBUG(D_QUOTA, "mdc ioctl %d failed: %d\n", cmd, rc);
                        if (copy_to_user((void *)arg, check, sizeof(*check)))
                                rc = -EFAULT;
                        GOTO(out_poll, rc);
                }

                rc = obd_iocontrol(cmd, sbi->ll_osc_exp, 0, (void *)check,
                                   NULL);
                if (rc) {
                        CDEBUG(D_QUOTA, "osc ioctl %d failed: %d\n", cmd, rc);
                        if (copy_to_user((void *)arg, check, sizeof(*check)))
                                rc = -EFAULT;
                        GOTO(out_poll, rc);
                }
        out_poll:                 
                OBD_FREE_PTR(check);
                RETURN(rc);
        }
#if HAVE_QUOTA_SUPPORT
        case OBD_IOC_QUOTACTL: {
                struct if_quotactl *qctl;
                struct obd_quotactl *oqctl;
                
                int cmd, type, id, rc = 0;

                OBD_ALLOC_PTR(qctl);
                if (!qctl)
                        RETURN(-ENOMEM);

                OBD_ALLOC_PTR(oqctl);
                if (!oqctl) {
                        OBD_FREE_PTR(qctl);
                        RETURN(-ENOMEM);
                }
                if (copy_from_user(qctl, (void *)arg, sizeof(*qctl)))
                        GOTO(out_quotactl, rc = -EFAULT);

                cmd = qctl->qc_cmd;
                type = qctl->qc_type;
                id = qctl->qc_id;
                switch (cmd) {
                case Q_QUOTAON:
                case Q_QUOTAOFF:
                case Q_SETQUOTA:
                case Q_SETINFO:
                        if (!capable(CAP_SYS_ADMIN))
                                GOTO(out_quotactl, rc = -EPERM);
                        break;
                case Q_GETQUOTA:
                        if (((type == USRQUOTA && current->euid != id) ||
                             (type == GRPQUOTA && !in_egroup_p(id))) &&
                            !capable(CAP_SYS_ADMIN))
                                GOTO(out_quotactl, rc = -EPERM);

                        /* XXX: dqb_valid is borrowed as a flag to mark that
                         *      only mds quota is wanted */
                        if (qctl->qc_dqblk.dqb_valid)
                                qctl->obd_uuid = 
                                       sbi->ll_mdc_exp->exp_obd->u.cli.
                                       cl_import->imp_target_uuid;
                        break;
                case Q_GETINFO:
                        break;
                default:
                        CERROR("unsupported quotactl op: %#x\n", cmd);
                        GOTO(out_quotactl, -ENOTTY);
                }

                QCTL_COPY(oqctl, qctl);

                if (qctl->obd_uuid.uuid[0]) {
                        struct obd_device *obd;
                        struct obd_uuid *uuid = &qctl->obd_uuid;

                        obd = class_find_client_notype(uuid,
                                         &sbi->ll_osc_exp->exp_obd->obd_uuid);
                        if (!obd)
                                GOTO(out_quotactl, rc = -ENOENT);

                        if (cmd == Q_GETINFO)
                                oqctl->qc_cmd = Q_GETOINFO;
                        else if (cmd == Q_GETQUOTA)
                                oqctl->qc_cmd = Q_GETOQUOTA;
                        else
                                GOTO(out_quotactl, rc = -EINVAL);

                        if (sbi->ll_mdc_exp->exp_obd == obd) {
                                rc = obd_quotactl(sbi->ll_mdc_exp, oqctl);
                        } else {
                                int i;
                                struct obd_export *exp;
                                struct lov_obd *lov = &sbi->ll_osc_exp->
                                                            exp_obd->u.lov;

                                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                                        exp = lov->tgts[i].ltd_exp;

                                        if (!lov->tgts[i].active)
                                                continue;

                                        if (exp->exp_obd == obd) {
                                                rc = obd_quotactl(exp, oqctl);
                                                break;
                                        }
                                }
                        }

                        oqctl->qc_cmd = cmd;
                        QCTL_COPY(qctl, oqctl);

                        if (copy_to_user((void *)arg, qctl, sizeof(*qctl)))
                                rc = -EFAULT;

                        GOTO(out_quotactl, rc);
                }

                rc = obd_quotactl(sbi->ll_mdc_exp, oqctl);
                if (rc && rc != -EBUSY && cmd == Q_QUOTAON) {
                        oqctl->qc_cmd = Q_QUOTAOFF;
                        obd_quotactl(sbi->ll_mdc_exp, oqctl);
                }

                QCTL_COPY(qctl, oqctl);

                if (copy_to_user((void *)arg, qctl, sizeof(*qctl)))
                        rc = -EFAULT;
        out_quotactl:
                OBD_FREE_PTR(qctl);
                OBD_FREE_PTR(oqctl);
                RETURN(rc);
        }
#endif /* HAVE_QUOTA_SUPPORT */
        case OBD_IOC_GETNAME: {  
                struct obd_device *obd = class_exp2obd(sbi->ll_osc_exp);
                if (!obd)
                        RETURN(-EFAULT);
                if (copy_to_user((void *)arg, obd->obd_name, 
                                strlen(obd->obd_name) + 1))
                        RETURN (-EFAULT);
                RETURN(0);
        }
        default:
                RETURN(obd_iocontrol(cmd, sbi->ll_osc_exp,0,NULL,(void *)arg));
        }
}

int ll_dir_open(struct inode *inode, struct file *file)
{
        ENTRY;
        RETURN(ll_file_open(inode, file));
}

int ll_dir_release(struct inode *inode, struct file *file)
{
        ENTRY;
        RETURN(ll_file_release(inode, file));
}

struct file_operations ll_dir_operations = {
        .open     = ll_dir_open,
        .release  = ll_dir_release,
        .read     = generic_read_dir,
        .readdir  = ll_readdir,
        .ioctl    = ll_dir_ioctl
};

