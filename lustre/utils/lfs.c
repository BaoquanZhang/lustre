/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include <lnet/api-support.h>
#include <lnet/lnetctl.h>

#include <liblustre.h>
#include <linux/lustre_idl.h>
#include <lustre/liblustreapi.h>
#include <lustre/lustre_user.h>

#include "parser.h"
#include "obdctl.h"

unsigned int libcfs_subsystem_debug = 0;

/* all functions */
static int lfs_setstripe(int argc, char **argv);
static int lfs_find(int argc, char **argv);
static int lfs_getstripe(int argc, char **argv);
static int lfs_osts(int argc, char **argv);
static int lfs_df(int argc, char **argv);
static int lfs_check(int argc, char **argv);
static int lfs_catinfo(int argc, char **argv);
#ifdef HAVE_QUOTA_SUPPORT
static int lfs_quotachown(int argc, char **argv);
static int lfs_quotacheck(int argc, char **argv);
static int lfs_quotaon(int argc, char **argv);
static int lfs_quotaoff(int argc, char **argv);
static int lfs_setquota(int argc, char **argv);
static int lfs_quota(int argc, char **argv);
#endif
static int lfs_join(int argc, char **argv);

/* all avaialable commands */
command_t cmdlist[] = {
        {"setstripe", lfs_setstripe, 0,
         "Create a new file with a specific striping pattern or\n"
         "set the default striping pattern on an existing directory or\n"
         "delete the default striping pattern from an existing directory\n"
         "usage: setstripe <filename|dirname> <stripe size> <stripe start> <stripe count>\n"
         "       or \n"
         "       setstripe -d <dirname>   (to delete default striping)\n"
         "\tstripe size:  Number of bytes on each OST (0 filesystem default)\n"
         "\tstripe start: OST index of first stripe (-1 filesystem default)\n"
         "\tstripe count: Number of OSTs to stripe over (0 default, -1 all)"},
        {"find", lfs_find, 0,
         "To list the extended attributes for a given filename or files in a\n"
         "directory or recursively for all files in a directory tree.\n"
         "usage: find [--obd <uuid>] [--quiet | --verbose] [--recursive] <dir|file> ..."},
        {"getstripe", lfs_getstripe, 0,
         "To list the striping pattern for given filename.\n"
         "usage:getstripe <filename>"},
        {"check", lfs_check, 0,
         "Display the status of MDS or OSTs (as specified in the command)\n"
         "or all the servers (MDS and OSTs).\n"
         "usage: check <osts|mds|servers>"},
        {"catinfo", lfs_catinfo, 0,
         "Show information of specified type logs.\n"
         "usage: catinfo {keyword} [node name]\n"
         "\tkeywords are one of followings: config, deletions.\n"
         "\tnode name must be provided when use keyword config."},
        {"join", lfs_join, 0,
         "join two lustre files into one - join A, B, will be like cat B >> A & del B\n"
         "usage: join <filename_A> <filename_B>\n"},
        {"osts", lfs_osts, 0, "osts"},
        {"df", lfs_df, 0,
         "report filesystem disk space usage or inodes usage"
         "of each MDS/OSD.\n"
         "Usage: df [-i] [-h] [path]"},
#ifdef HAVE_QUOTA_SUPPORT
        {"quotachown",lfs_quotachown, 0,
         "Change files' owner or group on the specified filesystem.\n"
         "usage: quotachown [-i] <filesystem>\n"
         "\t-i: ignore error if file is not exist\n"},
        {"quotacheck", lfs_quotacheck, 0,
         "Scan the specified filesystem for disk usage, and create,\n"
         "or update quota files.\n"
         "usage: quotacheck [ -ug ] <filesystem>"},
        {"quotaon", lfs_quotaon, 0, "Turn filesystem quotas on.\n"
         "usage: quotaon [ -ugf ] <filesystem>"},
        {"quotaoff", lfs_quotaoff, 0, "Turn filesystem quotas off.\n"
         "usage: quotaoff [ -ug ] <filesystem>"},
        {"setquota", lfs_setquota, 0, "Set filesystem quotas.\n"
         "usage: setquota [ -u | -g ] <name> <block-softlimit> <block-hardlimit> <inode-softlimit> <inode-hardlimit> <filesystem>\n"
         "       setquota -t [ -u | -g ] <block-grace> <inode-grace> <filesystem>"},
        {"quota", lfs_quota, 0, "Display disk usage and limits.\n"
         "usage: quota [ -o obd_uuid ] [ -u | -g ] [name] <filesystem>"},
#endif
        {"help", Parser_help, 0, "help"},
        {"exit", Parser_quit, 0, "quit"},
        {"quit", Parser_quit, 0, "quit"},
        { 0, 0, 0, NULL }
};

