/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
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
 *   Top level header file for LProc SNMP
 *   Author: Hariharan Thantry thantry@users.sourceforge.net
 */
#ifndef _LPROCFS_SNMP_H
#define _LPROCFS_SNMP_H

#if defined(__linux__)
#include <linux/lprocfs_status.h>
#elif defined(__APPLE__)
#include <darwin/lprocfs_status.h>
#elif defined(__WINNT__)
#include <winnt/lprocfs_status.h>
#else
#error Unsupported operating system.
#endif

#undef LPROCFS
#if (defined(__KERNEL__) && defined(CONFIG_PROC_FS))
# define LPROCFS
#endif

struct lprocfs_vars {
        const char   *name;
        cfs_read_proc_t *read_fptr;
        cfs_write_proc_t *write_fptr;
        void *data;
        struct file_operations *fops;
};

struct lprocfs_static_vars {
        struct lprocfs_vars *module_vars;
        struct lprocfs_vars *obd_vars;
};

/* if we find more consumers this could be generalized */
#define OBD_HIST_MAX 32
struct obd_histogram {
        spinlock_t      oh_lock;
        unsigned long   oh_buckets[OBD_HIST_MAX];
};

enum {
        BRW_R_PAGES = 0,
        BRW_W_PAGES,
        BRW_R_RPC_HIST,
        BRW_W_RPC_HIST,
        BRW_R_IO_TIME,
        BRW_W_IO_TIME,
        BRW_R_DISCONT_PAGES,
        BRW_W_DISCONT_PAGES,
        BRW_R_DISCONT_BLOCKS,
        BRW_W_DISCONT_BLOCKS,
        BRW_R_DISK_IOSIZE,
        BRW_W_DISK_IOSIZE,
        BRW_R_DIO_FRAGS,
        BRW_W_DIO_FRAGS,
        BRW_LAST,
};

struct brw_stats {
        struct obd_histogram hist[BRW_LAST];
};


/* An lprocfs counter can be configured using the enum bit masks below.
 *
 * LPROCFS_CNTR_EXTERNALLOCK indicates that an external lock already
 * protects this counter from concurrent updates. If not specified,
 * lprocfs an internal per-counter lock variable. External locks are
 * not used to protect counter increments, but are used to protect
 * counter readout and resets.
 *
 * LPROCFS_CNTR_AVGMINMAX indicates a multi-valued counter samples,
 * (i.e. counter can be incremented by more than "1"). When specified,
 * the counter maintains min, max and sum in addition to a simple
 * invocation count. This allows averages to be be computed.
 * If not specified, the counter is an increment-by-1 counter.
 * min, max, sum, etc. are not maintained.
 *
 * LPROCFS_CNTR_STDDEV indicates that the counter should track sum of
 * squares (for multi-valued counter samples only). This allows
 * external computation of standard deviation, but involves a 64-bit
 * multiply per counter increment.
 */

enum {
        LPROCFS_CNTR_EXTERNALLOCK = 0x0001,
        LPROCFS_CNTR_AVGMINMAX    = 0x0002,
        LPROCFS_CNTR_STDDEV       = 0x0004,

        /* counter data type */
        LPROCFS_TYPE_REGS         = 0x0100,
        LPROCFS_TYPE_BYTES        = 0x0200,
        LPROCFS_TYPE_PAGES        = 0x0400,
        LPROCFS_TYPE_CYCLE        = 0x0800,
};

struct lprocfs_atomic {
        atomic_t               la_entry;
        atomic_t               la_exit;
};

struct lprocfs_counter {
        struct lprocfs_atomic  lc_cntl;  /* may need to move to per set */
        unsigned int           lc_config;
        __s64                  lc_count;
        __s64                  lc_sum;
        __s64                  lc_min;
        __s64                  lc_max;
        __s64                  lc_sumsquare;
        const char            *lc_name;   /* must be static */
        const char            *lc_units;  /* must be static */
};

struct lprocfs_percpu {
        struct lprocfs_counter lp_cntr[0];
};

#define LPROCFS_GET_NUM_CPU 0x0001
#define LPROCFS_GET_SMP_ID  0x0002

