/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Hariharan Thantry <thantry@users.sourceforge.net>
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
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_CLASS

#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <obd_class.h>
#include <lprocfs_status.h>
#include <lustre_fsfilt.h>

#if defined(LPROCFS)

#define MAX_STRING_SIZE 128

/* for bug 10866, global variable */
DECLARE_RWSEM(_lprocfs_lock);
EXPORT_SYMBOL(_lprocfs_lock);

int lprocfs_seq_release(struct inode *inode, struct file *file)
{
        LPROCFS_EXIT();
        return seq_release(inode, file);
}
EXPORT_SYMBOL(lprocfs_seq_release);

struct proc_dir_entry *lprocfs_srch(struct proc_dir_entry *head,
                                    const char *name)
{
        struct proc_dir_entry *temp;

        if (head == NULL)
                return NULL;

        temp = head->subdir;
        while (temp != NULL) {
                if (strcmp(temp->name, name) == 0)
                        return temp;

                temp = temp->next;
        }
        return NULL;
}

/* lprocfs API calls */

/* Function that emulates snprintf but also has the side effect of advancing
   the page pointer for the next write into the buffer, incrementing the total
   length written to the buffer, and decrementing the size left in the
   buffer. */
static int lprocfs_obd_snprintf(char **page, int end, int *len,
                                const char *format, ...)
{
        va_list list;
        int n;

        if (*len >= end)
                return 0;

        va_start(list, format);
        n = vsnprintf(*page, end - *len, format, list);
        va_end(list);

        *page += n; *len += n;
        return n;
}

int lprocfs_add_simple(struct proc_dir_entry *root, char *name,
                       read_proc_t *read_proc, write_proc_t *write_proc,
                       void *data)
{
        struct proc_dir_entry *proc;
        mode_t mode = 0;
        
        if (root == NULL || name == NULL)
                return -EINVAL;
        if (read_proc)
                mode = 0444;
        if (write_proc)
                mode |= 0200;
        proc = create_proc_entry(name, mode, root);
        if (!proc) {
                CERROR("LprocFS: No memory to create /proc entry %s", name);
                return -ENOMEM;
        }
        proc->read_proc = read_proc;
        proc->write_proc = write_proc;
        proc->data = data;
        return 0;
}


static ssize_t lprocfs_fops_read(struct file *f, char __user *buf, size_t size, loff_t *ppos)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        char *page, *start = NULL;
        int rc = 0, eof = 1, count;

        if (*ppos >= PAGE_SIZE)
                return 0;

        page = (char *)__get_free_page(GFP_KERNEL);
        if (page == NULL)
                return -ENOMEM;

        LPROCFS_ENTRY();
        OBD_FAIL_TIMEOUT(OBD_FAIL_LPROC_REMOVE, 10);
        if (!dp->deleted && dp->read_proc)
                rc = dp->read_proc(page, &start, *ppos, PAGE_SIZE, 
                        &eof, dp->data);
        LPROCFS_EXIT();
        if (rc <= 0)
                goto out;

        /* for lustre proc read, the read count must be less than PAGE_SIZE */
        LASSERT(eof == 1);

        if (start == NULL) {
                rc -= *ppos;
                if (rc < 0)
                        rc = 0;
                if (rc == 0)
                        goto out;
                start = page + *ppos;
        } else if (start < page) {
                start = page;
        }

        count = (rc < size) ? rc : size;
        if (copy_to_user(buf, start, count)) {
                rc = -EFAULT;
                goto out;
        }
        *ppos += count;

out:
        free_page((unsigned long)page);
        return rc;
}

static ssize_t lprocfs_fops_write(struct file *f, const char __user *buf, size_t size, loff_t *ppos)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        int rc = 0;

        LPROCFS_ENTRY();
        if (!dp->deleted && dp->write_proc)
                rc = dp->write_proc(f, buf, size, dp->data);
        LPROCFS_EXIT();
        return rc;
}

static struct file_operations lprocfs_generic_fops = {
        .owner = THIS_MODULE,
        .read = lprocfs_fops_read,
        .write = lprocfs_fops_write,
};

int lprocfs_evict_client_open(struct inode *inode, struct file *f)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        struct obd_device *obd = dp->data;

        atomic_inc(&obd->obd_evict_inprogress);

        return 0;
}

int lprocfs_evict_client_release(struct inode *inode, struct file *f)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        struct obd_device *obd = dp->data;

        atomic_dec(&obd->obd_evict_inprogress);
        wake_up(&obd->obd_evict_inprogress_waitq);

        return 0;
}

struct file_operations lprocfs_evict_client_fops = {
        .owner = THIS_MODULE,
        .read = lprocfs_fops_read,
        .write = lprocfs_fops_write,
        .open = lprocfs_evict_client_open,
        .release = lprocfs_evict_client_release,
};
EXPORT_SYMBOL(lprocfs_evict_client_fops);

int lprocfs_add_vars(struct proc_dir_entry *root, struct lprocfs_vars *list,
                     void *data)
{
        if (root == NULL || list == NULL)
                return -EINVAL;

        while (list->name != NULL) {
                struct proc_dir_entry *cur_root, *proc;
                char *pathcopy, *cur, *next, pathbuf[64];
                int pathsize = strlen(list->name) + 1;

                proc = NULL;
                cur_root = root;

                /* need copy of path for strsep */
                if (strlen(list->name) > sizeof(pathbuf) - 1) {
                        OBD_ALLOC(pathcopy, pathsize);
                        if (pathcopy == NULL)
                                return -ENOMEM;
                } else {
                        pathcopy = pathbuf;
                }

                next = pathcopy;
                strcpy(pathcopy, list->name);

                while (cur_root != NULL && (cur = strsep(&next, "/"))) {
                        if (*cur =='\0') /* skip double/trailing "/" */
                                continue;

                        proc = lprocfs_srch(cur_root, cur);
                        CDEBUG(D_OTHER, "cur_root=%s, cur=%s, next=%s, (%s)\n",
                               cur_root->name, cur, next,
                               (proc ? "exists" : "new"));
                        if (next != NULL) {
                                cur_root = (proc ? proc :
                                            proc_mkdir(cur, cur_root));
                        } else if (proc == NULL) {
                                mode_t mode = 0;
                                if (list->read_fptr)
                                        mode = 0444;
                                if (list->write_fptr)
                                        mode |= 0200;
                                proc = create_proc_entry(cur, mode, cur_root);
                        }
                }

                if (pathcopy != pathbuf)
                        OBD_FREE(pathcopy, pathsize);

                if (cur_root == NULL || proc == NULL) {
                        CERROR("LprocFS: No memory to create /proc entry %s",
                               list->name);
                        return -ENOMEM;
                }

                if (list->fops)
                        proc->proc_fops = list->fops;
                else
                        proc->proc_fops = &lprocfs_generic_fops;
                proc->read_proc = list->read_fptr;
                proc->write_proc = list->write_fptr;
                proc->data = (list->data ? list->data : data);
                list++;
        }
        return 0;
}

