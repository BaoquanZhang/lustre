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

#ifndef __KERNEL__
#include <liblustre.h>
#endif
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_dlm.h>
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

#include "obdctl.h"
#include <portals/ptlctl.h>
#include "parser.h"
#include <stdio.h>

#define SHMEM_STATS 1
#if SHMEM_STATS
# include <sys/ipc.h>
# include <sys/shm.h>

# define MAX_SHMEM_COUNT 1024
static long long *shared_counters;
static long long counter_snapshot[2][MAX_SHMEM_COUNT];
struct timeval prev_time;
#endif

uint64_t conn_cookie = -1;
char rawbuf[8192];
char *buf = rawbuf;
int max = sizeof(rawbuf);

static int thread;

union lsm_buffer {
        char                 space [4096];
        struct lov_stripe_md lsm;
} lsm_buffer;

static char *cmdname(char *func);

#define IOC_INIT(data)                                                  \
do {                                                                    \
        memset(&data, 0, sizeof(data));                                 \
        data.ioc_cookie = conn_cookie;                                  \
} while (0)

#define IOC_PACK(func, data)                                            \
do {                                                                    \
        memset(buf, 0, sizeof(rawbuf));                                 \
        if (obd_ioctl_pack(&data, &buf, max)) {                         \
                fprintf(stderr, "error: %s: invalid ioctl\n",           \
                        cmdname(func));                                 \
                return -2;                                              \
        }                                                               \
} while (0)

#define IOC_UNPACK(func, data)                                          \
do {                                                                    \
        if (obd_ioctl_unpack(&data, buf, max)) {                        \
                fprintf(stderr, "error: %s: invalid reply\n",           \
                        cmdname(func));                                 \
                return -2;                                              \
        }                                                               \
} while (0)

char *obdo_print(struct obdo *obd)
{
        char buf[1024];

        sprintf(buf, "id: "LPX64"\ngrp: "LPX64"\natime: "LPU64"\nmtime: "LPU64
                "\nctime: "LPU64"\nsize: "LPU64"\nblocks: "LPU64
                "\nblksize: %u\nmode: %o\nuid: %d\ngid: %d\nflags: %x\n"
                "obdflags: %x\nnlink: %d,\nvalid %x\n",
                obd->o_id, obd->o_gr, obd->o_atime, obd->o_mtime, obd->o_ctime,
                obd->o_size, obd->o_blocks, obd->o_blksize, obd->o_mode,
                obd->o_uid, obd->o_gid, obd->o_flags, obd->o_obdflags,
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
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_NAME2DEV, buf);
        if (rc < 0)
                return errno;
        IOC_UNPACK(func, data);

        return data.ioc_dev + N2D_OFF;
}

static int do_uuid2dev(char *func, char *uuid)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        data.ioc_inllen1 = strlen(uuid) + 1;
        data.ioc_inlbuf1 = uuid;

        IOC_PACK(func, data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_UUID2DEV, buf);
        if (rc < 0)
                return errno;
        IOC_UNPACK(func, data);

        return data.ioc_dev + N2D_OFF;
}

/*
 * resolve a device name to a device number.
 * supports a number, $name or %uuid.
 */
