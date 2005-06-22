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
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#include <asm/statfs.h>
#endif
#include <linux/obd_class.h>
#include <linux/lprocfs_status.h>
#include <linux/seq_file.h>
#include "osc_internal.h"

#ifndef LPROCFS
static struct lprocfs_vars lprocfs_obd_vars[]  = { {0} };
static struct lprocfs_vars lprocfs_module_vars[] = { {0} };
#else

int osc_rd_max_pages_per_rpc(char *page, char **start, off_t off, int count,
                             int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        spin_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%d\n", cli->cl_max_pages_per_rpc);
        spin_unlock(&cli->cl_loi_list_lock);
        return rc;
}

int osc_wr_max_pages_per_rpc(struct file *file, const char *buffer,
                             unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 1 || val > PTLRPC_MAX_BRW_PAGES)
                return -ERANGE;

        spin_lock(&cli->cl_loi_list_lock);
        cli->cl_max_pages_per_rpc = val;
        spin_unlock(&cli->cl_loi_list_lock);

        return count;
}

int osc_rd_max_rpcs_in_flight(char *page, char **start, off_t off, int count,
                              int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        spin_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%u\n", cli->cl_max_rpcs_in_flight);
        spin_unlock(&cli->cl_loi_list_lock);
        return rc;
}

int osc_wr_max_rpcs_in_flight(struct file *file, const char *buffer,
                              unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 1 || val > OSC_MAX_RIF_MAX)
                return -ERANGE;

        spin_lock(&cli->cl_loi_list_lock);
        cli->cl_max_rpcs_in_flight = val;
        spin_unlock(&cli->cl_loi_list_lock);

        return count;
}

int osc_rd_max_dirty_mb(char *page, char **start, off_t off, int count,
                        int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        unsigned val;

        spin_lock(&cli->cl_loi_list_lock);
        val = cli->cl_dirty_max >> 20;
        spin_unlock(&cli->cl_loi_list_lock);

        return snprintf(page, count, "%u\n", val);
}

int osc_wr_max_dirty_mb(struct file *file, const char *buffer,
                        unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 0 || val > OSC_MAX_DIRTY_MB_MAX ||
            val > num_physpages >> (20 - PAGE_SHIFT - 2)) /* 1/4 of RAM */
                return -ERANGE;

        spin_lock(&cli->cl_loi_list_lock);
        cli->cl_dirty_max = (obd_count)val * 1024 * 1024;
        osc_wake_cache_waiters(cli);
        spin_unlock(&cli->cl_loi_list_lock);

        return count;
}

int osc_rd_cur_dirty_bytes(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        spin_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%lu\n", cli->cl_dirty);
        spin_unlock(&cli->cl_loi_list_lock);
        return rc;
}

int osc_rd_cur_grant_bytes(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        spin_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%lu\n", cli->cl_avail_grant);
        spin_unlock(&cli->cl_loi_list_lock);
        return rc;
}

