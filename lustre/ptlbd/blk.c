/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Zach Brown <zab@clusterfs.com>
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
#include <linux/module.h>
#include <linux/major.h>
#include <linux/smp.h>
#include <linux/hdreg.h>

#define DEBUG_SUBSYSTEM S_PTLBD

#include <linux/lustre_ha.h>
#include <linux/obd_support.h>
#include <linux/lustre_idl.h>
#include <linux/obd_ptlbd.h>

/*
 * todo:
 *   assign proper major number
 *   allow more minors
 *   discover actual block sizes?
 *   allow more than one sector per io
 *   think about vary-io
 *   restrict single ops to sequential block io
 *   ddn target addresses need to be 32 bit
 *   cant get to addresses after 0xFFFF0000
 */

#define PTLBD_MAJOR 253
#define PTLBD_MAX_MINOR 1

#define MAJOR_NR PTLBD_MAJOR
#define LOCAL_END_REQUEST
#include <linux/blk.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/devfs_fs_kernel.h>

static int ptlbd_size_size[PTLBD_MAX_MINOR];
static int ptlbd_size[PTLBD_MAX_MINOR];
static int ptlbd_hardsect_size[PTLBD_MAX_MINOR];
static int ptlbd_max_sectors[PTLBD_MAX_MINOR];
//RHism static char ptlbd_dev_varyio[PTLBD_MAX_MINOR];

/*
 * per minor state, indexed by minor.
 */

static struct ptlbd_obd *one_for_now;

void ptlbd_blk_register(struct ptlbd_obd *ptlbd)
{
        ENTRY;
        one_for_now = ptlbd;
        EXIT;
}

static struct ptlbd_obd * ptlbd_get_minor(int minor)
{
        ENTRY;
        if ( minor >= PTLBD_MAX_MINOR ) 
                RETURN( ERR_PTR(-ENODEV) );
        RETURN(one_for_now);
}

static struct ptlbd_obd * ptlbd_get_inode(struct inode  *inode)
{
        ENTRY;

        if ( inode == NULL ) /* can this really happen? */
                RETURN( ERR_PTR(-EINVAL) );

        return ptlbd_get_minor(MINOR(inode->i_rdev));
}

static int ptlbd_open(struct inode *inode, struct file  *file)
{
        struct ptlbd_obd *ptlbd = ptlbd_get_inode(inode);
        ENTRY;


        if ( IS_ERR(ptlbd) )
                RETURN(PTR_ERR(ptlbd));

        if (! ptlbd->bd_import->imp_remote_handle.cookie)
               if (ptlbd_do_connect(ptlbd))
                       RETURN(-ENOTCONN);

        ptlbd->refcount++;
        RETURN(0);
}


static int ptlbd_ioctl(struct inode *inode, struct file *file,
                unsigned int cmd, unsigned long arg)
{
        struct ptlbd_obd *ptlbd;
        int ret;
        __u16   major, minor, dev;
        struct hd_geometry geo;

        if ( ! capable(CAP_SYS_ADMIN) )
                RETURN(-EPERM);

        ptlbd = ptlbd_get_inode(inode);
        if ( IS_ERR(ptlbd) )
                RETURN( PTR_ERR(ptlbd) );

        major = MAJOR(inode->i_rdev);
        minor = MINOR(inode->i_rdev);
        dev = inode->i_rdev;

        switch(cmd) {
                case HDIO_GETGEO:
                        geo.heads = 64;
                        geo.sectors = 32;
                        geo.start = 4;
                        geo.cylinders = blk_size[major][minor]/
                                        (geo.heads * geo.sectors);
                        if (copy_to_user((void *) arg, &geo, sizeof(geo)))
                                ret = -EFAULT;
                        else  
                                ret = 0;
                        break;

                case BLKSECTGET:
                        ret = copy_to_user((void *) arg, 
                                & max_sectors[major][minor], sizeof(arg));
                        break;

                case BLKFLSBUF:
                        ret = blk_ioctl(dev, cmd, arg);
                        ptlbd_send_flush_req(ptlbd, PTLBD_FLUSH);
                        break;

                case BLKGETSIZE:
                case BLKGETSIZE64:
                case BLKROSET:
                case BLKROGET:
                case BLKRASET:
                case BLKRAGET:
                case BLKSSZGET:
                case BLKELVGET:
                case BLKELVSET:
                default:
                        ret = blk_ioctl(dev, cmd, arg);
                        break;

                case BLKSECTSET:       /* don't allow setting of max_sectors */

                case BLKRRPART:        /* not a partitionable device */
                case BLKPG:            /* "" */
                        ret = -EINVAL;
                        break;
        }

        RETURN(ret);
}