/* functions */
static int lfs_setstripe(int argc, char **argv)
{
        char *fname;
        int result;
        long st_size;
        int  st_offset, st_count;
        char *end;

        if (argc != 5 && argc != 3)
                return CMD_HELP;


        if (argc == 3) {
                if (strcmp(argv[1], "-d") != 0)
                        return CMD_HELP;

                fname = argv[2];
                st_size = 0;
                st_offset = -1;
                st_count = 0;
        } else {
                fname = argv[1];

                /* get the stripe size */
                st_size = strtoul(argv[2], &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe size '%s'\n",
                                argv[0], argv[2]);
                        return CMD_HELP;
                }

                /* get the stripe offset */
                st_offset = strtoul(argv[3], &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe offset '%s'\n",
                                argv[0], argv[3]);
                        return CMD_HELP;
                }
                /* get the stripe count */
                st_count = strtoul(argv[4], &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe count '%s'\n",
                                argv[0], argv[4]);
                        return CMD_HELP;
                }
        }

        result = llapi_file_create(fname, st_size, st_offset, st_count, 0);
        if (result)
                fprintf(stderr, "error: %s: create stripe file failed\n",
                                argv[0]);

        return result;
}

static int lfs_find(int argc, char **argv)
{
        struct option long_opts[] = {
                {"obd", 1, 0, 'o'},
                {"quiet", 0, 0, 'q'},
                {"recursive", 0, 0, 'r'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };
        char short_opts[] = "ho:qrv";
        int quiet, verbose, recursive, c, rc;
        struct obd_uuid *obduuid = NULL;

        optind = 0;
        quiet = verbose = recursive = 0;
        while ((c = getopt_long(argc, argv, short_opts,
                                        long_opts, NULL)) != -1) {
                switch (c) {
                case 'o':
                        if (obduuid) {
                                fprintf(stderr,
                                        "error: %s: only one obduuid allowed",
                                        argv[0]);
                                return CMD_HELP;
                        }
                        obduuid = (struct obd_uuid *)optarg;
                        break;
                case 'q':
                        quiet++;
                        verbose = 0;
                        break;
                case 'r':
                        recursive = 1;
                        break;
                case 'v':
                        verbose++;
                        quiet = 0;
                        break;
                case '?':
                        return CMD_HELP;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                        break;
                }
        }

        if (optind >= argc)
                return CMD_HELP;

        do {
                rc = llapi_find(argv[optind], obduuid, recursive,verbose,quiet);
        } while (++optind < argc && !rc);

        if (rc)
                fprintf(stderr, "error: %s: find failed\n", argv[0]);
        return rc;
}

static int lfs_getstripe(int argc, char **argv)
{
        struct option long_opts[] = {
                {"quiet", 0, 0, 'q'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };
        char short_opts[] = "qv";
        int quiet, verbose, recursive, c, rc;
        struct obd_uuid *obduuid = NULL;

        optind = 0;
        quiet = verbose = recursive = 0;
        while ((c = getopt_long(argc, argv, short_opts,
                                        long_opts, NULL)) != -1) {
                switch (c) {
                case 'o':
                        if (obduuid) {
                                fprintf(stderr,
                                        "error: %s: only one obduuid allowed",
                                        argv[0]);
                                return CMD_HELP;
                        }
                        obduuid = (struct obd_uuid *)optarg;
                        break;
                case 'q':
                        quiet++;
                        verbose = 0;
                        break;
                case 'v':
                        verbose++;
                        quiet = 0;
                        break;
                case '?':
                        return CMD_HELP;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                        break;
                }
        }

        if (optind >= argc)
                return CMD_HELP;

        do {
                rc = llapi_find(argv[optind], obduuid, recursive,verbose,quiet);
        } while (++optind < argc && !rc);

        if (rc)
                fprintf(stderr, "error: %s: getstripe failed for %s\n",
                        argv[0], argv[1]);

        return rc;
}

static int lfs_osts(int argc, char **argv)
{
        FILE *fp;
        struct mntent *mnt = NULL;
        struct obd_uuid *obduuid = NULL;
        int rc=0;

        if (argc != 1)
                return CMD_HELP;

        fp = setmntent(MOUNTED, "r");

        if (fp == NULL) {
                 fprintf(stderr, "%s: setmntent(%s): %s:", argv[0], MOUNTED,
                        strerror (errno));
        } else {
                mnt = getmntent(fp);
                while (feof(fp) == 0 && ferror(fp) ==0) {
                        if (llapi_is_lustre_mnttype(mnt->mnt_type)) {
                                rc = llapi_find(mnt->mnt_dir, obduuid, 0, 0, 0);
                                if (rc)
                                        fprintf(stderr,
                                               "error: %s: failed on %s\n",
                                               argv[0], mnt->mnt_dir);
                        }
                        mnt = getmntent(fp);
                }
                endmntent(fp);
        }

        return rc;
}

#define COOK(value)                                                     \
({                                                                      \
        int radix = 0;                                                  \
        while (value > 1024) {                                          \
                value /= 1024;                                          \
                radix++;                                                \
        }                                                               \
        radix;                                                          \
})
#define UUF     "%-20s"
#define CSF     "%9s"
#define CDF     "%9llu"
#define HSF     "%8s"
#define HDF     "%6.1f"
#define RSF     "%5s"
#define RDF     "%5d"

static int path2mnt(char *path, FILE *fp, char *mntdir, int dir_len)
{
        char rpath[PATH_MAX] = {'\0'};
        struct mntent *mnt;
        int rc, len, out_len = 0;

        if (!realpath(path, rpath)) {
                rc = -errno;
                fprintf(stderr, "error: lfs df: invalid path '%s': %s\n",
                        path, strerror(-rc));
                return rc;
        }

        len = 0;
        mnt = getmntent(fp);
        while (feof(fp) == 0 && ferror(fp) == 0) {
                if (llapi_is_lustre_mnttype(mnt->mnt_type)) {
                        len = strlen(mnt->mnt_dir);
                        if (len > out_len &&
                            !strncmp(rpath, mnt->mnt_dir, len)) {
                                out_len = len;
                                memset(mntdir, 0, dir_len);
                                strncpy(mntdir, mnt->mnt_dir, dir_len);
                        }
                }
                mnt = getmntent(fp);
        }

        if (out_len > 0)
                return 0;
        
        fprintf(stderr, "error: lfs df: %s isn't mounted on lustre\n", path);
        return -EINVAL;
}

static int showdf(char *mntdir, struct obd_statfs *stat,
                  struct obd_uuid *uuid, int ishow, int cooked,
                  char *type, int index, int rc)
{
        __u64 avail, used, total;
        double ratio = 0;
        int obd_type;
        char *suffix = "KMGTPEZY";
        char tbuf[10], ubuf[10], abuf[10], rbuf[10];

        if (!uuid || !stat || !type)
                return -EINVAL;
        if (!strncmp(type, "MDT", 3)) {
                obd_type = 0;
        } else if(!strncmp(type, "OST", 3)){
                obd_type = 1;
        } else {
                fprintf(stderr, "error: lfs df: invalid type '%s'\n", type);
                return -EINVAL;
        }

        if (rc == 0) {
                if (ishow) {
                        avail = stat->os_ffree;
                        used = stat->os_files - stat->os_ffree;
                        total = stat->os_files;
                } else {
                        avail = stat->os_bavail * stat->os_bsize / 1024;
                        used = stat->os_blocks - stat->os_bavail;
                        used = used * stat->os_bsize / 1024;
                        total = stat->os_blocks * stat->os_bsize / 1024;
                }

                if (total > 0)
                        ratio = (double)used / (double)total;

                if (cooked) {
                        int i;
                        double total_d, used_d, avail_d;
                        
                        total_d = (double)total;
                        i = COOK(total_d);
                        if (i > 0)
                                sprintf(tbuf, HDF"%c", total_d, suffix[i - 1]);
                        else
                                sprintf(tbuf, CDF, total);

                        used_d = (double)used;
                        i = COOK(used_d);
                        if (i > 0)
                                sprintf(ubuf, HDF"%c", used_d, suffix[i - 1]);
                        else
                                sprintf(ubuf, CDF, used);

                        avail_d = (double)avail;
                        i = COOK(avail_d);
                        if (i > 0)
                                sprintf(abuf, HDF"%c", avail_d, suffix[i - 1]);
                        else
                                sprintf(abuf, CDF, avail);
                } else {
                        sprintf(tbuf, CDF, total);
                        sprintf(ubuf, CDF, used);
                        sprintf(abuf, CDF, avail);
                }

                sprintf(rbuf, RDF, (int)(ratio * 100));
                if (obd_type == 0)
                        printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s[MDT:%d]\n",
                               (char *)uuid, tbuf, ubuf, abuf, rbuf,
                               mntdir, index);
                else
                        printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s[OST:%d]\n",
                               (char *)uuid, tbuf, ubuf, abuf, rbuf,
                               mntdir, index);

                return 0;
        }
        switch (rc) {
        case -ENODATA:
                printf(UUF": inactive OST\n", (char *)uuid);
                break;
        default:
                printf(UUF": %s\n", (char *)uuid, strerror(-rc));
                break;
        }

        return 0;
}