void lprocfs_remove(struct proc_dir_entry **rooth)
{
        struct proc_dir_entry *root = *rooth;
        struct proc_dir_entry *temp = root;
        struct proc_dir_entry *rm_entry;
        struct proc_dir_entry *parent;

        if (!root) 
                return;
        *rooth = NULL;

        parent = root->parent;
        LASSERT(parent != NULL);
 
        while (1) {
                while (temp->subdir != NULL)
                        temp = temp->subdir;

                rm_entry = temp;
                temp = temp->parent;

                /* Memory corruption once caused this to fail, and
                   without this LASSERT we would loop here forever. */
                LASSERTF(strlen(rm_entry->name) == rm_entry->namelen,
                         "0x%p  %s/%s len %d\n", rm_entry, temp->name,
                         rm_entry->name, (int)strlen(rm_entry->name));

                /* Now, the rm_entry->deleted flags is protected 
                 * by _lprocfs_lock. */
                down_write(&_lprocfs_lock);
                rm_entry->data = NULL;
                remove_proc_entry(rm_entry->name, rm_entry->parent);
                up_write(&_lprocfs_lock);
                if (temp == parent)
                        break;
        }
}

struct proc_dir_entry *lprocfs_register(const char *name,
                                        struct proc_dir_entry *parent,
                                        struct lprocfs_vars *list, void *data)
{
        struct proc_dir_entry *newchild;

        newchild = lprocfs_srch(parent, name);
        if (newchild != NULL) {
                CERROR(" Lproc: Attempting to register %s more than once \n",
                       name);
                return ERR_PTR(-EALREADY);
        }

        newchild = proc_mkdir(name, parent);
        if (newchild != NULL && list != NULL) {
                int rc = lprocfs_add_vars(newchild, list, data);
                if (rc) {
                        lprocfs_remove(&newchild);
                        return ERR_PTR(rc);
                }
        }
        return newchild;
}

/* Generic callbacks */
int lprocfs_rd_uint(char *page, char **start, off_t off,
                    int count, int *eof, void *data)
{
        unsigned int *temp = (unsigned int *)data;
        return snprintf(page, count, "%u\n", *temp);
}

int lprocfs_wr_uint(struct file *file, const char *buffer,
                    unsigned long count, void *data)
{
        unsigned *p = data;
        char dummy[MAX_STRING_SIZE + 1], *end;
        unsigned long tmp;

        dummy[MAX_STRING_SIZE] = '\0';
        if (copy_from_user(dummy, buffer, MAX_STRING_SIZE))
                return -EFAULT;

        tmp = simple_strtoul(dummy, &end, 0);
        if (dummy == end)
                return -EINVAL;

        *p = (unsigned int)tmp;
        return count;
}

int lprocfs_rd_u64(char *page, char **start, off_t off,
                   int count, int *eof, void *data)
{
        LASSERT(data != NULL);
        *eof = 1;
        return snprintf(page, count, LPU64"\n", *(__u64 *)data);
}

int lprocfs_rd_atomic(char *page, char **start, off_t off,
                   int count, int *eof, void *data)
{
        atomic_t *atom = (atomic_t *)data;
        LASSERT(atom != NULL);
        *eof = 1;
        return snprintf(page, count, "%d\n", atomic_read(atom));
}

int lprocfs_wr_atomic(struct file *file, const char *buffer,
                      unsigned long count, void *data)
{
        atomic_t *atm = data;
        int val = 0;
        int rc;
        
        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc < 0)
                return rc;

        if (val <= 0)
                return -ERANGE;
                
        atomic_set(atm, val);
        return count;
}

int lprocfs_rd_uuid(char *page, char **start, off_t off, int count,
                    int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device*)data;

        LASSERT(obd != NULL);
        *eof = 1;
        return snprintf(page, count, "%s\n", obd->obd_uuid.uuid);
}

int lprocfs_rd_name(char *page, char **start, off_t off, int count,
                    int *eof, void* data)
{
        struct obd_device *dev = (struct obd_device *)data;

        LASSERT(dev != NULL);
        LASSERT(dev->obd_name != NULL);
        *eof = 1;
        return snprintf(page, count, "%s\n", dev->obd_name);
}

int lprocfs_rd_fstype(char *page, char **start, off_t off, int count, int *eof,
                      void *data)
{
        struct obd_device *obd = (struct obd_device *)data;

        LASSERT(obd != NULL);
        LASSERT(obd->obd_fsops != NULL);
        LASSERT(obd->obd_fsops->fs_type != NULL);
        return snprintf(page, count, "%s\n", obd->obd_fsops->fs_type);
}

int lprocfs_rd_blksize(char *page, char **start, off_t off, int count,
                       int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs, cfs_time_current_64() - HZ);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, "%u\n", osfs.os_bsize);
        }
        return rc;
}

int lprocfs_rd_kbytestotal(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs, cfs_time_current_64() - HZ);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_blocks;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_rd_kbytesfree(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs, cfs_time_current_64() - HZ);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_bfree;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_rd_kbytesavail(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs, cfs_time_current_64() - HZ);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_bavail;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_rd_filestotal(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs, cfs_time_current_64() - HZ);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", osfs.os_files);
        }

        return rc;
}

int lprocfs_rd_filesfree(char *page, char **start, off_t off, int count,
                         int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs, cfs_time_current_64() - HZ);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", osfs.os_ffree);
        }
        return rc;
}

