/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*- 
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002, 2003 Cluster File Systems, Inc.
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

#ifndef _LUSTRE_ACL_H_
#define _LUSTRE_ACL_H_

#ifdef __KERNEL__
#include <linux/xattr_acl.h>
#endif

/*
* the value of LL_ACL_MAX_ENTRIES and LL_ACL_NOT_CACHED should be 
* kept step with related definition in ext3 (EXT3_ACL_MAX_ENTRIES and
* EXT3_ACL_NOT_CACHED)
*/
#define LL_ACL_MAX_ENTRIES      32      // EXT3_ACL_MAX_ENTRIES
#define LL_ACL_NOT_CACHED       ((void *)-1) //EXT3_ACL_NOT_CACHED

/* remote acl */
#define XATTR_NAME_LUSTRE_ACL   "system.lustre_acl"
#define LUSTRE_ACL_SIZE_MAX     (4096)

#endif
