/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <braam@clusterfs.com>
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
 * Data structures for object storage targets and client: OST & OSC's
 * 
 * See also lustre_idl.h for wire formats of requests.
 *
 */

#ifndef _LUSTRE_OST_H
#define _LUSTRE_OST_H

#include <linux/obd_class.h>

struct osc_brw_async_args {
        struct obdo     *aa_oa;
        int              aa_requested_nob;
        int              aa_nio_count;
        obd_count        aa_page_count;
        struct brw_page *aa_pga;
        struct client_obd *aa_cli;
        struct list_head aa_oaps;
};

struct osc_getattr_async_args {
        struct obdo     *aa_oa;
};

extern int ost_brw_write(struct ptlrpc_request *, struct obd_trans_info *);
extern int ost_handle(struct ptlrpc_request *req);

#endif