int lprocfs_rd_server_uuid(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct obd_import *imp;
        char *imp_state_name = NULL;
        int rc = 0;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        imp = obd->u.cli.cl_import;
        imp_state_name = ptlrpc_import_state_name(imp->imp_state);
        *eof = 1;
        rc = snprintf(page, count, "%s\t%s%s\n",
                        obd2cli_tgt(obd), imp_state_name,
                        imp->imp_deactive ? "\tDEACTIVATED" : "");

        LPROCFS_CLIMP_EXIT(obd);
        return rc;
}

int lprocfs_rd_conn_uuid(char *page, char **start, off_t off, int count,
                         int *eof,  void *data)
{
        struct obd_device *obd = (struct obd_device*)data;
        struct ptlrpc_connection *conn;
        int rc = 0;

        LASSERT(obd != NULL); 
        LPROCFS_CLIMP_CHECK(obd);
        conn = obd->u.cli.cl_import->imp_connection;
        LASSERT(conn != NULL);
        *eof = 1;
        rc = snprintf(page, count, "%s\n", conn->c_remote_uuid.uuid);

        LPROCFS_CLIMP_EXIT(obd);
        return rc;
}

int lprocfs_at_hist_helper(char *page, int count, int rc, 
                           struct adaptive_timeout *at)
{
        int i;
        for (i = 0; i < AT_BINS; i++)
                rc += snprintf(page + rc, count - rc, "%3u ", at->at_hist[i]); 
        rc += snprintf(page + rc, count - rc, "\n");
        return rc;
}

/* See also ptlrpc_lprocfs_rd_timeouts */
int lprocfs_rd_timeouts(char *page, char **start, off_t off, int count,
                        int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct obd_import *imp;
        unsigned int cur, worst;
        time_t now, worstt;
        struct dhms ts;
        int i, rc = 0;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        imp = obd->u.cli.cl_import;
        *eof = 1;

        now = cfs_time_current_sec();

        /* Some network health info for kicks */
        s2dhms(&ts, now - imp->imp_last_reply_time);
        rc += snprintf(page + rc, count - rc, 
                       "%-10s : %ld, "DHMS_FMT" ago\n",
                       "last reply", imp->imp_last_reply_time, DHMS_VARS(&ts));


        cur = at_get(&imp->imp_at.iat_net_latency);
        worst = imp->imp_at.iat_net_latency.at_worst_ever;
        worstt = imp->imp_at.iat_net_latency.at_worst_time;
        s2dhms(&ts, now - worstt);
        rc += snprintf(page + rc, count - rc, 
                       "%-10s : cur %3u  worst %3u (at %ld, "DHMS_FMT" ago) ",
                       "network", cur, worst, worstt, DHMS_VARS(&ts)); 
        rc = lprocfs_at_hist_helper(page, count, rc,
                                    &imp->imp_at.iat_net_latency); 

        for(i = 0; i < IMP_AT_MAX_PORTALS; i++) {
                if (imp->imp_at.iat_portal[i] == 0)
                        break;
                cur = at_get(&imp->imp_at.iat_service_estimate[i]);
                worst = imp->imp_at.iat_service_estimate[i].at_worst_ever;
                worstt = imp->imp_at.iat_service_estimate[i].at_worst_time;
                s2dhms(&ts, now - worstt);
                rc += snprintf(page + rc, count - rc,
                               "portal %-2d  : cur %3u  worst %3u (at %ld, "
                               DHMS_FMT" ago) ", imp->imp_at.iat_portal[i], 
                               cur, worst, worstt, DHMS_VARS(&ts));
                rc = lprocfs_at_hist_helper(page, count, rc,
                                          &imp->imp_at.iat_service_estimate[i]);
        }

        LPROCFS_CLIMP_EXIT(obd);
        return rc;
}

static const char *obd_connect_names[] = {
        "read_only",
        "lov_index",
        "unused",
        "write_grant",
        "server_lock",
        "version",
        "request_portal",
        "acl",
        "xattr",
        "create_on_write",
        "truncate_lock",
        "initial_transno",
        "inode_bit_locks",
        "join_file",
        "getattr_by_fid",
        "no_oh_for_devices",
        "local_1.8_client",
        "remote_1.8_client",
        "max_byte_per_rpc",
        "64bit_qdata",
        "fid_capability",
        "oss_capability",
        "early_lock_cancel",
        "size_on_mds",
        "adaptive_timeout",
        "lru_resize",
        "mds_mds_connection",
        "real_conn",
        NULL
};

int lprocfs_rd_connect_flags(char *page, char **start, off_t off,
                             int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        __u64 mask = 1, flags;
        int i, ret = 0;

        LPROCFS_CLIMP_CHECK(obd);
        flags = obd->u.cli.cl_import->imp_connect_data.ocd_connect_flags;
        ret = snprintf(page, count, "flags="LPX64"\n", flags);
        for (i = 0; obd_connect_names[i] != NULL; i++, mask <<= 1) {
                if (flags & mask)
                        ret += snprintf(page + ret, count - ret, "%s\n",
                                        obd_connect_names[i]);
        }
        if (flags & ~(mask - 1))
                ret += snprintf(page + ret, count - ret,
                                "unknown flags "LPX64"\n", flags & ~(mask - 1));

        LPROCFS_CLIMP_EXIT(obd);
        return ret;
}
EXPORT_SYMBOL(lprocfs_rd_connect_flags);

int lprocfs_rd_num_exports(char *page, char **start, off_t off, int count,
                           int *eof,  void *data)
{
        struct obd_device *obd = (struct obd_device*)data;

        LASSERT(obd != NULL);
        *eof = 1;
        return snprintf(page, count, "%u\n", obd->obd_num_exports);
}

int lprocfs_rd_numrefs(char *page, char **start, off_t off, int count,
                       int *eof, void *data)
{
        struct obd_type *class = (struct obd_type*) data;

        LASSERT(class != NULL);
        *eof = 1;
        return snprintf(page, count, "%d\n", class->typ_refcnt);
}