enum lprocfs_stats_flags {
        LPROCFS_STATS_FLAG_PERCPU   = 0x0000, /* per cpu counter */
        LPROCFS_STATS_FLAG_NOPERCPU = 0x0001, /* stats have no percpu
                                               * area and need locking */
};

enum lprocfs_fields_flags {
        LPROCFS_FIELDS_FLAGS_CONFIG     = 0x0001,
        LPROCFS_FIELDS_FLAGS_SUM        = 0x0002,
        LPROCFS_FIELDS_FLAGS_MIN        = 0x0003,
        LPROCFS_FIELDS_FLAGS_MAX        = 0x0004,
        LPROCFS_FIELDS_FLAGS_AVG        = 0x0005,
        LPROCFS_FIELDS_FLAGS_SUMSQUARE  = 0x0006,
        LPROCFS_FIELDS_FLAGS_COUNT      = 0x0007,
};

struct lprocfs_stats {
        unsigned int           ls_num;     /* # of counters */
        unsigned int           ls_percpu_size;
        int                    ls_flags; /* See LPROCFS_STATS_FLAG_* */
        spinlock_t             ls_lock;  /* Lock used only when there are
                                          * no percpu stats areas */
        struct lprocfs_percpu *ls_percpu[0];
};


/* class_obd.c */
extern cfs_proc_dir_entry_t *proc_lustre_root;

struct obd_device;
struct file;
struct obd_histogram;

#ifdef LPROCFS

static inline int lprocfs_stats_lock(struct lprocfs_stats *stats, int type)
{
        int rc = 0;

        if (stats->ls_flags & LPROCFS_STATS_FLAG_NOPERCPU) {
                if (type & LPROCFS_GET_NUM_CPU)
                        rc = 1;
                if (type & LPROCFS_GET_SMP_ID)
                        rc = 0;
                spin_lock(&stats->ls_lock);
        } else {
                if (type & LPROCFS_GET_NUM_CPU)
                        rc = num_possible_cpus();
                if (type & LPROCFS_GET_SMP_ID)
                        rc = smp_processor_id();
        }
        return rc;
}

static inline void lprocfs_stats_unlock(struct lprocfs_stats *stats)
{
        if (stats->ls_flags & LPROCFS_STATS_FLAG_NOPERCPU)
                spin_unlock(&stats->ls_lock);
}

/* Two optimized LPROCFS counter increment functions are provided:
 *     lprocfs_counter_incr(cntr, value) - optimized for by-one counters
 *     lprocfs_counter_add(cntr) - use for multi-valued counters
 * Counter data layout allows config flag, counter lock and the
 * count itself to reside within a single cache line.
 */

static inline void lprocfs_counter_add(struct lprocfs_stats *stats, int idx,
                                       long amount)
{
        struct lprocfs_counter *percpu_cntr;
        int smp_id;

        if (stats == NULL)
                return;

        /* With per-client stats, statistics are allocated only for
         * single CPU area, so the smp_id should be 0 always. */
        smp_id = lprocfs_stats_lock(stats, LPROCFS_GET_SMP_ID);

        percpu_cntr = &(stats->ls_percpu[smp_id]->lp_cntr[idx]);
        atomic_inc(&percpu_cntr->lc_cntl.la_entry);
        percpu_cntr->lc_count++;

        if (percpu_cntr->lc_config & LPROCFS_CNTR_AVGMINMAX) {
                percpu_cntr->lc_sum += amount;
                if (percpu_cntr->lc_config & LPROCFS_CNTR_STDDEV)
                        percpu_cntr->lc_sumsquare += (__u64)amount * amount;
                if (amount < percpu_cntr->lc_min)
                        percpu_cntr->lc_min = amount;
                if (amount > percpu_cntr->lc_max)
                        percpu_cntr->lc_max = amount;
        }
        atomic_inc(&percpu_cntr->lc_cntl.la_exit);
        lprocfs_stats_unlock(stats);
}

#define lprocfs_counter_incr(stats, idx) \
        lprocfs_counter_add(stats, idx, 1)

