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
#ifdef __KERNEL__
#include <linux/config.h> /* for CONFIG_PROC_FS */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/lp.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <asm/io.h>
#include <asm/ioctls.h>
#include <asm/system.h>
#include <asm/poll.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/smp_lock.h>
#include <linux/seq_file.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_debug.h>
#include <linux/lprocfs_status.h>
#ifdef __KERNEL__
#include <linux/lustre_build_version.h>
#include <linux/lustre_version.h>
#endif
#include <libcfs/list.h>
#include "llog_internal.h"

#ifndef __KERNEL__
/* liblustre workaround */
atomic_t libcfs_kmemory = {0};
#endif

struct obd_device obd_dev[MAX_OBD_DEVICES];
struct list_head obd_types;
spinlock_t obd_dev_lock;
#ifndef __KERNEL__
atomic_t obd_memory;
int obd_memmax;
#endif

int proc_version;

/* The following are visible and mutable through /proc/sys/lustre/. */
unsigned int obd_fail_loc;
unsigned int obd_dump_on_timeout;
unsigned int obd_timeout = 100; /* seconds */
unsigned int ldlm_timeout = 20; /* seconds */
unsigned int obd_health_check_timeout = 120; /* seconds */
char obd_lustre_upcall[128] = "DEFAULT"; /* or NONE or /full/path/to/upcall  */
unsigned int obd_sync_filter; /* = 0, don't sync by default */

DECLARE_WAIT_QUEUE_HEAD(obd_race_waitq);

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
static int obd_class_open(struct inode * inode, struct file * file)
{
        ENTRY;

        PORTAL_MODULE_USE;
        RETURN(0);
}

/*  closing /dev/obd */
static int obd_class_release(struct inode * inode, struct file * file)
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