int lprocfs_obd_setup(struct obd_device *obd, struct lprocfs_vars *list)
{
        int rc = 0;

        LASSERT(obd != NULL);
        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);
        LASSERT(obd->obd_type->typ_procroot != NULL);

        obd->obd_proc_entry = lprocfs_register(obd->obd_name,
                                               obd->obd_type->typ_procroot,
                                               list, obd);
        if (IS_ERR(obd->obd_proc_entry)) {
                rc = PTR_ERR(obd->obd_proc_entry);
                CERROR("error %d setting up lprocfs for %s\n",rc,obd->obd_name);
                obd->obd_proc_entry = NULL;
        }
        return rc;
}

int lprocfs_obd_cleanup(struct obd_device *obd)
{
        if (!obd) 
                return -EINVAL;
        if (obd->obd_proc_exports_entry) {
                /* Should be no exports left */
                LASSERT(obd->obd_proc_exports_entry->subdir == NULL);
                lprocfs_remove(&obd->obd_proc_exports_entry);
        }
        lprocfs_remove(&obd->obd_proc_entry);
        return 0;
}

void lprocfs_free_client_stats(nid_stat_t *client_stat)
{
        LASSERT(client_stat->nid_exp_ref_count == 0);

        list_del(&client_stat->nid_chain);

        if (client_stat->nid_proc)
                lprocfs_remove(&client_stat->nid_proc);

        if (client_stat->nid_stats)
                lprocfs_free_stats(&client_stat->nid_stats);

        if (client_stat->nid_brw_stats)
                OBD_FREE(client_stat->nid_brw_stats, sizeof(struct brw_stats));

        OBD_FREE(client_stat, sizeof(struct nid_stat));
        return;

}

void lprocfs_free_per_client_stats(struct obd_device *obd)
{

        struct list_head *nids= &obd->obd_proc_nid_list;
        nid_stat_t *client_stat = NULL, *nxt;
        ENTRY;

        spin_lock(&obd->nid_lock);

        list_for_each_entry_safe (client_stat, nxt, nids, nid_chain)
                lprocfs_free_client_stats(client_stat);

        spin_unlock(&obd->nid_lock);
        return;

}

struct lprocfs_stats *lprocfs_alloc_stats(unsigned int num,
                                          enum lprocfs_stats_flags flags)
{
        struct lprocfs_stats *stats;
        struct lprocfs_percpu *percpu;
        unsigned int percpusize;
        unsigned int i;
        unsigned int num_cpu;

        if (num == 0)
                return NULL;

        if (flags & LPROCFS_STATS_FLAG_NOPERCPU)
                num_cpu = 1;
        else
                num_cpu = num_possible_cpus();

        OBD_ALLOC(stats, offsetof(typeof(*stats), ls_percpu[num_cpu]));
        if (stats == NULL)
                return NULL;

        if (flags & LPROCFS_STATS_FLAG_NOPERCPU) {
                stats->ls_flags = flags;
                spin_lock_init(&stats->ls_lock);
                /* Use this lock only if there are no percpu areas */
        } else {
                stats->ls_flags = 0;
        }

        percpusize = offsetof(typeof(*percpu), lp_cntr[num]);
        if (num_cpu > 1)
                percpusize = L1_CACHE_ALIGN(percpusize);

        stats->ls_percpu_size = num_cpu * percpusize;
        OBD_ALLOC(stats->ls_percpu[0], stats->ls_percpu_size);
        if (stats->ls_percpu[0] == NULL) {
                OBD_FREE(stats, offsetof(typeof(*stats),
                                         ls_percpu[num_cpu]));
                return NULL;
        }

        stats->ls_num = num;
        for (i = 1; i < num_cpu; i++)
                stats->ls_percpu[i] = (void *)(stats->ls_percpu[i - 1]) +
                        percpusize;

        return stats;
}

void lprocfs_free_stats(struct lprocfs_stats **statsh)
{
        struct lprocfs_stats *stats = *statsh;
        unsigned int num_cpu;
        
        if (!stats || (stats->ls_num == 0))
                return;
        *statsh = NULL;
        if (stats->ls_flags & LPROCFS_STATS_FLAG_NOPERCPU)
                num_cpu = 1;
        else
                num_cpu = num_possible_cpus();

        OBD_FREE(stats->ls_percpu[0], stats->ls_percpu_size);
        OBD_FREE(stats, offsetof(typeof(*stats), ls_percpu[num_cpu]));
}

void lprocfs_clear_stats(struct lprocfs_stats *stats)
{
        struct lprocfs_counter *percpu_cntr;
        int i, j;
        unsigned int num_cpu;

        num_cpu = lprocfs_stats_lock(stats, LPROCFS_GET_NUM_CPU);

        for (i = 0; i < num_cpu; i++) {
                for (j = 0; j < stats->ls_num; j++) {        
                        percpu_cntr = &(stats->ls_percpu[i])->lp_cntr[j];
                        atomic_inc(&percpu_cntr->lc_cntl.la_entry);
                        percpu_cntr->lc_count = 0;
                        percpu_cntr->lc_sum = 0;
                        percpu_cntr->lc_min = ~(__u64)0;
                        percpu_cntr->lc_max = 0;
                        percpu_cntr->lc_sumsquare = 0;
                        atomic_inc(&percpu_cntr->lc_cntl.la_exit);
                }
        }

        lprocfs_stats_unlock(stats);
}

static ssize_t lprocfs_stats_seq_write(struct file *file, const char *buf,
                                       size_t len, loff_t *off)
{
        struct seq_file *seq = file->private_data;
        struct lprocfs_stats *stats = seq->private;

        lprocfs_clear_stats(stats);

        return len;
}

static void *lprocfs_stats_seq_start(struct seq_file *p, loff_t *pos)
{
        struct lprocfs_stats *stats = p->private;
        /* return 1st cpu location */
        return (*pos >= stats->ls_num) ? NULL :
                &(stats->ls_percpu[0]->lp_cntr[*pos]);
}

static void lprocfs_stats_seq_stop(struct seq_file *p, void *v)
{
}

static void *lprocfs_stats_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        struct lprocfs_stats *stats = p->private;
        ++*pos;
        return (*pos >= stats->ls_num) ? NULL :
                &(stats->ls_percpu[0]->lp_cntr[*pos]);
}

