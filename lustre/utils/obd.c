/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Robert Read <rread@clusterfs.com>
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


#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include "obdctl.h"

#include <linux/obd.h>          /* for struct lov_stripe_md */
#include <linux/lustre_build_version.h>

#include <unistd.h>
#include <sys/un.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>

#include <asm/page.h>           /* needed for PAGE_SIZE - rread */

#define __KERNEL__
#include <linux/list.h>
#undef __KERNEL__

#include <linux/obd_class.h>
#include <portals/ptlctl.h>
#include "parser.h"
#include <stdio.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>


#define MAX_STRING_SIZE 128
#define DEVICES_LIST "/proc/fs/lustre/devices"

#define MAX_THREADS 1024
struct shared_data {
        __u64 counters[MAX_THREADS];
        __u64 offsets[MAX_THREADS];
        int   running;
        int   barrier;
        pthread_mutex_t mutex;
        pthread_cond_t  cond;
};
static struct shared_data *shared_data;
static __u64 counter_snapshot[2][MAX_THREADS];
static int prev_valid;
struct timeval prev_time;

static int jt_recording;
static char rawbuf[8192];
static char *buf = rawbuf;
static int max = sizeof(rawbuf);

static int thread;
static int nthreads;

static uint32_t cur_device = MAX_OBD_DEVICES;

union lsm_buffer {
        char                 space [4096];
        struct lov_stripe_md lsm;
} lsm_buffer;

static int l2_ioctl(int dev_id, int opc, void *buf)
{
        return l_ioctl(dev_id, opc, buf);
}

#define IOC_INIT(data)                                                  \
do {                                                                    \
        memset(&data, 0, sizeof(data));                                 \
        data.ioc_dev = cur_device;                                      \
} while (0)

#define IOC_PACK(func, data)                                            \
do {                                                                    \
        memset(buf, 0, sizeof(rawbuf));                                 \
        if (obd_ioctl_pack(&data, &buf, max)) {                         \
                fprintf(stderr, "error: %s: invalid ioctl\n",           \
                        jt_cmdname(func));                                 \
                return -2;                                              \
        }                                                               \
} while (0)

#define IOC_UNPACK(func, data)                                          \
do {                                                                    \
        if (obd_ioctl_unpack(&data, buf, max)) {                        \
                fprintf(stderr, "error: %s: invalid reply\n",           \
                        jt_cmdname(func));                                 \
                return -2;                                              \
        }                                                               \
} while (0)

int obd_record(enum cfg_record_type type, int len, void *ptr)
{
        struct obd_ioctl_data data;

        IOC_INIT(data);
        data.ioc_type = type;
        data.ioc_plen1 = len;
        data.ioc_pbuf1 = ptr;
        IOC_PACK("obd_record", data);

        return  l_ioctl(OBD_DEV_ID, OBD_IOC_DORECORD, &data);
}

int lcfg_ioctl(char * func, int dev_id, struct lustre_cfg *lcfg)
{
        int opc;
        char lcfg_rawbuf[8192];
        char * lcfg_buf= lcfg_rawbuf;
        struct obd_ioctl_data data;
        int len;
        int rc;

        memset(lcfg_buf, 0, sizeof(lcfg_rawbuf));
        if (lustre_cfg_pack(lcfg, &lcfg_buf, sizeof(lcfg_rawbuf), &len)) {
                fprintf(stderr, "error: %s: invalid ioctl\n",
                        jt_cmdname(func));
                return -2;
        }

        IOC_INIT(data);
        data.ioc_type = LUSTRE_CFG_TYPE;
        data.ioc_plen1 = len;
        data.ioc_pbuf1 = lcfg_buf;
        IOC_PACK(func, data);

        if (jt_recording)
                opc = OBD_IOC_DORECORD;
        else
                opc = OBD_IOC_PROCESS_CFG;

        rc =  l_ioctl(dev_id, opc, buf);
        if (rc == 0)
                rc = lustre_cfg_unpack(lcfg, lcfg_buf, sizeof(lcfg_rawbuf));

        return rc;
}

char *obdo_print(struct obdo *obd)
{
        char buf[1024];

        sprintf(buf, "id: "LPX64"\ngrp: "LPX64"\natime: "LPU64"\nmtime: "LPU64
                "\nctime: "LPU64"\nsize: "LPU64"\nblocks: "LPU64
                "\nblksize: %u\nmode: %o\nuid: %d\ngid: %d\nflags: %x\n"
                "misc: %x\nnlink: %d,\nvalid %x\n",
                obd->o_id, obd->o_gr, obd->o_atime, obd->o_mtime, obd->o_ctime,
                obd->o_size, obd->o_blocks, obd->o_blksize, obd->o_mode,
                obd->o_uid, obd->o_gid, obd->o_flags, obd->o_misc,
                obd->o_nlink, obd->o_valid);
        return strdup(buf);
}


#define BAD_VERBOSE (-999999999)

#define N2D_OFF 0x100      /* So we can tell between error codes and devices */

static int do_name2dev(char *func, char *name)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        data.ioc_inllen1 = strlen(name) + 1;
        data.ioc_inlbuf1 = name;

        IOC_PACK(func, data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_NAME2DEV, buf);
        if (rc < 0)
                return errno;
        IOC_UNPACK(func, data);

        return data.ioc_dev + N2D_OFF;
}

/*
 * resolve a device name to a device number.
 * supports a number, $name or %uuid.
 */
int parse_devname(char *func, char *name)
{
        int rc;
        int ret = -1;

        if (!name)
                return ret;
        if (name[0] == '$' || name[0] == '%') {
                name++;
                rc = do_name2dev(func, name);
                if (rc >= N2D_OFF) {
                        ret = rc - N2D_OFF;
                        printf("Name %s is device %d\n", name, ret);
                } else {
                        printf("No device found for name %s: %s\n",
                               name, strerror(rc));
                }
        } else {
                /* Assume it's a number.  This means that bogus strings become
                 * 0.  I might care about that some day. */
                ret = strtoul(name, NULL, 0);
                //printf("Selected device %d\n", ret);
        }

        return ret;
}

static char *
lsm_string (struct lov_stripe_md *lsm)
{
        static char buffer[4096];
        char       *p = buffer;
        int         space = sizeof (buffer);
        int         i;
        int         nob;

        *p = 0;
        space--;

        nob = snprintf(p, space, LPX64, lsm->lsm_object_id);
        p += nob;
        space -= nob;

        if (lsm->lsm_stripe_count != 0) {
                nob = snprintf (p, space, "=%u#%u",
                                lsm->lsm_stripe_size,
                                lsm->lsm_stripe_count);
                p += nob;
                space -= nob;

                for (i = 0; i < lsm->lsm_stripe_count; i++) {
                        nob = snprintf (p, space, "@%u:"LPX64,
                                        lsm->lsm_oinfo[i].loi_ost_idx,
                                        lsm->lsm_oinfo[i].loi_id);
                        p += nob;
                        space -= nob;
                }
        }

        if (space == 0) {                       /* probable overflow */
                fprintf (stderr, "lsm_string() overflowed buffer\n");
                abort ();
        }

        return (buffer);
}

static void
reset_lsmb (union lsm_buffer *lsmb)
{
        memset (lsmb->space, 0, sizeof (lsmb->space));
        lsmb->lsm.lsm_magic = LOV_MAGIC;
}