static struct lprocfs_vars lprocfs_obd_vars[] = {
        { "uuid",            lprocfs_rd_uuid,        0, 0 },
        { "blocksize",       lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",     lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",      lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",     lprocfs_rd_kbytesavail, 0, 0 },
        { "filestotal",      lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",       lprocfs_rd_filesfree,   0, 0 },
        //{ "filegroups",      lprocfs_rd_filegroups,  0, 0 },
        { "ost_server_uuid", lprocfs_rd_server_uuid, 0, 0 },
        { "ost_conn_uuid",   lprocfs_rd_conn_uuid, 0, 0 },
        { "max_pages_per_rpc", osc_rd_max_pages_per_rpc,
                               osc_wr_max_pages_per_rpc, 0 },
        { "max_rpcs_in_flight", osc_rd_max_rpcs_in_flight,
                                osc_wr_max_rpcs_in_flight, 0 },
        { "max_dirty_mb", osc_rd_max_dirty_mb, osc_wr_max_dirty_mb, 0 },
        { "cur_dirty_bytes", osc_rd_cur_dirty_bytes, 0, 0 },
        { "cur_grant_bytes", osc_rd_cur_grant_bytes, 0, 0 },
        { 0 }
};

static struct lprocfs_vars lprocfs_module_vars[] = {
        { "num_refs",        lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

#define pct(a,b) (b ? a * 100 / b : 0)

#define PRINTF_STIME(stime) (unsigned long)(stime)->st_num,     \
        lprocfs_stime_avg_ms(stime), lprocfs_stime_avg_us(stime)


static int osc_rpc_stats_seq_show(struct seq_file *seq, void *v)
{
        struct timeval now;
        struct obd_device *dev = seq->private;
        struct client_obd *cli = &dev->u.cli;
        unsigned long flags;
        unsigned long read_tot = 0, write_tot = 0, read_cum, write_cum;
        int i;

        do_gettimeofday(&now);

        spin_lock_irqsave(&cli->cl_loi_list_lock, flags);

        seq_printf(seq, "snapshot_time:         %lu.%lu (secs.usecs)\n",
                   now.tv_sec, now.tv_usec);

        seq_printf(seq, "read RPCs in flight:  %d\n",
                   cli->cl_r_in_flight);
        seq_printf(seq, "write RPCs in flight: %d\n",
                   cli->cl_w_in_flight);
        seq_printf(seq, "pending write pages:  %d\n",
                   cli->cl_pending_w_pages);
        seq_printf(seq, "pending read pages:   %d\n",
                   cli->cl_pending_r_pages);


        seq_printf(seq, "\n\t\t\tread\t\t\twrite\n");
        seq_printf(seq, "pages per rpc         rpcs   %% cum %% |");
        seq_printf(seq, "       rpcs   %% cum %%\n");

        read_tot = lprocfs_oh_sum(&cli->cl_read_page_hist);
        write_tot = lprocfs_oh_sum(&cli->cl_write_page_hist);

        read_cum = 0;
        write_cum = 0;
        for (i = 0; i < OBD_HIST_MAX; i++) {
                unsigned long r = cli->cl_read_page_hist.oh_buckets[i];
                unsigned long w = cli->cl_write_page_hist.oh_buckets[i];
                read_cum += r;
                write_cum += w;
                seq_printf(seq, "%d:\t\t%10lu %3lu %3lu   | %10lu %3lu %3lu\n",
                                 1 << i, r, pct(r, read_tot),
                                 pct(read_cum, read_tot), w,
                                 pct(w, write_tot),
                                 pct(write_cum, write_tot));
                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }

        seq_printf(seq, "\n\t\t\tread\t\t\twrite\n");
        seq_printf(seq, "rpcs in flight        rpcs   %% cum %% |");
        seq_printf(seq, "       rpcs   %% cum %%\n");

        read_tot = lprocfs_oh_sum(&cli->cl_read_rpc_hist);
        write_tot = lprocfs_oh_sum(&cli->cl_write_rpc_hist);

        read_cum = 0;
        write_cum = 0;
        for (i = 0; i < OBD_HIST_MAX; i++) {
                unsigned long r = cli->cl_read_rpc_hist.oh_buckets[i];
                unsigned long w = cli->cl_write_rpc_hist.oh_buckets[i];
                read_cum += r;
                write_cum += w;
                seq_printf(seq, "%d:\t\t%10lu %3lu %3lu   | %10lu %3lu %3lu\n",
                                 i, r, pct(r, read_tot),
                                 pct(read_cum, read_tot), w,
                                 pct(w, write_tot),
                                 pct(write_cum, write_tot));
                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }

        seq_printf(seq, "\nrpc service time: (rpcs, average ms)\n");
        seq_printf(seq, "\tread\t%lu\t%lu.%04lu\n",
                        PRINTF_STIME(&cli->cl_read_stime));
        seq_printf(seq, "\twrite\t%lu\t%lu.%04lu\n\n",
                        PRINTF_STIME(&cli->cl_write_stime));

        seq_printf(seq, "app waiting: (num, average ms)\n");
        seq_printf(seq, "\tenter cache\t%lu\t%lu.%04lu\n",
                        PRINTF_STIME(&cli->cl_enter_stime));

        spin_unlock_irqrestore(&cli->cl_loi_list_lock, flags);

        return 0;
}
#undef pct

static void *osc_rpc_stats_seq_start(struct seq_file *p, loff_t *pos)
{
        if (*pos == 0)
                return (void *)1;
        return NULL;
}
static void *osc_rpc_stats_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        ++*pos;
        return NULL;
}
static void osc_rpc_stats_seq_stop(struct seq_file *p, void *v)
{
}
struct seq_operations osc_rpc_stats_seq_sops = {
        .start = osc_rpc_stats_seq_start,
        .stop = osc_rpc_stats_seq_stop,
        .next = osc_rpc_stats_seq_next,
        .show = osc_rpc_stats_seq_show,
};

static int osc_rpc_stats_seq_open(struct inode *inode, struct file *file)
{
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file *seq;
        int rc;

        rc = seq_open(file, &osc_rpc_stats_seq_sops);
        if (rc)
                return rc;
        seq = file->private_data;
        seq->private = dp->data;
        return 0;
}

static ssize_t osc_rpc_stats_seq_write(struct file *file, const char *buf,
                                       size_t len, loff_t *off)
{
        struct seq_file *seq = file->private_data;
        struct obd_device *dev = seq->private;
        struct client_obd *cli = &dev->u.cli;

        lprocfs_oh_clear(&cli->cl_read_rpc_hist);
        lprocfs_oh_clear(&cli->cl_write_rpc_hist);
        lprocfs_oh_clear(&cli->cl_read_page_hist);
        lprocfs_oh_clear(&cli->cl_write_page_hist);

        memset(&cli->cl_read_stime, 0, sizeof(cli->cl_read_stime));
        memset(&cli->cl_write_stime, 0, sizeof(cli->cl_write_stime));
        memset(&cli->cl_enter_stime, 0, sizeof(cli->cl_enter_stime));

        return len;
}

struct file_operations osc_rpc_stats_fops = {
        .owner   = THIS_MODULE,
        .open    = osc_rpc_stats_seq_open,
        .read    = seq_read,
        .write   = osc_rpc_stats_seq_write,
        .llseek  = seq_lseek,
        .release = seq_release,
};

int lproc_osc_attach_seqstat(struct obd_device *dev)
{
        return lprocfs_obd_seq_create(dev, "rpc_stats", 0444,
                                      &osc_rpc_stats_fops, dev);
}

#endif /* LPROCFS */
LPROCFS_INIT_VARS(osc, lprocfs_module_vars, lprocfs_obd_vars)