/* seq file export of one lprocfs counter */
static int lprocfs_stats_seq_show(struct seq_file *p, void *v)
{
       struct lprocfs_stats *stats = p->private;
       struct lprocfs_counter  *cntr = v;
       struct lprocfs_counter  t, ret = { .lc_min = ~(__u64)0 };
       int i, idx, rc = 0;
       unsigned int num_cpu;

       if (cntr == &(stats->ls_percpu[0])->lp_cntr[0]) {
               struct timeval now;
               do_gettimeofday(&now);
               rc = seq_printf(p, "%-25s %lu.%lu secs.usecs\n",
                               "snapshot_time", now.tv_sec, now.tv_usec);
               if (rc < 0)
                       return rc;
       }
       idx = cntr - &(stats->ls_percpu[0])->lp_cntr[0];

       if (stats->ls_flags & LPROCFS_STATS_FLAG_NOPERCPU)
               num_cpu = 1;
       else
               num_cpu = num_possible_cpus();

       for (i = 0; i < num_cpu; i++) {
               struct lprocfs_counter *percpu_cntr =
                       &(stats->ls_percpu[i])->lp_cntr[idx];
               int centry;

               do {
                       centry = atomic_read(&percpu_cntr->lc_cntl.la_entry);
                       t.lc_count = percpu_cntr->lc_count;
                       t.lc_sum = percpu_cntr->lc_sum;
                       t.lc_min = percpu_cntr->lc_min;
                       t.lc_max = percpu_cntr->lc_max;
                       t.lc_sumsquare = percpu_cntr->lc_sumsquare;
               } while (centry != atomic_read(&percpu_cntr->lc_cntl.la_entry) &&
                        centry != atomic_read(&percpu_cntr->lc_cntl.la_exit));
               ret.lc_count += t.lc_count;
               ret.lc_sum += t.lc_sum;
               if (t.lc_min < ret.lc_min)
                       ret.lc_min = t.lc_min;
               if (t.lc_max > ret.lc_max)
                       ret.lc_max = t.lc_max;
               ret.lc_sumsquare += t.lc_sumsquare;
       }

       if (ret.lc_count == 0)
               goto out;

       rc = seq_printf(p, "%-25s "LPU64" samples [%s]", cntr->lc_name,
                       ret.lc_count, cntr->lc_units);
       if (rc < 0)
               goto out;

       if ((cntr->lc_config & LPROCFS_CNTR_AVGMINMAX) && (ret.lc_count > 0)) {
               rc = seq_printf(p, " "LPU64" "LPU64" "LPU64,
                               ret.lc_min, ret.lc_max, ret.lc_sum);
               if (rc < 0)
                       goto out;
               if (cntr->lc_config & LPROCFS_CNTR_STDDEV)
                       rc = seq_printf(p, " "LPU64, ret.lc_sumsquare);
               if (rc < 0)
                       goto out;
       }
       rc = seq_printf(p, "\n");
 out:
       return (rc < 0) ? rc : 0;
}

struct seq_operations lprocfs_stats_seq_sops = {
        start: lprocfs_stats_seq_start,
        stop:  lprocfs_stats_seq_stop,
        next:  lprocfs_stats_seq_next,
        show:  lprocfs_stats_seq_show,
};

static int lprocfs_stats_seq_open(struct inode *inode, struct file *file)
{
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file *seq;
        int rc;

        LPROCFS_ENTRY_AND_CHECK(dp);
        rc = seq_open(file, &lprocfs_stats_seq_sops);
        if (rc) {
                LPROCFS_EXIT();
                return rc;
        }

        seq = file->private_data;
        seq->private = dp->data;
        return 0;
}

struct file_operations lprocfs_stats_seq_fops = {
        .owner   = THIS_MODULE,
        .open    = lprocfs_stats_seq_open,
        .read    = seq_read,
        .write   = lprocfs_stats_seq_write,
        .llseek  = seq_lseek,
        .release = lprocfs_seq_release,
};

int lprocfs_register_stats(struct proc_dir_entry *root, const char *name,
                           struct lprocfs_stats *stats)
{
        struct proc_dir_entry *entry;
        LASSERT(root != NULL);

        entry = create_proc_entry(name, 0644, root);
        if (entry == NULL)
                return -ENOMEM;
        entry->proc_fops = &lprocfs_stats_seq_fops;
        entry->data = (void *)stats;
        return 0;
}

void lprocfs_counter_init(struct lprocfs_stats *stats, int index,
                          unsigned conf, const char *name, const char *units)
{
        struct lprocfs_counter *c;
        int i;
        unsigned int num_cpu;

        LASSERT(stats != NULL);

        num_cpu = lprocfs_stats_lock(stats, LPROCFS_GET_NUM_CPU);

        for (i = 0; i < num_cpu; i++) {
                c = &(stats->ls_percpu[i]->lp_cntr[index]);
                c->lc_config = conf;
                c->lc_count = 0;
                c->lc_sum = 0;
                c->lc_min = ~(__u64)0;
                c->lc_max = 0;
                c->lc_name = name;
                c->lc_units = units;
        }

        lprocfs_stats_unlock(stats);
}
EXPORT_SYMBOL(lprocfs_counter_init);

#define LPROCFS_OBD_OP_INIT(base, stats, op)                               \
do {                                                                       \
        unsigned int coffset = base + OBD_COUNTER_OFFSET(op);              \
        LASSERT(coffset < stats->ls_num);                                  \
        lprocfs_counter_init(stats, coffset, 0, #op, "reqs");              \
} while (0)

void lprocfs_init_ops_stats(int num_private_stats, struct lprocfs_stats *stats)
{
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, iocontrol);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, get_info);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, set_info_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, attach);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, detach);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, setup);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, precleanup);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, cleanup);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, process_config);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, postrecov);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, add_conn);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, del_conn);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, connect);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, reconnect);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, disconnect);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, statfs);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, statfs_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, packmd);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, unpackmd);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, checkmd);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, preallocate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, precreate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, create);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, destroy);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, setattr);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, setattr_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, getattr);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, getattr_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, brw);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, brw_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, prep_async_page);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, queue_async_io);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, queue_group_io);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, trigger_group_io);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, set_async_flags);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, teardown_async_page);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, merge_lvb);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, adjust_kms);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, punch);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, sync);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, migrate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, copy);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, iterate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, preprw);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, commitrw);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, enqueue);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, match);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, change_cbdata);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, cancel);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, cancel_unused);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, join_lru);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, init_export);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, destroy_export);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, extent_calc);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, llog_init);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, llog_finish);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, pin);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, unpin);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, import_event);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, notify);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, health_check);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, quotacheck);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, quotactl);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, ping);
}