static inline void lprocfs_counter_sub(struct lprocfs_stats *stats, int idx,
                                       long amount)
{
        struct lprocfs_counter *percpu_cntr;
        int smp_id;

        if (stats == NULL)
                return;

        /* With per-client stats, statistics are allocated only for
         * single CPU area, so the smp_id should be 0 always. */
        smp_id = lprocfs_stats_lock(stats, LPROCFS_GET_SMP_ID);

        percpu_cntr = &(stats->ls_percpu[smp_id]->lp_cntr[idx]);
        atomic_inc(&percpu_cntr->lc_cntl.la_entry);
        if (percpu_cntr->lc_config & LPROCFS_CNTR_AVGMINMAX)
                percpu_cntr->lc_sum -= amount;
        atomic_inc(&percpu_cntr->lc_cntl.la_exit);
        lprocfs_stats_unlock(stats);
}
#define lprocfs_counter_decr(stats, idx) \
        lprocfs_counter_sub(stats, idx, 1)

extern __s64 lprocfs_read_helper(struct lprocfs_counter *lc, 
                                 enum lprocfs_fields_flags field);
static inline __u64 lprocfs_stats_collector(struct lprocfs_stats *stats, 
                                            int idx, 
                                            enum lprocfs_fields_flags field)
{
        __u64 ret = 0;
        int i;

        LASSERT(stats != NULL);
        for (i = 0; i < num_possible_cpus(); i++)
                ret += lprocfs_read_helper(&(stats->ls_percpu[i]->lp_cntr[idx]),
                                           field);
        return ret;
}

extern struct lprocfs_stats *lprocfs_alloc_stats(unsigned int num,
                                                 enum lprocfs_stats_flags flags);
extern void lprocfs_clear_stats(struct lprocfs_stats *stats);
extern void lprocfs_free_stats(struct lprocfs_stats **stats);
extern void lprocfs_init_ops_stats(int num_private_stats, 
                                   struct lprocfs_stats *stats);
extern int lprocfs_alloc_obd_stats(struct obd_device *obddev,
                                   unsigned int num_private_stats);
extern void lprocfs_counter_init(struct lprocfs_stats *stats, int index,
                                 unsigned conf, const char *name,
                                 const char *units);
extern void lprocfs_free_obd_stats(struct obd_device *obddev);
struct obd_export;
extern int lprocfs_exp_setup(struct obd_export *exp);
extern int lprocfs_exp_cleanup(struct obd_export *exp);
extern int lprocfs_register_stats(cfs_proc_dir_entry_t *root, const char *name,
                                  struct lprocfs_stats *stats);

/* lprocfs_status.c */
extern int lprocfs_add_vars(cfs_proc_dir_entry_t *root,
                            struct lprocfs_vars *var,
                            void *data);

extern cfs_proc_dir_entry_t *lprocfs_register(const char *name,
                                               cfs_proc_dir_entry_t *parent,
                                               struct lprocfs_vars *list,
                                               void *data);

extern void lprocfs_remove(cfs_proc_dir_entry_t **root);

extern cfs_proc_dir_entry_t *lprocfs_srch(cfs_proc_dir_entry_t *root,
                                           const char *name);

extern int lprocfs_obd_setup(struct obd_device *obd, struct lprocfs_vars *list);
extern int lprocfs_obd_cleanup(struct obd_device *obd);
extern struct file_operations lprocfs_evict_client_fops;

extern int lprocfs_seq_create(cfs_proc_dir_entry_t *parent, char *name, 
                              mode_t mode, struct file_operations *seq_fops,
                              void *data);
extern int lprocfs_obd_seq_create(struct obd_device *dev, char *name,
                                  mode_t mode, struct file_operations *seq_fops,
                                  void *data);

/* Generic callbacks */

extern int lprocfs_rd_u64(char *page, char **start, off_t off,
                          int count, int *eof, void *data);
extern int lprocfs_rd_atomic(char *page, char **start, off_t off,
                             int count, int *eof, void *data);
extern int lprocfs_wr_atomic(struct file *file, const char *buffer,
                             unsigned long count, void *data);
extern int lprocfs_rd_uint(char *page, char **start, off_t off,
                           int count, int *eof, void *data);
extern int lprocfs_wr_uint(struct file *file, const char *buffer,
                           unsigned long count, void *data);