static int parse_devname(char *func, char *name)
{
        int rc;
        int ret = -1;

        if (!name)
                return ret;
        if (name[0] == '$') {
                name++;
                rc = do_name2dev(func, name);
                if (rc >= N2D_OFF) {
                        ret = rc - N2D_OFF;
                        printf("Name %s is device %d\n", name, ret);
                } else {
                        printf("No device found for name %s: %s\n",
                               name, strerror(rc));
                }
        } else if (name[0] == '%') {
                name++;
                rc = do_uuid2dev(func, name);
                if (rc >= N2D_OFF) {
                        ret = rc - N2D_OFF;
                        printf("UUID %s is device %d\n", name, ret);
                } else {
                        printf("No device found for UUID %s: %s\n",
                               name, strerror(rc));
                }
        } else {
                /* Assume it's a number.  This means that bogus strings become
                 * 0.  I might care about that some day. */
                ret = strtoul(name, NULL, 0);
                printf("Selected device %d\n", ret);
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
                nob = snprintf (p, space, "=%u#%u@%d",
                                lsm->lsm_stripe_size,
                                lsm->lsm_stripe_count,
                                lsm->lsm_stripe_offset);
                p += nob;
                space -= nob;

                for (i = 0; i < lsm->lsm_stripe_count; i++) {
                        nob = snprintf (p, space, ":"LPX64,
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
         * object_id[=size#count[@offset][:id]*]
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

        if (*string == '@') {
                string++;
                lsm->lsm_stripe_offset = strtol (string, &end, 0);
                if (end == string)
                        return (-1);
                string = end;
        }

        if (*string == 0)               /* don't have to specify obj ids */
                return (0);

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                if (*string != ':')
                        return (-1);
                string++;
                lsm->lsm_oinfo[i].loi_id = strtoull (string, &end, 0);
                string = end;
        }

        if (*string != 0)
                return (-1);

        return (0);
}

static char *cmdname(char *func)
{
        static char buf[512];

        if (thread) {
                sprintf(buf, "%s-%d", func, thread);
                return buf;
        }

        return func;
}

#define difftime(a, b)                                          \
        ((double)(a)->tv_sec - (b)->tv_sec +                    \
         ((double)((a)->tv_usec - (b)->tv_usec) / 1000000))

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
        if (verbose < 0 && next_time != NULL && difftime(&now, next_time) >= 0){
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
                                cmdname(func), arg);
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
        int rc;
        struct obd_ioctl_data data;

        if (conn_cookie == -1)
                return 0;

        IOC_INIT(data);

        IOC_PACK(func, data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_DISCONNECT, buf);
        if (rc < 0) {
                fprintf(stderr, "error: %s: OPD_IOC_DISCONNECT %s\n",
                        cmdname(func),strerror(errno));
        } else {
                if (verbose)
                        printf("%s: disconnected conn "LPX64"\n", cmdname(func),
                               conn_cookie);
                conn_cookie = -1;
        }

        return rc;
}

#if SHMEM_STATS
static void shmem_setup(void)
{
        /* Create new segment */
        int shmid = shmget(IPC_PRIVATE, sizeof(counter_snapshot[0]), 0600);

        if (shmid == -1) {
                fprintf(stderr, "Can't create shared memory counters: %s\n",
                        strerror(errno));
                return;
        }

        /* Attatch to new segment */
        shared_counters = (long long *)shmat(shmid, NULL, 0);

        if (shared_counters == (long long *)(-1)) {
                fprintf(stderr, "Can't attach shared memory counters: %s\n",
                        strerror(errno));
                shared_counters = NULL;
                return;
        }

        /* Mark segment as destroyed, so it will disappear when we exit.
         * Forks will inherit attached segments, so we should be OK.
         */
        if (shmctl(shmid, IPC_RMID, NULL) == -1) {
                fprintf(stderr, "Can't destroy shared memory counters: %s\n",
                        strerror(errno));
        }
}

static inline void shmem_reset(void)
{
        if (shared_counters == NULL)
                return;

        memset(shared_counters, 0, sizeof(counter_snapshot[0]));
        memset(counter_snapshot, 0, sizeof(counter_snapshot));
        gettimeofday(&prev_time, NULL);
}

static inline void shmem_bump(void)
{
        if (shared_counters == NULL || thread <= 0 || thread > MAX_SHMEM_COUNT)
                return;

        shared_counters[thread - 1]++;
}

static void shmem_snap(int n)
{
        struct timeval this_time;
        int non_zero = 0;
        long long total = 0;
        double secs;
        int i;

        if (shared_counters == NULL || n > MAX_SHMEM_COUNT)
                return;

        memcpy(counter_snapshot[1], counter_snapshot[0],
               n * sizeof(counter_snapshot[0][0]));
        memcpy(counter_snapshot[0], shared_counters,
               n * sizeof(counter_snapshot[0][0]));
        gettimeofday(&this_time, NULL);

        for (i = 0; i < n; i++) {
                long long this_count =
                        counter_snapshot[0][i] - counter_snapshot[1][i];

                if (this_count != 0) {
                        non_zero++;
                        total += this_count;
                }
        }

        secs = (this_time.tv_sec + this_time.tv_usec / 1000000.0) -
                (prev_time.tv_sec + prev_time.tv_usec / 1000000.0);

        printf("%d/%d Total: %f/second\n", non_zero, n, total / secs);

        prev_time = this_time;
}

#define SHMEM_SETUP()   shmem_setup()
#define SHMEM_RESET()   shmem_reset()
#define SHMEM_BUMP()    shmem_bump()
#define SHMEM_SNAP(n)   shmem_snap(n)
#else
#define SHMEM_SETUP()
#define SHMEM_RESET()
#define SHMEM_BUMP()
#define SHMEM_SNAP(n)
#endif

extern command_t cmdlist[];

static int do_device(char *func, int dev)
{
        struct obd_ioctl_data data;

        memset(&data, 0, sizeof(data));

        data.ioc_dev = dev;

        IOC_PACK(func, data);
        return l_ioctl(OBD_DEV_ID, OBD_IOC_DEVICE, buf);
}

int jt_obd_device(int argc, char **argv)
{
        int rc, dev;
        do_disconnect(argv[0], 1);

        if (argc != 2)
                return CMD_HELP;

        dev = parse_devname(argv[0], argv[1]);
        if (dev < 0)
                return -1;

        rc = do_device(argv[0], dev);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}

int jt_obd_connect(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        do_disconnect(argv[0], 1);

        /* XXX TODO: implement timeout per lctl usage for probe */
        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CONNECT, buf);
        IOC_UNPACK(argv[0], data);
        if (rc < 0)
                fprintf(stderr, "error: %s: OBD_IOC_CONNECT %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
        else
                conn_cookie = data.ioc_cookie;
        return rc;
}

int jt_obd_disconnect(int argc, char **argv)
{
        if (argc != 1)
                return CMD_HELP;

        if (conn_cookie == -1)
                return 0;

        return do_disconnect(argv[0], 0);
}

int jt_opt_device(int argc, char **argv)
{
        char *arg2[3];
        int ret;
        int rc;

        if (argc < 3)
                return CMD_HELP;

        rc = do_device("device", parse_devname(argv[0], argv[1]));

        if (!rc) {
                arg2[0] = "connect";
                arg2[1] = NULL;
                rc = jt_obd_connect(1, arg2);
        }

        if (!rc)
                rc = Parser_execarg(argc - 2, argv + 2, cmdlist);

        ret = do_disconnect(argv[0], 0);
        if (!rc)
                rc = ret;

        return rc;
}

int jt_opt_threads(int argc, char **argv)
{
        __u64 threads, next_thread;
        int verbose;
        int rc = 0;
        char *end;
        int i;

        if (argc < 5)
                return CMD_HELP;

        threads = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid page count '%s'\n",
                        cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        verbose = get_verbose(argv[0], argv[2]);
        if (verbose == BAD_VERBOSE)
                return CMD_HELP;

        if (verbose != 0)
                printf("%s: starting "LPD64" threads on device %s running %s\n",
                       argv[0], threads, argv[3], argv[4]);

        SHMEM_RESET();

        for (i = 1, next_thread = verbose; i <= threads; i++) {
                rc = fork();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #%d - %s\n", argv[0], i,
                                strerror(rc = errno));
                        break;
                } else if (rc == 0) {
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

                while (live_threads > 0) {
                        int status;
                        pid_t ret;

                        ret = waitpid(0, &status, verbose < 0 ? WNOHANG : 0);
                        if (ret == 0) {
                                if (verbose >= 0)
                                        abort();

                                sleep(-verbose);
                                SHMEM_SNAP(threads);
                                continue;
                        }

                        if (ret < 0) {
                                fprintf(stderr, "error: %s: wait - %s\n",
                                        argv[0], strerror(errno));
                                if (!rc)
                                        rc = errno;
                        } else {
                                /*
                                 * This is a hack.  We _should_ be able to use
                                 * WIFEXITED(status) to see if there was an
                                 * error, but it appears to be broken and it
                                 * always returns 1 (OK).  See wait(2).
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
        }

        return rc;
}

int jt_obd_detach(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_DETACH, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}

int jt_obd_cleanup(int argc, char **argv)
{
        struct obd_ioctl_data data;
        char force = 'F';
        char failover = 'A';
        char flags[3];
        int flag_cnt = 0, n;
        int rc;

        IOC_INIT(data);

        if (argc < 1 || argc > 3)
                return CMD_HELP;

        for (n = 1; n < argc; n++) 
                if (strcmp(argv[n], "force") == 0) {
                        flags[flag_cnt++] = force;
                } else if (strcmp(argv[n], "failover") == 0) {
                        flags[flag_cnt++] = failover;
                } else {
                        fprintf(stderr, "unknown option: %s", argv[n]);
                        return CMD_HELP;
                }

        data.ioc_inllen1 = flag_cnt;
        if (flag_cnt)
                data.ioc_inlbuf1 = flags;

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CLEANUP, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));

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
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_NO_TRANSNO, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
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
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_SET_READONLY, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
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
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_ABORT_RECOVERY, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
}

int jt_obd_newdev(int argc, char **argv)
{
        int rc;
        struct obd_ioctl_data data;

        IOC_INIT(data);

        if (argc != 1)
                return CMD_HELP;

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_NEWDEV, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));
        else {
                IOC_UNPACK(argv[0], data);
                printf("Current device set to %d\n", data.ioc_dev);
        }

        return rc;
}

int jt_obd_mount_option(int argc, char **argv)
{
        int rc;
        struct obd_ioctl_data data;

        IOC_INIT(data);

        if (argc != 2)
                return CMD_HELP;

        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_MOUNTOPT, buf);
        if (rc < 0) {
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));
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
        data->ioc_cookie = conn_cookie;
        data->ioc_inllen1 = sizeof(buf) - size_round(sizeof(*data));
        data->ioc_len = obd_ioctl_packlen(data);

        rc = l_ioctl(OBD_DEV_ID, OBD_GET_VERSION, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
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
        char buf[8192];
        struct obd_ioctl_data *data = (struct obd_ioctl_data *)buf;

        if (argc != 1)
                return CMD_HELP;

        memset(buf, 0, sizeof(buf));
        data->ioc_version = OBD_IOCTL_VERSION;
        data->ioc_cookie = conn_cookie;
        data->ioc_inllen1 = sizeof(buf) - size_round(sizeof(*data));
        data->ioc_len = obd_ioctl_packlen(data);

        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LIST, data);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));
        else {
                printf("%s", data->ioc_bulk);
        }

        return rc;
}

int jt_obd_attach(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc != 2 && argc != 3 && argc != 4)
                return CMD_HELP;

        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        if (argc >= 3) {
                data.ioc_inllen2 = strlen(argv[2]) + 1;
                data.ioc_inlbuf2 = argv[2];
        }

        if (argc == 4) {
                data.ioc_inllen3 = strlen(argv[3]) + 1;
                data.ioc_inlbuf3 = argv[3];
        }

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_ATTACH, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: OBD_IOC_ATTACH %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
        else if (argc == 3) {
                char name[1024];
                if (strlen(argv[2]) > 128) {
                        printf("Name too long to set environment\n");
                        return -EINVAL;
                }
                snprintf(name, 512, "LUSTRE_DEV_%s", argv[2]);
                rc = setenv(name, argv[1], 1);
                if (rc) {
                        printf("error setting env variable %s\n", name);
                }
        }

        return rc;
}

int jt_obd_setup(int argc, char **argv)
{
        struct obd_ioctl_data data;
        int rc;

        IOC_INIT(data);

        if (argc > 4)
                return CMD_HELP;

        data.ioc_dev = -1;
        if (argc > 1) {
                data.ioc_dev = parse_devname(argv[0], argv[1]);
                if (data.ioc_dev < 0)
                        return -1;
                data.ioc_inllen1 = strlen(argv[1]) + 1;
                data.ioc_inlbuf1 = argv[1];
        }
        if (argc > 2) {
                data.ioc_inllen2 = strlen(argv[2]) + 1;
                data.ioc_inlbuf2 = argv[2];
        }
        if (argc > 3) {
                data.ioc_inllen3 = strlen(argv[3]) + 1;
                data.ioc_inlbuf3 = argv[3];
        }

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_SETUP, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));

        return rc;
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
                         cmdname (argv[0]), argv[1]);
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
        rc = l_ioctl(OBD_DEV_ID, ECHO_IOC_GET_STRIPE, buf);
        IOC_UNPACK(argv[0], data);

        if (rc != 0) {
                fprintf (stderr, "Error: %s: rc %d(%s)\n",
                         cmdname (argv[0]), rc, strerror (errno));
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
                         cmdname (argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc > 2) {
                count = strtol (argv[2], &end, 0);
                if (*end != 0) {
                        fprintf (stderr, "error: %s: invalid count '%s'\n",
                                 cmdname (argv[0]), argv[1]);
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
                rc = l_ioctl (OBD_DEV_ID, ECHO_IOC_SET_STRIPE, buf);
                IOC_UNPACK (argv[0], data);

                if (rc != 0) {
                        fprintf (stderr, "Error: %s: rc %d(%s)\n",
                                 cmdname (argv[0]), rc, strerror (errno));
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
                         cmdname (argv[0]), argv[1]);
                return CMD_HELP;
        }

        IOC_INIT (data);
        data.ioc_obdo1.o_id = id;
        data.ioc_obdo1.o_mode = S_IFREG | 0644;
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLMODE;

        IOC_PACK (argv[0], data);
        rc = l_ioctl (OBD_DEV_ID, ECHO_IOC_SET_STRIPE, buf);
        IOC_UNPACK (argv[0], data);

        if (rc != 0)
                fprintf (stderr, "Error: %s: rc %d(%s)\n",
                         cmdname (argv[0]), rc, strerror (errno));

        return (0);
}

/* Create one or more objects, arg[4] may describe stripe meta-data.  If
 * not, defaults assumed.  This echo-client instance stashes the stripe
 * object ids.  Use get_stripe on this node to print full lsm and
 * set_stripe on another node to cut/paste between nodes.
 */
int jt_obd_create(int argc, char **argv)
{
        static __u64 base_id = 1;

        struct obd_ioctl_data data;
        struct timeval next_time;
        __u64 count = 1, next_count;
        int verbose = 1, mode = 0100644, rc = 0, i;
        char *end;

        IOC_INIT(data);
        if (argc < 2 || argc > 5)
                return CMD_HELP;

        count = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid iteration count '%s'\n",
                        cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc > 2) {
                mode = strtoul(argv[2], &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: invalid mode '%s'\n",
                                cmdname(argv[0]), argv[2]);
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
                                cmdname(argv[0]), argv[4]);
                        return CMD_HELP;
                }
                base_id = lsm_buffer.lsm.lsm_object_id;
        }

        printf("%s: "LPD64" objects\n", cmdname(argv[0]), count);
        gettimeofday(&next_time, NULL);
        next_time.tv_sec -= verbose;

        for (i = 1, next_count = verbose; i <= count; i++) {
                data.ioc_obdo1.o_mode = mode;
                data.ioc_obdo1.o_id = base_id++;
                data.ioc_obdo1.o_uid = 0;
                data.ioc_obdo1.o_gid = 0;
                data.ioc_obdo1.o_valid = OBD_MD_FLTYPE | OBD_MD_FLMODE |
                                         OBD_MD_FLID | OBD_MD_FLUID | OBD_MD_FLGID;

                data.ioc_plen1 = sizeof (lsm_buffer);
                data.ioc_pbuf1 = (char *)&lsm_buffer;

                IOC_PACK(argv[0], data);
                rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CREATE, buf);
                IOC_UNPACK(argv[0], data);
                SHMEM_BUMP();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #%d - %s\n",
                                cmdname(argv[0]), i, strerror(rc = errno));
                        break;
                }
                if (!(data.ioc_obdo1.o_valid & OBD_MD_FLID)) {
                        fprintf(stderr, "error: %s: objid not valid #%d:%08x\n",
                                cmdname(argv[0]), i, data.ioc_obdo1.o_valid);
                        rc = EINVAL;
                        break;
                }

                if (be_verbose(verbose, &next_time, i, &next_count, count))
                        printf("%s: #%d is object id "LPX64"\n",
                                cmdname(argv[0]), i, data.ioc_obdo1.o_id);
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
                        cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }
        data.ioc_obdo1.o_mode = S_IFREG | strtoul(argv[2], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid mode '%s'\n",
                        cmdname(argv[0]), argv[2]);
                return CMD_HELP;
        }
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLMODE;

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_SETATTR, buf);
        if (rc < 0)
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));

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
                        cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }
        if (argc > 2) {
                count = strtoull(argv[2], &end, 0);
                if (*end) {
                        fprintf(stderr,
                                "error: %s: invalid iteration count '%s'\n",
                                cmdname(argv[0]), argv[2]);
                        return CMD_HELP;
                }
        }

        if (argc > 3) {
                verbose = get_verbose(argv[0], argv[3]);
                if (verbose == BAD_VERBOSE)
                        return CMD_HELP;
        }

        printf("%s: "LPD64" objects\n", cmdname(argv[0]), count);
        gettimeofday(&next_time, NULL);
        next_time.tv_sec -= verbose;

        for (i = 1, next_count = verbose; i <= count; i++, id++) {
                data.ioc_obdo1.o_id = id;
                data.ioc_obdo1.o_mode = S_IFREG | 0644;
                data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLMODE;

                IOC_PACK(argv[0], data);
                rc = l_ioctl(OBD_DEV_ID, OBD_IOC_DESTROY, buf);
                IOC_UNPACK(argv[0], data);
                SHMEM_BUMP();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: objid "LPX64": %s\n",
                                cmdname(argv[0]), id, strerror(rc = errno));
                        break;
                }

                if (be_verbose(verbose, &next_time, i, &next_count, count))
                        printf("%s: #%d is object id "LPX64"\n",
                               cmdname(argv[0]), i, id);
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
                        cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }
        /* to help obd filter */
        data.ioc_obdo1.o_mode = 0100644;
        data.ioc_obdo1.o_valid = 0xffffffff;
        printf("%s: object id "LPX64"\n", cmdname(argv[0]),data.ioc_obdo1.o_id);

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_GETATTR, buf);
        IOC_UNPACK(argv[0], data);
        if (rc) {
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
                        strerror(rc = errno));
        } else {
                printf("%s: object id "LPX64", mode %o\n", cmdname(argv[0]),
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

        if (argc < 2 && argc > 4)
                return CMD_HELP;

        IOC_INIT(data);
        count = strtoull(argv[1], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: invalid iteration count '%s'\n",
                        cmdname(argv[0]), argv[1]);
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
                                cmdname(argv[0]), argv[3]);
                        return CMD_HELP;
                }
        }

        gettimeofday(&start, NULL);
        next_time.tv_sec = start.tv_sec - verbose;
        next_time.tv_usec = start.tv_usec;
        if (verbose != 0)
                printf("%s: getting "LPD64" attrs (objid "LPX64"): %s",
                       cmdname(argv[0]), count, objid, ctime(&start.tv_sec));

        for (i = 1, next_count = verbose; i <= count; i++) {
                data.ioc_obdo1.o_id = objid;
                data.ioc_obdo1.o_mode = S_IFREG;
                data.ioc_obdo1.o_valid = 0xffffffff;
                IOC_PACK(argv[0], data);
                rc = l_ioctl(OBD_DEV_ID, OBD_IOC_GETATTR, &data);
                SHMEM_BUMP();
                if (rc < 0) {
                        fprintf(stderr, "error: %s: #"LPD64" - %d:%s\n",
                                cmdname(argv[0]), i, errno, strerror(rc = errno));
                        break;
                } else {
                        if (be_verbose
                            (verbose, &next_time, i, &next_count, count))
                                printf("%s: got attr #"LPD64"\n",
                                       cmdname(argv[0]), i);
                }
        }

        if (!rc) {
                struct timeval end;
                double diff;

                gettimeofday(&end, NULL);

                diff = difftime(&end, &start);

                --i;
                if (verbose != 0)
                        printf("%s: "LPD64" attrs in %.4gs (%.4g attr/s): %s",
                               cmdname(argv[0]), i, diff, (double)i / diff,
                               ctime(&end.tv_sec));
        }
        return rc;
}