static int mntdf(char *mntdir, int ishow, int cooked)
{
        struct obd_statfs stat_buf;
        struct obd_uuid uuid_buf;
        __u32 index;
        __u64 avail_sum, used_sum, total_sum;
        char tbuf[10], ubuf[10], abuf[10], rbuf[10];        
        double ratio_sum;
        int rc;

        if (ishow)
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", "Inodes", "IUsed", "IFree",
                       "IUse%", "Mounted on");
        else
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", "1K-blocks", "Used", "Available",
                       "Use%", "Mounted on");

        avail_sum = total_sum = 0; 
        for (index = 0; ; index++) {
                memset(&stat_buf, 0, sizeof(struct obd_statfs));
                memset(&uuid_buf, 0, sizeof(struct obd_uuid));
                rc = llapi_obd_statfs(mntdir, LL_STATFS_MDC, index,
                                      &stat_buf, &uuid_buf);
                if (rc == -ENODEV)
                        break;

                if (rc == -ENOTCONN || rc == -ETIMEDOUT || rc == -EIO ||
                    rc == -ENODATA || rc == 0) {
                        showdf(mntdir, &stat_buf, &uuid_buf, ishow, cooked,
                               "MDT", index, rc);
                } else {
                        fprintf(stderr,
                                "error: llapi_obd_statfs(%s): %s (%d)\n",
                                uuid_buf.uuid, strerror(-rc), rc);
                        return rc;
                }
                if (!rc && ishow) {
                        avail_sum += stat_buf.os_ffree;
                        total_sum += stat_buf.os_files;
                }
        }

        for (index = 0;;index++) {
                memset(&stat_buf, 0, sizeof(struct obd_statfs));
                memset(&uuid_buf, 0, sizeof(struct obd_uuid));
                rc = llapi_obd_statfs(mntdir, LL_STATFS_LOV, index,
                                      &stat_buf, &uuid_buf);
                if (rc == -ENODEV)
                        break;

                if (rc == -ENOTCONN || rc == -ETIMEDOUT || rc == -EIO ||
                    rc == -ENODATA || rc == 0) {
                        showdf(mntdir, &stat_buf, &uuid_buf, ishow, cooked,
                               "OST", index, rc);
                } else {
                        fprintf(stderr,
                                "error: llapi_obd_statfs failed: %s (%d)\n",
                                strerror(-rc), rc);
                        return rc;
                }
                if (!rc && !ishow) {
                        __u64 avail, total;
                        avail = stat_buf.os_bavail * stat_buf.os_bsize;
                        avail /= 1024;
                        total = stat_buf.os_blocks * stat_buf.os_bsize;
                        total /= 1024;
                        
                        avail_sum += avail;
                        total_sum += total;
                }
        }

        used_sum = total_sum - avail_sum;
        ratio_sum = (double)(total_sum - avail_sum) / (double)total_sum;
        sprintf(rbuf, RDF, (int)(ratio_sum * 100));
        if (cooked) {
                int i;
                char *suffix = "KMGTPEZY";
                double total_sum_d, used_sum_d, avail_sum_d;

                total_sum_d = (double)total_sum;
                i = COOK(total_sum_d);
                if (i > 0)
                        sprintf(tbuf, HDF"%c", total_sum_d, suffix[i - 1]);
                else
                        sprintf(tbuf, CDF, total_sum);
                
                used_sum_d = (double)used_sum;
                i = COOK(used_sum_d);
                if (i > 0)
                        sprintf(ubuf, HDF"%c", used_sum_d, suffix[i - 1]);
                else
                        sprintf(ubuf, CDF, used_sum);
                        
                avail_sum_d = (double)avail_sum;
                i = COOK(avail_sum_d);
                if (i > 0)
                        sprintf(abuf, HDF"%c", avail_sum_d, suffix[i - 1]);
                else
                        sprintf(abuf, CDF, avail_sum);
        } else {
                sprintf(tbuf, CDF, total_sum);
                sprintf(ubuf, CDF, used_sum);
                sprintf(abuf, CDF, avail_sum);
        }
       
        printf("\n"UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
               "filesystem summary:", tbuf, ubuf, abuf, rbuf, mntdir);

        return 0;
}