extern int lprocfs_rd_uuid(char *page, char **start, off_t off,
                           int count, int *eof, void *data);
extern int lprocfs_rd_name(char *page, char **start, off_t off,
                           int count, int *eof, void *data);
extern int lprocfs_rd_fstype(char *page, char **start, off_t off,
                             int count, int *eof, void *data);
extern int lprocfs_rd_server_uuid(char *page, char **start, off_t off,
                                  int count, int *eof, void *data);
extern int lprocfs_rd_conn_uuid(char *page, char **start, off_t off,
                                int count, int *eof, void *data);
extern int lprocfs_rd_connect_flags(char *page, char **start, off_t off,
                                    int count, int *eof, void *data);
extern int lprocfs_rd_num_exports(char *page, char **start, off_t off,
                                  int count, int *eof, void *data);
extern int lprocfs_rd_numrefs(char *page, char **start, off_t off,
                              int count, int *eof, void *data);
extern int lprocfs_wr_evict_client(struct file *file, const char *buffer,
                                   unsigned long count, void *data);
extern int lprocfs_wr_ping(struct file *file, const char *buffer,
                           unsigned long count, void *data);

/* Statfs helpers */
extern int lprocfs_rd_blksize(char *page, char **start, off_t off,
                              int count, int *eof, void *data);
extern int lprocfs_rd_kbytestotal(char *page, char **start, off_t off,
                                  int count, int *eof, void *data);
extern int lprocfs_rd_kbytesfree(char *page, char **start, off_t off,
                                 int count, int *eof, void *data);
extern int lprocfs_rd_kbytesavail(char *page, char **start, off_t off,
                                 int count, int *eof, void *data);
extern int lprocfs_rd_filestotal(char *page, char **start, off_t off,
                                 int count, int *eof, void *data);
extern int lprocfs_rd_filesfree(char *page, char **start, off_t off,
                                int count, int *eof, void *data);
extern int lprocfs_rd_filegroups(char *page, char **start, off_t off,
                                 int count, int *eof, void *data);

extern int lprocfs_write_helper(const char *buffer, unsigned long count,
                                int *val);
extern int lprocfs_write_frac_helper(const char *buffer, unsigned long count,
                                     int *val, int mult);
extern int lprocfs_read_frac_helper(char *buffer, unsigned long count, 
                                    long val, int mult);
extern int lprocfs_write_u64_helper(const char *buffer, unsigned long count,
                                    __u64 *val);
extern int lprocfs_write_frac_u64_helper(const char *buffer, unsigned long count,
                                         __u64 *val, int mult);
void lprocfs_oh_tally(struct obd_histogram *oh, unsigned int value);
void lprocfs_oh_tally_log2(struct obd_histogram *oh, unsigned int value);
void lprocfs_oh_clear(struct obd_histogram *oh);
unsigned long lprocfs_oh_sum(struct obd_histogram *oh);

/* lprocfs_status.c: counter read/write functions */
extern int lprocfs_counter_read(char *page, char **start, off_t off,
                                int count, int *eof, void *data);
extern int lprocfs_counter_write(struct file *file, const char *buffer,
                                 unsigned long count, void *data);

/* lprocfs_status.c: recovery status */
int lprocfs_obd_rd_recovery_status(char *page, char **start, off_t off,
                                   int count, int *eof, void *data);

extern int lprocfs_seq_release(struct inode *, struct file *);

/* in lprocfs_stat.c, to protect the private data for proc entries */
extern struct rw_semaphore _lprocfs_lock;
#define LPROCFS_ENTRY()           do {  \
        down_read(&_lprocfs_lock);      \
} while(0)
#define LPROCFS_EXIT()            do {  \
        up_read(&_lprocfs_lock);        \
} while(0)
#define LPROCFS_ENTRY_AND_CHECK(dp) do {        \
        typecheck(struct proc_dir_entry *, dp); \
        LPROCFS_ENTRY();                        \
        if ((dp)->deleted) {                    \
                LPROCFS_EXIT();                 \
                return -ENODEV;                 \
        }                                       \
} while(0)

/* You must use these macros when you want to refer to 
 * the import in a client obd_device for a lprocfs entry */
