/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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
 */

#define DEBUG_SUBSYSTEM S_RPC

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/init.h>
# include <linux/list.h>
#else
# include <liblustre.h>
#endif
#include <linux/obd.h>
#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_ha.h>
#include <linux/lustre_net.h>
#include <linux/lprocfs_status.h>

struct uuid_nid_data {
        struct list_head un_list;
        lnet_nid_t       un_nid;
        char            *un_uuid;
};

/* FIXME: This should probably become more elegant than a global linked list */
static struct list_head g_uuid_list;
static spinlock_t       g_uuid_lock;

void class_init_uuidlist(void)
{
        INIT_LIST_HEAD(&g_uuid_list);
        spin_lock_init(&g_uuid_lock);
}

void class_exit_uuidlist(void)
{
        /* delete all */
        class_del_uuid(NULL);
}

int lustre_uuid_to_peer(char *uuid, lnet_nid_t *peer_nid, int index)
{
        struct list_head *tmp;

        spin_lock (&g_uuid_lock);

        list_for_each(tmp, &g_uuid_list) {
                struct uuid_nid_data *data =
                        list_entry(tmp, struct uuid_nid_data, un_list);

                if (!strcmp(data->un_uuid, uuid) &&
                    index-- == 0) {
                        *peer_nid = data->un_nid;

                        spin_unlock (&g_uuid_lock);
                        return 0;
                }
        }

        spin_unlock (&g_uuid_lock);
        return -ENOENT;
}

int class_add_uuid(char *uuid, __u64 nid)
{
        struct uuid_nid_data *data;
        int rc;
        int nob = strnlen (uuid, PAGE_SIZE) + 1;

        LASSERT(nid != 0);  /* valid newconfig NID is never zero */

        if (nob > PAGE_SIZE)
                return -EINVAL;

        rc = -ENOMEM;
        OBD_ALLOC(data, sizeof(*data));
        if (data == NULL)
                return -ENOMEM;

        OBD_ALLOC(data->un_uuid, nob);
        if (data == NULL) {
                OBD_FREE(data, sizeof(*data));
                return -ENOMEM;
        }

        CDEBUG(D_INFO, "add uuid %s %s\n", uuid, libcfs_nid2str(nid));
        memcpy(data->un_uuid, uuid, nob);
        data->un_nid = nid;

        spin_lock (&g_uuid_lock);

        list_add(&data->un_list, &g_uuid_list);

        spin_unlock (&g_uuid_lock);

        return 0;
}

/* delete only one entry if uuid is specified, otherwise delete all */
int class_del_uuid (char *uuid)
{
        struct list_head  deathrow;
        struct list_head *tmp;
        struct list_head *n;
        struct uuid_nid_data *data;

        INIT_LIST_HEAD (&deathrow);

        spin_lock (&g_uuid_lock);

        list_for_each_safe(tmp, n, &g_uuid_list) {
                data = list_entry(tmp, struct uuid_nid_data, un_list);

                if (uuid == NULL || strcmp(data->un_uuid, uuid) == 0) {
                        list_del (&data->un_list);
                        list_add (&data->un_list, &deathrow);
                        if (uuid)
                                break;
                }
        }

        spin_unlock (&g_uuid_lock);

        if (list_empty (&deathrow)) {
                if (uuid)
                        CERROR("delete non-existent uuid %s\n", uuid);
                return -EINVAL;
        }

        do {
                data = list_entry(deathrow.next, struct uuid_nid_data, un_list);

                list_del (&data->un_list);
                CDEBUG(D_INFO, "del uuid %s\n", data->un_uuid);

                OBD_FREE(data->un_uuid, strlen(data->un_uuid) + 1);
                OBD_FREE(data, sizeof(*data));
        } while (!list_empty (&deathrow));

        return 0;
}
