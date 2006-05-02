/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Extention of lu_object.h for metadata objects
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
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

#ifndef _LINUX_MD_OBJECT_H
#define _LINUX_MD_OBJECT_H

/*
 * Sub-class of lu_object with methods common for "meta-data" objects in MDT
 * stack.
 *
 * Meta-data objects implement namespace operations: you can link, unlink
 * them, and treat them as directories.
 *
 * Examples: mdt, cmm, and mdt are implementations of md interface.
 */


/*
 * super-class definitions.
 */
#include <linux/lu_object.h>

struct md_device;
struct md_device_operations;
struct md_object;

/*
 * Operations implemented for each md object (both directory and leaf).
 */
struct md_object_operations {
        int (*moo_attr_get)(struct lu_context *ctxt, struct md_object *dt,
                            struct lu_attr *attr);
        int (*moo_attr_set)(struct lu_context *ctxt, struct md_object *dt,
                            struct lu_attr *attr);

        int (*moo_xattr_get)(struct lu_context *ctxt, struct md_object *obj,
                             void *buf, int buf_len, const char *name);

        int (*moo_xattr_set)(struct lu_context *ctxt, struct md_object *obj,
                             void *buf, int buf_len, const char *name);
};

/*
 * Operations implemented for each directory object.
 */
struct md_dir_operations {
        int (*mdo_mkdir)(struct lu_context *ctxt, struct lu_attr *attr,
                         struct md_object *obj,
                         const char *name, struct md_object *child);

        int (*mdo_rename)(struct lu_context *ctxt, struct md_object *spobj,
                          struct md_object *tpobj, struct md_object *sobj,
                          const char *sname, struct md_object *tobj,
                          const char *tname);

        int (*mdo_link)(struct lu_context *ctxt, struct md_object *tobj,
                        struct md_object *sobj, const char *name);
        
        /* partial ops for cross-ref case */
        int (*mdo_name_insert)(struct lu_context *, struct md_object *,
                               const char *name, struct lu_fid *,
                               struct lu_attr *);
        int (*mdo_name_remove)(struct lu_context *, struct md_object *,
                               const char *name, struct lu_attr *);
};

struct md_device_operations {
        /* method for getting/setting device wide back stored config data, like
         * last used meta-sequence, etc. */
        int (*mdo_config) (struct lu_context *ctx,
                           struct md_device *m, const char *name,
                           void *buf, int size, int mode);

        /* meta-data device related handlers. */
        int (*mdo_root_get)(struct lu_context *ctx,
                            struct md_device *m, struct lu_fid *f);
        int (*mdo_statfs)(struct lu_context *ctx,
                          struct md_device *m, struct kstatfs *sfs);
        
        /* part of cross-ref operation */
        int (*mdo_object_create)(struct lu_context *, struct md_object *);
        int (*mdo_object_destroy)(struct lu_context *, struct md_object *);

};

struct md_device {
        struct lu_device             md_lu_dev;
        struct md_device_operations *md_ops;
};

struct md_object {
        struct lu_object             mo_lu;
        struct md_object_operations *mo_ops;
        struct md_dir_operations    *mo_dir_ops;
};

static inline int lu_device_is_md(const struct lu_device *d)
{
        return ergo(d != NULL, d->ld_type->ldt_tags & LU_DEVICE_MD);
}

static inline struct md_device *lu2md_dev(const struct lu_device *d)
{
        LASSERT(lu_device_is_md(d));
        return container_of0(d, struct md_device, md_lu_dev);
}

static inline struct lu_device *md2lu_dev(struct md_device *d)
{
        return &d->md_lu_dev;
}

static inline struct md_object *lu2md(const struct lu_object *o)
{
        LASSERT(lu_device_is_md(o->lo_dev));
        return container_of0(o, struct md_object, mo_lu);
}

static inline struct md_object *md_object_next(const struct md_object *obj)
{
        return lu2md(lu_object_next(&obj->mo_lu));
}

static inline struct md_device *md_device_get(const struct md_object *o)
{
        LASSERT(lu_device_is_md(o->mo_lu.lo_dev));
        return container_of0(o->mo_lu.lo_dev, struct md_device, md_lu_dev);
}

static inline int md_device_init(struct md_device *md, struct lu_device_type *t)
{
	return lu_device_init(&md->md_lu_dev, t);
}

static inline void md_device_fini(struct md_device *md)
{
	lu_device_fini(&md->md_lu_dev);
}

#endif /* _LINUX_MD_OBJECT_H */