#define LPROCFS_CLIMP_CHECK(obd) do {           \
        typecheck(struct obd_device *, obd);    \
        mutex_down(&(obd)->u.cli.cl_sem);       \
        if ((obd)->u.cli.cl_import == NULL) {   \
             mutex_up(&(obd)->u.cli.cl_sem);    \
             return -ENODEV;                    \
        }                                       \
} while(0)
#define LPROCFS_CLIMP_EXIT(obd)                 \
        mutex_up(&(obd)->u.cli.cl_sem);


/* write the name##_seq_show function, call LPROC_SEQ_FOPS_RO for read-only 
  proc entries; otherwise, you will define name##_seq_write function also for 
  a read-write proc entry, and then call LPROC_SEQ_SEQ instead. Finally,
  call lprocfs_obd_seq_create(obd, filename, 0444, &name#_fops, data); */
#define __LPROC_SEQ_FOPS(name, custom_seq_write)                           \
static int name##_seq_open(struct inode *inode, struct file *file) {       \
        struct proc_dir_entry *dp = PDE(inode);                            \
        int rc;                                                            \
        LPROCFS_ENTRY_AND_CHECK(dp);                                       \
        rc = single_open(file, name##_seq_show, dp->data);                 \
        if (rc) {                                                          \
                LPROCFS_EXIT();                                            \
                return rc;                                                 \
        }                                                                  \
        return 0;                                                          \
}                                                                          \
struct file_operations name##_fops = {                                     \
        .owner   = THIS_MODULE,                                            \
        .open    = name##_seq_open,                                        \
        .read    = seq_read,                                               \
        .write   = custom_seq_write,                                       \
        .llseek  = seq_lseek,                                              \
        .release = lprocfs_seq_release,                                    \
}

#define LPROC_SEQ_FOPS_RO(name)         __LPROC_SEQ_FOPS(name, NULL)
#define LPROC_SEQ_FOPS(name)            __LPROC_SEQ_FOPS(name, name##_seq_write)

/* lprocfs_status.c: read recovery max time bz13079 */
int lprocfs_obd_rd_recovery_maxtime(char *page, char **start, off_t off,
                                    int count, int *eof, void *data);

/* lprocfs_status.c: write recovery max time bz13079 */
int lprocfs_obd_wr_recovery_maxtime(struct file *file, const char *buffer,
                                    unsigned long count, void *data);
#else
/* LPROCFS is not defined */
static inline void lprocfs_counter_add(struct lprocfs_stats *stats,
                                       int index, long amount) { return; }
static inline void lprocfs_counter_incr(struct lprocfs_stats *stats,
                                        int index) { return; }
static inline void lprocfs_counter_sub(struct lprocfs_stats *stats,
                                       int index, long amount) { return; }
static inline void lprocfs_counter_init(struct lprocfs_stats *stats,
                                        int index, unsigned conf,
                                        const char *name, const char *units)
{ return; }

static inline __u64 lc_read_helper(struct lprocfs_counter *lc, 
                                   enum lprocfs_fields_flags field) 
{ return 0; }

static inline struct lprocfs_stats* lprocfs_alloc_stats(unsigned int num,
                                                        enum lprocfs_stats_flags flags)
{ return NULL; }
static inline void lprocfs_clear_stats(struct lprocfs_stats *stats)
{ return; }
static inline void lprocfs_free_stats(struct lprocfs_stats **stats)
{ return; }
static inline int lprocfs_register_stats(cfs_proc_dir_entry_t *root,
                                            const char *name,
                                            struct lprocfs_stats *stats)
{ return 0; }
static inline void lprocfs_init_ops_stats(int num_private_stats, 
                                          struct lprocfs_stats *stats)
{ return; }
static inline int lprocfs_alloc_obd_stats(struct obd_device *obddev,
                                             unsigned int num_private_stats)
{ return 0; }
static inline void lprocfs_free_obd_stats(struct obd_device *obddev)
{ return; }

struct obd_export;
static inline int lprocfs_exp_setup(struct obd_export *exp)
{ return 0; }
static inline int lprocfs_exp_cleanup(struct obd_export *exp)
{ return 0; }

