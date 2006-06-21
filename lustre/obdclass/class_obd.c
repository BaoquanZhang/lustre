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
# include <liblustre.h>
#endif

#include <obd_support.h>
#include <obd_class.h>
#include <lustre_debug.h>
#include <lprocfs_status.h>
#ifdef __KERNEL__
#include <linux/lustre_build_version.h>
#endif
#include <libcfs/list.h>
#include <lustre_ver.h>
#include "llog_internal.h"

#ifndef __KERNEL__
/* liblustre workaround */
atomic_t libcfs_kmemory = {0};
#endif

struct obd_device *obd_devs[MAX_OBD_DEVICES];
struct list_head obd_types;
spinlock_t obd_dev_lock = SPIN_LOCK_UNLOCKED;
#ifndef __KERNEL__
atomic_t obd_memory;
int obd_memmax;
#endif

/* The following are visible and mutable through /proc/sys/lustre/. */
unsigned int obd_fail_loc;
unsigned int obd_dump_on_timeout;
unsigned int obd_timeout = 100; /* seconds */
unsigned int ldlm_timeout = 20; /* seconds */
unsigned int obd_health_check_timeout = 120; /* seconds */
char obd_lustre_upcall[128] = "DEFAULT"; /* or NONE or /full/path/to/upcall  */

cfs_waitq_t obd_race_waitq;

#ifdef __KERNEL__
unsigned int obd_print_fail_loc(void)
{
        CWARN("obd_fail_loc = %x\n", obd_fail_loc);
        return obd_fail_loc;
}

void obd_set_fail_loc(unsigned int fl)
{
        obd_fail_loc = fl;
}

/*  opening /dev/obd */
static int obd_class_open(unsigned long flags, void *args)
{
        ENTRY;

        PORTAL_MODULE_USE;
        RETURN(0);
}

/*  closing /dev/obd */
static int obd_class_release(unsigned long flags, void *args)
{
        ENTRY;

        PORTAL_MODULE_UNUSE;
        RETURN(0);
}
#endif

static inline void obd_data2conn(struct lustre_handle *conn,
                                 struct obd_ioctl_data *data)
{
        memset(conn, 0, sizeof *conn);
        conn->cookie = data->ioc_cookie;
}

static inline void obd_conn2data(struct obd_ioctl_data *data,
                                 struct lustre_handle *conn)
{
        data->ioc_cookie = conn->cookie;
}

int class_resolve_dev_name(uint32_t len, const char *name)
{
        int rc;
        int dev;

        ENTRY;
        if (!len || !name) {
                CERROR("No name passed,!\n");
                GOTO(out, rc = -EINVAL);
        }
        if (name[len - 1] != 0) {
                CERROR("Name not nul terminated!\n");
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_IOCTL, "device name %s\n", name);
        dev = class_name2dev(name);
        if (dev == -1) {
                CDEBUG(D_IOCTL, "No device for name %s!\n", name);
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_IOCTL, "device name %s, dev %d\n", name, dev);
        rc = dev;

out:
        RETURN(rc);
}