static int lfs_df(int argc, char **argv)
{
        FILE *fp;
        char *path = NULL;
        struct mntent *mnt = NULL;
        char mntdir[PATH_MAX] = {'\0'};
        int ishow = 0, cooked = 0;
        int c, rc = 0;

        optind = 0;
        while ((c = getopt(argc, argv, "ih")) != -1) {
                switch (c) {
                case 'i':
                        ishow = 1;
                        break;
                case 'h':
                        cooked = 1;
                        break;
                default:
                        return CMD_HELP;
                }
        }
        if (optind < argc )
                path = argv[optind];

        fp = setmntent(MOUNTED, "r");
        if (fp == NULL) {
                rc = -errno;
                fprintf(stderr, "error: %s: open %s failed( %s )\n",
                        argv[0], MOUNTED, strerror(errno));
                return rc;
        }
        if (path) {
                rc = path2mnt(path, fp, mntdir, sizeof(mntdir));
                if (rc) {
                        endmntent(fp);
                        return rc;
                }

                rc = mntdf(mntdir, ishow, cooked);
                printf("\n");
                endmntent(fp);
        } else {
                mnt = getmntent(fp);
                while (feof(fp) == 0 && ferror(fp) == 0) {
                        if (llapi_is_lustre_mnttype(mnt->mnt_type)) {
                                rc = mntdf(mnt->mnt_dir, ishow, cooked);
                                if (rc)
                                        break;
                                printf("\n");
                        }
                        mnt = getmntent(fp);
                }
                endmntent(fp);
        }

        return rc;
}

