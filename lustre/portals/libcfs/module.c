/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
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

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_PORTALS

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <asm/segment.h>
#include <linux/miscdevice.h>

#include <portals/lib-p30.h>
#include <portals/p30.h>
#include <linux/kp30.h>
#include <linux/portals_compat25.h>

#define PORTAL_MINOR 240

struct nal_cmd_handler {
        int                  nch_number;
        nal_cmd_handler_fn  *nch_handler;
        void                *nch_private;
};

static struct nal_cmd_handler nal_cmd[16];
static DECLARE_MUTEX(nal_cmd_sem);

#ifdef PORTAL_DEBUG
void kportal_assertion_failed(char *expr, char *file, const char *func,
                              const int line)
{
        portals_debug_msg(0, D_EMERG, file, func, line, CDEBUG_STACK,
                          "ASSERTION(%s) failed\n", expr);
        LBUG_WITH_LOC(file, func, line);
}
#endif

void
kportal_daemonize (char *str) 
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,63))
        daemonize(str);
#else
        daemonize();
        snprintf (current->comm, sizeof (current->comm), "%s", str);
#endif
}

void
kportal_memhog_free (struct portals_device_userstate *pdu)
{
        struct page **level0p = &pdu->pdu_memhog_root_page;
        struct page **level1p;
        struct page **level2p;
        int           count1;
        int           count2;
        
        if (*level0p != NULL) {

                level1p = (struct page **)page_address(*level0p);
                count1 = 0;
                
                while (count1 < PAGE_SIZE/sizeof(struct page *) &&
                       *level1p != NULL) {

                        level2p = (struct page **)page_address(*level1p);
                        count2 = 0;
                        
                        while (count2 < PAGE_SIZE/sizeof(struct page *) &&
                               *level2p != NULL) {
                                
                                __free_page(*level2p);
                                pdu->pdu_memhog_pages--;
                                level2p++;
                                count2++;
                        }
                        
                        __free_page(*level1p);
                        pdu->pdu_memhog_pages--;
                        level1p++;
                        count1++;
                }
                
                __free_page(*level0p);
                pdu->pdu_memhog_pages--;

                *level0p = NULL;
        }
        
        LASSERT (pdu->pdu_memhog_pages == 0);
}

int
kportal_memhog_alloc (struct portals_device_userstate *pdu, int npages, int flags)
{
        struct page **level0p;
        struct page **level1p;
        struct page **level2p;
        int           count1;
        int           count2;
        
        LASSERT (pdu->pdu_memhog_pages == 0);
        LASSERT (pdu->pdu_memhog_root_page == NULL);

        if (npages < 0)
                return -EINVAL;

        if (npages == 0)
                return 0;

        level0p = &pdu->pdu_memhog_root_page;
        *level0p = alloc_page(flags);
        if (*level0p == NULL)
                return -ENOMEM;
        pdu->pdu_memhog_pages++;

        level1p = (struct page **)page_address(*level0p);
        count1 = 0;
        memset(level1p, 0, PAGE_SIZE);
        
        while (pdu->pdu_memhog_pages < npages &&
               count1 < PAGE_SIZE/sizeof(struct page *)) {

                if (signal_pending(current))
                        return (-EINTR);
                
                *level1p = alloc_page(flags);
                if (*level1p == NULL)
                        return -ENOMEM;
                pdu->pdu_memhog_pages++;

                level2p = (struct page **)page_address(*level1p);
                count2 = 0;
                memset(level2p, 0, PAGE_SIZE);
                
                while (pdu->pdu_memhog_pages < npages &&
                       count2 < PAGE_SIZE/sizeof(struct page *)) {
                        
                        if (signal_pending(current))
                                return (-EINTR);

                        *level2p = alloc_page(flags);
                        if (*level2p == NULL)
                                return (-ENOMEM);
                        pdu->pdu_memhog_pages++;
                        
                        level2p++;
                        count2++;
                }
                
                level1p++;
                count1++;
        }

        return 0;
}

void
kportal_blockallsigs ()
{
        unsigned long  flags;

        SIGNAL_MASK_LOCK(current, flags);
        sigfillset(&current->blocked);
        RECALC_SIGPENDING;
        SIGNAL_MASK_UNLOCK(current, flags);
}