static int
parse_lsm (union lsm_buffer *lsmb, char *string)
{
        struct lov_stripe_md *lsm = &lsmb->lsm;
        char                 *end;
        int                   i;

        /*
         * object_id[=size#count[@offset:id]*]
         */

        reset_lsmb (lsmb);

        lsm->lsm_object_id = strtoull (string, &end, 0);
        if (end == string)
                return (-1);
        string = end;

        if (*string == 0)
                return (0);

        if (*string != '=')
                return (-1);
        string++;

        lsm->lsm_stripe_size = strtoul (string, &end, 0);
        if (end == string)
                return (-1);
        string = end;

        if (*string != '#')
                return (-1);
        string++;

        lsm->lsm_stripe_count = strtoul (string, &end, 0);
        if (end == string)
                return (-1);
        string = end;

        if (*string == 0)               /* don't have to specify obj ids */
                return (0);

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                if (*string != '@')
                        return (-1);
                string++;
                lsm->lsm_oinfo[i].loi_ost_idx = strtoul(string, &end, 0);
                if (*end != ':')
                        return (-1);
                string = end + 1;
                lsm->lsm_oinfo[i].loi_id = strtoull(string, &end, 0);
                string = end;
        }

        if (*string != 0)
                return (-1);

        return (0);
}

char *jt_cmdname(char *func)
{
        static char buf[512];

        if (thread) {
                sprintf(buf, "%s-%d", func, thread);
                return buf;
        }

        return func;
}

#define difftime(a, b)                                  \
        ((a)->tv_sec - (b)->tv_sec +                    \
         ((a)->tv_usec - (b)->tv_usec) / 1000000.0)

static int be_verbose(int verbose, struct timeval *next_time,
                      __u64 num, __u64 *next_num, int num_total)
{
        struct timeval now;

        if (!verbose)
                return 0;

        if (next_time != NULL)
                gettimeofday(&now, NULL);

        /* A positive verbosity means to print every X iterations */
        if (verbose > 0 &&
            (next_num == NULL || num >= *next_num || num >= num_total)) {
                *next_num += verbose;
                if (next_time) {
                        next_time->tv_sec = now.tv_sec - verbose;
                        next_time->tv_usec = now.tv_usec;
                }
                return 1;
        }

        /* A negative verbosity means to print at most each X seconds */
        if (verbose < 0 && next_time != NULL &&
            difftime(&now, next_time) >= 0.0){
                next_time->tv_sec = now.tv_sec - verbose;
                next_time->tv_usec = now.tv_usec;
                if (next_num)
                        *next_num = num;
                return 1;
        }

        return 0;
}

static int get_verbose(char *func, const char *arg)
{
        int verbose;
        char *end;

        if (!arg || arg[0] == 'v')
                verbose = 1;
        else if (arg[0] == 's' || arg[0] == 'q')
                verbose = 0;
        else {
                verbose = (int)strtoul(arg, &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: bad verbose option '%s'\n",
                                jt_cmdname(func), arg);
                        return BAD_VERBOSE;
                }
        }

        if (verbose < 0)
                printf("Print status every %d seconds\n", -verbose);
        else if (verbose == 1)
                printf("Print status every operation\n");
        else if (verbose > 1)
                printf("Print status every %d operations\n", verbose);

        return verbose;
}

int do_disconnect(char *func, int verbose)
{
        cur_device = MAX_OBD_DEVICES;
        return 0;
}

static void shmem_setup(void)
{
        /* Create new segment */
        int shmid = shmget(IPC_PRIVATE, sizeof(*shared_data), 0600);

        if (shmid == -1) {
                fprintf(stderr, "Can't create shared data: %s\n",
                        strerror(errno));
                return;
        }

        /* Attatch to new segment */
        shared_data = (struct shared_data *)shmat(shmid, NULL, 0);
        
        if (shared_data == (struct shared_data *)(-1)) {
                fprintf(stderr, "Can't attach shared data: %s\n",
                        strerror(errno));
                shared_data = NULL;
                return;
        }

        /* Mark segment as destroyed, so it will disappear when we exit.
         * Forks will inherit attached segments, so we should be OK.
         */
        if (shmctl(shmid, IPC_RMID, NULL) == -1) {
                fprintf(stderr, "Can't destroy shared data: %s\n",
                        strerror(errno));
        }
}

static inline void shmem_reset(int total_threads)
{
        if (shared_data == NULL)
                return;

        memset(shared_data, 0, sizeof(*shared_data));
        pthread_mutex_init(&shared_data->mutex, NULL);
        pthread_cond_init(&shared_data->cond, NULL);

        memset(counter_snapshot, 0, sizeof(counter_snapshot));
        prev_valid = 0;
        shared_data->barrier = total_threads;
}

static inline void shmem_bump(void)
{
        static int bumped_running;

        if (shared_data == NULL || thread <= 0 || thread > MAX_THREADS)
                return;
        pthread_mutex_lock(&shared_data->mutex);
        shared_data->counters[thread - 1]++;
        if (!bumped_running)
                shared_data->running++;
        pthread_mutex_unlock(&shared_data->mutex);
        bumped_running = 1;
}

static void shmem_snap(int total_threads, int live_threads)
{
        struct timeval this_time;
        int non_zero = 0;
        __u64 total = 0;
        double secs;
        int running;
        int i;

        if (shared_data == NULL || total_threads > MAX_THREADS)
                return;
        
        pthread_mutex_lock(&shared_data->mutex);
        memcpy(counter_snapshot[0], shared_data->counters,
               total_threads * sizeof(counter_snapshot[0][0]));
        running = shared_data->running;
        pthread_mutex_unlock(&shared_data->mutex);

        gettimeofday(&this_time, NULL);

        for (i = 0; i < total_threads; i++) {
                long long this_count =
                        counter_snapshot[0][i] - counter_snapshot[1][i];

                if (this_count != 0) {
                        non_zero++;
                        total += this_count;
                }
        }

        secs = (this_time.tv_sec + this_time.tv_usec / 1000000.0) -
                (prev_time.tv_sec + prev_time.tv_usec / 1000000.0);

        if (prev_valid &&
            live_threads == total_threads &&
            secs > 0.0)                    /* someone screwed with the time? */
                printf("%d/%d Total: %f/second\n", non_zero, total_threads, total / secs);
                                                                                                                                                                                                     
        memcpy(counter_snapshot[1], counter_snapshot[0],
               total_threads * sizeof(counter_snapshot[0][0]));
        prev_time = this_time;
        if (!prev_valid &&
            running == total_threads)
                prev_valid = 1;
}

extern command_t cmdlist[];

static int do_device(char *func, char *devname)
{
        struct obd_ioctl_data data;
        int dev;

        memset(&data, 0, sizeof(data));

        dev = parse_devname(func, devname);
        if (dev < 0)
                return -1;

        cur_device = dev;
        return 0;
}

int jt_obd_device(int argc, char **argv)
{
        int rc;
        do_disconnect(argv[0], 1);

        if (argc != 2)
                return CMD_HELP;

        rc = do_device(argv[0], argv[1]);
        return rc;
}

int jt_obd_connect(int argc, char **argv)
{
        return 0;
}

int jt_obd_disconnect(int argc, char **argv)
{
        if (argc != 1)
                return CMD_HELP;

        return do_disconnect(argv[0], 0);
}

int jt_opt_device(int argc, char **argv)
{
        int ret;
        int rc;

        if (argc < 3)
                return CMD_HELP;

        rc = do_device("device", argv[1]);

        if (!rc)
                rc = Parser_execarg(argc - 2, argv + 2, cmdlist);

        ret = do_disconnect(argv[0], 0);
        if (!rc)
                rc = ret;

        return rc;
}

static void parent_sighandler (int sig)
{
        return;
}

