/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Object Devices Class Driver
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 *
 * These are the only exported functions, they provide some generic
 * infrastructure for managing object devices
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifndef __KERNEL__
#include <liblustre.h>
#else
#include <linux/module.h>
#include <linux/obd_class.h>
#include <linux/lustre_idl.h>
#endif

#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/pagemap.h> /* for PAGE_CACHE_SIZE */

void obdo_from_iattr(struct obdo *oa, struct iattr *attr, unsigned int ia_valid)
{
        if (ia_valid & ATTR_ATIME) {
                oa->o_atime = LTIME_S(attr->ia_atime);
                oa->o_valid |= OBD_MD_FLATIME;
        }
        if (ia_valid & ATTR_MTIME) {
                oa->o_mtime = LTIME_S(attr->ia_mtime);
                oa->o_valid |= OBD_MD_FLMTIME;
        }
        if (ia_valid & ATTR_CTIME) {
                oa->o_ctime = LTIME_S(attr->ia_ctime);
                oa->o_valid |= OBD_MD_FLCTIME;
        }
        if (ia_valid & ATTR_SIZE) {
                oa->o_size = attr->ia_size;
                oa->o_valid |= OBD_MD_FLSIZE;
        }
        if (ia_valid & ATTR_MODE) {
                oa->o_mode = attr->ia_mode;
                oa->o_valid |= OBD_MD_FLTYPE | OBD_MD_FLMODE;
                if (!in_group_p(oa->o_gid) && !capable(CAP_FSETID))
                        oa->o_mode &= ~S_ISGID;
        }
        if (ia_valid & ATTR_UID) {
                oa->o_uid = attr->ia_uid;
                oa->o_valid |= OBD_MD_FLUID;
        }
        if (ia_valid & ATTR_GID) {
                oa->o_gid = attr->ia_gid;
                oa->o_valid |= OBD_MD_FLGID;
        }
}
EXPORT_SYMBOL(obdo_from_iattr);

void iattr_from_obdo(struct iattr *attr, struct obdo *oa, obd_flag valid)
{
        valid &= oa->o_valid;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE, "valid "LPX64", new time "LPU64"/"LPU64"\n",
                       oa->o_valid, oa->o_mtime, oa->o_ctime);

        attr->ia_valid = 0;
        if (valid & OBD_MD_FLATIME) {
                LTIME_S(attr->ia_atime) = oa->o_atime;
                attr->ia_valid |= ATTR_ATIME;
        }
        if (valid & OBD_MD_FLMTIME) {
                LTIME_S(attr->ia_mtime) = oa->o_mtime;
                attr->ia_valid |= ATTR_MTIME;
        }
        if (valid & OBD_MD_FLCTIME) {
                LTIME_S(attr->ia_ctime) = oa->o_ctime;
                attr->ia_valid |= ATTR_CTIME;
        }
        if (valid & OBD_MD_FLSIZE) {
                attr->ia_size = oa->o_size;
                attr->ia_valid |= ATTR_SIZE;
        }
#if 0   /* you shouldn't be able to change a file's type with setattr */
        if (valid & OBD_MD_FLTYPE) {
                attr->ia_mode = (attr->ia_mode & ~S_IFMT)|(oa->o_mode & S_IFMT);
                attr->ia_valid |= ATTR_MODE;
        }
#endif
        if (valid & OBD_MD_FLMODE) {
                attr->ia_mode = (attr->ia_mode & S_IFMT)|(oa->o_mode & ~S_IFMT);
                attr->ia_valid |= ATTR_MODE;
                if (!in_group_p(oa->o_gid) && !capable(CAP_FSETID))
                        attr->ia_mode &= ~S_ISGID;
        }
        if (valid & OBD_MD_FLUID) {
                attr->ia_uid = oa->o_uid;
                attr->ia_valid |= ATTR_UID;
        }
        if (valid & OBD_MD_FLGID) {
                attr->ia_gid = oa->o_gid;
                attr->ia_valid |= ATTR_GID;
        }
        if (valid & OBD_MD_FLFLAGS) {
                attr->ia_attr_flags = oa->o_flags;
                attr->ia_valid |= ATTR_ATTR_FLAG;
        }
}
EXPORT_SYMBOL(iattr_from_obdo);

/* WARNING: the file systems must take care not to tinker with
   attributes they don't manage (such as blocks). */
