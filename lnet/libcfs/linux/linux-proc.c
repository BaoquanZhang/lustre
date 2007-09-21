/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *   Author: Zach Brown <zab@zabbo.net>
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <linux/uio.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <asm/segment.h>

#include <linux/proc_fs.h>
#include <linux/sysctl.h>

# define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/kp30.h>
#include <asm/div64.h>
#include "tracefile.h"

static cfs_sysctl_table_header_t *lnet_table_header = NULL;
extern char lnet_upcall[1024];

#define PSDEV_LNET  (0x100)
enum {
        PSDEV_DEBUG = 1,          /* control debugging */
        PSDEV_SUBSYSTEM_DEBUG,    /* control debugging */
        PSDEV_PRINTK,             /* force all messages to console */
        PSDEV_CONSOLE_RATELIMIT,  /* ratelimit console messages */
        PSDEV_DEBUG_PATH,         /* crashdump log location */
        PSDEV_DEBUG_DUMP_PATH,    /* crashdump tracelog location */
        PSDEV_LNET_UPCALL,        /* User mode upcall script  */
        PSDEV_LNET_MEMUSED,       /* bytes currently PORTAL_ALLOCated */
        PSDEV_LNET_CATASTROPHE,   /* if we have LBUGged or panic'd */
        PSDEV_LNET_PANIC_ON_LBUG, /* flag to panic on LBUG */
        PSDEV_LNET_DUMP_KERNEL,   /* snapshot kernel debug buffer to file */
        PSDEV_LNET_DAEMON_FILE,   /* spool kernel debug buffer to file */
        PSDEV_LNET_DEBUG_MB,      /* size of debug buffer */
};

static int 
proc_call_handler(void *data, int write, 
                  loff_t *ppos, void *buffer, size_t *lenp, 
                  int (*handler)(void *data, int write,
                                 loff_t pos, void *buffer, int len))
{
        int rc = handler(data, write, *ppos, buffer, *lenp);

        if (rc < 0)
                return rc;

        if (write) {
                *ppos += *lenp;
        } else {
                *lenp = rc;
                *ppos += rc;
        }
        return 0;
}