int jt_opt_threads(int argc, char **argv)
{
        sigset_t         saveset;
        sigset_t         sigset;
        struct sigaction sigact;
        struct sigaction saveact1;
        struct sigaction saveact2;
        __u64 threads, next_thread;
        int verbose;
        int rc = 0;
        char *end;
        int i;

        if (argc < 5)
                return CMD_HELP;

        threads = strtoull(argv[1], &end, 0);
        if (*end || threads > MAX_THREADS) {
                fprintf(stderr, "error: %s: invalid thread count '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        verbose = get_verbose(argv[0], argv[2]);
        if (verbose == BAD_VERBOSE)
                return CMD_HELP;

        if (verbose != 0)
                printf("%s: starting "LPD64" threads on device %s running %s\n",
                       argv[0], threads, argv[3], argv[4]);

        shmem_reset(threads);

        sigemptyset(&sigset);
        sigaddset(&sigset, SIGALRM);
        sigaddset(&sigset, SIGCHLD);
        sigprocmask(SIG_BLOCK, &sigset, &saveset);

        nthreads = threads;


        for (i = 1, next_thread = verbose; i <= threads; i++) {
                rc = fork();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #%d - %s\n", argv[0], i,
                                strerror(rc = errno));
                        break;
                } else if (rc == 0) {
                        sigprocmask(SIG_SETMASK, &saveset, NULL);
                        thread = i;
                        argv[2] = "--device";
                        return jt_opt_device(argc - 2, argv + 2);
                } else if (be_verbose(verbose, NULL, i, &next_thread, threads))
                        printf("%s: thread #%d (PID %d) started\n",
                               argv[0], i, rc);
                rc = 0;
        }

        if (!thread) {          /* parent process */
                int live_threads = threads;

                sigemptyset(&sigset);
                sigemptyset(&sigact.sa_mask);
                sigact.sa_handler = parent_sighandler;
                sigact.sa_flags = 0;

                sigaction(SIGALRM, &sigact, &saveact1);
                sigaction(SIGCHLD, &sigact, &saveact2);

                while (live_threads > 0) {
                        int status;
                        pid_t ret;

                        if (verbose < 0)        /* periodic stats */
                                alarm(-verbose);

                        sigsuspend(&sigset);
                        alarm(0);

                        while (live_threads > 0) {
                                ret = waitpid(0, &status, WNOHANG);
                                if (ret == 0)
                                        break;

                                if (ret < 0) {
                                        fprintf(stderr, "error: %s: wait - %s\n",
                                                argv[0], strerror(errno));
                                        if (!rc)
                                                rc = errno;
                                        continue;
                                } else {
                                        /*
                                         * This is a hack.  We _should_ be able
                                         * to use WIFEXITED(status) to see if
                                         * there was an error, but it appears
                                         * to be broken and it always returns 1
                                         * (OK).  See wait(2).
                                         */
                                        int err = WEXITSTATUS(status);
                                        if (err || WIFSIGNALED(status))
                                                fprintf(stderr,
                                                        "%s: PID %d had rc=%d\n",
                                                        argv[0], ret, err);
                                        if (!rc)
                                                rc = err;
                                        live_threads--;
                                }
                        }
                        /* Show stats while all threads running */
                        if (verbose < 0)
                                shmem_snap(threads, live_threads);
                }
                sigaction(SIGCHLD, &saveact2, NULL);
                sigaction(SIGALRM, &saveact1, NULL);
        }
        sigprocmask(SIG_SETMASK, &saveset, NULL);
        return rc;
}

int jt_opt_net(int argc, char **argv)
{
        char *arg2[3];
        int rc;

        if (argc < 3)
                return CMD_HELP;

        arg2[0] = argv[0];
        arg2[1] = argv[1];
        arg2[2] = NULL;
        rc = jt_ptl_network (2, arg2);

        if (!rc)
                rc = Parser_execarg(argc - 2, argv + 2, cmdlist);

        return rc;
}

int jt_obd_no_transno(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_NO_TRANSNO, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}

int jt_obd_set_readonly(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_SET_READONLY, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}

int jt_obd_abort_recovery(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_ABORT_RECOVERY, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}
static int is_number(char *str)
{
        int i, len;

        if (*str == '-' || *str == '+')
                str++;

        if (!strncmp(str, "0x", 2))
                str += 2;

        len = strlen(str);
        if (!len)
                return 0;

        for (i = 0; i < len; i++)
                if (!isdigit(str[i]))
                        return 0;
        return 1;
}

static int str2ugid(char *str, uint32_t *uid, uint32_t *gid)
{
        struct passwd *pwd;
        struct group  *grp;
        char *p;

        p = strchr(str, ':');
        if (!p)
                return -1;
        *p++ = 0;

        pwd = getpwnam(str);
        if (pwd)
                *uid = pwd->pw_uid;
        else {
                if (!is_number(str))
                        return -1;
                *uid = atoi(str);
        }

        grp = getgrnam(p);
        if (grp)
                *gid = grp->gr_gid;
        else {
                if (!is_number(p))
                        return -1;
                *gid = atoi(p);
        }
        return 0;
}

static void ugid2str(char *uidname, char *gidname, uint32_t uid, uint32_t gid)
{
        struct passwd *pwd;
        struct group  *grp;

        pwd = getpwuid(uid);
        if (pwd)
                snprintf(uidname, 128, "%s", pwd->pw_name);
        else
                snprintf(uidname, 128, "%d", uid);

        grp = getgrgid(gid);
        if (grp)
                snprintf(gidname, 128, "%s", grp->gr_name);
        else
                snprintf(uidname, 128, "%d", gid);
}

#define SQUASH_IOC_SIZE (4 * 4 + sizeof(ptl_nid_t))
int jt_obd_root_squash(int argc, char **argv)
{
        struct obd_ioctl_data   data;
        char                    mybuf[SQUASH_IOC_SIZE];
        uint32_t                *dir, *uid, *gid;
        ptl_nid_t               *nid;
        int                     rc;

        IOC_INIT(data);

        if (argc > 3)
                return CMD_HELP;

        memset(mybuf, 0, sizeof(mybuf));
        dir = (uint32_t *) mybuf;
        uid = dir + 2;
        gid = dir + 3;
        nid = (ptl_nid_t *) (dir + 4);

        if (argc == 1) {
                *dir = 0;
        } else {
                *dir = 1;
                if (str2ugid(argv[1], uid, gid)) {
                        fprintf(stderr, "error: %s: can't parse ugid %s\n",
                                jt_cmdname(argv[0]), argv[1]);
                        return -1;
                }
                if (argc == 3) {
                        if (ptl_parse_nid(nid, argv[2])) {
                                fprintf(stderr,
                                        "error: %s: can't parse nid %s\n",
                                        jt_cmdname(argv[0]), argv[2]);
                                return -1;
                        }
                } else
                        *nid = 0;
        }

        data.ioc_inllen1= SQUASH_IOC_SIZE;
        data.ioc_inlbuf1 = mybuf;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_ROOT_SQUASH, buf);
        IOC_UNPACK(argv[0], data);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));
        else if (argc == 1) {
                char uidname[128], gidname[128];
                char nidstr[256] = {0, };

                ugid2str(uidname, gidname, *uid, *gid);
                ptl_nid2str(nidstr, *nid);
                printf("squash (root:root) => (%s:%s), except nid %s\n",
                        uidname, gidname, nidstr);
        }

        return rc;
}

int jt_get_version(int argc, char **argv)
{
        int rc;
        char buf[8192];
        struct obd_ioctl_data *data = (struct obd_ioctl_data *)buf;

        if (argc != 1)
                return CMD_HELP;

        memset(buf, 0, sizeof(buf));
        data->ioc_version = OBD_IOCTL_VERSION;
        data->ioc_inllen1 = sizeof(buf) - size_round(sizeof(*data));
        data->ioc_len = obd_ioctl_packlen(data);

        rc = l2_ioctl(OBD_DEV_ID, OBD_GET_VERSION, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));
        else {
                printf("Lustre version: %s\n", data->ioc_bulk);
        }

        printf("lctl   version: %s\n", BUILD_VERSION);
        return rc;
}

int jt_obd_list(int argc, char **argv)
{
        int rc;
        char buf[MAX_STRING_SIZE];
        FILE *fp = fopen(DEVICES_LIST, "r");

        if (fp == NULL) {
                fprintf(stderr, "error: %s: %s could not open file "
                        DEVICES_LIST " .\n",
                        jt_cmdname(argv[0]), strerror(rc =  errno));
                return rc;
        }

        if (argc != 1)
                return CMD_HELP;

        while (fgets(buf, sizeof(buf), fp) != NULL)
                printf("%s", buf);

        fclose(fp);

        return 0;
}