int lprocfs_alloc_obd_stats(struct obd_device *obd, unsigned num_private_stats)
{
        struct lprocfs_stats *stats;
        unsigned int num_stats;
        int rc, i;

        LASSERT(obd->obd_stats == NULL);
        LASSERT(obd->obd_proc_entry != NULL);
        LASSERT(obd->obd_cntr_base == 0);

        num_stats = ((int)sizeof(*obd->obd_type->typ_ops) / sizeof(void *)) +
                num_private_stats - 1 /* o_owner */;
        stats = lprocfs_alloc_stats(num_stats, 0);
        if (stats == NULL)
                return -ENOMEM;

        lprocfs_init_ops_stats(num_private_stats, stats);

        for (i = num_private_stats; i < num_stats; i++) {
                /* If this LBUGs, it is likely that an obd
                 * operation was added to struct obd_ops in
                 * <obd.h>, and that the corresponding line item
                 * LPROCFS_OBD_OP_INIT(.., .., opname)
                 * is missing from the list above. */
                LASSERTF(stats->ls_percpu[0]->lp_cntr[i].lc_name != NULL,
                         "Missing obd_stat initializer obd_op "
                         "operation at offset %d.\n", i - num_private_stats);
        }
        rc = lprocfs_register_stats(obd->obd_proc_entry, "stats", stats);
        if (rc < 0) {
                lprocfs_free_stats(&stats);
        } else {
                obd->obd_stats  = stats;
                obd->obd_cntr_base = num_private_stats;
        }
        return rc;
}

void lprocfs_free_obd_stats(struct obd_device *obd)
{
        if (obd->obd_stats) 
                lprocfs_free_stats(&obd->obd_stats);
}

int lprocfs_exp_rd_nid(char *page, char **start, off_t off, int count,
                         int *eof,  void *data)
{
        struct obd_export *exp = (struct obd_export*)data;
        LASSERT(exp != NULL);
        *eof = 1;
        return snprintf(page, count, "%s\n", obd_export_nid2str(exp));
}

void lprocfs_exp_print_uuid(void *cb_data)
{
        struct exp_uuid_cb_data *data = (struct exp_rd_uuid_cb_data *)cb_data;

        if (data->exp->exp_nid_stats)
                *data->len += snprintf((data->page + *data->len),
                                       data->count, "%s\n",
                                       obd_uuid2str(&data->exp->exp_client_uuid));
}

int lprocfs_exp_rd_uuid(char *page, char **start, off_t off, int count,
                        int *eof,  void *data)
{
        struct nid_stat *stats = (struct nid_stat *)data;
        struct exp_uuid_cb_data cb_data;
        struct obd_device *obd = stats->nid_obd;
        int len = 0;

        *eof = 1;
        page[0] = '\0';
        LASSERT(obd != NULL);

        cb_data.page = page;
        cb_data.count = count;
        cb_data.eof = eof;
        cb_data.len = &len;
        cb_data.exp = NULL;
        lustre_hash_bucket_iterate(obd->obd_nid_hash_body,
                                   &stats->nid, lprocfs_exp_print_uuid,
                                   &cb_data);
        return (*cb_data.len);
}

int lprocfs_exp_setup(struct obd_export *exp, lnet_nid_t nid, int *newnid)
{
        int rc = 0;
        struct nid_stat *tmp = NULL;
        struct obd_device *obd = NULL;
        struct obd_export *export = NULL;
        ENTRY;

        if (exp->exp_obd)
                obd = exp->exp_obd;

        if (!exp || !exp->exp_obd || !exp->exp_obd->obd_proc_exports_entry ||
            !obd->obd_nid_hash_body)
                RETURN(-EINVAL);

        *newnid = 0;

        if (!nid)
                RETURN(rc);

        export = lustre_hash_get_object_by_key(obd->obd_nid_hash_body,
                                               &nid);
        if (export) {
                exp->exp_nid_stats = export->exp_nid_stats;
                *newnid = 0;
                class_export_put(export);
        } else {
                OBD_ALLOC(tmp, sizeof(struct nid_stat));
                if (tmp == NULL)
                        GOTO(out, rc = -ENOMEM);

                tmp->nid = nid;
                tmp->nid_obd = exp->exp_obd;
                tmp->nid_exp_ref_count = 0;
                tmp->nid_proc = proc_mkdir(libcfs_nid2str(nid),
                                           exp->exp_obd->obd_proc_exports_entry);
                if (!tmp->nid_proc) {
                        CERROR("Error making export directory for"
                               " nid %s\n", libcfs_nid2str(nid));
                        OBD_FREE(tmp, sizeof(struct nid_stat));
                        GOTO(out, rc = -ENOMEM);
                }

                rc = lprocfs_add_simple(tmp->nid_proc, "uuid",
                                        lprocfs_exp_rd_uuid, NULL, tmp);
                if (rc)
                        CERROR("Error adding the uuid file\n");

                exp->exp_nid_stats = tmp;
                *newnid = 1;

                spin_lock(&obd->nid_lock);

                list_add_tail(&tmp->nid_chain,
                              &exp->exp_obd->obd_proc_nid_list);

                spin_unlock(&obd->nid_lock);

        }
        if (exp->exp_nid_stats)
                exp->exp_nid_stats->nid_exp_ref_count++;
out:
        RETURN(rc);
}

int lprocfs_exp_cleanup(struct obd_export *exp)
{
        if (exp->exp_nid_stats) {
                exp->exp_nid_stats->nid_exp_ref_count--;
                exp->exp_nid_stats = NULL;
        }
        return 0;
}

int lprocfs_write_helper(const char *buffer, unsigned long count,
                         int *val)
{
        return lprocfs_write_frac_helper(buffer, count, val, 1);
}