static int lfs_check(int argc, char **argv)
{
        int rc;
        FILE *fp;
        struct mntent *mnt = NULL;
        int num_types = 1;
        char *obd_types[2];
        char obd_type1[4];
        char obd_type2[4];

        if (argc != 2)
                return CMD_HELP;

        obd_types[0] = obd_type1;
        obd_types[1] = obd_type2;

        if (strcmp(argv[1], "osts") == 0) {
                strcpy(obd_types[0], "osc");
        } else if (strcmp(argv[1], "mds") == 0) {
                strcpy(obd_types[0], "mdc");
        } else if (strcmp(argv[1], "servers") == 0) {
                num_types = 2;
                strcpy(obd_types[0], "osc");
                strcpy(obd_types[1], "mdc");
        } else {
                fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[1]);
                        return CMD_HELP;
        }

        fp = setmntent(MOUNTED, "r");
        if (fp == NULL) {
                 fprintf(stderr, "setmntent(%s): %s:", MOUNTED,
                        strerror (errno));
        } else {
                mnt = getmntent(fp);
                while (feof(fp) == 0 && ferror(fp) ==0) {
                        if (llapi_is_lustre_mnttype(mnt->mnt_type))
                                break;
                        mnt = getmntent(fp);
                }
                endmntent(fp);
        }

        if (!mnt) {
                fprintf(stderr, "No suitable Lustre mount found\n");
                return -1;
        }

        rc = llapi_target_check(num_types, obd_types, mnt->mnt_dir);

        if (rc)
                fprintf(stderr, "error: %s: %s status failed\n",
                                argv[0],argv[1]);

        return rc;

}

static int lfs_catinfo(int argc, char **argv)
{
        FILE *fp;
        struct mntent *mnt = NULL;
        int rc;

        if (argc < 2 || (!strcmp(argv[1],"config") && argc < 3))
                return CMD_HELP;

        if (strcmp(argv[1], "config") && strcmp(argv[1], "deletions"))
                return CMD_HELP;

        fp = setmntent(MOUNTED, "r");
        if (fp == NULL) {
                 fprintf(stderr, "setmntent(%s): %s:", MOUNTED,
                         strerror(errno));
        } else {
                mnt = getmntent(fp);
                while (feof(fp) == 0 && ferror(fp) == 0) {
                        if (llapi_is_lustre_mnttype(mnt->mnt_type))
                                break;
                        mnt = getmntent(fp);
                }
                endmntent(fp);
        }

        if (mnt) {
                if (argc == 3)
                        rc = llapi_catinfo(mnt->mnt_dir, argv[1], argv[2]);
                else
                        rc = llapi_catinfo(mnt->mnt_dir, argv[1], NULL);
        } else {
                fprintf(stderr, "no lustre_lite mounted.\n");
                rc = -1;
        }

        return rc;
}

int lfs_join(int argc, char **argv)
{
        char *name_head, *name_tail;
        int fd, rc;
        loff_t size;

        if (argc != 3)
                return CMD_HELP;
        name_head = argv[1];
        fd = open(name_head, O_WRONLY);
        if (fd < 0) {
                fprintf(stderr, "Can not open name_head %s rc=%d\n",
                        name_head, fd);
                return fd;
        }
        size = lseek(fd, 0, SEEK_END);
        if (size % JOIN_FILE_ALIGN) {
                fprintf(stderr,"head file %s size %llu must be mutiple of %d\n",
                        name_head, size, JOIN_FILE_ALIGN);
                rc = -EINVAL;
                goto out;
        }
        name_tail = argv[2];
        rc = ioctl(fd, LL_IOC_JOIN, name_tail);
out:
        close(fd);
        if (rc) {
                fprintf(stderr, "Lustre joining files: %s, %s, failed\n",
                        argv[1], argv[2]);
        }
        return rc;
}