void obdo_from_inode(struct obdo *dst, struct inode *src, obd_flag valid)
{
        obd_flag newvalid = 0;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE, "valid %x, new time %lu/%lu\n",
                       valid, LTIME_S(src->i_mtime), 
                       LTIME_S(src->i_ctime));

        if (valid & OBD_MD_FLATIME) {
                dst->o_atime = LTIME_S(src->i_atime);
                newvalid |= OBD_MD_FLATIME;
        }
        if (valid & OBD_MD_FLMTIME) {
                dst->o_mtime = LTIME_S(src->i_mtime);
                newvalid |= OBD_MD_FLMTIME;
        }
        if (valid & OBD_MD_FLCTIME) {
                dst->o_ctime = LTIME_S(src->i_ctime);
                newvalid |= OBD_MD_FLCTIME;
        }
        if (valid & OBD_MD_FLSIZE) {
                dst->o_size = src->i_size;
                newvalid |= OBD_MD_FLSIZE;
        }
        if (valid & OBD_MD_FLBLOCKS) {  /* allocation of space (x512 bytes) */
                dst->o_blocks = src->i_blocks;
                newvalid |= OBD_MD_FLBLOCKS;
        }
        if (valid & OBD_MD_FLBLKSZ) {   /* optimal block size */
                dst->o_blksize = src->i_blksize;
                newvalid |= OBD_MD_FLBLKSZ;
        }
        if (valid & OBD_MD_FLTYPE) {
                dst->o_mode = (dst->o_mode & S_IALLUGO)|(src->i_mode & S_IFMT);
                newvalid |= OBD_MD_FLTYPE;
        }
        if (valid & OBD_MD_FLMODE) {
                dst->o_mode = (dst->o_mode & S_IFMT)|(src->i_mode & S_IALLUGO);
                newvalid |= OBD_MD_FLMODE;
        }
        if (valid & OBD_MD_FLUID) {
                dst->o_uid = src->i_uid;
                newvalid |= OBD_MD_FLUID;
        }
        if (valid & OBD_MD_FLGID) {
                dst->o_gid = src->i_gid;
                newvalid |= OBD_MD_FLGID;
        }
        if (valid & OBD_MD_FLFLAGS) {
                dst->o_flags = src->i_flags;
                newvalid |= OBD_MD_FLFLAGS;
        }
        if (valid & OBD_MD_FLGENER) {
                dst->o_generation = src->i_generation;
                newvalid |= OBD_MD_FLGENER;
        }

        dst->o_valid |= newvalid;
}
EXPORT_SYMBOL(obdo_from_inode);

void obdo_refresh_inode(struct inode *dst, struct obdo *src, obd_flag valid)
{
        valid &= src->o_valid;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE,
                       "valid "LPX64", cur time %lu/%lu, new "LPU64"/"LPU64"\n",
                       src->o_valid, LTIME_S(dst->i_mtime),
                       LTIME_S(dst->i_ctime), src->o_mtime, src->o_ctime);

        if (valid & OBD_MD_FLATIME && src->o_atime > LTIME_S(dst->i_atime))
                LTIME_S(dst->i_atime) = src->o_atime;
        if (valid & OBD_MD_FLMTIME && src->o_mtime > LTIME_S(dst->i_mtime))
                LTIME_S(dst->i_mtime) = src->o_mtime;
        if (valid & OBD_MD_FLCTIME && src->o_ctime > LTIME_S(dst->i_ctime))
                LTIME_S(dst->i_ctime) = src->o_ctime;
        if (valid & OBD_MD_FLSIZE) 
                dst->i_size = src->o_size;
        /* optimum IO size */
        if (valid & OBD_MD_FLBLKSZ && src->o_blksize > dst->i_blksize)
                dst->i_blksize = src->o_blksize;
        if (dst->i_blksize < PAGE_CACHE_SIZE)
                dst->i_blksize = PAGE_CACHE_SIZE;
        /* allocation of space */
        if (valid & OBD_MD_FLBLOCKS && src->o_blocks > dst->i_blocks)
                dst->i_blocks = src->o_blocks;
}
EXPORT_SYMBOL(obdo_refresh_inode);

void obdo_to_inode(struct inode *dst, struct obdo *src, obd_flag valid)
{
        valid &= src->o_valid;

        if (valid & (OBD_MD_FLCTIME | OBD_MD_FLMTIME))
                CDEBUG(D_INODE,
                       "valid "LPX64", cur time %lu/%lu, new "LPU64"/"LPU64"\n",
                       src->o_valid, LTIME_S(dst->i_mtime),
                       LTIME_S(dst->i_ctime), src->o_mtime, src->o_ctime);

        if (valid & OBD_MD_FLATIME)
                LTIME_S(dst->i_atime) = src->o_atime;
        if (valid & OBD_MD_FLMTIME)
                LTIME_S(dst->i_mtime) = src->o_mtime;
        if (valid & OBD_MD_FLCTIME && src->o_ctime > LTIME_S(dst->i_ctime))
                LTIME_S(dst->i_ctime) = src->o_ctime;
        if (valid & OBD_MD_FLSIZE)
                dst->i_size = src->o_size;
        if (valid & OBD_MD_FLBLOCKS) /* allocation of space */
                dst->i_blocks = src->o_blocks;
        if (valid & OBD_MD_FLBLKSZ)
                dst->i_blksize = src->o_blksize;
        if (valid & OBD_MD_FLTYPE)
                dst->i_mode = (dst->i_mode & ~S_IFMT) | (src->o_mode & S_IFMT);
        if (valid & OBD_MD_FLMODE)
                dst->i_mode = (dst->i_mode & S_IFMT) | (src->o_mode & ~S_IFMT);
        if (valid & OBD_MD_FLUID)
                dst->i_uid = src->o_uid;
        if (valid & OBD_MD_FLGID)
                dst->i_gid = src->o_gid;
        if (valid & OBD_MD_FLFLAGS)
                dst->i_flags = src->o_flags;
        if (valid & OBD_MD_FLGENER)
                dst->i_generation = src->o_generation;
}
EXPORT_SYMBOL(obdo_to_inode);
#endif