int lprocfs_write_frac_helper(const char *buffer, unsigned long count,
                              int *val, int mult)
{
        char kernbuf[20], *end, *pbuf;

        if (count > (sizeof(kernbuf) - 1))
                return -EINVAL;

        if (copy_from_user(kernbuf, buffer, count))
                return -EFAULT;

        kernbuf[count] = '\0';
        pbuf = kernbuf;
        if (*pbuf == '-') {
                mult = -mult;
                pbuf++;
        }

        *val = (int)simple_strtoul(pbuf, &end, 10) * mult;
        if (pbuf == end)
                return -EINVAL;

        if (end != NULL && *end == '.') {
                int temp_val, pow = 1;
                int i;

                pbuf = end + 1;
                if (strlen(pbuf) > 5)
                        pbuf[5] = '\0'; /*only allow 5bits fractional*/

                temp_val = (int)simple_strtoul(pbuf, &end, 10) * mult;

                if (pbuf < end) {
                        for (i = 0; i < (end - pbuf); i++)
                                pow *= 10;

                        *val += temp_val / pow;
                }
        }
        return 0;
}

int lprocfs_read_frac_helper(char *buffer, unsigned long count, long val, int mult)
{
        long decimal_val, frac_val;
        int prtn;

        if (count < 10)
                return -EINVAL;

        decimal_val = val / mult;
        prtn = snprintf(buffer, count, "%ld", decimal_val);
        frac_val = val % mult;

        if (prtn < (count - 4) && frac_val > 0) {
                long temp_frac;
                int i, temp_mult = 1, frac_bits = 0;

                temp_frac = frac_val * 10;
                buffer[prtn++] = '.';
                while (frac_bits < 2 && (temp_frac / mult) < 1 ) { /*only reserved 2bits fraction*/
                        buffer[prtn++] ='0';
                        temp_frac *= 10;
                        frac_bits++;
                }
                /*
                  Need to think these cases :
                        1. #echo x.00 > /proc/xxx       output result : x
                        2. #echo x.0x > /proc/xxx       output result : x.0x
                        3. #echo x.x0 > /proc/xxx       output result : x.x
                        4. #echo x.xx > /proc/xxx       output result : x.xx
                        Only reserved 2bits fraction.       
                 */
                for (i = 0; i < (5 - prtn); i++)
                        temp_mult *= 10;

                frac_bits = min((int)count - prtn, 3 - frac_bits);
                prtn += snprintf(buffer + prtn, frac_bits, "%ld", frac_val * temp_mult / mult);

                prtn--;
                while(buffer[prtn] < '1' || buffer[prtn] > '9') {
                        prtn--;
                        if (buffer[prtn] == '.') {
                                prtn--;
                                break;
                        }
                }
                prtn++;
        }
        buffer[prtn++] ='\n';
        return prtn;
}

int lprocfs_write_u64_helper(const char *buffer, unsigned long count,__u64 *val)
{
        return lprocfs_write_frac_u64_helper(buffer, count, val, 1);
}

int lprocfs_write_frac_u64_helper(const char *buffer, unsigned long count,
                              __u64 *val, int mult)
{
        char kernbuf[22], *end, *pbuf;
        __u64 whole, frac = 0, units;
        unsigned frac_d = 1;

        if (count > (sizeof(kernbuf) - 1) )
                return -EINVAL;

        if (copy_from_user(kernbuf, buffer, count))
                return -EFAULT;

        kernbuf[count] = '\0';
        pbuf = kernbuf;
        if (*pbuf == '-') {
                mult = -mult;
                pbuf++;
        }

        whole = simple_strtoull(pbuf, &end, 10);
        if (pbuf == end)
                return -EINVAL;

        if (end != NULL && *end == '.') {
                int i;
                pbuf = end + 1;

                /* need to limit frac_d to a __u32 */
                if (strlen(pbuf) > 10)
                        pbuf[10] = '\0';

                frac = simple_strtoull(pbuf, &end, 10);
                /* count decimal places */
                for (i = 0; i < (end - pbuf); i++)
                        frac_d *= 10;
        }

        units = 1;
        switch(*end) {
        case 'p': case 'P':
                units <<= 10;
        case 't': case 'T':
                units <<= 10;
        case 'g': case 'G':
                units <<= 10;
        case 'm': case 'M':
                units <<= 10;
        case 'k': case 'K':
                units <<= 10;
        }
        /* Specified units override the multiplier */
        if (units)
                mult = mult < 0 ? -units : units;

        frac *= mult;
        do_div(frac, frac_d);
        *val = whole * mult + frac;
        return 0;
}

int lprocfs_seq_create(cfs_proc_dir_entry_t *parent, 
                       char *name, mode_t mode,
                       struct file_operations *seq_fops, void *data)
{
        struct proc_dir_entry *entry;
        ENTRY;

        entry = create_proc_entry(name, mode, parent);
        if (entry == NULL)
                RETURN(-ENOMEM);
        entry->proc_fops = seq_fops;
        entry->data = data;

        RETURN(0);
}
EXPORT_SYMBOL(lprocfs_seq_create);

__inline__ int lprocfs_obd_seq_create(struct obd_device *dev, char *name,
                                      mode_t mode,
                                      struct file_operations *seq_fops,
                                      void *data)
{
        return (lprocfs_seq_create(dev->obd_proc_entry, name, 
                                   mode, seq_fops, data));
}
EXPORT_SYMBOL(lprocfs_obd_seq_create);

void lprocfs_oh_tally(struct obd_histogram *oh, unsigned int value)
{
        if (value >= OBD_HIST_MAX)
                value = OBD_HIST_MAX - 1;

        spin_lock(&oh->oh_lock);
        oh->oh_buckets[value]++;
        spin_unlock(&oh->oh_lock);
}
EXPORT_SYMBOL(lprocfs_oh_tally);

void lprocfs_oh_tally_log2(struct obd_histogram *oh, unsigned int value)
{
        unsigned int val;

        for (val = 0; ((1 << val) < value) && (val <= OBD_HIST_MAX); val++)
                ;

        lprocfs_oh_tally(oh, val);
}
EXPORT_SYMBOL(lprocfs_oh_tally_log2);

unsigned long lprocfs_oh_sum(struct obd_histogram *oh)
{
        unsigned long ret = 0;
        int i;

        for (i = 0; i < OBD_HIST_MAX; i++)
                ret +=  oh->oh_buckets[i];
        return ret;
}
EXPORT_SYMBOL(lprocfs_oh_sum);