/* Get echo client's stripe meta-data for the given object
 */
int jt_obd_get_stripe (int argc, char **argv)
{
        struct obd_ioctl_data data;
        __u64 id;
        int   rc;
        char *end;

        if (argc != 2)
                return (CMD_HELP);

        id = strtoull (argv[1], &end, 0);
        if (*end) {
                fprintf (stderr, "Error: %s: invalid object id '%s'\n",
                         jt_cmdname (argv[0]), argv[1]);
                return (CMD_HELP);
        }

        memset (&lsm_buffer, 0, sizeof (lsm_buffer));

        IOC_INIT (data);
        data.ioc_obdo1.o_id = id;
        data.ioc_obdo1.o_mode = S_IFREG | 0644;
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLMODE;
        data.ioc_pbuf1 = (char *)&lsm_buffer;
        data.ioc_plen1 = sizeof (lsm_buffer);

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, ECHO_IOC_GET_STRIPE, buf);
        IOC_UNPACK(argv[0], data);

        if (rc != 0) {
                fprintf (stderr, "Error: %s: rc %d(%s)\n",
                         jt_cmdname (argv[0]), rc, strerror (errno));
                return (rc);
        }

        printf ("%s\n", lsm_string (&lsm_buffer.lsm));

        return (rc);
}

/* Set stripe meta-data for 1 or more objects.  Object must be new to
 * this echo client instance.
 */
int jt_obd_set_stripe (int argc, char **argv)
{
        struct obd_ioctl_data data;
        char *end;
        int count = 1;
        int i;
        int rc;

        if (argc < 2 || argc > 3)
                return CMD_HELP;

        rc = parse_lsm (&lsm_buffer, argv[1]);
        if (rc != 0) {
                fprintf (stderr, "error: %s: invalid object '%s'\n",
                         jt_cmdname (argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc > 2) {
                count = strtol (argv[2], &end, 0);
                if (*end != 0) {
                        fprintf (stderr, "error: %s: invalid count '%s'\n",
                                 jt_cmdname (argv[0]), argv[1]);
                        return CMD_HELP;
                }
        }

        for (i = 0; i < count; i++) {
                IOC_INIT (data);
                data.ioc_obdo1.o_id = lsm_buffer.lsm.lsm_object_id + i;
                data.ioc_obdo1.o_mode = S_IFREG | 0644;
                data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLMODE;
                data.ioc_pbuf1 = (char *)&lsm_buffer;
                data.ioc_plen1 = sizeof (lsm_buffer);

                IOC_PACK (argv[0], data);
                rc = l2_ioctl (OBD_DEV_ID, ECHO_IOC_SET_STRIPE, buf);
                IOC_UNPACK (argv[0], data);

                if (rc != 0) {
                        fprintf (stderr, "Error: %s: rc %d(%s)\n",
                                 jt_cmdname (argv[0]), rc, strerror (errno));
                        return (rc);
                }
        }

        return (0);
}

/* Clear stripe meta-data info for an object on this echo-client instance
 */
int jt_obd_unset_stripe (int argc, char **argv)
{
        struct obd_ioctl_data data;
        char *end;
        obd_id id;
        int rc;

        if (argc != 2)
                return CMD_HELP;

        id = strtoull (argv[1], &end, 0);
        if (*end != 0) {
                fprintf (stderr, "error: %s: invalid object id '%s'\n",
                         jt_cmdname (argv[0]), argv[1]);
                return CMD_HELP;
        }

        IOC_INIT (data);
        data.ioc_obdo1.o_id = id;
        data.ioc_obdo1.o_mode = S_IFREG | 0644;
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLMODE;

        IOC_PACK (argv[0], data);
        rc = l2_ioctl (OBD_DEV_ID, ECHO_IOC_SET_STRIPE, buf);
        IOC_UNPACK (argv[0], data);

        if (rc != 0)
                fprintf (stderr, "Error: %s: rc %d(%s)\n",
                         jt_cmdname (argv[0]), rc, strerror (errno));

        return (0);
}

/* Create one or more objects, arg[4] may describe stripe meta-data.  If
 * not, defaults assumed.  This echo-client instance stashes the stripe
 * object ids.  Use get_stripe on this node to print full lsm and
 * set_stripe on another node to cut/paste between nodes.
 */
int jt_obd_create(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct timeval next_time;
        __u64 count = 1, next_count, base_id = 0;
        int verbose = 1, mode = 0100644, rc = 0, i, valid_lsm = 0;
        char *end;

        IOC_INIT(data);
        if (argc < 2 || argc > 5)
                return CMD_HELP;

        count = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid iteration count '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc > 2) {
                mode = strtoul(argv[2], &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: invalid mode '%s'\n",
                                jt_cmdname(argv[0]), argv[2]);
                        return CMD_HELP;
                }
                if (!(mode & S_IFMT))
                        mode |= S_IFREG;
        }

        if (argc > 3) {
                verbose = get_verbose(argv[0], argv[3]);
                if (verbose == BAD_VERBOSE)
                        return CMD_HELP;
        }

        if (argc < 5)
                reset_lsmb (&lsm_buffer);       /* will set default */
        else {
                rc = parse_lsm (&lsm_buffer, argv[4]);
                if (rc != 0) {
                        fprintf(stderr, "error: %s: invalid lsm '%s'\n",
                                jt_cmdname(argv[0]), argv[4]);
                        return CMD_HELP;
                }
                base_id = lsm_buffer.lsm.lsm_object_id;
                valid_lsm = 1;
        }

        printf("%s: "LPD64" objects\n", jt_cmdname(argv[0]), count);
        gettimeofday(&next_time, NULL);
        next_time.tv_sec -= verbose;

        for (i = 1, next_count = verbose; i <= count; i++) {
                data.ioc_obdo1.o_mode = mode;
                data.ioc_obdo1.o_id = base_id;
                data.ioc_obdo1.o_uid = 0;
                data.ioc_obdo1.o_gid = 0;
                data.ioc_obdo1.o_valid = OBD_MD_FLTYPE | OBD_MD_FLMODE |
                        OBD_MD_FLID | OBD_MD_FLUID | OBD_MD_FLGID;

                if (valid_lsm) {
                        data.ioc_plen1 = sizeof lsm_buffer;
                        data.ioc_pbuf1 = (char *)&lsm_buffer;
                }

                IOC_PACK(argv[0], data);
                rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_CREATE, buf);
                IOC_UNPACK(argv[0], data);
                shmem_bump();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #%d - %s\n",
                                jt_cmdname(argv[0]), i, strerror(rc = errno));
                        break;
                }
                if (!(data.ioc_obdo1.o_valid & OBD_MD_FLID)) {
                        fprintf(stderr, "error: %s: objid not valid #%d:%08x\n",
                                jt_cmdname(argv[0]), i, data.ioc_obdo1.o_valid);
                        rc = EINVAL;
                        break;
                }

                if (be_verbose(verbose, &next_time, i, &next_count, count))
                        printf("%s: #%d is object id "LPX64"\n",
                                jt_cmdname(argv[0]), i, data.ioc_obdo1.o_id);
        }
        return rc;
}

int jt_obd_setattr(int argc, char **argv)
{
        struct obd_ioctl_data data;
        char *end;
        int rc;

        IOC_INIT(data);
        if (argc != 2)
                return CMD_HELP;

        data.ioc_obdo1.o_id = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid objid '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }
        data.ioc_obdo1.o_mode = S_IFREG | strtoul(argv[2], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid mode '%s'\n",
                        jt_cmdname(argv[0]), argv[2]);
                return CMD_HELP;
        }
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLMODE;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_SETATTR, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}