int class_resolve_dev_name(uint32_t len, char *name)
{
        int rc;
        int dev;

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

#ifdef __KERNEL__
        if (current->fsuid != 0)
                RETURN(err = -EACCES);
#endif

        if ((cmd & 0xffffff00) == ((int)'T') << 8) /* ignore all tty ioctls */
                RETURN(err = -ENOTTY);

        /* only for debugging */
        if (cmd == LIBCFS_IOC_DEBUG_MASK) {
                debug_data = (struct libcfs_debug_ioctl_data*)arg;
                libcfs_subsystem_debug = debug_data->subs;
                libcfs_debug = debug_data->debug;
                return 0;
        }

        CDEBUG(D_IOCTL, "cmd = %x, obd = %p\n", cmd, obd);
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

                err = lustre_cfg_sanity_check(data->ioc_pbuf1,
                                              data->ioc_plen1);
                if (err)
                        GOTO(out, err);

                OBD_ALLOC(lcfg, data->ioc_plen1);
                err = copy_from_user(lcfg, data->ioc_pbuf1, data->ioc_plen1);
                if (!err)
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

                err = copy_to_user((void *)arg, data, len);
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

                err = copy_to_user((void *)arg, data, sizeof(*data));
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
                err = copy_to_user((void *)arg, data, sizeof(*data));
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }


        case OBD_IOC_CLOSE_UUID: {
                CDEBUG(D_IOCTL, "closing all connections to uuid %s (NOOP)\n",
                       data->ioc_inlbuf1);
                GOTO(out, err = 0);
        }

        }

        if (data->ioc_dev >= MAX_OBD_DEVICES) {
                CERROR("OBD ioctl: No device\n");
                GOTO(out, err = -EINVAL);
        }
        obd = &obd_dev[data->ioc_dev];
        if (!(obd && obd->obd_set_up) || obd->obd_stopping) {
                CERROR("OBD ioctl: device not setup %d \n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }

        switch(cmd) {
        case OBD_IOC_NO_TRANSNO: {
                if (!obd->obd_attached) {
                        CERROR("Device %d not attached\n", obd->obd_minor);
                        GOTO(out, err = -ENODEV);
                }
                CDEBUG(D_IOCTL,
                       "disabling committed-transno notifications on %d\n",
                       obd->obd_minor);
                obd->obd_no_transno = 1;
                GOTO(out, err = 0);
        }

        default: {
                err = obd_iocontrol(cmd, obd->obd_self_export, len, data, NULL);
                if (err)
                        GOTO(out, err);

                err = copy_to_user((void *)arg, data, len);
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
static int obd_class_ioctl(struct inode *inode, struct file *filp,
                           unsigned int cmd, unsigned long arg)
{
        return class_handle_ioctl(cmd, arg);
}

/* declare character device */
static struct file_operations obd_psdev_fops = {
        .owner   = THIS_MODULE,
        .ioctl   = obd_class_ioctl,     /* ioctl */
        .open    = obd_class_open,      /* open */
        .release = obd_class_release,   /* release */
};

/* modules setup */
static struct miscdevice obd_psdev = {
        .minor = OBD_MINOR,
        .name  = "obd",
        .fops  = &obd_psdev_fops,
};
#else
void *obd_psdev = NULL;
#endif

EXPORT_SYMBOL(obd_dev);
EXPORT_SYMBOL(obd_fail_loc);
EXPORT_SYMBOL(obd_print_fail_loc);
EXPORT_SYMBOL(obd_race_waitq);
EXPORT_SYMBOL(obd_dump_on_timeout);
EXPORT_SYMBOL(obd_timeout);
EXPORT_SYMBOL(ldlm_timeout);
EXPORT_SYMBOL(obd_health_check_timeout);
EXPORT_SYMBOL(obd_lustre_upcall);
EXPORT_SYMBOL(obd_sync_filter);
EXPORT_SYMBOL(ptlrpc_put_connection_superhack);
EXPORT_SYMBOL(ptlrpc_abort_inflight_superhack);

struct proc_dir_entry *proc_lustre_root;
EXPORT_SYMBOL(proc_lustre_root);

EXPORT_SYMBOL(class_register_type);
EXPORT_SYMBOL(class_unregister_type);
EXPORT_SYMBOL(class_search_type);
EXPORT_SYMBOL(class_get_type);
EXPORT_SYMBOL(class_put_type);
EXPORT_SYMBOL(class_name2dev);
EXPORT_SYMBOL(class_name2obd);
EXPORT_SYMBOL(class_uuid2dev);
EXPORT_SYMBOL(class_uuid2obd);
EXPORT_SYMBOL(class_obd_list);
EXPORT_SYMBOL(class_find_client_obd);
EXPORT_SYMBOL(class_find_client_notype);
EXPORT_SYMBOL(class_devices_in_group);
EXPORT_SYMBOL(class_conn2export);
EXPORT_SYMBOL(class_exp2obd);
EXPORT_SYMBOL(class_conn2obd);
EXPORT_SYMBOL(class_exp2cliimp);
EXPORT_SYMBOL(class_conn2cliimp);
EXPORT_SYMBOL(class_disconnect);

/* uuid.c */
EXPORT_SYMBOL(class_uuid_unparse);
EXPORT_SYMBOL(lustre_uuid_to_peer);

EXPORT_SYMBOL(class_handle_hash);
EXPORT_SYMBOL(class_handle_unhash);
EXPORT_SYMBOL(class_handle2object);

/* config.c */
EXPORT_SYMBOL(class_decref);
EXPORT_SYMBOL(class_get_profile);
EXPORT_SYMBOL(class_del_profile);
EXPORT_SYMBOL(class_process_config);
EXPORT_SYMBOL(class_config_parse_llog);
EXPORT_SYMBOL(class_config_dump_llog);
EXPORT_SYMBOL(class_attach);
EXPORT_SYMBOL(class_setup);
EXPORT_SYMBOL(class_cleanup);
EXPORT_SYMBOL(class_detach);
EXPORT_SYMBOL(class_manual_cleanup);

#ifdef LPROCFS
int obd_proc_read_version(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        *eof = 1;
        return snprintf(page, count, "%s\n", BUILD_VERSION);
}

int obd_proc_read_kernel_version(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        *eof = 1;
        return snprintf(page, count, "%u\n", LUSTRE_KERNEL_VERSION);
}

int obd_proc_read_pinger(char *page, char **start, off_t off, int count,
                         int *eof, void *data)
{
        *eof = 1;
        return snprintf(page, count, "%s\n",
#ifdef ENABLE_PINGER
                        "on"
#else
                        "off"
#endif
                       );
}

static int obd_proc_read_health(char *page, char **start, off_t off,
                                int count, int *eof, void *data)
{
        int rc = 0, i;
        *eof = 1;

        if (libcfs_catastrophe)
                rc += snprintf(page + rc, count - rc, "LBUG\n");

        spin_lock(&obd_dev_lock);
        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obd;

                obd = &obd_dev[i];
                if (obd->obd_type == NULL)
                        continue;

                atomic_inc(&obd->obd_refcount);
                spin_unlock(&obd_dev_lock);

                if (obd_health_check(obd)) {
                        rc += snprintf(page + rc, count - rc,
                                       "device %s reported unhealthy\n",
                                       obd->obd_name);
                }
                class_decref(obd);
                spin_lock(&obd_dev_lock);
        }
        spin_unlock(&obd_dev_lock);

        if (rc == 0)
                return snprintf(page, count, "healthy\n");

        rc += snprintf(page + rc, count - rc, "NOT HEALTHY\n");
        return rc;
}

static int obd_proc_rd_health_timeout(char *page, char **start, off_t off,
                                      int count, int *eof, void *data)
{
        *eof = 1;
        return snprintf(page, count, "%d\n", obd_health_check_timeout);
}

static int obd_proc_wr_health_timeout(struct file *file, const char *buffer,
                                      unsigned long count, void *data)
{
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        obd_health_check_timeout = val;

        return count;
}

/* Root for /proc/fs/lustre */
struct lprocfs_vars lprocfs_base[] = {
        { "version", obd_proc_read_version, NULL, NULL },
        { "kernel_version", obd_proc_read_kernel_version, NULL, NULL },
        { "pinger", obd_proc_read_pinger, NULL, NULL },
        { "health_check", obd_proc_read_health, NULL, NULL },
        { "health_check_timeout", obd_proc_rd_health_timeout,
          obd_proc_wr_health_timeout, NULL },        
        { 0 }
};
#else
#define lprocfs_base NULL
#endif /* LPROCFS */

#ifdef __KERNEL__
static void *obd_device_list_seq_start(struct seq_file *p, loff_t*pos)
{
        if (*pos >= MAX_OBD_DEVICES)
                return NULL;
        return &obd_dev[*pos];
}

static void obd_device_list_seq_stop(struct seq_file *p, void *v)
{
}

static void *obd_device_list_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        ++*pos;
        if (*pos >= MAX_OBD_DEVICES)
                return NULL;
        return &obd_dev[*pos];
}

static int obd_device_list_seq_show(struct seq_file *p, void *v)
{
        struct obd_device *obd = (struct obd_device *)v;
        int index = obd - &obd_dev[0];
        char *status;

        if (!obd->obd_type)
                return 0;
        if (obd->obd_stopping)
                status = "ST";
        else if (obd->obd_set_up)
                status = "UP";
        else if (obd->obd_attached)
                status = "AT";
        else
                status = "--";

        return seq_printf(p, "%3d %s %s %s %s %d\n",
                          (int)index, status, obd->obd_type->typ_name,
                          obd->obd_name, obd->obd_uuid.uuid,
                          atomic_read(&obd->obd_refcount));
}

struct seq_operations obd_device_list_sops = {
        .start = obd_device_list_seq_start,
        .stop = obd_device_list_seq_stop,
        .next = obd_device_list_seq_next,
        .show = obd_device_list_seq_show,
};

static int obd_device_list_open(struct inode *inode, struct file *file)
{
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file *seq;
        int rc = seq_open(file, &obd_device_list_sops);

        if (rc)
                return rc;

        seq = file->private_data;
        seq->private = dp->data;

        return 0;
}

struct file_operations obd_device_list_fops = {
        .owner   = THIS_MODULE,
        .open    = obd_device_list_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = seq_release,
};
#endif

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
        if ((u64val & ~PAGE_MASK) >= PAGE_SIZE) {
                CWARN("mask failed: u64val "LPU64" >= %lu\n", u64val,PAGE_SIZE);
                ret = -EINVAL;
        }

        return ret;
}
#else
#define obd_init_checks() do {} while(0)
#endif

#ifdef __KERNEL__
static int __init init_obdclass(void)
#else
int init_obdclass(void)
#endif
{
        struct obd_device *obd;
#ifdef __KERNEL__
        struct proc_dir_entry *entry;
        int lustre_register_fs(void);
#endif
        int err;
        int i;

#ifdef __KERNEL__
        printk(KERN_INFO "Lustre: OBD class driver Build Version: "
               BUILD_VERSION", info@clusterfs.com\n");
#else
        CDEBUG(D_INFO, "Lustre: OBD class driver Build Version: "
               BUILD_VERSION", info@clusterfs.com\n");
#endif

        err = obd_init_checks();
        if (err == -EOVERFLOW)
                return err;

        class_init_uuidlist();
        err = class_handle_init();
        if (err)
                return err;

        spin_lock_init(&obd_dev_lock);
        INIT_LIST_HEAD(&obd_types);

        err = misc_register(&obd_psdev);
        if (err) {
                CERROR("cannot register %d err %d\n", OBD_MINOR, err);
                return err;
        }

        /* This struct is already zerod for us (static global) */
        for (i = 0, obd = obd_dev; i < MAX_OBD_DEVICES; i++, obd++)
                obd->obd_minor = i;

        err = obd_init_caches();
        if (err)
                return err;

#ifdef __KERNEL__
        obd_sysctl_init();

        proc_lustre_root = proc_mkdir("lustre", proc_root_fs);
        if (!proc_lustre_root) {
                printk(KERN_ERR
                       "LustreError: error registering /proc/fs/lustre\n");
                RETURN(-ENOMEM);
        }
        proc_version = lprocfs_add_vars(proc_lustre_root, lprocfs_base, NULL);
        entry = create_proc_entry("devices", 0444, proc_lustre_root);
        if (entry == NULL) {
                CERROR("error registering /proc/fs/lustre/devices\n");
                lprocfs_remove(proc_lustre_root);
                RETURN(-ENOMEM);
        }
        entry->proc_fops = &obd_device_list_fops;

        lustre_register_fs();
#endif
        return 0;
}

/* liblustre doesn't call cleanup_obdclass, apparently.  we carry on in this
 * ifdef to the end of the file to cover module and versioning goo.*/
#ifdef __KERNEL__

static void cleanup_obdclass(void)
{
        int i;
        int leaked;
        int lustre_unregister_fs(void);
        ENTRY;

        lustre_unregister_fs();

        misc_deregister(&obd_psdev);
        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obd = &obd_dev[i];
                if (obd->obd_type && obd->obd_set_up &&
                    OBT(obd) && OBP(obd, detach)) {
                        /* XXX should this call generic detach otherwise? */
                        OBP(obd, detach)(obd);
                }
        }

        obd_cleanup_caches();
        obd_sysctl_clean();

        if (proc_lustre_root) {
                lprocfs_remove(proc_lustre_root);
                proc_lustre_root = NULL;
        }

        class_handle_cleanup();
        class_exit_uuidlist();

        leaked = atomic_read(&obd_memory);
        CDEBUG(leaked ? D_ERROR : D_INFO,
               "obd mem max: %d leaked: %d\n", obd_memmax, leaked);

        EXIT;
}

/* Check that we're building against the appropriate version of the Lustre
 * kernel patch */
#include <linux/lustre_version.h>
#define LUSTRE_MIN_VERSION 37
#define LUSTRE_MAX_VERSION 47
#if (LUSTRE_KERNEL_VERSION < LUSTRE_MIN_VERSION)
# error Cannot continue: Your Lustre kernel patch is older than the sources
#elif (LUSTRE_KERNEL_VERSION > LUSTRE_MAX_VERSION)
# error Cannot continue: Your Lustre sources are older than the kernel patch
#endif

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Class Driver Build Version: " BUILD_VERSION);
MODULE_LICENSE("GPL");

module_init(init_obdclass);
module_exit(cleanup_obdclass);
#endif