void lprocfs_oh_clear(struct obd_histogram *oh)
{
        spin_lock(&oh->oh_lock);
        memset(oh->oh_buckets, 0, sizeof(oh->oh_buckets));
        spin_unlock(&oh->oh_lock);
}
EXPORT_SYMBOL(lprocfs_oh_clear);

int lprocfs_obd_rd_recovery_status(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        int len = 0, size;

        LASSERT(obd != NULL);
        LASSERT(count >= 0);

        /* Set start of user data returned to
           page + off since the user may have
           requested to read much smaller than
           what we need to read */
        *start = page + off;

        /* We know we are allocated a page here.
           Also we know that this function will
           not need to write more than a page
           so we can truncate at CFS_PAGE_SIZE.  */
        size = min(count + (int)off + 1, (int)CFS_PAGE_SIZE);

        /* Initialize the page */
        memset(page, 0, size);

        if (lprocfs_obd_snprintf(&page, size, &len, "status: ") <= 0)
                goto out;
        if (obd->obd_max_recoverable_clients == 0) {
                lprocfs_obd_snprintf(&page, size, &len, "INACTIVE\n");
                goto fclose;
        }

        /* sampled unlocked, but really... */
        if (obd->obd_recovering == 0) {
                if (lprocfs_obd_snprintf(&page, size, &len, "COMPLETE\n") <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "recovery_start: %lu\n",
                                         obd->obd_recovery_start) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "recovery_duration: %lu\n",
                                         obd->obd_recovery_end -
                                         obd->obd_recovery_start) <= 0)
                        goto out;
                /* Number of clients that have completed recovery */
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "completed_clients: %d/%d\n",
                                         obd->obd_max_recoverable_clients -
                                         obd->obd_recoverable_clients,
                                         obd->obd_max_recoverable_clients) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "replayed_requests: %d\n",
                                         obd->obd_replayed_requests) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "last_transno: "LPD64"\n",
                                         obd->obd_next_recovery_transno - 1)<=0)
                        goto out;
                goto fclose;
        }

        if (lprocfs_obd_snprintf(&page, size, &len, "RECOVERING\n") <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "recovery_start: %lu\n",
                                 obd->obd_recovery_start) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "time_remaining: %lu\n",
                           cfs_time_current_sec() >= obd->obd_recovery_end ? 0 :
                           obd->obd_recovery_end - cfs_time_current_sec()) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len,"connected_clients: %d/%d\n",
                                 obd->obd_connected_clients,
                                 obd->obd_max_recoverable_clients) <= 0)
                goto out;
        /* Number of clients that have completed recovery */
        if (lprocfs_obd_snprintf(&page, size, &len,"completed_clients: %d/%d\n",
                                 obd->obd_max_recoverable_clients -
                                 obd->obd_recoverable_clients,
                                 obd->obd_max_recoverable_clients) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len,"replayed_requests: %d/??\n",
                                 obd->obd_replayed_requests) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "queued_requests: %d\n",
                                 obd->obd_requests_queued_for_recovery) <= 0)
                goto out;
        lprocfs_obd_snprintf(&page, size, &len, "next_transno: "LPD64"\n",
                             obd->obd_next_recovery_transno);

fclose:
        *eof = 1;
out:
        return min(count, len - (int)off);
}
EXPORT_SYMBOL(lprocfs_obd_rd_recovery_status);

EXPORT_SYMBOL(lprocfs_register);
EXPORT_SYMBOL(lprocfs_srch);
EXPORT_SYMBOL(lprocfs_remove);
EXPORT_SYMBOL(lprocfs_add_vars);
EXPORT_SYMBOL(lprocfs_obd_setup);
EXPORT_SYMBOL(lprocfs_obd_cleanup);
EXPORT_SYMBOL(lprocfs_add_simple);
EXPORT_SYMBOL(lprocfs_free_client_stats);
EXPORT_SYMBOL(lprocfs_free_per_client_stats);
EXPORT_SYMBOL(lprocfs_alloc_stats);
EXPORT_SYMBOL(lprocfs_free_stats);
EXPORT_SYMBOL(lprocfs_clear_stats);
EXPORT_SYMBOL(lprocfs_register_stats);
EXPORT_SYMBOL(lprocfs_init_ops_stats);
EXPORT_SYMBOL(lprocfs_alloc_obd_stats);
EXPORT_SYMBOL(lprocfs_free_obd_stats);
EXPORT_SYMBOL(lprocfs_exp_setup);
EXPORT_SYMBOL(lprocfs_exp_cleanup);

EXPORT_SYMBOL(lprocfs_rd_u64);
EXPORT_SYMBOL(lprocfs_rd_atomic);
EXPORT_SYMBOL(lprocfs_wr_atomic);
EXPORT_SYMBOL(lprocfs_rd_uint);
EXPORT_SYMBOL(lprocfs_wr_uint);
EXPORT_SYMBOL(lprocfs_rd_uuid);
EXPORT_SYMBOL(lprocfs_rd_name);
EXPORT_SYMBOL(lprocfs_rd_fstype);
EXPORT_SYMBOL(lprocfs_rd_server_uuid);
EXPORT_SYMBOL(lprocfs_rd_conn_uuid);
EXPORT_SYMBOL(lprocfs_rd_num_exports);
EXPORT_SYMBOL(lprocfs_rd_numrefs);
EXPORT_SYMBOL(lprocfs_at_hist_helper);
EXPORT_SYMBOL(lprocfs_rd_timeouts);
EXPORT_SYMBOL(lprocfs_rd_blksize);
EXPORT_SYMBOL(lprocfs_rd_kbytestotal);
EXPORT_SYMBOL(lprocfs_rd_kbytesfree);
EXPORT_SYMBOL(lprocfs_rd_kbytesavail);
EXPORT_SYMBOL(lprocfs_rd_filestotal);
EXPORT_SYMBOL(lprocfs_rd_filesfree);

EXPORT_SYMBOL(lprocfs_write_helper);
EXPORT_SYMBOL(lprocfs_write_frac_helper);
EXPORT_SYMBOL(lprocfs_read_frac_helper);
EXPORT_SYMBOL(lprocfs_write_u64_helper);
EXPORT_SYMBOL(lprocfs_write_frac_u64_helper);
#endif /* LPROCFS*/