int jt_obd_test_setattr(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct timeval start, next_time;
        __u64 i, count, next_count;
        int verbose = 1;
        obd_id objid = 3;
        char *end;
        int rc = 0;

        if (argc < 2 && argc > 4)
                return CMD_HELP;

        IOC_INIT(data);
        count = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid iteration count '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc >= 3) {
                verbose = get_verbose(argv[0], argv[2]);
                if (verbose == BAD_VERBOSE)
                        return CMD_HELP;
        }

        if (argc >= 4) {
                if (argv[3][0] == 't') {
                        objid = strtoull(argv[3] + 1, &end, 0);
                        if (thread)
                                objid += thread - 1;
                } else
                        objid = strtoull(argv[3], &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: invalid objid '%s'\n",
                                jt_cmdname(argv[0]), argv[3]);
                        return CMD_HELP;
                }
        }

        gettimeofday(&start, NULL);
        next_time.tv_sec = start.tv_sec - verbose;
        next_time.tv_usec = start.tv_usec;
        if (verbose != 0)
                printf("%s: setting "LPD64" attrs (objid "LPX64"): %s",
                       jt_cmdname(argv[0]), count, objid, ctime(&start.tv_sec));

        for (i = 1, next_count = verbose; i <= count; i++) {
                data.ioc_obdo1.o_id = objid;
                data.ioc_obdo1.o_mode = S_IFREG;
                data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLMODE;
                IOC_PACK(argv[0], data);
                rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_SETATTR, &data);
                shmem_bump();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #"LPD64" - %d:%s\n",
                                jt_cmdname(argv[0]), i, errno, strerror(rc = errno));
                        break;
                } else {
                        if (be_verbose
                            (verbose, &next_time, i, &next_count, count))
                                printf("%s: set attr #"LPD64"\n",
                                       jt_cmdname(argv[0]), i);
                }
        }

        if (!rc) {
                struct timeval end;
                double diff;

                gettimeofday(&end, NULL);

                diff = difftime(&end, &start);

                --i;
                if (verbose != 0)
                        printf("%s: "LPD64" attrs in %.3fs (%.3f attr/s): %s",
                               jt_cmdname(argv[0]), i, diff, i / diff,
                               ctime(&end.tv_sec));
        }
        return rc;
}


int jt_obd_destroy(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct timeval next_time;
        __u64 count = 1, next_count;
        int verbose = 1;
        __u64 id;
        char *end;
        int rc = 0, i;

        IOC_INIT(data);
        if (argc < 2 || argc > 4)
                return CMD_HELP;

        id = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid objid '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }
        if (argc > 2) {
                count = strtoull(argv[2], &end, 0);
                if (*end) {
                        fprintf(stderr,
                                "error: %s: invalid iteration count '%s'\n",
                                jt_cmdname(argv[0]), argv[2]);
                        return CMD_HELP;
                }
        }

        if (argc > 3) {
                verbose = get_verbose(argv[0], argv[3]);
                if (verbose == BAD_VERBOSE)
                        return CMD_HELP;
        }

        printf("%s: "LPD64" objects\n", jt_cmdname(argv[0]), count);
        gettimeofday(&next_time, NULL);
        next_time.tv_sec -= verbose;

        for (i = 1, next_count = verbose; i <= count; i++, id++) {
                data.ioc_obdo1.o_id = id;
                data.ioc_obdo1.o_mode = S_IFREG | 0644;
                data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLMODE;

                IOC_PACK(argv[0], data);
                rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_DESTROY, buf);
                IOC_UNPACK(argv[0], data);
                shmem_bump();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: objid "LPX64": %s\n",
                                jt_cmdname(argv[0]), id, strerror(rc = errno));
                        break;
                }

                if (be_verbose(verbose, &next_time, i, &next_count, count))
                        printf("%s: #%d is object id "LPX64"\n",
                               jt_cmdname(argv[0]), i, id);
        }

        return rc;
}

int jt_obd_getattr(int argc, char **argv)
{
        struct obd_ioctl_data data;
        char *end;
        int rc;

        if (argc != 2)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_obdo1.o_id = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid objid '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }
        /* to help obd filter */
        data.ioc_obdo1.o_mode = 0100644;
        data.ioc_obdo1.o_valid = 0xffffffff;
        printf("%s: object id "LPX64"\n", jt_cmdname(argv[0]),data.ioc_obdo1.o_id);

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_GETATTR, buf);
        IOC_UNPACK(argv[0], data);
        if (rc) {
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));
        } else {
                printf("%s: object id "LPX64", mode %o\n", jt_cmdname(argv[0]),
                       data.ioc_obdo1.o_id, data.ioc_obdo1.o_mode);
        }
        return rc;
}

int jt_obd_test_getattr(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct timeval start, next_time;
        __u64 i, count, next_count;
        int verbose = 1;
        obd_id objid = 3;
        char *end;
        int rc = 0;

        if (argc < 2 || argc > 4)
                return CMD_HELP;

        IOC_INIT(data);
        count = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid iteration count '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc >= 3) {
                verbose = get_verbose(argv[0], argv[2]);
                if (verbose == BAD_VERBOSE)
                        return CMD_HELP;
        }

        if (argc >= 4) {
                if (argv[3][0] == 't') {
                        objid = strtoull(argv[3] + 1, &end, 0);
                        if (thread)
                                objid += thread - 1;
                } else
                        objid = strtoull(argv[3], &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: invalid objid '%s'\n",
                                jt_cmdname(argv[0]), argv[3]);
                        return CMD_HELP;
                }
        }

        gettimeofday(&start, NULL);
        next_time.tv_sec = start.tv_sec - verbose;
        next_time.tv_usec = start.tv_usec;
        if (verbose != 0)
                printf("%s: getting "LPD64" attrs (objid "LPX64"): %s",
                       jt_cmdname(argv[0]), count, objid, ctime(&start.tv_sec));

        for (i = 1, next_count = verbose; i <= count; i++) {
                data.ioc_obdo1.o_id = objid;
                data.ioc_obdo1.o_mode = S_IFREG;
                data.ioc_obdo1.o_valid = 0xffffffff;
                IOC_PACK(argv[0], data);
                rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_GETATTR, &data);
                shmem_bump();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #"LPD64" - %d:%s\n",
                                jt_cmdname(argv[0]), i, errno, strerror(rc = errno));
                        break;
                } else {
                        if (be_verbose
                            (verbose, &next_time, i, &next_count, count))
                                printf("%s: got attr #"LPD64"\n",
                                       jt_cmdname(argv[0]), i);
                }
        }

        if (!rc) {
                struct timeval end;
                double diff;

                gettimeofday(&end, NULL);

                diff = difftime(&end, &start);

                --i;
                if (verbose != 0)
                        printf("%s: "LPD64" attrs in %.3fs (%.3f attr/s): %s",
                               jt_cmdname(argv[0]), i, diff, i / diff,
                               ctime(&end.tv_sec));
        }
        return rc;
}