#ifdef HAVE_QUOTA_SUPPORT
static int lfs_quotachown(int argc, char **argv)
{

        int c,rc;
        int flag = 0;

        while ((c = getopt(argc, argv, "i")) != -1) {
                switch (c) {
                case 'i':
                        flag++;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }
        if (optind == argc)
                return CMD_HELP;
        rc = llapi_quotachown(argv[optind], flag);
        if(rc)
                fprintf(stderr,"error: change file owner/group failed.\n");
        return rc;
}


static int lfs_quotacheck(int argc, char **argv)
{
        int c, check_type = 0;
        char *mnt;
        struct if_quotacheck qchk;
        struct if_quotactl qctl;
        char *obd_type = qchk.obd_type;
        char *obd_uuid = qchk.obd_uuid.uuid;
        int rc;

        memset(&qchk, 0, sizeof(qchk));

        optind = 0;
        while ((c = getopt(argc, argv, "ug")) != -1) {
                switch (c) {
                case 'u':
                        check_type |= 0x01;
                        break;
                case 'g':
                        check_type |= 0x02;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (check_type)
                check_type--;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;
        qctl.qc_id = QFMT_LDISKFS;
        qctl.qc_type = check_type;
        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                fprintf(stderr, "quota off failed: %s\n", strerror(errno));
                return rc;
        }

        rc = llapi_quotacheck(mnt, check_type);
        if (rc) {
                fprintf(stderr, "quotacheck failed: %s\n", strerror(errno));
                return rc;
        }

        rc = llapi_poll_quotacheck(mnt, &qchk);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type, obd_uuid);
                fprintf(stderr, "quota check failed: %s\n", strerror(errno));
                return rc;
        }

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAON;
        qctl.qc_id = QFMT_LDISKFS;
        qctl.qc_type = check_type;
        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ",
                                qctl.obd_type, qctl.obd_uuid.uuid);
                fprintf(stderr, "%s turn on quota failed: %s\n",
                        argv[0], strerror(errno));
                return rc;
        }

        return 0;
}

static int lfs_quotaon(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = qctl.obd_type;
        char *obd_uuid = qctl.obd_uuid.uuid;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAON;
        qctl.qc_id = QFMT_LDISKFS;

        optind = 0;
        while ((c = getopt(argc, argv, "ugf")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                case 'f':
                        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type, obd_uuid);
                fprintf(stderr, "%s failed: %s\n", argv[0], strerror(errno));
                return rc;
        }

        return 0;
}

static int lfs_quotaoff(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = qctl.obd_type;
        char *obd_uuid = qctl.obd_uuid.uuid;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;

        optind = 0;
        while ((c = getopt(argc, argv, "ug")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type, obd_uuid);
                fprintf(stderr, "quotaoff failed: %s\n", strerror(errno));
                return rc;
        }

        return 0;
}