int class_handle_ioctl(unsigned int cmd, unsigned long arg)
{
        char *buf = NULL;
        struct obd_ioctl_data *data;
        struct libcfs_debug_ioctl_data *debug_data;
        struct obd_device *obd = NULL;
        int err = 0, len = 0;
        ENTRY;

        /* only for debugging */
        if (cmd == LIBCFS_IOC_DEBUG_MASK) {
                debug_data = (struct libcfs_debug_ioctl_data*)arg;
                libcfs_subsystem_debug = debug_data->subs;
                libcfs_debug = debug_data->debug;
                return 0;
        }

        CDEBUG(D_IOCTL, "cmd = %x\n", cmd);
        if (obd_ioctl_getdata(&buf, &len, (void *)arg)) {
                CERROR("OBD ioctl: data error\n");
                GOTO(out, err = -EINVAL);
        }
        data = (struct obd_ioctl_data *)buf;

        switch (cmd) {
        case OBD_IOC_PROCESS_CFG: {
                struct lustre_cfg *lcfg;

                if (!data->ioc_plen1 || !data->ioc_pbuf1) {
                        CERROR("No config buffer passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                OBD_ALLOC(lcfg, data->ioc_plen1);
                err = copy_from_user(lcfg, data->ioc_pbuf1, data->ioc_plen1);
                if (err) {
                        OBD_FREE(lcfg, data->ioc_plen1);
                        GOTO(out, err);
                }
                err = lustre_cfg_sanity_check(lcfg, data->ioc_plen1);
                if (err) {
                        OBD_FREE(lcfg, data->ioc_plen1);
                        GOTO(out, err);
                }
                err = class_process_config(lcfg);

                OBD_FREE(lcfg, data->ioc_plen1);
                GOTO(out, err);
        }

        case OBD_GET_VERSION:
                if (!data->ioc_inlbuf1) {
                        CERROR("No buffer passed in ioctl\n");
                        GOTO(out, err = -EINVAL);
                }

                if (strlen(BUILD_VERSION) + 1 > data->ioc_inllen1) {
                        CERROR("ioctl buffer too small to hold version\n");
                        GOTO(out, err = -EINVAL);
                }

                memcpy(data->ioc_bulk, BUILD_VERSION,
                       strlen(BUILD_VERSION) + 1);

                err = obd_ioctl_popdata((void *)arg, data, len);
                if (err)
                        err = -EFAULT;
                GOTO(out, err);

        case OBD_IOC_NAME2DEV: {
                /* Resolve a device name.  This does not change the
                 * currently selected device.
                 */
                int dev;

                dev = class_resolve_dev_name(data->ioc_inllen1,
                                             data->ioc_inlbuf1);
                data->ioc_dev = dev;
                if (dev < 0)
                        GOTO(out, err = -EINVAL);

                err = obd_ioctl_popdata((void *)arg, data, sizeof(*data));
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_UUID2DEV: {
                /* Resolve a device uuid.  This does not change the
                 * currently selected device.
                 */
                int dev;
                struct obd_uuid uuid;

                if (!data->ioc_inllen1 || !data->ioc_inlbuf1) {
                        CERROR("No UUID passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inlbuf1[data->ioc_inllen1 - 1] != 0) {
                        CERROR("UUID not NUL terminated!\n");
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s\n", data->ioc_inlbuf1);
                obd_str2uuid(&uuid, data->ioc_inlbuf1);
                dev = class_uuid2dev(&uuid);
                data->ioc_dev = dev;
                if (dev == -1) {
                        CDEBUG(D_IOCTL, "No device for UUID %s!\n",
                               data->ioc_inlbuf1);
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s, dev %d\n", data->ioc_inlbuf1,
                       dev);
                err = obd_ioctl_popdata((void *)arg, data, sizeof(*data));
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_CLOSE_UUID: {
                CDEBUG(D_IOCTL, "closing all connections to uuid %s (NOOP)\n",
                       data->ioc_inlbuf1);
                GOTO(out, err = 0);
        }

        case OBD_IOC_GETDEVICE: {
                int     index = data->ioc_count;
                char    *status, *str;

                if (!data->ioc_inlbuf1) {
                        CERROR("No buffer passed in ioctl\n");
                        GOTO(out, err = -EINVAL);
                } 
                if (data->ioc_inllen1 < 128) {
                        CERROR("ioctl buffer too small to hold version\n");
                        GOTO(out, err = -EINVAL);
                }
                                
                if (index >= MAX_OBD_DEVICES)
                        GOTO(out, err = -ENOENT);
                obd = obd_devs[index];
                if (!obd->obd_type)
                        GOTO(out, err = -ENOENT);
                
                if (obd->obd_stopping)
                        status = "ST";
                else if (obd->obd_set_up)
                        status = "UP";
                else if (obd->obd_attached)
                        status = "AT";
                else
                        status = "--"; 
                str = (char *)data->ioc_bulk;
                snprintf(str, len - sizeof(*data), "%3d %s %s %s %s %d",
                         (int)index, status, obd->obd_type->typ_name,
                         obd->obd_name, obd->obd_uuid.uuid,
                         atomic_read(&obd->obd_refcount));
                err = obd_ioctl_popdata((void *)arg, data, len);

                GOTO(out, err = 0);
        }

        }

        if (data->ioc_dev >= class_devno_max()) {
                CERROR("OBD ioctl: No device\n");
                GOTO(out, err = -EINVAL);
        }

        obd = class_num2obd(data->ioc_dev);
        if (obd == NULL) {
                CERROR("OBD ioctl : No Device %d\n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }
        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);

        if (!obd->obd_set_up || obd->obd_stopping) {
                CERROR("OBD ioctl: device not setup %d \n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }

        switch(cmd) {
        case OBD_IOC_NO_TRANSNO: {
                if (!obd->obd_attached) {
                        CERROR("Device %d not attached\n", obd->obd_minor);
                        GOTO(out, err = -ENODEV);
                }
                CDEBUG(D_HA, "%s: disabling committed-transno notification\n",
                       obd->obd_name);
                obd->obd_no_transno = 1;
                GOTO(out, err = 0);
        }

        default: {
                err = obd_iocontrol(cmd, obd->obd_self_export, len, data, NULL);
                if (err)
                        GOTO(out, err);

                err = obd_ioctl_popdata((void *)arg, data, len);
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }
        }

 out:
        if (buf)
                obd_ioctl_freedata(buf, len);
        RETURN(err);
} /* class_handle_ioctl */



#define OBD_MINOR 241
#ifdef __KERNEL__
/* to control /dev/obd */
static int obd_class_ioctl (struct cfs_psdev_file *pfile, unsigned long cmd, void *arg)
{
        return class_handle_ioctl(cmd, (unsigned long)arg);
}

/* declare character device */
struct cfs_psdev_ops obd_psdev_ops = {
        /* .p_open    = */ obd_class_open,      /* open */
        /* .p_close   = */ obd_class_release,   /* release */
        /* .p_read    = */ NULL,
        /* .p_write   = */ NULL,
        /* .p_ioctl   = */ obd_class_ioctl     /* ioctl */
};

extern cfs_psdev_t obd_psdev;
#else
void *obd_psdev = NULL;
#endif

EXPORT_SYMBOL(obd_devs);
EXPORT_SYMBOL(obd_fail_loc);
EXPORT_SYMBOL(obd_print_fail_loc);
EXPORT_SYMBOL(obd_race_waitq);
EXPORT_SYMBOL(obd_dump_on_timeout);
EXPORT_SYMBOL(obd_timeout);
EXPORT_SYMBOL(ldlm_timeout);
EXPORT_SYMBOL(obd_health_check_timeout);
EXPORT_SYMBOL(obd_lustre_upcall);
EXPORT_SYMBOL(ptlrpc_put_connection_superhack);

EXPORT_SYMBOL(proc_lustre_root);

EXPORT_SYMBOL(class_register_type);
EXPORT_SYMBOL(class_unregister_type);
EXPORT_SYMBOL(class_get_type);
EXPORT_SYMBOL(class_put_type);
EXPORT_SYMBOL(class_name2dev);
EXPORT_SYMBOL(class_name2obd);
EXPORT_SYMBOL(class_uuid2dev);
EXPORT_SYMBOL(class_uuid2obd);
EXPORT_SYMBOL(class_find_client_obd);
EXPORT_SYMBOL(class_find_client_notype);
EXPORT_SYMBOL(class_devices_in_group);
EXPORT_SYMBOL(class_conn2export);
EXPORT_SYMBOL(class_exp2obd);
EXPORT_SYMBOL(class_conn2obd);
EXPORT_SYMBOL(class_exp2cliimp);
EXPORT_SYMBOL(class_conn2cliimp);
EXPORT_SYMBOL(class_disconnect);
EXPORT_SYMBOL(class_num2obd);

/* uuid.c */
EXPORT_SYMBOL(class_generate_random_uuid);
EXPORT_SYMBOL(class_uuid_unparse);
EXPORT_SYMBOL(lustre_uuid_to_peer);

EXPORT_SYMBOL(class_handle_hash);
EXPORT_SYMBOL(class_handle_unhash);
EXPORT_SYMBOL(class_handle2object);

/* obd_config.c */
EXPORT_SYMBOL(class_incref);
EXPORT_SYMBOL(class_decref);
EXPORT_SYMBOL(class_get_profile);
EXPORT_SYMBOL(class_del_profile);
EXPORT_SYMBOL(class_del_profiles);
EXPORT_SYMBOL(class_process_config);
EXPORT_SYMBOL(class_config_parse_llog);
EXPORT_SYMBOL(class_config_dump_llog);
EXPORT_SYMBOL(class_attach);
EXPORT_SYMBOL(class_setup);
EXPORT_SYMBOL(class_cleanup);
EXPORT_SYMBOL(class_detach);
EXPORT_SYMBOL(class_manual_cleanup);

#define OBD_INIT_CHECK
#ifdef OBD_INIT_CHECK
int obd_init_checks(void)
{
        __u64 u64val, div64val;
        char buf[64];
        int len, ret = 0;

        CDEBUG(D_INFO, "LPU64=%s, LPD64=%s, LPX64=%s, LPSZ=%s, LPSSZ=%s\n",
               LPU64, LPD64, LPX64, LPSZ, LPSSZ);

        CDEBUG(D_INFO, "OBD_OBJECT_EOF = "LPX64"\n", (__u64)OBD_OBJECT_EOF);

        u64val = OBD_OBJECT_EOF;
        CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = "LPX64"\n", u64val);
        if (u64val != OBD_OBJECT_EOF) {
                CERROR("__u64 "LPX64"(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                ret = -EINVAL;
        }
        len = snprintf(buf, sizeof(buf), LPX64, u64val);
        if (len != 18) {
                CWARN("LPX64 wrong length! strlen(%s)=%d != 18\n", buf, len);
                ret = -EINVAL;
        }

        div64val = OBD_OBJECT_EOF;
        CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = "LPX64"\n", u64val);
        if (u64val != OBD_OBJECT_EOF) {
                CERROR("__u64 "LPX64"(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                ret = -EOVERFLOW;
        }
        if (u64val >> 8 != OBD_OBJECT_EOF >> 8) {
                CERROR("__u64 "LPX64"(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                return -EOVERFLOW;
        }
        if (do_div(div64val, 256) != (u64val & 255)) {
                CERROR("do_div("LPX64",256) != "LPU64"\n", u64val, u64val &255);
                return -EOVERFLOW;
        }
        if (u64val >> 8 != div64val) {
                CERROR("do_div("LPX64",256) "LPU64" != "LPU64"\n",
                       u64val, div64val, u64val >> 8);
                return -EOVERFLOW;
        }
        len = snprintf(buf, sizeof(buf), LPX64, u64val);
        if (len != 18) {
                CWARN("LPX64 wrong length! strlen(%s)=%d != 18\n", buf, len);
                ret = -EINVAL;
        }
        len = snprintf(buf, sizeof(buf), LPU64, u64val);
        if (len != 20) {
                CWARN("LPU64 wrong length! strlen(%s)=%d != 20\n", buf, len);
                ret = -EINVAL;
        }
        len = snprintf(buf, sizeof(buf), LPD64, u64val);
        if (len != 2) {
                CWARN("LPD64 wrong length! strlen(%s)=%d != 2\n", buf, len);
                ret = -EINVAL;
        }
        if ((u64val & ~CFS_PAGE_MASK) >= CFS_PAGE_SIZE) {
                CWARN("mask failed: u64val "LPU64" >= %lu\n", u64val,CFS_PAGE_SIZE);
                ret = -EINVAL;
        }

        return ret;
}
#else
#define obd_init_checks() do {} while(0)
#endif

extern spinlock_t obd_types_lock;
extern spinlock_t handle_lock;
extern int class_procfs_init(void);
extern int class_procfs_clean(void);

#ifdef __KERNEL__
static int __init init_obdclass(void)
#else
int init_obdclass(void)
#endif
{
        int i, err;
#ifdef __KERNEL__
        int lustre_register_fs(void);

        printk(KERN_INFO "Lustre: OBD class driver Build Version: "
               BUILD_VERSION", info@clusterfs.com\n");
#else
        CDEBUG(D_INFO, "Lustre: OBD class driver Build Version: "
               BUILD_VERSION", info@clusterfs.com\n");
#endif

        spin_lock_init(&obd_types_lock);
        spin_lock_init(&handle_lock);
        cfs_waitq_init(&obd_race_waitq);

        err = obd_init_checks();
        if (err == -EOVERFLOW)
                return err;

        class_init_uuidlist();
        err = class_handle_init();
        if (err)
                return err;

        spin_lock_init(&obd_dev_lock);
        INIT_LIST_HEAD(&obd_types);

        err = cfs_psdev_register(&obd_psdev);
        if (err) {
                CERROR("cannot register %d err %d\n", OBD_MINOR, err);
                return err;
        }

        /* This struct is already zerod for us (static global) */
        for (i = 0; i < class_devno_max(); i++)
                obd_devs[i] = NULL;

        err = obd_init_caches();
        if (err)
                return err;
#ifdef __KERNEL__
        err = class_procfs_init();
        lustre_register_fs();
#endif

        return err;
}

/* liblustre doesn't call cleanup_obdclass, apparently.  we carry on in this
 * ifdef to the end of the file to cover module and versioning goo.*/
#ifdef __KERNEL__
static void cleanup_obdclass(void)
{
        int i;
        int lustre_unregister_fs(void);
        ENTRY;

        lustre_unregister_fs();

        cfs_psdev_deregister(&obd_psdev);
        for (i = 0; i < class_devno_max(); i++) {
                struct obd_device *obd = class_num2obd(i);
                if (obd && obd->obd_set_up &&
                    OBT(obd) && OBP(obd, detach)) {
                        /* XXX should this call generic detach otherwise? */
                        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);
                        OBP(obd, detach)(obd);
                }
        }

        obd_cleanup_caches();
        obd_sysctl_clean();

        class_procfs_clean();

        class_handle_cleanup();
        class_exit_uuidlist();
        EXIT;
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Class Driver Build Version: " BUILD_VERSION);
MODULE_LICENSE("GPL");

cfs_module(obdclass, "1.0.0", init_obdclass, cleanup_obdclass);
#endif