int jt_obd_test_brw(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct timeval start, next_time;
        __u64 count, next_count, len, stride, thr_offset = 0, objid = 3;
        int write = 0, verbose = 1, cmd, i, rc = 0, pages = 1;
        long n;
        int repeat_offset = 0;
        unsigned long long ull;
        int  nthr_per_obj = 0;
        int  verify = 1;
        int  obj_idx = 0;
        char *end;

        if (argc < 2 || argc > 7) {
                fprintf(stderr, "error: %s: bad number of arguments: %d\n",
                        jt_cmdname(argv[0]), argc);
                return CMD_HELP;
        }

        /* make each thread write to a different offset */
        count = strtoull(argv[1], &end, 0);
 
        if (*end) {
                fprintf(stderr, "error: %s: bad iteration count '%s'\n",
                        jt_cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc >= 3) {
                if (argv[2][0] == 'w' || argv[2][0] == '1')
                        write = 1;
                /* else it's a read */
                if (argv[2][0] != 0)
                        for (i = 1; argv[2][i] != 0; i++)
                                switch (argv[2][i]) {
                                case 'r':
                                        repeat_offset = 1;
                                        break;

                                case 'x':
                                        verify = 0;
                                        break;

                                default:
                                        fprintf (stderr, "Can't parse cmd '%s'\n",
                                                 argv[2]);
                                        return CMD_HELP;
                                }
        }

        if (argc >= 4) {
                verbose = get_verbose(argv[0], argv[3]);
                if (verbose == BAD_VERBOSE)
                        return CMD_HELP;
        }

        if (argc >= 5) {
                pages = strtoul(argv[4], &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: bad page count '%s'\n",
                                jt_cmdname(argv[0]), argv[4]);
                        return CMD_HELP;
                }
        }
        if (argc >= 6) {
                if (thread &&
                    (n = strtol(argv[5], &end, 0)) > 0 &&
                    *end == 't' &&
                    (ull = strtoull(end + 1, &end, 0)) > 0 &&
                    *end == 0) {
                        nthr_per_obj = n;
                        objid = ull;
                } else if (thread &&
                           argv[5][0] == 't') {
                        nthr_per_obj = 1;
                        objid = strtoull(argv[5] + 1, &end, 0);
                } else {
                        nthr_per_obj = 0;
                        objid = strtoull(argv[5], &end, 0);
                }
                if (*end) {
                        fprintf(stderr, "error: %s: bad objid '%s'\n",
                                jt_cmdname(argv[0]), argv[5]);
                        return CMD_HELP;
                }
        }

        IOC_INIT(data);

        /* communicate the 'type' of brw test and batching to echo_client.
         * don't start.  we'd love to refactor this lctl->echo_client
         * interface */
        data.ioc_pbuf1 = (void *)1;
        data.ioc_plen1 = 1;

        if (argc >= 7) {
                switch(argv[6][0]) {
                        case 'g': /* plug and unplug */
                                data.ioc_pbuf1 = (void *)2;
                                data.ioc_plen1 = strtoull(argv[6] + 1, &end,
                                                          0);
                                break;
                        case 'p': /* prep and commit */
                                data.ioc_pbuf1 = (void *)3;
                                data.ioc_plen1 = strtoull(argv[6] + 1, &end,
                                                          0);
                                break;
                        default:
                                fprintf(stderr, "error: %s: batching '%s' "
                                        "needs to specify 'p' or 'g'\n",
                                        jt_cmdname(argv[0]), argv[6]);
                                return CMD_HELP;
                }

                if (*end) {
                        fprintf(stderr, "error: %s: bad batching '%s'\n",
                                jt_cmdname(argv[0]), argv[6]);
                        return CMD_HELP;
                }
                data.ioc_plen1 *= PAGE_SIZE;
        }

        len = pages * PAGE_SIZE;
        stride = len;

        if (thread) {
                pthread_mutex_lock (&shared_data->mutex);
                if (nthr_per_obj != 0) {
                        /* threads interleave */
                        obj_idx = (thread - 1)/nthr_per_obj;
                        objid += obj_idx;
                        stride *= nthr_per_obj;
                        thr_offset = ((thread - 1) % nthr_per_obj) * len;
                        if (thr_offset == 0)
                                shared_data->offsets[obj_idx] = stride;
                } else {
                        /* threads disjoint */
                        thr_offset = (thread - 1) * len;
                }

                shared_data->barrier--;
                if (shared_data->barrier == 0)
                        pthread_cond_broadcast(&shared_data->cond);
                else
                        pthread_cond_wait(&shared_data->cond,
                                          &shared_data->mutex);

                pthread_mutex_unlock (&shared_data->mutex);
        }


        data.ioc_obdo1.o_id = objid;
        data.ioc_obdo1.o_mode = S_IFREG;
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLMODE | OBD_MD_FLFLAGS;
        data.ioc_obdo1.o_flags = (verify ? OBD_FL_DEBUG_CHECK : 0);

        data.ioc_count = len;
        data.ioc_offset = (repeat_offset ? 0 : thr_offset);

        gettimeofday(&start, NULL);
        next_time.tv_sec = start.tv_sec - verbose;
        next_time.tv_usec = start.tv_usec;

        if (verbose != 0)
                printf("%s: %s "LPU64"x%d pages (obj "LPX64", off "LPU64"): %s",
                       jt_cmdname(argv[0]), write ? "writing" : "reading", count,
                       pages, objid, data.ioc_offset, ctime(&start.tv_sec));

        cmd = write ? OBD_IOC_BRW_WRITE : OBD_IOC_BRW_READ;
        for (i = 1, next_count = verbose; i <= count; i++) {
                data.ioc_obdo1.o_valid &= ~(OBD_MD_FLBLOCKS|OBD_MD_FLGRANT);
                IOC_PACK(argv[0], data);
                rc = l2_ioctl(OBD_DEV_ID, cmd, buf);
                shmem_bump();
                if (rc) {
                        fprintf(stderr, "error: %s: #%d - %s on %s\n",
                                jt_cmdname(argv[0]), i, strerror(rc = errno),
                                write ? "write" : "read");
                        break;
                } else if (be_verbose(verbose, &next_time,i, &next_count,count))
                        printf("%s: %s number %d @ "LPD64":"LPU64" for %d\n",
                               jt_cmdname(argv[0]), write ? "write" : "read", i,
                               data.ioc_obdo1.o_id, data.ioc_offset,
                               (int)(pages * PAGE_SIZE));

                if (!repeat_offset) {
                        if (stride == len) {
                                data.ioc_offset += stride;
                        } else if (i < count) {
                                pthread_mutex_lock (&shared_data->mutex);
                                data.ioc_offset = shared_data->offsets[obj_idx];
                                shared_data->offsets[obj_idx] += len;
                                pthread_mutex_unlock (&shared_data->mutex);
                        }
                }

        }

        if (!rc) {
                struct timeval end;
                double diff;

                gettimeofday(&end, NULL);

                diff = difftime(&end, &start);

                --i;
                if (verbose != 0)
                        printf("%s: %s %dx%d pages in %.3fs (%.3f MB/s): %s",
                               jt_cmdname(argv[0]), write ? "wrote" : "read",
                               i, pages, diff,
                               ((double)i * pages * getpagesize()) /
                               (diff * 1048576.0),
                               ctime(&end.tv_sec));
        }

        return rc;
}

int jt_obd_lov_getconfig(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct lov_desc desc;
        struct obd_uuid *uuidarray;
        __u32 *obdgens;
        char *path;
        int rc, fd;

        IOC_INIT(data);

        if (argc != 2)
                return CMD_HELP;

        path = argv[1];
        fd = open(path, O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "open \"%s\" failed: %s\n", path,
                        strerror(errno));
                return -errno;
        }

        memset(&desc, 0, sizeof(desc));
        obd_str2uuid(&desc.ld_uuid, argv[1]);
        desc.ld_tgt_count = ((OBD_MAX_IOCTL_BUFFER-sizeof(data)-sizeof(desc)) /
                             (sizeof(*uuidarray) + sizeof(*obdgens)));