static int name2id(unsigned int *id, char *name, int type)
{
        if (type == USRQUOTA) {
                struct passwd *entry;

                if (!(entry = getpwnam(name))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *id = entry->pw_uid;
        } else {
                struct group *entry;

                if (!(entry = getgrnam(name))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *id = entry->gr_gid;
        }

        return 0;
}

static int id2name(char **name, unsigned int id, int type)
{
        if (type == USRQUOTA) {
                struct passwd *entry;

                if (!(entry = getpwuid(id))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *name = entry->pw_name;
        } else {
                struct group *entry;

                if (!(entry = getgrgid(id))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *name = entry->gr_name;
        }

        return 0;
}

#define ARG2INT(nr, str, msg)                                           \
do {                                                                    \
        char *endp;                                                     \
        nr = strtol(str, &endp, 0);                                     \
        if (*endp) {                                                    \
                fprintf(stderr, "error: bad %s: %s\n", msg, str);       \
                return CMD_HELP;                                        \
        }                                                               \
} while (0)

int lfs_setquota(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = qctl.obd_type;
        char *obd_uuid = qctl.obd_uuid.uuid;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_SETQUOTA;

        optind = 0;
        while ((c = getopt(argc, argv, "ugt")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                case 't':
                        qctl.qc_cmd = LUSTRE_Q_SETINFO;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;

        if (qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: user and group quotas can't be set "
                                "both\n");
                return CMD_HELP;
        }

        if (qctl.qc_cmd == LUSTRE_Q_SETQUOTA) {
                struct obd_dqblk *dqb = &qctl.qc_dqblk;

                if (optind + 6 != argc)
                        return CMD_HELP;

                rc = name2id(&qctl.qc_id, argv[optind++], qctl.qc_type);
                if (rc) {
                        fprintf(stderr, "error: find id for name %s failed: %s\n",
                                argv[optind - 1], strerror(errno));
                        return CMD_HELP;
                }

                ARG2INT(dqb->dqb_bsoftlimit, argv[optind++], "block-softlimit");
                ARG2INT(dqb->dqb_bhardlimit, argv[optind++], "block-hardlimit");
                ARG2INT(dqb->dqb_isoftlimit, argv[optind++], "inode-softlimit");
                ARG2INT(dqb->dqb_ihardlimit, argv[optind++], "inode-hardlimit");

                dqb->dqb_valid = QIF_LIMITS;
        } else {
                struct obd_dqinfo *dqi = &qctl.qc_dqinfo;

                if (optind + 3 != argc)
                        return CMD_HELP;

                ARG2INT(dqi->dqi_bgrace, argv[optind++], "block-grace");
                ARG2INT(dqi->dqi_igrace, argv[optind++], "inode-grace");
        }

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type, obd_uuid);
                fprintf(stderr, "setquota failed: %s\n", strerror(errno));
                return rc;
        }

        return 0;
}

static inline char *type2name(int check_type)
{
        if (check_type == USRQUOTA)
                return "user";
        else if (check_type == GRPQUOTA)
                return "group";
        else
                return "unknown";
}


static void grace2str(time_t seconds,char *buf)
{
        uint minutes, hours, days;

        minutes = (seconds + 30) / 60;
        hours = minutes / 60;
        minutes %= 60;
        days = hours / 24;
        hours %= 24;
        if (days >= 2)
                snprintf(buf, 40, "%ddays", days);
        else
                snprintf(buf, 40, "%02d:%02d", hours + days * 24, minutes);
}


static void diff2str(time_t seconds, char *buf, time_t now)
{

        buf[0] = 0;
        if (!seconds)
                return;
        if (seconds <= now) {
                strcpy(buf, "none");
                return;
        }
        grace2str(seconds - now, buf);
}

static void print_quota_title(char *name, struct if_quotactl *qctl)
{
        printf("Disk quotas for %s %s (%cid %u):\n",
               type2name(qctl->qc_type), name,
               *type2name(qctl->qc_type), qctl->qc_id);
        printf("%15s%8s %7s%8s%8s%8s %7s%8s%8s\n",
               "Filesystem",
               "blocks", "quota", "limit", "grace",
               "files", "quota", "limit", "grace");
}

static void print_quota(char *mnt, struct if_quotactl *qctl, int ost_only)
{
        time_t now;

        time(&now);

        if (qctl->qc_cmd == LUSTRE_Q_GETQUOTA || qctl->qc_cmd == Q_GETOQUOTA) {
                int bover = 0, iover = 0;
                struct obd_dqblk *dqb = &qctl->qc_dqblk;

                if (dqb->dqb_bhardlimit &&
                    toqb(dqb->dqb_curspace) > dqb->dqb_bhardlimit) {
                        bover = 1;
                } else if (dqb->dqb_bsoftlimit &&
                           toqb(dqb->dqb_curspace) > dqb->dqb_bsoftlimit) {
                        if (dqb->dqb_btime > now) {
                                bover = 2;
                        } else {
                                bover = 3;
                        }
                }

                if (dqb->dqb_ihardlimit &&
                    dqb->dqb_curinodes > dqb->dqb_ihardlimit) {
                        iover = 1;
                } else if (dqb->dqb_isoftlimit &&
                           dqb->dqb_curinodes > dqb->dqb_isoftlimit) {
                        if (dqb->dqb_btime > now) {
                                iover = 2;
                        } else {
                                iover = 3;
                        }
                }

#if 0           /* XXX: always print quotas even when no usages */
                if (dqb->dqb_curspace || dqb->dqb_curinodes)
#endif
                {
                        char numbuf[3][32];
                        char timebuf[40];

                        if (strlen(mnt) > 15)
                                printf("%s\n%15s", mnt, "");
                        else
                                printf("%15s", mnt);

                        if (bover)
                                diff2str(dqb->dqb_btime, timebuf, now);

                        sprintf(numbuf[0], "%llu", toqb(dqb->dqb_curspace));
                        sprintf(numbuf[1], "%llu", dqb->dqb_bsoftlimit);
                        sprintf(numbuf[2], "%llu", dqb->dqb_bhardlimit);
                        printf(" %7s%c %6s %7s %7s",
                               numbuf[0], bover ? '*' : ' ', numbuf[1],
                               numbuf[2], bover > 1 ? timebuf : "");

                        if (iover)
                                diff2str(dqb->dqb_itime, timebuf, now);

                        sprintf(numbuf[0], "%llu", dqb->dqb_curinodes);
                        sprintf(numbuf[1], "%llu", dqb->dqb_isoftlimit);
                        sprintf(numbuf[2], "%llu", dqb->dqb_ihardlimit);
                        if (!ost_only)
                                printf(" %7s%c %6s %7s %7s",
                                       numbuf[0], iover ? '*' : ' ', numbuf[1],
                                       numbuf[2], iover > 1 ? timebuf : "");
                        printf("\n");
                }
        } else if (qctl->qc_cmd == LUSTRE_Q_GETINFO ||
                   qctl->qc_cmd == Q_GETOINFO) {
                char bgtimebuf[40];
                char igtimebuf[40];

                grace2str(qctl->qc_dqinfo.dqi_bgrace, bgtimebuf);
                grace2str(qctl->qc_dqinfo.dqi_igrace, igtimebuf);
                printf("Block grace time: %s; Inode grace time: %s\n",
                       bgtimebuf, igtimebuf);
        }
}

static void print_mds_quota(char *mnt, struct if_quotactl *qctl)
{
        int rc;

        /* XXX: this is a flag to mark that only mds quota is wanted */
        qctl->qc_dqblk.dqb_valid = 1;
        rc = llapi_quotactl(mnt, qctl);
        if (rc) {
                fprintf(stderr, "quotactl failed: %s\n", strerror(errno));
                return;
        }
        qctl->qc_dqblk.dqb_valid = 0;

        print_quota(qctl->obd_uuid.uuid, qctl, 0);
}

static void print_lov_quota(char *mnt, struct if_quotactl *qctl)
{
        DIR *dir;
        struct obd_uuid uuids[1024], *uuidp;
        int obdcount = 1024;
        int i, rc;

        dir = opendir(mnt);
        if (!dir) {
                fprintf(stderr, "open %s failed: %s\n", mnt, strerror(errno));
                return;
        }

        rc = llapi_lov_get_uuids(dirfd(dir), uuids, &obdcount);
        if (rc != 0) {
                fprintf(stderr, "get ost uuid failed: %s\n", strerror(errno));
                goto out;
        }

        for (i = 0, uuidp = uuids; i < obdcount; i++, uuidp++) {
                memcpy(&qctl->obd_uuid, uuidp, sizeof(*uuidp));

                rc = llapi_quotactl(mnt, qctl);
                if (rc) {
                        fprintf(stderr, "%s quotactl failed: %s\n",
                                uuidp->uuid, strerror(errno));
                        continue;
                }

                print_quota(uuidp->uuid, qctl, 1);
        }

out:
        closedir(dir);
        return;
}

static int lfs_quota(int argc, char **argv)
{
        int c;
        char *name = NULL, *mnt;
        struct if_quotactl qctl;
        char *obd_type = qctl.obd_type;
        char *obd_uuid = qctl.obd_uuid.uuid;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_GETQUOTA;

        optind = 0;
        while ((c = getopt(argc, argv, "ugto:")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                case 't':
                        qctl.qc_cmd = LUSTRE_Q_GETINFO;
                        break;
                case 'o':
                        strncpy(obd_uuid, optarg, sizeof(qctl.obd_uuid));
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;

        if (qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: user or group can't be specified"
                                "both\n");
                return CMD_HELP;
        }

        if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA) {
                if (optind + 2 != argc)
                        return CMD_HELP;

                name = argv[optind++];
                rc = name2id(&qctl.qc_id, name, qctl.qc_type);
                if (rc) {
                        fprintf(stderr, "error: find id for name %s failed: %s\n",
                                name, strerror(errno));
                        return CMD_HELP;
                }
                print_quota_title(name, &qctl);
        } else if (optind + 1 != argc) {
                return CMD_HELP;
        }

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type, obd_uuid);
                fprintf(stderr, "quota failed: %s\n", strerror(errno));
                return rc;
        }

        if (!name)
                rc = id2name(&name, getuid(), qctl.qc_type);

        if (*obd_uuid) {
                mnt = "";
                name = obd_uuid;
        }

        print_quota(mnt, &qctl, 0);

        if (!*obd_uuid && qctl.qc_cmd != LUSTRE_Q_GETINFO) {
                print_mds_quota(mnt, &qctl);
                print_lov_quota(mnt, &qctl);
        }

        return 0;
}
#endif /* HAVE_QUOTA_SUPPORT */

int main(int argc, char **argv)
{
        int rc;

        setlinebuf(stdout);

        ptl_initialize(argc, argv);
        if (obd_initialize(argc, argv) < 0)
                exit(2);
        if (dbg_initialize(argc, argv) < 0)
                exit(3);

        Parser_init("lfs > ", cmdlist);

        if (argc > 1) {
                rc = Parser_execarg(argc - 1, argv + 1, cmdlist);
        } else {
                rc = Parser_commands();
        }

        obd_finalize(argc, argv);
        return rc;
}