int jt_obd_test_brw(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct timeval start, next_time;
        int pages = 1;
        __u64 count, next_count;
        __u64 objid = 3;
        int verbose = 1, write = 0, rw;
        char *end;
        int thr_offset = 0;
        int i;
        int len;
        int rc = 0;

        if (argc < 2 || argc > 6) {
                fprintf(stderr, "error: %s: bad number of arguments: %d\n",
                        cmdname(argv[0]), argc);
                return CMD_HELP;
        }

        /* make each thread write to a different offset */
        if (argv[1][0] == 't') {
                count = strtoull(argv[1] + 1, &end, 0);
                if (thread)
                        thr_offset = thread - 1;
        } else
                count = strtoull(argv[1], &end, 0);

        if (*end) {
                fprintf(stderr, "error: %s: bad iteration count '%s'\n",
                        cmdname(argv[0]), argv[1]);
                return CMD_HELP;
        }

        if (argc >= 3) {
                if (argv[2][0] == 'w' || argv[2][0] == '1')
                        write = 1;
                else if (argv[2][0] == 'r' || argv[2][0] == '0')
                        write = 0;
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
                                cmdname(argv[0]), argv[4]);
                        return CMD_HELP;
                }
        }
        if (argc >= 6) {
                if (argv[5][0] == 't') {
                        objid = strtoull(argv[5] + 1, &end, 0);
                        if (thread)
                                objid += thread - 1;
                } else
                        objid = strtoull(argv[5], &end, 0);
                if (*end) {
                        fprintf(stderr, "error: %s: bad objid '%s'\n",
                                cmdname(argv[0]), argv[5]);
                        return CMD_HELP;
                }
        }

        len = pages * PAGE_SIZE;

        IOC_INIT(data);
        data.ioc_obdo1.o_id = objid;
        data.ioc_obdo1.o_mode = S_IFREG;
        data.ioc_obdo1.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLMODE;
        data.ioc_count = len;
        data.ioc_offset = thr_offset * len * count;

        gettimeofday(&start, NULL);
        next_time.tv_sec = start.tv_sec - verbose;
        next_time.tv_usec = start.tv_usec;

        if (verbose != 0)
                printf("%s: %s "LPU64"x%d pages (obj "LPX64", off "LPU64"): %s",
                       cmdname(argv[0]), write ? "writing" : "reading", count,
                       pages, objid, data.ioc_offset, ctime(&start.tv_sec));

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_OPEN, buf);
        IOC_UNPACK(argv[0], data);
        if (rc) {
                fprintf(stderr, "error: brw_open: %s\n", strerror(rc = errno));
                return rc;
        }

        rw = write ? OBD_IOC_BRW_WRITE : OBD_IOC_BRW_READ;
        for (i = 1, next_count = verbose; i <= count; i++) {
                rc = l_ioctl(OBD_DEV_ID, rw, buf);
                SHMEM_BUMP();
                if (rc) {
                        fprintf(stderr, "error: %s: #%d - %s on %s\n",
                                cmdname(argv[0]), i, strerror(rc = errno),
                                write ? "write" : "read");
                        break;
                } else if (be_verbose(verbose, &next_time,i, &next_count,count))
                        printf("%s: %s number %dx%d\n", cmdname(argv[0]),
                               write ? "write" : "read", i, pages);

                data.ioc_offset += len;
        }

        if (!rc) {
                struct timeval end;
                double diff;

                gettimeofday(&end, NULL);

                diff = difftime(&end, &start);

                --i;
                if (verbose != 0)
                        printf("%s: %s %dx%d pages in %.4gs (%.4g pg/s): %s",
                               cmdname(argv[0]), write ? "wrote" : "read",
                               i, pages, diff, (double)i * pages / diff,
                               ctime(&end.tv_sec));
        }
        rw = l_ioctl(OBD_DEV_ID, OBD_IOC_CLOSE, buf);
        if (rw) {
                fprintf(stderr, "error: brw_close: %s\n", strerror(rw = errno));
                if (!rc)
                        rc = rw;
        }

        return rc;
}