repeat:
        uuidarray = calloc(desc.ld_tgt_count, sizeof(*uuidarray));
        if (!uuidarray) {
                fprintf(stderr, "error: %s: no memory for %d uuid's\n",
                        jt_cmdname(argv[0]), desc.ld_tgt_count);
                rc = -ENOMEM;
                goto out;
        }
        obdgens = calloc(desc.ld_tgt_count, sizeof(*obdgens));
        if (!obdgens) {
                fprintf(stderr, "error: %s: no memory for %d generation #'s\n",
                        jt_cmdname(argv[0]), desc.ld_tgt_count);
                rc = -ENOMEM;
                goto out_uuidarray;
        }

        data.ioc_inllen1 = sizeof(desc);
        data.ioc_inlbuf1 = (char *)&desc;
        data.ioc_inllen2 = desc.ld_tgt_count * sizeof(*uuidarray);
        data.ioc_inlbuf2 = (char *)uuidarray;
        data.ioc_inllen3 = desc.ld_tgt_count * sizeof(*obdgens);
        data.ioc_inlbuf3 = (char *)obdgens;

        if (obd_ioctl_pack(&data, &buf, max)) {
                fprintf(stderr, "error: %s: invalid ioctl\n",
                        jt_cmdname(argv[0]));
                rc = -EINVAL;
                goto out_obdgens;
        }
        rc = ioctl(fd, OBD_IOC_LOV_GET_CONFIG, buf);
        if (rc == -ENOSPC) {
                free(uuidarray);
                free(obdgens);
                goto repeat;
        } else if (rc) {
                fprintf(stderr, "error: %s: ioctl error: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));
        } else {
                struct obd_uuid *uuidp;
                __u32 *genp;
                int i;

                if (obd_ioctl_unpack(&data, buf, max)) {
                        fprintf(stderr, "error: %s: invalid reply\n",
                                jt_cmdname(argv[0]));
                        rc = -EINVAL;
                        goto out;
                }
                printf("default_stripe_count: %u\n",
                       desc.ld_default_stripe_count);
                printf("default_stripe_size: "LPU64"\n",
                       desc.ld_default_stripe_size);
                printf("default_stripe_offset: "LPU64"\n",
                       desc.ld_default_stripe_offset);
                printf("default_stripe_pattern: %u\n", desc.ld_pattern);
                printf("obd_count: %u\n", desc.ld_tgt_count);
                printf("OBDS:\tobdidx\t\tobdgen\t\t obduuid\n");
                uuidp = uuidarray;
                genp = obdgens;
                for (i = 0; i < desc.ld_tgt_count; i++, uuidp++, genp++)
                        printf("\t%6u\t%14u\t\t %s\n", i, *genp, (char *)uuidp);
        }
out_obdgens:
        free(obdgens);
out_uuidarray:
        free(uuidarray);
out:
        close(fd);
        return rc;
}

int jt_obd_test_ldlm(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);
        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, IOC_LDLM_TEST, buf);
        if (rc)
                fprintf(stderr, "error: %s: test failed: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));
        return rc;
}

int jt_obd_dump_ldlm(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);
        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, IOC_LDLM_DUMP, buf);
        if (rc)
                fprintf(stderr, "error: %s failed: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));
        return rc;
}

int jt_obd_ldlm_regress_start(int argc, char **argv)
{
        int rc;
        struct obd_ioctl_data data;
        char argstring[200];
        int i, count = sizeof(argstring) - 1;

        IOC_INIT(data);
        if (argc > 5)
                return CMD_HELP;

        argstring[0] = '\0';
        for (i = 1; i < argc; i++) {
                strncat(argstring, " ", count);
                count--;
                strncat(argstring, argv[i], count);
                count -= strlen(argv[i]);
        }

        if (strlen(argstring)) {
                data.ioc_inlbuf1 = argstring;
                data.ioc_inllen1 = strlen(argstring) + 1;
        }

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, IOC_LDLM_REGRESS_START, buf);
        if (rc)
                fprintf(stderr, "error: %s: test failed: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));

        return rc;
}

int jt_obd_ldlm_regress_stop(int argc, char **argv)
{
        int rc;
        struct obd_ioctl_data data;
        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, IOC_LDLM_REGRESS_STOP, buf);

        if (rc)
                fprintf(stderr, "error: %s: test failed: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));
        return rc;
}

static int do_activate(int argc, char **argv, int flag)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);
        if (argc != 1)
                return CMD_HELP;

        /* reuse offset for 'active' */
        data.ioc_offset = flag;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, IOC_OSC_SET_ACTIVE, buf);
        if (rc)
                fprintf(stderr, "error: %s: failed: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));

        return rc;
}

int jt_obd_deactivate(int argc, char **argv)
{
        return do_activate(argc, argv, 0);
}

int jt_obd_activate(int argc, char **argv)
{
        return do_activate(argc, argv, 1);
}

int jt_obd_recover(int argc, char **argv)
{
        int rc;
        struct obd_ioctl_data data;

        IOC_INIT(data);
        if (argc > 2)
                return CMD_HELP;

        if (argc == 2) {
                data.ioc_inllen1 = strlen(argv[1]) + 1;
                data.ioc_inlbuf1 = argv[1];
        }

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_CLIENT_RECOVER, buf);
        if (rc < 0) {
                fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
                        strerror(rc = errno));
        }

        return rc;
}

int jt_obd_mdc_lookup(int argc, char **argv)
{
        struct obd_ioctl_data data;
        char *parent, *child;
        int rc, fd, verbose = 1;

        if (argc < 3 || argc > 4)
                return CMD_HELP;

        parent = argv[1];
        child = argv[2];
        if (argc == 4)
                verbose = get_verbose(argv[0], argv[3]);

        IOC_INIT(data);

        data.ioc_inllen1 = strlen(child) + 1;
        data.ioc_inlbuf1 = child;

        IOC_PACK(argv[0], data);

        fd = open(parent, O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "open \"%s\" failed: %s\n", parent,
                        strerror(errno));
                return -1;
        }

        rc = ioctl(fd, IOC_MDC_LOOKUP, buf);
        if (rc < 0) {
                fprintf(stderr, "error: %s: ioctl error: %s\n",
                        jt_cmdname(argv[0]), strerror(rc = errno));
        }
        close(fd);

        if (verbose) {
                IOC_UNPACK(argv[0], data);
                printf("%s: mode %o uid %d gid %d\n", child,
                       data.ioc_obdo1.o_mode, data.ioc_obdo1.o_uid,
                       data.ioc_obdo1.o_gid);
        }

        return rc;
}

int jt_obd_finish_gns(int argc, char **argv)
{
        char *mtpt;
        int rc, fd;
        struct obd_ioctl_data data;

        if (argc != 2)
                return CMD_HELP;

        mtpt = argv[1];

        fd = open(mtpt, O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "open \"%s\" failed: %s\n", mtpt,
                        strerror(errno));
                return -1;
        }

        IOC_INIT(data);
        IOC_PACK(argv[0], data);
        rc = ioctl(fd, IOC_MDC_FINISH_GNS, buf);
        if (rc < 0) {
                fprintf(stderr, "error: %s(%s) ioctl error: %s\n",
                        jt_cmdname(argv[0]), mtpt, strerror(rc = errno));
        }
        close(fd);

        return rc;
}

int jt_obd_close_uuid(int argc, char **argv)
{
        int rc, nal;
        struct obd_ioctl_data data;

        if (argc != 3) {
                fprintf(stderr, "usage: %s <uuid> <net-type>\n", argv[0]);
                return 0;
        }

        nal = ptl_name2nal(argv[2]);

        if (nal <= 0) {
                fprintf (stderr, "Can't parse NAL %s\n", argv[2]);
                return -1;
        }

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        data.ioc_nal = nal;

        IOC_PACK(argv[0], data);
        rc = l2_ioctl(OBD_DEV_ID, OBD_IOC_CLOSE_UUID, buf);
        if (rc) {
                fprintf(stderr, "IOC_PORTAL_CLOSE_UUID failed: %s\n",
                        strerror(errno));
                return -1;
        }
        return 0;
}


int jt_cfg_record(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 2)
                return CMD_HELP;

        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_RECORD, buf);
        if (rc == 0) {
                jt_recording = 1;
                ptl_set_cfg_record_cb(obd_record);
        } else {
                fprintf(stderr, "OBD_IOC_RECORD failed: %s\n",
                        strerror(errno));
        }

        return rc;
}

int jt_cfg_parse(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 2)
                return CMD_HELP;

        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_PARSE, buf);
        if (rc < 0)
                fprintf(stderr, "OBD_IOC_PARSE failed: %s\n",
                        strerror(errno));

        return rc;
}


int jt_cfg_dump_log(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 2)
                return CMD_HELP;

        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_DUMP_LOG, buf);
        if (rc < 0)
                fprintf(stderr, "OBD_IOC_DUMP_LOG failed: %s\n",
                        strerror(errno));

        return rc;
}

int jt_cfg_clear_log(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 2)
                return CMD_HELP;

        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CLEAR_LOG, buf);
        if (rc < 0)
                fprintf(stderr, "OBD_IOC_CLEAR_LOG failed: %s\n",
                        strerror(errno));

        return rc;
}