/* called when opening /dev/device */
static int libcfs_psdev_open(struct inode * inode, struct file * file)
{
        struct portals_device_userstate *pdu;
        ENTRY;
        
        if (!inode)
                RETURN(-EINVAL);

        PORTAL_MODULE_USE;

        PORTAL_ALLOC(pdu, sizeof(*pdu));
        if (pdu != NULL) {
                pdu->pdu_memhog_pages = 0;
                pdu->pdu_memhog_root_page = NULL;
        }
        file->private_data = pdu;
        
        RETURN(0);
}

/* called when closing /dev/device */
static int libcfs_psdev_release(struct inode * inode, struct file * file)
{
        struct portals_device_userstate *pdu;
        ENTRY;

        if (!inode)
                RETURN(-EINVAL);

        pdu = file->private_data;
        if (pdu != NULL) {
                kportal_memhog_free(pdu);
                PORTAL_FREE(pdu, sizeof(*pdu));
        }
        
        PORTAL_MODULE_UNUSE;
        RETURN(0);
}

static inline void freedata(void *data, int len)
{
        PORTAL_FREE(data, len);
}

struct nal_cmd_handler *
libcfs_find_nal_cmd_handler(int nal)
{
        int    i;

        for (i = 0; i < sizeof(nal_cmd)/sizeof(nal_cmd[0]); i++)
                if (nal_cmd[i].nch_handler != NULL &&
                    nal_cmd[i].nch_number == nal)
                        return (&nal_cmd[i]);

        return (NULL);
}

int
libcfs_nal_cmd_register(int nal, nal_cmd_handler_fn *handler, void *private)
{
        struct nal_cmd_handler *cmd;
        int                     i;
        int                     rc;

        CDEBUG(D_IOCTL, "Register NAL %d, handler: %p\n", nal, handler);

        down(&nal_cmd_sem);

        if (libcfs_find_nal_cmd_handler(nal) != NULL) {
                up (&nal_cmd_sem);
                return (-EBUSY);
        }

        cmd = NULL;
        for (i = 0; i < sizeof(nal_cmd)/sizeof(nal_cmd[0]); i++)
                if (nal_cmd[i].nch_handler == NULL) {
                        cmd = &nal_cmd[i];
                        break;
                }
        
        if (cmd == NULL) {
                rc = -EBUSY;
        } else {
                rc = 0;
                cmd->nch_number = nal;
                cmd->nch_handler = handler;
                cmd->nch_private = private;
        }

        up(&nal_cmd_sem);

        return rc;
}
EXPORT_SYMBOL(libcfs_nal_cmd_register);

void
libcfs_nal_cmd_unregister(int nal)
{
        struct nal_cmd_handler *cmd;

        CDEBUG(D_IOCTL, "Unregister NAL %d\n", nal);

        down(&nal_cmd_sem);
        cmd = libcfs_find_nal_cmd_handler(nal);
        LASSERT (cmd != NULL);
        cmd->nch_handler = NULL;
        cmd->nch_private = NULL;
        up(&nal_cmd_sem);
}
EXPORT_SYMBOL(libcfs_nal_cmd_unregister);

int
libcfs_nal_cmd(struct portals_cfg *pcfg)
{
        struct nal_cmd_handler *cmd;
        __u32 nal = pcfg->pcfg_nal;
        int   rc = -EINVAL;
        ENTRY;

        down(&nal_cmd_sem);
        cmd = libcfs_find_nal_cmd_handler(nal);
        if (cmd != NULL) {
                CDEBUG(D_IOCTL, "calling handler nal: %d, cmd: %d\n", nal, 
                       pcfg->pcfg_command);
                rc = cmd->nch_handler(pcfg, cmd->nch_private);
        } else {
                CERROR("invalid nal: %d, cmd: %d\n", nal, pcfg->pcfg_command);
        }
        up(&nal_cmd_sem);

        RETURN(rc);
}
EXPORT_SYMBOL(libcfs_nal_cmd);

static DECLARE_RWSEM(ioctl_list_sem);
static LIST_HEAD(ioctl_list);