int jt_obd_lov_setconfig(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct lov_desc desc;
        struct obd_uuid *uuidarray, *ptr;
        int rc, i;
        char *end;

        IOC_INIT(data);

        if (argc <= 6)
                return CMD_HELP;

        if (strlen(argv[1]) > sizeof(desc.ld_uuid) - 1) {
                fprintf(stderr,
                        "error: %s: LOV uuid '%s' longer than "LPSZ" chars\n",
                        cmdname(argv[0]), argv[1], sizeof(desc.ld_uuid) - 1);
                return -EINVAL;
        }

        memset(&desc, 0, sizeof(desc));
        obd_str2uuid(&desc.ld_uuid, argv[1]);
        desc.ld_tgt_count = argc - 6;
        desc.ld_default_stripe_count = strtoul(argv[2], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: bad default stripe count '%s'\n",
                        cmdname(argv[0]), argv[2]);
                return CMD_HELP;
        }
        if (desc.ld_default_stripe_count > desc.ld_tgt_count) {
                fprintf(stderr,
                        "error: %s: default stripe count %u > OST count %u\n",
                        cmdname(argv[0]), desc.ld_default_stripe_count,
                        desc.ld_tgt_count);
                return -EINVAL;
        }

        desc.ld_default_stripe_size = strtoull(argv[3], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: bad default stripe size '%s'\n",
                        cmdname(argv[0]), argv[3]);
                return CMD_HELP;
        }
        if (desc.ld_default_stripe_size < 4096) {
                fprintf(stderr,
                        "error: %s: default stripe size "LPU64" too small\n",
                        cmdname(argv[0]), desc.ld_default_stripe_size);
                return -EINVAL;
        } else if ((long)desc.ld_default_stripe_size <
                   desc.ld_default_stripe_size) {
                fprintf(stderr,
                        "error: %s: default stripe size "LPU64" too large\n",
                        cmdname(argv[0]), desc.ld_default_stripe_size);
                return -EINVAL;
        }
        desc.ld_default_stripe_offset = strtoull(argv[4], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: bad default stripe offset '%s'\n",
                        cmdname(argv[0]), argv[4]);
                return CMD_HELP;
        }
        desc.ld_pattern = strtoul(argv[5], &end, 0);
        if (*end) {
                fprintf(stderr, "error: %s: bad stripe pattern '%s'\n",
                        cmdname(argv[0]), argv[5]);
                return CMD_HELP;
        }

        /* NOTE: it is possible to overwrite the default striping parameters,
         *       but EXTREME care must be taken when saving the OST UUID list.
         *       It must be EXACTLY the same, or have only additions at the
         *       end of the list, or only overwrite individual OST entries
         *       that are restored from backups of the previous OST.
         */
        uuidarray = calloc(desc.ld_tgt_count, sizeof(*uuidarray));
        if (!uuidarray) {
                fprintf(stderr, "error: %s: no memory for %d UUIDs\n",
                        cmdname(argv[0]), desc.ld_tgt_count);
                rc = -ENOMEM;
                goto out;
        }
        for (i = 6, ptr = uuidarray; i < argc; i++, ptr++) {
                if (strlen(argv[i]) >= sizeof(*ptr)) {
                        fprintf(stderr, "error: %s: arg %d (%s) too long\n",
                                cmdname(argv[0]), i, argv[i]);
                        rc = -EINVAL;
                        goto out;
                }
                strcpy((char *)ptr, argv[i]);
        }

        data.ioc_inllen1 = sizeof(desc);
        data.ioc_inlbuf1 = (char *)&desc;
        data.ioc_inllen2 = desc.ld_tgt_count * sizeof(*uuidarray);
        data.ioc_inlbuf2 = (char *)uuidarray;

        if (obd_ioctl_pack(&data, &buf, max)) {
                fprintf(stderr, "error: %s: invalid ioctl\n", cmdname(argv[0]));
                rc = -EINVAL;
                goto out;
        }
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_LOV_SET_CONFIG, buf);
        if (rc)
                fprintf(stderr, "error: %s: ioctl error: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
out:
        free(uuidarray);
        return rc;
}