static inline cfs_proc_dir_entry_t *
lprocfs_register(const char *name, cfs_proc_dir_entry_t *parent,
                 struct lprocfs_vars *list, void *data) { return NULL; }
static inline int lprocfs_add_vars(cfs_proc_dir_entry_t *root,
                                   struct lprocfs_vars *var,
                                   void *data) { return 0; }
static inline void lprocfs_remove(cfs_proc_dir_entry_t **root) {};
static inline cfs_proc_dir_entry_t *lprocfs_srch(cfs_proc_dir_entry_t *head,
                                    const char *name) {return 0;}
static inline int lprocfs_obd_setup(struct obd_device *dev,
                                    struct lprocfs_vars *list) { return 0; }
static inline int lprocfs_obd_cleanup(struct obd_device *dev)  { return 0; }
static inline int lprocfs_rd_u64(char *page, char **start, off_t off,
                                 int count, int *eof, void *data) { return 0; }
static inline int lprocfs_rd_uuid(char *page, char **start, off_t off,
                                  int count, int *eof, void *data) { return 0; }
static inline int lprocfs_rd_name(char *page, char **start, off_t off,
                                  int count, int *eof, void *data) { return 0; }
static inline int lprocfs_rd_server_uuid(char *page, char **start, off_t off,
                                         int count, int *eof, void *data)
{ return 0; }
static inline int lprocfs_rd_conn_uuid(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{ return 0; }
static inline int lprocfs_rd_connect_flags(char *page, char **start, off_t off,
                                           int count, int *eof, void *data)
{ return 0; }
static inline int lprocfs_rd_num_exports(char *page, char **start, off_t off,
                                         int count, int *eof, void *data)
{ return 0; }
static inline int lprocfs_rd_numrefs(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{ return 0; }
static inline int lprocfs_wr_evict_client(struct file *file, const char *buffer,
                                          unsigned long count, void *data)
{ return 0; }
static inline int lprocfs_wr_ping(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{ return 0; }


/* Statfs helpers */
static inline
int lprocfs_rd_blksize(char *page, char **start, off_t off,
                       int count, int *eof, void *data) { return 0; }
static inline
int lprocfs_rd_kbytestotal(char *page, char **start, off_t off,
                           int count, int *eof, void *data) { return 0; }
static inline
int lprocfs_rd_kbytesfree(char *page, char **start, off_t off,
                          int count, int *eof, void *data) { return 0; }
static inline
int lprocfs_rd_kbytesavail(char *page, char **start, off_t off,
                           int count, int *eof, void *data) { return 0; }
static inline
int lprocfs_rd_filestotal(char *page, char **start, off_t off,
                          int count, int *eof, void *data) { return 0; }
static inline
int lprocfs_rd_filesfree(char *page, char **start, off_t off,
                         int count, int *eof, void *data)  { return 0; }
static inline
int lprocfs_rd_filegroups(char *page, char **start, off_t off,
                          int count, int *eof, void *data) { return 0; }
static inline
void lprocfs_oh_tally(struct obd_histogram *oh, unsigned int value) {}
static inline
void lprocfs_oh_tally_log2(struct obd_histogram *oh, unsigned int value) {}
static inline
void lprocfs_oh_clear(struct obd_histogram *oh) {}
static inline
unsigned long lprocfs_oh_sum(struct obd_histogram *oh) { return 0; }
static inline
int lprocfs_counter_read(char *page, char **start, off_t off,
                         int count, int *eof, void *data) { return 0; }
static inline
int lprocfs_counter_write(struct file *file, const char *buffer,
                          unsigned long count, void *data) { return 0; }

static inline
__u64 lprocfs_stats_collector(struct lprocfs_stats *stats, int idx, 
                               enum lprocfs_fields_flags field)
{ return (__u64)0; }

#define LPROCFS_ENTRY()
#define LPROCFS_EXIT()
#define LPROCFS_ENTRY_AND_CHECK(dp)
#define LPROC_SEQ_FOPS_RO(name)
#define LPROC_SEQ_FOPS(name)

#endif /* LPROCFS */

#endif /* LPROCFS_SNMP_H */
