/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
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

#ifndef _CMM_INTERNAL_H
#define _CMM_INTERNAL_H

#if defined(__KERNEL__)

#include <linux/obd.h>
#include <linux/md_object.h>

struct cmm_mdc_device {
        struct md_device mdc_md_dev;
        /* other MD servers in cluster */
        __u32            cmm_tgt_count;
        struct list_head cmm_tgt_linkage;
};

struct cmm_device {
        struct md_device cmm_md_dev;
        /* underlaying device in MDS stack, usually MDD */
        struct md_device *cmm_child;
        /* other MD servers in cluster */
        __u32            local_num;
        __u32            cmm_tgt_count;
        struct list_head cmm_targets;
};

static inline struct md_device_operations *cmm_child_ops(struct cmm_device *d)
{
        return (d->cmm_child->md_ops);
}

static inline struct cmm_device *md2cmm_dev(struct md_device *m)
{
        return container_of0(m, struct cmm_device, cmm_md_dev);
}

static inline struct cmm_device *lu2cmm_dev(struct lu_device *d)
{
	//LASSERT(lu_device_is_cmm(d));
	return container_of0(d, struct cmm_device, cmm_md_dev.md_lu_dev);
}

static inline struct lu_device *cmm2lu_dev(struct cmm_device *d)
{
	return (&d->cmm_md_dev.md_lu_dev);
}

struct cmm_object {
	struct md_object        cmo_obj;
};

static inline struct cmm_device *cmm_obj2dev(struct cmm_object *c)
{
	return (md2cmm_dev(md_device_get(&c->cmo_obj)));
}

static inline struct cmm_object *lu2cmm_obj(struct lu_object *o)
{
	//LASSERT(lu_device_is_cmm(o->lo_dev));
	return container_of0(o, struct cmm_object, cmo_obj.mo_lu);
}

/* get cmm object from md_object */
static inline struct cmm_object *md2cmm_obj(struct md_object *o)
{
	return container_of0(o, struct cmm_object, cmo_obj);
}
/* get lower-layer object */
static inline struct md_object *cmm2child_obj(struct cmm_object *o)
{
        return lu2md(lu_object_next(&o->cmo_obj.mo_lu));
}

/* cmm device */
int cmm_add_mdc(struct cmm_device *, struct lustre_cfg *);

/* cmm_object.c */
int cmm_object_init(struct lu_context *ctxt, struct lu_object*);
struct lu_object *cmm_object_alloc(struct lu_context *ctx, struct lu_device *);
void cmm_object_free(struct lu_context *ctx, struct lu_object *o);
void cmm_object_release(struct lu_context *ctxt, struct lu_object *o);
//int cmm_getattr(struct lu_object *o, struct lu_attr *a);
//int cmm_setattr(struct lu_object *o, struct lu_attr *a);

/* cmm md operations */
int cmm_config(struct lu_context *ctx, struct md_device *md, const char *name,
               void *buf, int size, int mode);
int cmm_root_get(struct lu_context *ctx, struct md_device *m, struct lu_fid *f);
int cmm_statfs(struct lu_context *ctx,
               struct md_device *m, struct kstatfs *sfs);
int cmm_mkdir(struct lu_context *ctxt, struct md_object *o, const char *name,
              struct md_object *child);
int cmm_xattr_get(struct lu_context *ctxt,
                  struct md_object *obj, void *buf, int size,
                  const char *name);

#endif /* __KERNEL__ */
#endif /* _CMM_INTERNAL_H */