#define DEF_UUID_ARRAY_LEN (8192 / 40)

int jt_obd_lov_getconfig(int argc, char **argv)
{
        struct obd_ioctl_data data;
        struct lov_desc desc;
        struct obd_uuid *uuidarray;
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
                return -1;
        }

        memset(&desc, 0, sizeof(desc));
        obd_str2uuid(&desc.ld_uuid, argv[1]);
        desc.ld_tgt_count = DEF_UUID_ARRAY_LEN;
repeat:
        uuidarray = calloc(desc.ld_tgt_count, sizeof(*uuidarray));
        if (!uuidarray) {
                fprintf(stderr, "error: %s: no memory for %d uuid's\n",
                        cmdname(argv[0]), desc.ld_tgt_count);
                rc = -ENOMEM;
                goto out;
        }

        data.ioc_inllen1 = sizeof(desc);
        data.ioc_inlbuf1 = (char *)&desc;
        data.ioc_inllen2 = desc.ld_tgt_count * sizeof(*uuidarray);
        data.ioc_inlbuf2 = (char *)uuidarray;

        if (obd_ioctl_pack(&data, &buf, max)) {
                fprintf(stderr, "error: %s: invalid ioctl\n", cmdname(argv[0]));
                rc = -EINVAL;
                goto out;
        }
        rc = ioctl(fd, OBD_IOC_LOV_GET_CONFIG, buf);
        if (rc == -ENOSPC) {
                free(uuidarray);
                goto repeat;
        } else if (rc) {
                fprintf(stderr, "error: %s: ioctl error: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
        } else {
                struct obd_uuid *ptr;
                int i;

                if (obd_ioctl_unpack(&data, buf, max)) {
                        fprintf(stderr, "error: %s: invalid reply\n",
                                cmdname(argv[0]));
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
                for (i = 0, ptr = uuidarray; i < desc.ld_tgt_count; i++, ptr++)
                        printf("%u: %s\n", i, (char *)ptr);
        }
out:
        free(uuidarray);
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
        rc = l_ioctl(OBD_DEV_ID, IOC_LDLM_TEST, buf);
        if (rc)
                fprintf(stderr, "error: %s: test failed: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
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
        rc = l_ioctl(OBD_DEV_ID, IOC_LDLM_DUMP, buf);
        if (rc)
                fprintf(stderr, "error: %s failed: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
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
        rc = l_ioctl(OBD_DEV_ID, IOC_LDLM_REGRESS_START, buf);
        if (rc)
                fprintf(stderr, "error: %s: test failed: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));

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
        rc = l_ioctl(OBD_DEV_ID, IOC_LDLM_REGRESS_STOP, buf);

        if (rc)
                fprintf(stderr, "error: %s: test failed: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));
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
        rc = l_ioctl(OBD_DEV_ID, IOC_OSC_SET_ACTIVE, buf);
        if (rc)
                fprintf(stderr, "error: %s: failed: %s\n",
                        cmdname(argv[0]), strerror(rc = errno));

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
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CLIENT_RECOVER, buf);
        if (rc < 0) {
                fprintf(stderr, "error: %s: %s\n", cmdname(argv[0]),
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
                        cmdname(argv[0]), strerror(rc = errno));
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

static 
int do_add_uuid(char * func, char *uuid, ptl_nid_t nid, int nal) 
{
        char tmp[64];
        int rc;
        struct obd_ioctl_data data;

        IOC_INIT(data);
        data.ioc_nid = nid;
        data.ioc_inllen1 = strlen(uuid) + 1;
        data.ioc_inlbuf1 = uuid;
        data.ioc_nal = nal;

        IOC_PACK(func, data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_ADD_UUID, buf);
        if (rc) {
                fprintf(stderr, "IOC_PORTAL_ADD_UUID failed: %s\n",
                        strerror(errno));
                return -1;
        }

        printf ("Added uuid %s: %s\n", uuid, ptl_nid2str (tmp, nid));
        return 0;
}

int jt_obd_add_uuid(int argc, char **argv)
{
        ptl_nid_t nid = 0;
        int nal;
        
        if (argc != 4) {                
                return CMD_HELP;
        }

        if (ptl_parse_nid (&nid, argv[2]) != 0) {
                fprintf (stderr, "Can't parse NID %s\n", argv[2]);
                        return (-1);
        }

        nal = ptl_name2nal(argv[3]);

        if (nal == 0) {
                fprintf (stderr, "Can't parse NAL %s\n", argv[3]);
                return -1;
        }

        return do_add_uuid(argv[0], argv[1], nid, nal);
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

        if (nal == 0) {
                fprintf (stderr, "Can't parse NAL %s\n", argv[2]);
                return -1;
        }

        IOC_INIT(data);
        data.ioc_inllen1 = strlen(argv[1]) + 1;
        data.ioc_inlbuf1 = argv[1];
        data.ioc_nal = nal;

        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_CLOSE_UUID, buf);
        if (rc) {
                fprintf(stderr, "IOC_PORTAL_CLOSE_UUID failed: %s\n",
                        strerror(errno));
                return -1;
        }
        return 0;
}


int jt_obd_del_uuid(int argc, char **argv)
{
        int rc;
        struct obd_ioctl_data data;

        if (argc != 2) {
                fprintf(stderr, "usage: %s <uuid>\n", argv[0]);
                return 0;
        }

        IOC_INIT(data);

        if (strcmp (argv[1], "_all_"))
        {
                data.ioc_inllen1 = strlen(argv[1]) + 1;
                data.ioc_inlbuf1 = argv[1];
        }
        
        IOC_PACK(argv[0], data);
        rc = l_ioctl(OBD_DEV_ID, OBD_IOC_DEL_UUID, buf);
        if (rc) {
                fprintf(stderr, "IOC_PORTAL_DEL_UUID failed: %s\n",
                        strerror(errno));
                return -1;
        }
        return 0;
}

static void signal_server(int sig)
{
        if (sig == SIGINT) {
                do_disconnect("sigint", 1);
                exit(1);
        } else
                fprintf(stderr, "%s: got signal %d\n", cmdname("sigint"), sig);
}

int obd_initialize(int argc, char **argv)
{
        SHMEM_SETUP();
        register_ioc_dev(OBD_DEV_ID, OBD_DEV_PATH);

        return 0;
}


void obd_cleanup(int argc, char **argv)
{
        struct sigaction sigact;

        sigact.sa_handler = signal_server;
        sigfillset(&sigact.sa_mask);
        sigact.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sigact, NULL);

        do_disconnect(argv[0], 1);
}