static int ptlbd_release(struct inode *inode, struct file *file)
{
        struct ptlbd_obd *ptlbd = ptlbd_get_inode(inode);
        ENTRY;

        if ( IS_ERR(ptlbd) ) 
                RETURN( PTR_ERR(ptlbd) );

        if (--ptlbd->refcount == 0)
                ptlbd_do_disconnect(ptlbd);

        RETURN(0);
}

static void ptlbd_end_request_havelock(struct request *req)
{
        struct buffer_head *bh;
        int uptodate = 1;

        if ( req->errors )
                uptodate = 0;

        while( (bh = req->bh) != NULL ) {
                blk_finished_io(bh->b_size >> 9);
                req->bh = bh->b_reqnext;
                bh->b_reqnext = NULL;
                bh->b_end_io(bh, uptodate);
        }
        blkdev_release_request(req);
}

#if 0
static void ptlbd_end_request_getlock(struct request *req)
{
        unsigned long flags;

        spin_lock_irqsave(&io_request_lock, flags);
        ptlbd_end_request_havelock(req);
        spin_unlock_irqrestore(&io_request_lock, flags);
}
#endif

static void ptlbd_request(request_queue_t *q)
{
        struct ptlbd_obd *ptlbd;
        struct request *req;
        ptlbd_cmd_t cmd;
        int     errors = 0;
        ENTRY;

        while ( !QUEUE_EMPTY ) {
                req = CURRENT;
                ptlbd = ptlbd_get_minor(MINOR(req->rq_dev));

                blkdev_dequeue_request(req);

                if ( ptlbd->refcount <= 0 ) {
                        req->errors++;
                        ptlbd_end_request_havelock(req);
                        return;
                }

                spin_unlock_irq(&io_request_lock);

                if ( req->cmd == READ )
                        cmd = PTLBD_READ;
                else 
                        cmd = PTLBD_WRITE;

                errors = ptlbd_send_rw_req(ptlbd, cmd, req->bh);

                spin_lock_irq(&io_request_lock);

                if (errors)
                        req->errors += errors;

                ptlbd_end_request_havelock(req);
        }
}

static struct block_device_operations ptlbd_ops = {
        .owner = THIS_MODULE,
        .open = ptlbd_open,
        .release = ptlbd_release,
        .ioctl = ptlbd_ioctl,
};

int ptlbd_blk_init(void)
{
        int ret;
        int i;
        ENTRY;

        ret = register_blkdev(PTLBD_MAJOR, "ptlbd", &ptlbd_ops);
        if ( ret < 0 ) 
                RETURN(ret);

        blk_size[PTLBD_MAJOR] = ptlbd_size;
        blksize_size[PTLBD_MAJOR] = ptlbd_size_size;
        hardsect_size[PTLBD_MAJOR] = ptlbd_hardsect_size;
        max_sectors[PTLBD_MAJOR] = ptlbd_max_sectors;

        blk_init_queue(BLK_DEFAULT_QUEUE(PTLBD_MAJOR), ptlbd_request);
        blk_queue_headactive(BLK_DEFAULT_QUEUE(MAJOR_NR), 0);

        for ( i = 0 ; i < PTLBD_MAX_MINOR ; i++) {
                ptlbd_size_size[i] = 4096;
                /* avoid integer overflow */
                ptlbd_size[i] = (16*1024*((1024*1024) >> BLOCK_SIZE_BITS));
                ptlbd_hardsect_size[i] = 4096;
                ptlbd_max_sectors[i] = PTL_MD_MAX_IOV * (4096/512);
        }

        return 0;
}

void ptlbd_blk_exit(void)
{
        ENTRY;
        blk_cleanup_queue(BLK_DEFAULT_QUEUE(PTLBD_MAJOR));
        unregister_blkdev(PTLBD_MAJOR, "ptlbd");
}

#undef MAJOR_NR