#define DECLARE_PROC_HANDLER(name)                      \
static int                                              \
LL_PROC_PROTO(name)                                     \
{                                                       \
        DECLARE_LL_PROC_PPOS;                           \
                                                        \
        return proc_call_handler(table->data, write,    \
                                 ppos, buffer, lenp,    \
                                 __##name);             \
}

static int __proc_dobitmasks(void *data, int write, 
                             loff_t pos, void *buffer, int nob)
{
        const int     tmpstrlen = 512;
        char         *tmpstr;
        int           rc;
        unsigned int *mask = data;
        int           is_subsys = (mask == &libcfs_subsystem_debug) ? 1 : 0;

        rc = trace_allocate_string_buffer(&tmpstr, tmpstrlen);
        if (rc < 0)
                return rc;
        
        if (!write) {
                libcfs_debug_mask2str(tmpstr, tmpstrlen, *mask, is_subsys);
                rc = strlen(tmpstr);

                if (pos >= rc) {
                        rc = 0;
                } else {
                        rc = trace_copyout_string(buffer, nob,
                                                  tmpstr + pos, "\n");
                }
        } else {
                rc = trace_copyin_string(tmpstr, tmpstrlen, buffer, nob);
                if (rc < 0)
                        return rc;
                
                rc = libcfs_debug_str2mask(mask, tmpstr, is_subsys);
        }

        trace_free_string_buffer(tmpstr, tmpstrlen);
        return rc;
}

DECLARE_PROC_HANDLER(proc_dobitmasks)

static int __proc_dump_kernel(void *data, int write,
                              loff_t pos, void *buffer, int nob)
{
        if (!write)
                return 0;
        
        return trace_dump_debug_buffer_usrstr(buffer, nob);
}

DECLARE_PROC_HANDLER(proc_dump_kernel)

static int __proc_daemon_file(void *data, int write,
                              loff_t pos, void *buffer, int nob)
{
        if (!write) {
                int len = strlen(tracefile);
                
                if (pos >= len)
                        return 0;
                
                return trace_copyout_string(buffer, nob, 
                                            tracefile + pos, "\n");
        }
        
        return trace_daemon_command_usrstr(buffer, nob);
}

DECLARE_PROC_HANDLER(proc_daemon_file)

static int __proc_debug_mb(void *data, int write,
                           loff_t pos, void *buffer, int nob)
{
        if (!write) {
                char tmpstr[32];
                int  len = snprintf(tmpstr, sizeof(tmpstr), "%d",
                                    trace_get_debug_mb());

                if (pos >= len)
                        return 0;
                
                return trace_copyout_string(buffer, nob, tmpstr + pos, "\n");
        }
        
        return trace_set_debug_mb_usrstr(buffer, nob);
}

DECLARE_PROC_HANDLER(proc_debug_mb)

static cfs_sysctl_table_t lnet_table[] = {
        /*
         * NB No .strategy entries have been provided since sysctl(8) prefers
         * to go via /proc for portability.
         */
        {
                .ctl_name = PSDEV_DEBUG,
                .procname = "debug",
                .data     = &libcfs_debug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks
        },
        {
                .ctl_name = PSDEV_SUBSYSTEM_DEBUG,
                .procname = "subsystem_debug",
                .data     = &libcfs_subsystem_debug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks
        },
        {
                .ctl_name = PSDEV_PRINTK,
                .procname = "printk",
                .data     = &libcfs_printk,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks
        },
        {
                .ctl_name = PSDEV_CONSOLE_RATELIMIT,
                .procname = "console_ratelimit",
                .data     = &libcfs_console_ratelimit,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },

        {
                .ctl_name = PSDEV_DEBUG_PATH,
                .procname = "debug_path",
                .data     = debug_file_path,
                .maxlen   = sizeof(debug_file_path),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
        },

        {
                .ctl_name = PSDEV_LNET_UPCALL,
                .procname = "upcall",
                .data     = lnet_upcall,
                .maxlen   = sizeof(lnet_upcall),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
        },
        {
                .ctl_name = PSDEV_LNET_MEMUSED,
                .procname = "memused",
                .data     = (int *)&libcfs_kmemory.counter,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = PSDEV_LNET_CATASTROPHE,
                .procname = "catastrophe",
                .data     = &libcfs_catastrophe,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = PSDEV_LNET_PANIC_ON_LBUG,
                .procname = "panic_on_lbug",
                .data     = &libcfs_panic_on_lbug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = PSDEV_LNET_DUMP_KERNEL,
                .procname = "dump_kernel",
                .mode     = 0200,
                .proc_handler = &proc_dump_kernel,
        },
        {
                .ctl_name = PSDEV_LNET_DAEMON_FILE,
                .procname = "daemon_file",
                .mode     = 0644,
                .proc_handler = &proc_daemon_file,
        },
        {
                .ctl_name = PSDEV_LNET_DEBUG_MB,
                .procname = "debug_mb",
                .mode     = 0644,
                .proc_handler = &proc_debug_mb,
        },
        {0}
};

static cfs_sysctl_table_t top_table[2] = {
        {
                .ctl_name = PSDEV_LNET,
                .procname = "lnet",
                .data     = NULL,
                .maxlen   = 0,
                .mode     = 0555,
                .child    = lnet_table
        },
        {0}
};

int insert_proc(void)
{
#ifdef CONFIG_SYSCTL
        if (lnet_table_header == NULL)
                lnet_table_header = cfs_register_sysctl_table(top_table, 0);
#endif
        return 0;
}

void remove_proc(void)
{
#ifdef CONFIG_SYSCTL
        if (lnet_table_header != NULL)
                cfs_unregister_sysctl_table(lnet_table_header);

        lnet_table_header = NULL;
#endif
}