int jt_cfg_endrecord(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        if (!jt_recording) {
                fprintf(stderr, "Not recording, so endrecord doesn't make "
                        "sense.\n");
                return 0;
        }

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_ENDRECORD, buf);
        if (rc == 0) {
                jt_recording = 0;
                ptl_set_cfg_record_cb(NULL);
        } else {
                fprintf(stderr, "OBD_IOC_ENDRECORD failed: %s\n",
                        strerror(errno));
        }
        return rc;
}

int jt_llog_catlist(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        if (argc != 1)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_inllen1 = max - size_round(sizeof(data));
        IOC_PACK(argv[0], data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CATLOGLIST, buf);
        if (rc == 0)
                fprintf(stdout, "%s", ((struct obd_ioctl_data*)buf)->ioc_bulk);
        else
                fprintf(stderr, "OBD_IOC_CATLOGLIST failed: %s\n",
                        strerror(errno));

        return rc;
}

int jt_llog_info(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        if (argc != 2)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        data.ioc_inllen2 = max - size_round(sizeof(data)) -
                size_round(data.ioc_inllen1);
        IOC_PACK(argv[0], data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LLOG_INFO, buf);
        if (rc == 0)
                fprintf(stdout, "%s", ((struct obd_ioctl_data*)buf)->ioc_bulk);
        else
                fprintf(stderr, "OBD_IOC_LLOG_INFO failed: %s\n",
                        strerror(errno));

        return rc;
}

int jt_llog_print(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        if (argc != 2 && argc != 4)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        if (argc == 4) {
                data.ioc_inllen2 = strlen(argv[2]) + 1;
                data.ioc_inlbuf2 = argv[2];
                data.ioc_inllen3 = strlen(argv[3]) + 1;
                data.ioc_inlbuf3 = argv[3];
        } else {
                char from[2] = "1", to[3] = "-1";
                data.ioc_inllen2 = strlen(from) + 1;
                data.ioc_inlbuf2 = from;
                data.ioc_inllen3 = strlen(to) + 1;
                data.ioc_inlbuf3 = to;
        }
        data.ioc_inllen4 = max - size_round(sizeof(data)) -
                size_round(data.ioc_inllen1) -
                size_round(data.ioc_inllen2) -
                size_round(data.ioc_inllen3);
        IOC_PACK(argv[0], data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LLOG_PRINT, buf);
        if (rc == 0)
                fprintf(stdout, "%s", ((struct obd_ioctl_data*)buf)->ioc_bulk);
        else
                fprintf(stderr, "OBD_IOC_LLOG_PRINT failed: %s\n",
                        strerror(errno));

        return rc;
}

int jt_llog_cancel(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        if (argc != 4)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        data.ioc_inllen2 = strlen(argv[2]) + 1;
        data.ioc_inlbuf2 = argv[2];
        data.ioc_inllen3 = strlen(argv[3]) + 1;
        data.ioc_inlbuf3 = argv[3];
        IOC_PACK(argv[0], data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LLOG_CANCEL, buf);
        if (rc == 0)
                fprintf(stdout, "index %s be canceled.\n", argv[3]);
        else
                fprintf(stderr, "OBD_IOC_LLOG_CANCEL failed: %s\n",
                        strerror(errno));

        return rc;

}

int jt_llog_check(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        if (argc != 2 && argc != 4)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        if (argc == 4) {
                data.ioc_inllen2 = strlen(argv[2]) + 1;
                data.ioc_inlbuf2 = argv[2];
                data.ioc_inllen3 = strlen(argv[3]) + 1;
                data.ioc_inlbuf3 = argv[3];
        } else {
                char from[2] = "1", to[3] = "-1";
                data.ioc_inllen2 = strlen(from) + 1;
                data.ioc_inlbuf2 = from;
                data.ioc_inllen3 = strlen(to) + 1;
                data.ioc_inlbuf3 = to;
        }
        data.ioc_inllen4 = max - size_round(sizeof(data)) -
                size_round(data.ioc_inllen1) -
                size_round(data.ioc_inllen2) -
                size_round(data.ioc_inllen3);
        IOC_PACK(argv[0], data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LLOG_CHECK, buf);
        if (rc == 0)
                fprintf(stdout, "%s", ((struct obd_ioctl_data*)buf)->ioc_bulk);
        else
                fprintf(stderr, "OBD_IOC_LLOG_CHECK failed: %s\n",
                        strerror(errno));
        return rc;
}

int jt_llog_remove(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        if (argc != 3 && argc != 2)
                return CMD_HELP;

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        if (argc == 3){
                data.ioc_inllen2 = strlen(argv[2]) + 1;
                data.ioc_inlbuf2 = argv[2];
        }
        IOC_PACK(argv[0], data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LLOG_REMOVE, buf);
        if (rc == 0) {
                if (argc == 3)
                        fprintf(stdout, "log %s are removed.\n", argv[2]);
                else
                        fprintf(stdout, "the log in catalog %s are removed. \n", argv[1]);
        } else
                fprintf(stderr, "OBD_IOC_LLOG_REMOVE failed: %s\n",
                        strerror(errno));

        return rc;
}

int jt_obd_reint_sync(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc = 0;
      
        IOC_INIT(data);
        if (argc != 1)
               return CMD_HELP; 
        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CMOBD_SYNC, buf);
       
        if (rc)
                fprintf(stderr, "OBD_IOC_CMOBD_SYNC failed: rc=%d\n",
                        rc);
        return rc;  
               
}

int jt_obd_cache_on(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc = 0;
      
        IOC_INIT(data);
        if (argc != 1)
               return CMD_HELP; 
        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_COBD_CON, buf);
       
        if (rc)
                fprintf(stderr, "OBD_IOC_CMOBD_SYNC failed: rc=%d\n",
                        rc);
        return rc;  
               
}

int jt_obd_cache_off(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc = 0;
      
        IOC_INIT(data);
        if (argc != 1)
               return CMD_HELP; 
        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_COBD_COFF, buf);
        if (rc)
                fprintf(stderr, "OBD_IOC_CMOBD_SYNC failed: rc=%d\n",
                        rc);
        return rc;  
}

int jt_obd_snap_add(int argc, char **argv)
{
#if 1
        return -1;
#else
# error "FIX the missing #defines before committing"        
        struct obd_ioctl_data data;
        int rc = 0;
      
        if (argc != 3)
               return CMD_HELP;

        shmem_setup();   
        register_ioc_dev(SMFS_DEV_ID, SMFS_DEV_PATH);
        
        IOC_INIT(data);
        
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        data.ioc_inllen2 = strlen(argv[2]) + 2;
        data.ioc_inlbuf2 = argv[2];

        IOC_PACK(argv[0], data);
       
        rc = l_ioctl(SMFS_DEV_ID, OBD_IOC_SMFS_SNAP_ADD, buf);
        
        unregister_ioc_dev(SMFS_DEV_ID);       
 
        if (rc)
                fprintf(stderr, "OBD_IOC_SNAP_ADD failed: rc=%d\n", rc);
        return rc;
#endif
}

static void signal_server(int sig)
{
        if (sig == SIGINT) {
                do_disconnect("sigint", 1);
                exit(1);
        } else
                fprintf(stderr, "%s: got signal %d\n", jt_cmdname("sigint"), sig);
}

int obd_initialize(int argc, char **argv)
{
        shmem_setup();
        register_ioc_dev(OBD_DEV_ID, OBD_DEV_PATH);

        return 0;
}


void obd_finalize(int argc, char **argv)
{
        struct sigaction sigact;

        sigact.sa_handler = signal_server;
        sigfillset(&sigact.sa_mask);
        sigact.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sigact, NULL);

        if (jt_recording) {
                printf("END RECORD\n");
                jt_cfg_endrecord(argc, argv);
        }

        do_disconnect(argv[0], 1);
}