int libcfs_register_ioctl(struct libcfs_ioctl_handler *hand)
{
        int rc = 0;
        down_read(&ioctl_list_sem);
        if (!list_empty(&hand->item))
                rc = -EBUSY;
        up_read(&ioctl_list_sem);

        if (rc == 0) {
                down_write(&ioctl_list_sem);
                list_add_tail(&hand->item, &ioctl_list);
                up_write(&ioctl_list_sem);
        }
        RETURN(0);
}
EXPORT_SYMBOL(libcfs_register_ioctl);

int libcfs_deregister_ioctl(struct libcfs_ioctl_handler *hand)
{
        int rc = 0;
        down_read(&ioctl_list_sem);
        if (list_empty(&hand->item))
                rc = -ENOENT;
        up_read(&ioctl_list_sem);

        if (rc == 0) {
                down_write(&ioctl_list_sem);
                list_del_init(&hand->item);
                up_write(&ioctl_list_sem);
        }
        RETURN(0);
}
EXPORT_SYMBOL(libcfs_deregister_ioctl);

static int libcfs_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
        int err = -EINVAL;
        char buf[1024];
        struct portal_ioctl_data *data;
        ENTRY;

        if (current->fsuid != 0)
                RETURN(err = -EACCES);

        if ( _IOC_TYPE(cmd) != IOC_PORTAL_TYPE ||
             _IOC_NR(cmd) < IOC_PORTAL_MIN_NR  ||
             _IOC_NR(cmd) > IOC_PORTAL_MAX_NR ) {
                CDEBUG(D_IOCTL, "invalid ioctl ( type %d, nr %d, size %d )\n",
                                _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
                RETURN(-EINVAL);
        }

        if (portal_ioctl_getdata(buf, buf + 800, (void *)arg)) {
                CERROR("PORTALS ioctl: data error\n");
                RETURN(-EINVAL);
        }

        data = (struct portal_ioctl_data *)buf;

        switch (cmd) {
        case IOC_PORTAL_CLEAR_DEBUG:
                portals_debug_clear_buffer();
                RETURN(0);
        case IOC_PORTAL_PANIC:
                if (!capable (CAP_SYS_BOOT))
                        RETURN (-EPERM);
                panic("debugctl-invoked panic");
                RETURN(0);
        case IOC_PORTAL_MARK_DEBUG:
                if (data->ioc_inlbuf1 == NULL ||
                    data->ioc_inlbuf1[data->ioc_inllen1 - 1] != '\0')
                        RETURN(-EINVAL);
                portals_debug_mark_buffer(data->ioc_inlbuf1);
                RETURN(0);
#if LWT_SUPPORT
        case IOC_PORTAL_LWT_CONTROL:
                err = lwt_control (data->ioc_flags, data->ioc_misc);
                break;

        case IOC_PORTAL_LWT_SNAPSHOT: {
                cycles_t   now;
                int        ncpu;
                int        total_size;

                err = lwt_snapshot (&now, &ncpu, &total_size,
                                    data->ioc_pbuf1, data->ioc_plen1);
                data->ioc_nid = now;
                data->ioc_count = ncpu;
                data->ioc_misc = total_size;

                /* Hedge against broken user/kernel typedefs (e.g. cycles_t) */
                data->ioc_nid = sizeof(lwt_event_t);
                data->ioc_nid2 = offsetof(lwt_event_t, lwte_where);

                if (err == 0 &&
                    copy_to_user((char *)arg, data, sizeof (*data)))
                        err = -EFAULT;
                break;
        }

        case IOC_PORTAL_LWT_LOOKUP_STRING:
                err = lwt_lookup_string (&data->ioc_count, data->ioc_pbuf1,
                                         data->ioc_pbuf2, data->ioc_plen2);
                if (err == 0 &&
                    copy_to_user((char *)arg, data, sizeof (*data)))
                        err = -EFAULT;
                break;
#endif
        case IOC_PORTAL_NAL_CMD: {
                struct portals_cfg pcfg;

                if (data->ioc_plen1 != sizeof(pcfg)) {
                        CERROR("Bad ioc_plen1 %d (wanted %d)\n",
                               data->ioc_plen1, sizeof(pcfg));
                        err = -EINVAL;
                        break;
                }

                if (copy_from_user(&pcfg, (void *)data->ioc_pbuf1,
                                   sizeof(pcfg))) {
                        err = -EFAULT;
                        break;
                }

                CDEBUG (D_IOCTL, "nal command nal %d cmd %d\n", pcfg.pcfg_nal,
                        pcfg.pcfg_command);
                err = libcfs_nal_cmd(&pcfg);

                if (err == 0 &&
                    copy_to_user((char *)data->ioc_pbuf1, &pcfg,
                                 sizeof (pcfg)))
                        err = -EFAULT;
                break;
        }

        case IOC_PORTAL_MEMHOG:
                if (!capable (CAP_SYS_ADMIN))
                        err = -EPERM;
                else if (file->private_data == NULL) {
                        err = -EINVAL;
                } else {
                        kportal_memhog_free(file->private_data);
                        err = kportal_memhog_alloc(file->private_data,
                                                   data->ioc_count,
                                                   data->ioc_flags);
                        if (err != 0)
                                kportal_memhog_free(file->private_data);
                }
                break;

        default: {
                struct libcfs_ioctl_handler *hand;
                err = -EINVAL;
                down_read(&ioctl_list_sem);
                list_for_each_entry(hand, &ioctl_list, item) {
                        err = hand->handle_ioctl(data, cmd, arg);
                        if (err != -EINVAL)
                                break;
                }
                up_read(&ioctl_list_sem);
                } break;
        }

        RETURN(err);
}