void obdo_cpy_md(struct obdo *dst, struct obdo *src, obd_flag valid)
{
#ifdef __KERNEL__
        CDEBUG(D_INODE, "src obdo "LPX64" valid "LPX64", dst obdo "LPX64"\n",
               src->o_id, src->o_valid, dst->o_id);
#endif
        if (valid & OBD_MD_FLATIME)
                dst->o_atime = src->o_atime;
        if (valid & OBD_MD_FLMTIME)
                dst->o_mtime = src->o_mtime;
        if (valid & OBD_MD_FLCTIME)
                dst->o_ctime = src->o_ctime;
        if (valid & OBD_MD_FLSIZE)
                dst->o_size = src->o_size;
        if (valid & OBD_MD_FLBLOCKS) /* allocation of space */
                dst->o_blocks = src->o_blocks;
        if (valid & OBD_MD_FLBLKSZ)
                dst->o_blksize = src->o_blksize;
        if (valid & OBD_MD_FLTYPE)
                dst->o_mode = (dst->o_mode & ~S_IFMT) | (src->o_mode & S_IFMT);
        if (valid & OBD_MD_FLMODE)
                dst->o_mode = (dst->o_mode & S_IFMT) | (src->o_mode & ~S_IFMT);
        if (valid & OBD_MD_FLUID)
                dst->o_uid = src->o_uid;
        if (valid & OBD_MD_FLGID)
                dst->o_gid = src->o_gid;
        if (valid & OBD_MD_FLFLAGS)
                dst->o_flags = src->o_flags;
        if (valid & OBD_MD_FLGENER)
                dst->o_generation = src->o_generation;
        if (valid & OBD_MD_FLINLINE)
                memcpy(dst->o_inline, src->o_inline, sizeof(src->o_inline));

        dst->o_valid |= valid;
}
EXPORT_SYMBOL(obdo_cpy_md);

/* returns FALSE if comparison (by flags) is same, TRUE if changed */
int obdo_cmp_md(struct obdo *dst, struct obdo *src, obd_flag compare)
{
        int res = 0;

        if ( compare & OBD_MD_FLATIME )
                res = (res || (dst->o_atime != src->o_atime));
        if ( compare & OBD_MD_FLMTIME )
                res = (res || (dst->o_mtime != src->o_mtime));
        if ( compare & OBD_MD_FLCTIME )
                res = (res || (dst->o_ctime != src->o_ctime));
        if ( compare & OBD_MD_FLSIZE )
                res = (res || (dst->o_size != src->o_size));
        if ( compare & OBD_MD_FLBLOCKS ) /* allocation of space */
                res = (res || (dst->o_blocks != src->o_blocks));
        if ( compare & OBD_MD_FLBLKSZ )
                res = (res || (dst->o_blksize != src->o_blksize));
        if ( compare & OBD_MD_FLTYPE )
                res = (res || (((dst->o_mode ^ src->o_mode) & S_IFMT) != 0));
        if ( compare & OBD_MD_FLMODE )
                res = (res || (((dst->o_mode ^ src->o_mode) & ~S_IFMT) != 0));
        if ( compare & OBD_MD_FLUID )
                res = (res || (dst->o_uid != src->o_uid));
        if ( compare & OBD_MD_FLGID )
                res = (res || (dst->o_gid != src->o_gid));
        if ( compare & OBD_MD_FLFLAGS )
                res = (res || (dst->o_flags != src->o_flags));
        if ( compare & OBD_MD_FLNLINK )
                res = (res || (dst->o_nlink != src->o_nlink));
        if ( compare & OBD_MD_FLGENER )
                res = (res || (dst->o_generation != src->o_generation));
        /* XXX Don't know if thses should be included here - wasn't previously
        if ( compare & OBD_MD_FLINLINE )
                res = (res || memcmp(dst->o_inline, src->o_inline));
        */
        return res;
}
EXPORT_SYMBOL(obdo_cmp_md);

void obdo_to_ioobj(struct obdo *oa, struct obd_ioobj *ioobj)
{
        ioobj->ioo_id = oa->o_id;
        if (oa->o_valid & OBD_MD_FLGROUP)
                ioobj->ioo_gr = oa->o_gr;
        else 
                ioobj->ioo_gr = 0;
        ioobj->ioo_type = oa->o_mode;
}
EXPORT_SYMBOL(obdo_to_ioobj);