static struct file_operations libcfs_fops = {
        ioctl:   libcfs_ioctl,
        open:    libcfs_psdev_open,
        release: libcfs_psdev_release
};


static struct miscdevice libcfs_dev = {
        PORTAL_MINOR,
        "portals",
        &libcfs_fops
};

extern int insert_proc(void);
extern void remove_proc(void);
MODULE_AUTHOR("Peter J. Braam <braam@clusterfs.com>");
MODULE_DESCRIPTION("Portals v3.1");
MODULE_LICENSE("GPL");

static int init_libcfs_module(void)
{
        int rc;

        rc = portals_debug_init(5 * 1024 * 1024);
        if (rc < 0) {
                printk(KERN_ERR "LustreError: portals_debug_init: %d\n", rc);
                return (rc);
        }

#if LWT_SUPPORT
        rc = lwt_init();
        if (rc != 0) {
                CERROR("lwt_init: error %d\n", rc);
                goto cleanup_debug;
        }
#endif
        rc = misc_register(&libcfs_dev);
        if (rc) {
                CERROR("misc_register: error %d\n", rc);
                goto cleanup_lwt;
        }

        rc = insert_proc();
        if (rc) {
                CERROR("insert_proc: error %d\n", rc);
                goto cleanup_deregister;
        }

        CDEBUG (D_OTHER, "portals setup OK\n");
        return (0);

 cleanup_deregister:
        misc_deregister(&libcfs_dev);
 cleanup_lwt:
#if LWT_SUPPORT
        lwt_fini();
 cleanup_debug:
#endif
        portals_debug_cleanup();
        return rc;
}

static void exit_libcfs_module(void)
{
        int rc;

        remove_proc();

        CDEBUG(D_MALLOC, "before Portals cleanup: kmem %d\n",
               atomic_read(&portal_kmemory));

        rc = misc_deregister(&libcfs_dev);
        if (rc)
                CERROR("misc_deregister error %d\n", rc);

#if LWT_SUPPORT
        lwt_fini();
#endif

        if (atomic_read(&portal_kmemory) != 0)
                CERROR("Portals memory leaked: %d bytes\n",
                       atomic_read(&portal_kmemory));

        rc = portals_debug_cleanup();
        if (rc)
                printk(KERN_ERR "LustreError: portals_debug_cleanup: %d\n", rc);
}

EXPORT_SYMBOL(kportal_daemonize);
EXPORT_SYMBOL(kportal_blockallsigs);
EXPORT_SYMBOL(kportal_assertion_failed);

module_init(init_libcfs_module);
module_exit(exit_libcfs_module);
