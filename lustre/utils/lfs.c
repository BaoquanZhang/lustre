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
#include <time.h>
#include <ctype.h>

#include <lnet/api-support.h>
#include <lnet/lnetctl.h>

#include <liblustre.h>
#include <lustre/lustre_idl.h>
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
         "usage: setstripe <filename|dirname> <stripe_size> <stripe_index> <stripe_count>\n"
         "       or \n"
         "       setstripe <filename|dirname> [--size|-s stripe_size]\n"
         "                                    [--index|-i stripe_index]\n"
         "                                    [--count|-c stripe_count]\n"
         "       or \n"
         "       setstripe -d <dirname>   (to delete default striping)\n"
         "\tstripe_size:  Number of bytes on each OST (0 filesystem default)\n"
         "\t              Can be specified with k, m or g (in KB, MB and GB respectively)\n"
         "\tstripe_index: OST index of first stripe (-1 filesystem default)\n"
         "\tstripe_count: Number of OSTs to stripe over (0 default, -1 all)"},
        {"getstripe", lfs_getstripe, 0,
         "To list the striping info for a given filename or files in a\n"
         "directory or recursively for all files in a directory tree.\n"
         "usage: getstripe [--obd|-O <uuid>] [--quiet | -q] [--verbose | -v]\n"
         "                 [--recursive | -r] <dir|file> ..."},
        {"find", lfs_find, 0,
         "To find files that match given parameters recursively in a directory tree.\n"
         "usage: find <dir/file> ... \n"
         "     [[!] --atime|-A [+-]N] [[!] --mtime|-M [+-]N] [[!] --ctime|-C [+-]N]\n"
         "     [--maxdepth|-D N] [[!] --name|-n <pattern>] [--print0|-P]\n"
         "     [--print|-p] [--obd|-O <uuid>] [[!] --type|-t <filetype>]\n"
         "\t !: used before an option indicates 'NOT' the requested attribute\n"
         "\t -: used before an value indicates 'AT MOST' the requested value\n"
         "\t +: used before an option indicates 'AT LEAST' the requested value\n"},
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
         "usage: quota [ -o obd_uuid ] [{-u|-g  <name>}|-t] <filesystem>"},
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
        unsigned long long st_size;
        int  st_offset, st_count;
        char *end;
        int c;
        int delete = 0;
        char *stripe_size_arg = NULL;
        char *stripe_off_arg = NULL;
        char *stripe_count_arg = NULL;
        unsigned long long size_units;

        struct option long_opts[] = {
                {"size",        required_argument, 0, 's'},
                {"count",       required_argument, 0, 'c'},
                {"index",       required_argument, 0, 'i'},
                {"delete",      no_argument,       0, 'd'},
                {0, 0, 0, 0}
        };

        st_size = 0;
        st_offset = -1;
        st_count = 0;
        if (argc == 3 && strcmp(argv[1], "-d") == 0) {
                /* for compatibility with the existing positional parameter
                 * usage */
                fname = argv[2];
                optind = 2;
        } else if (argc == 5  && 
                   (argv[2][0] != '-' || isdigit(argv[2][1])) &&
                   (argv[3][0] != '-' || isdigit(argv[3][1])) &&
                   (argv[4][0] != '-' || isdigit(argv[4][1])) ) {
                /* for compatibility with the existing positional parameter
                 * usage */
                fname = argv[1];
                stripe_size_arg = argv[2];
                stripe_off_arg = argv[3];
                stripe_count_arg = argv[4];
                optind = 4;
        } else {
                while ((c = getopt_long(argc, argv, "c:di:s:",
                                                long_opts, NULL)) >= 0) 
                {
                        switch (c) {
                        case 0:
                                /* Long options. */
                                break;
                        case 'c':
                                stripe_count_arg = optarg;
                                break;
                        case 'd':
                                /* delete the default striping pattern */
                                delete = 1;
                                break;
                        case 'i':
                                stripe_off_arg = optarg;
                                break;
                        case 's':
                                stripe_size_arg = optarg;
                                break;
                        case '?':
                                return CMD_HELP;
                        default:
                                fprintf(stderr, "error: %s: option '%s' "
                                                "unrecognized\n",
                                                argv[0], argv[optind - 1]);
                                return CMD_HELP;
                        }
                }

                if (optind < argc)
                        fname = argv[optind];
                else
                        return CMD_HELP;

                if (delete && 
                    (stripe_size_arg != NULL || stripe_off_arg != NULL || 
                     stripe_count_arg != NULL)) {
                        fprintf(stderr, "error: %s: cannot specify -d with "
                                        "-s, -c or -i options\n",
                                        argv[0]);
                        return CMD_HELP;
                }
        }

        if (optind != argc - 1) {
                fprintf(stderr, "error: %s: only 1 filename|dirname can be "
                                "specified: '%s'\n",
                                argv[0], argv[argc-1]);
                return CMD_HELP;
        }

        /* get the stripe size */
        if (stripe_size_arg != NULL) {
                result = parse_size(stripe_size_arg, &st_size, &size_units);
                if (result) {
                        fprintf(stderr,"error: bad size '%s'\n",
                                stripe_size_arg);
                        return result;
                }
        }
        /* get the stripe offset */
        if (stripe_off_arg != NULL) {
                st_offset = strtoul(stripe_off_arg, &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe offset '%s'\n",
                                        argv[0], stripe_off_arg);
                        return CMD_HELP;
                }
        }
        /* get the stripe count */
        if (stripe_count_arg != NULL) {
                st_count = strtoul(stripe_count_arg, &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe count '%s'\n",
                                        argv[0], stripe_count_arg);
                        return CMD_HELP;
                }
        }

        result = llapi_file_create(fname, st_size, st_offset, st_count, 0);
        if (result)
                fprintf(stderr, "error: %s: create stripe file failed\n",
                                argv[0]);

        return result;
}

static int set_time(time_t *time, time_t *set, char *str)
{
        time_t t;
        int res = 0;
        
        if (str[0] == '+')
                res = 1;
        else if (str[0] == '-')
                res = -1;

        if (res)
                str++;

        t = strtol(str, NULL, 0);
        if (*time < t * 24 * 60 * 60) {
                if (res)
                        str--;
                fprintf(stderr, "Wrong time '%s' is specified.\n", str);
                return INT_MAX;
        }

        *set = *time - t * 24 * 60 * 60;
        return res;
}

static int lfs_find(int argc, char **argv)
{
        int new_fashion = 1;
        int c, ret;
        time_t t;
        struct find_param param = { .maxdepth = -1 };
        char str[1024];
        struct option long_opts[] = {
                /* New find options. */
                {"atime",     required_argument, 0, 'A'},
                {"ctime",     required_argument, 0, 'C'},
                {"mtime",     required_argument, 0, 'M'},
                {"maxdepth",  required_argument, 0, 'D'},
                {"name",      required_argument, 0, 'n'},
                /* --obd is considered as a new option. */
                {"obd",       required_argument, 0, 'O'},
                {"ost",       required_argument, 0, 'O'},
                {"print",     no_argument,       0, 'P'},
                {"print0",    no_argument,       0, 'p'},
                /* Old find options. */
                {"quiet",     no_argument,       0, 'q'},
                {"recursive", no_argument,       0, 'r'},
                {"size",      required_argument, 0, 's'},
                {"type",      required_argument, 0, 't'},
                {"verbose",   no_argument,       0, 'v'},
                {0, 0, 0, 0}
        };
        int pathstart = -1;
        int pathend = -1;
        int neg_opt = 0;
        time_t *xtime;
        int *xsign;
        int isoption;

        time(&t);

        while ((c = getopt_long_only(argc, argv, "-A:C:D:M:n:PpO:qrs:t:v",
                                     long_opts, NULL)) >= 0) {
                xtime = NULL;
                xsign = NULL;
                if (neg_opt)
                        --neg_opt;
                /* '!' is part of option */
                isoption = (c != 1) || (strcmp(optarg, "!") == 0);
                if (!isoption && pathend != -1) {
                        fprintf(stderr, "err: %s: filename|dirname must either "
                                        "precede options or follow options\n",
                                        argv[0]);
                        return CMD_HELP;
                }
                if (!isoption && pathstart == -1)
                        pathstart = optind - 1;
                if (isoption && pathstart != -1 && pathend == -1) {
                        pathend = optind - 2;
                        if ((c == 1 && strcmp(optarg, "!") == 0) ||
                            c == 'P' || c == 'p' || c == 'O' ||
                            c == 'q' || c == 'r' || c == 'v')
                                pathend = optind - 1;
                }
                switch (c) {
                case 0:
                        /* Long options. */
                        break;
                case 1:
                        if (strcmp(optarg, "!") == 0)
                                neg_opt = 2;
                      break;
                case 'A':
                        xtime = &param.atime;
                        xsign = &param.asign;
                case 'C':
                        if (c == 'C') {
                                xtime = &param.ctime;
                                xsign = &param.csign;
                        }
                case 'M':
                        if (c == 'M') {
                                xtime = &param.mtime;
                                xsign = &param.msign;
                        }
                        new_fashion = 1;
                        if (neg_opt) {
                                if (optarg[0] == '-')
                                        optarg[0] = '+';
                                else if (optarg[0] == '+')
                                        optarg[0] = '-';
                                else {
                                        str[0] = '-';
                                        str[1] = '\0';
                                        strcat(str, optarg);
                                        optarg = str;
                                }
                        }
                        ret = set_time(&t, xtime, optarg);
                        if (ret == INT_MAX)
                                return -1;
                        if (ret)
                                *xsign = ret;
                        break;
                case 'D':
                        new_fashion = 1;
                        param.maxdepth = strtol(optarg, 0, 0);
                        break;
                case 'n':
                        new_fashion = 1;
                        param.pattern = (char *)optarg;
                        param.exclude_pattern = !!neg_opt;
                        break;
                case 'O': {
                        char *buf, *token, *next, *p;
                        int len;

                        len = strlen((char *)optarg);
                        buf = malloc(len+1);
                        if (buf == NULL)
                                return -ENOMEM;
                        strcpy(buf, (char *)optarg);

                        if (param.num_alloc_obds == 0) {
                                param.obduuid = (struct obd_uuid *)malloc(FIND_MAX_OSTS *
                                                       sizeof(struct obd_uuid));
                                if (param.obduuid == NULL)
                                        return -ENOMEM;
                                param.num_alloc_obds = INIT_ALLOC_NUM_OSTS;
                        }

                        for (token = buf; token && *token; token = next) {
                                p = strchr(token, ',');
                                next = 0;
                                if (p) {
                                        *p = 0;
                                        next = p+1;
                                }
                                strcpy((char *)&param.obduuid[param.num_obds++].uuid,
                                       token);
                        }

                        if (buf)
                                free(buf);
                        break;
                }
                case 'p':
                        new_fashion = 1;
                        param.zeroend = 1;
                        break;
                case 'P':
                        break;
                case 'q':
                        new_fashion = 0;
                        param.quiet++;
                        param.verbose = 0;
                        break;
                case 'r':
                        new_fashion = 0;
                        param.recursive = 1;
                        break;
                case 't':
                        param.exclude_type = !!neg_opt;
                        switch(optarg[0]) {
                        case 'b': param.type = S_IFBLK; break;
                        case 'c': param.type = S_IFCHR; break;
                        case 'd': param.type = S_IFDIR; break;
                        case 'f': param.type = S_IFREG; break;
                        case 'l': param.type = S_IFLNK; break;
                        case 'p': param.type = S_IFIFO; break;
                        case 's': param.type = S_IFSOCK; break;
#ifdef S_IFDOOR /* Solaris only */
                        case 'D': param.type = S_IFDOOR; break;
#endif
                        default: fprintf(stderr, "error: %s: bad type '%s'\n",
                                         argv[0], optarg);
                                 return CMD_HELP;
                        };
                        break;
                case 's':
                        if (neg_opt) {
                                if (optarg[0] == '-')
                                        optarg[0] = '+';
                                else if (optarg[0] == '+')
                                        optarg[0] = '-';
                                else {
                                        str[0] = '-';
                                        str[1] = '\0';
                                        strcat(str, optarg);
                                        optarg = str;
                                }
                        }
                        if (optarg[0] == '+')
                                param.size_sign = -1;
                        else if (optarg[0] == '-')
                                param.size_sign = +1;

                        if (param.size_sign)
                                optarg++;
                        ret = parse_size(optarg, &param.size,&param.size_units);
                        if (ret) {
                                fprintf(stderr,"error: bad size '%s'\n",
                                        optarg);
                                return ret;
                        }
                        param.size_check = 1;
                        break;
                case 'v':
                        new_fashion = 0;
                        param.verbose++;
                        param.quiet = 0;
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                };
        }

        if (pathstart == -1) {
                fprintf(stderr, "error: %s: no filename|pathname\n",
                                argv[0]);
                return CMD_HELP;
        } else if (pathend == -1) {
                /* no options */
                pathend = argc;
        }

        if (new_fashion) {
                param.quiet = 1;
        } else {
                static int deprecated_warning;
                if (!deprecated_warning) {
                        fprintf(stderr, "lfs find: -q, -r, -v options "
                                "deprecated.  Use 'lfs getstripe' instead.\n");
                        deprecated_warning = 1;
                }
                if (!param.recursive && param.maxdepth == -1)
                        param.maxdepth = 1;
        }

        do {
                if (new_fashion)
                        ret = llapi_find(argv[pathstart], &param);
                else
                        ret = llapi_getstripe(argv[pathstart], &param);
        } while (++pathstart < pathend && !ret);

        if (ret)
                fprintf(stderr, "error: %s failed for %s.\n",
                        argv[0], argv[optind - 1]);

        if (param.obduuid && param.num_alloc_obds)
                free(param.obduuid);

        return ret;
}

static int lfs_getstripe(int argc, char **argv)
{
        struct option long_opts[] = {
                {"obd", 1, 0, 'O'},
                {"quiet", 0, 0, 'q'},
                {"recursive", 0, 0, 'r'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };
        char short_opts[] = "hO:qrv";
        int c, rc;
        struct find_param param = { 0 };

        optind = 0;
        while ((c = getopt_long(argc, argv, short_opts,
                                long_opts, NULL)) != -1) {
                switch (c) {
                case 'O':
                        if (param.obduuid) {
                                fprintf(stderr,
                                        "error: %s: only one obduuid allowed",
                                        argv[0]);
                                return CMD_HELP;
                        }
                        param.obduuid = (struct obd_uuid *)optarg;
                        break;
                case 'q':
                        param.quiet++;
                        param.verbose = 0;
                        break;
                case 'r':
                        param.recursive = 1;
                        break;
                case 'v':
                        param.verbose++;
                        param.quiet = 0;
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                }
        }

        if (optind >= argc)
                return CMD_HELP;

        param.maxdepth = param.recursive ? -1 : 1;

        do {
                rc = llapi_getstripe(argv[optind], &param);
        } while (++optind < argc && !rc);

        if (rc)
                fprintf(stderr, "error: %s failed for %s.\n", 
                        argv[0], argv[optind - 1]);
        return rc;
}

static int lfs_osts(int argc, char **argv)
{
        FILE *fp;
        struct mntent *mnt = NULL;
        struct obd_uuid *obduuid = NULL;
        struct find_param param;
        int rc=0;

        if (argc != 1)
                return CMD_HELP;

        fp = setmntent(MOUNTED, "r");

        if (fp == NULL) {
                 fprintf(stderr, "%s: setmntent(%s): %s:", argv[0], MOUNTED,
                        strerror (errno));
        } else {
                mnt = getmntent(fp);
                memset(&param, 0, sizeof(param));
                param.obduuid = obduuid;
                while (feof(fp) == 0 && ferror(fp) ==0) {
                        if (llapi_is_lustre_mnt(mnt)) {
                                rc = llapi_getstripe(mnt->mnt_dir, &param);
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
#define RDF     "%4d%%"

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
                if (llapi_is_lustre_mnt(mnt)) {
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
                  char *uuid, int ishow, int cooked,
                  char *type, int index, int rc)
{
        long long avail, used, total;
        double ratio = 0;
        char *suffix = "KMGTPEZY";
        char tbuf[10], ubuf[10], abuf[10], rbuf[10];

        if (!uuid || !stat)
                return -EINVAL;

        switch (rc) {
        case 0:
                if (ishow) {
                        avail = stat->os_ffree;
                        used = stat->os_files - stat->os_ffree;
                        total = stat->os_files;
                } else {
                        int shift = cooked ? 0 : 10;

                        avail = (stat->os_bavail * stat->os_bsize) >> shift;
                        used  = ((stat->os_blocks - stat->os_bfree) *
                                 stat->os_bsize) >> shift;
                        total = (stat->os_blocks * stat->os_bsize) >> shift;
                }

                if (total > 0)
                        ratio = (double)used / (double)total;

                if (cooked) {
                        int i;
                        double cook_val;

                        cook_val = (double)total;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(tbuf, HDF"%c", cook_val, suffix[i - 1]);
                        else
                                sprintf(tbuf, CDF, total);

                        cook_val = (double)used;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(ubuf, HDF"%c", cook_val, suffix[i - 1]);
                        else
                                sprintf(ubuf, CDF, used);

                        cook_val = (double)avail;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(abuf, HDF"%c", cook_val, suffix[i - 1]);
                        else
                                sprintf(abuf, CDF, avail);
                } else {
                        sprintf(tbuf, CDF, total);
                        sprintf(ubuf, CDF, used);
                        sprintf(abuf, CDF, avail);
                }

                sprintf(rbuf, RDF, (int)(ratio * 100));
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s",
                       uuid, tbuf, ubuf, abuf, rbuf, mntdir);
                if (type)
                        printf("[%s:%d]\n", type, index);
                else
                        printf("\n");

                break;
        case -ENODATA:
                printf(UUF": inactive device\n", uuid);
                break;
        default:
                printf(UUF": %s\n", uuid, strerror(-rc));
                break;
        }

        return 0;
}

static int mntdf(char *mntdir, int ishow, int cooked)
{
        struct obd_statfs stat_buf, sum = { .os_bsize = 1 };
        struct obd_uuid uuid_buf;
        __u32 index;
        int rc;

        if (ishow)
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", "Inodes", "IUsed", "IFree",
                       "IUse%", "Mounted on");
        else
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", cooked ? "bytes" : "1K-blocks",
                       "Used", "Available", "Use%", "Mounted on");

        for (index = 0; ; index++) {
                memset(&stat_buf, 0, sizeof(struct obd_statfs));
                memset(&uuid_buf, 0, sizeof(struct obd_uuid));
                rc = llapi_obd_statfs(mntdir, LL_STATFS_MDC, index,
                                      &stat_buf, &uuid_buf);
                if (rc == -ENODEV)
                        break;

                if (rc == -EAGAIN)
                        continue;

                if (rc == -ENOTCONN || rc == -ETIMEDOUT || rc == -EIO ||
                    rc == -ENODATA || rc == 0) {
                        showdf(mntdir, &stat_buf, obd_uuid2str(&uuid_buf),
                               ishow, cooked, "MDT", index, rc);
                } else {
                        fprintf(stderr,
                                "error: llapi_obd_statfs(%s): %s (%d)\n",
                                obd_uuid2str(&uuid_buf), strerror(-rc), rc);
                        return rc;
                }
                if (rc == 0) {
                        sum.os_ffree += stat_buf.os_ffree;
                        sum.os_files += stat_buf.os_files;
                }
        }

        for (index = 0; ; index++) {
                memset(&stat_buf, 0, sizeof(struct obd_statfs));
                memset(&uuid_buf, 0, sizeof(struct obd_uuid));
                rc = llapi_obd_statfs(mntdir, LL_STATFS_LOV, index,
                                      &stat_buf, &uuid_buf);
                if (rc == -ENODEV)
                        break;

                if (rc == -EAGAIN)
                        continue;

                if (rc == -ENOTCONN || rc == -ETIMEDOUT || rc == -EIO ||
                    rc == -ENODATA || rc == 0) {
                        showdf(mntdir, &stat_buf, obd_uuid2str(&uuid_buf),
                               ishow, cooked, "OST", index, rc);
                } else {
                        fprintf(stderr,
                                "error: llapi_obd_statfs failed: %s (%d)\n",
                                strerror(-rc), rc);
                        return rc;
                }
                if (rc == 0) {
                        sum.os_blocks += stat_buf.os_blocks * stat_buf.os_bsize;
                        sum.os_bfree  += stat_buf.os_bfree * stat_buf.os_bsize;
                        sum.os_bavail += stat_buf.os_bavail * stat_buf.os_bsize;
                }
        }

        printf("\n");
        showdf(mntdir, &sum, "filesystem summary:", ishow, cooked, NULL, 0,0);

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
                        if (llapi_is_lustre_mnt(mnt)) {
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
                        if (llapi_is_lustre_mnt(mnt))
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
                        if (llapi_is_lustre_mnt(mnt))
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
                        name_head, (long long)size, JOIN_FILE_ALIGN);
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
        char *obd_type = (char *)qchk.obd_type;
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
        else    /* do quotacheck for both user & group quota by default */
                check_type = 0x02;

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
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qchk.obd_uuid));
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
                        fprintf(stderr, "%s %s ", (char *)qctl.obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
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
        char *obd_type = (char *)qctl.obd_type;
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
        else /* by default, enable quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
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
        char *obd_type = (char *)qctl.obd_type;
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
        else /* by default, disable quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
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

#define ADD_OVERFLOW(a,b) ((a + b) < a) ? (a = ULONG_MAX) : (a = a + b)

/* Convert format time string "XXwXXdXXhXXmXXs" into seconds value
 * returns the value or ULONG_MAX on integer overflow or incorrect format
 * Notes:
 *        1. the order of specifiers is arbitrary (may be: 5w3s or 3s5w)
 *        2. specifiers may be encountered multiple times (2s3s is 5 seconds)
 *        3. empty integer value is interpreted as 0
 */

static unsigned long str2sec(const char* timestr) {
        const char spec[] = "smhdw";
        const unsigned long mult[] = {1, 60, 60*60, 24*60*60, 7*24*60*60};
        unsigned long val = 0;
        char *tail;

        if (strpbrk(timestr, spec) == NULL) {
                /* no specifiers inside the time string,
                   should treat it as an integer value */
                val = strtoul(timestr, &tail, 10);
                return *tail ? ULONG_MAX : val;
        }

        /* format string is XXwXXdXXhXXmXXs */
        while (*timestr) {
                unsigned long v;
                int ind;
                char* ptr;

                v = strtoul(timestr, &tail, 10);
                if (v == ULONG_MAX || *tail == '\0')
                        /* value too large (ULONG_MAX or more) 
                           or missing specifier */
                        goto error;

                ptr = strchr(spec, *tail);
                if (ptr == NULL)
                        /* unknown specifier */
                        goto error;

                ind = ptr - spec;

                /* check if product will overflow the type */
                if (!(v < ULONG_MAX / mult[ind]))
                        goto error;

                ADD_OVERFLOW(val, mult[ind] * v);
                if (val == ULONG_MAX)
                        goto error;

                timestr = tail + 1;
        }

        return val;

error:
        return ULONG_MAX;
}

int lfs_setquota(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = (char *)qctl.obd_type;
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

                if ((dqi->dqi_bgrace = str2sec(argv[optind++])) == ULONG_MAX) {
                        fprintf(stderr, "error: bad %s: %s\n", "block-grace", argv[optind - 1]);
                        return CMD_HELP;
                }
                if ((dqi->dqi_igrace = str2sec(argv[optind++])) == ULONG_MAX) {
                        fprintf(stderr, "error: bad %s: %s\n", "inode-grace", argv[optind - 1]);
                        return CMD_HELP;
		}
        }

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
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


/* Converts seconds value into format string
 * result is returned in buf
 * Notes:
 *        1. result is in descenting order: 1w2d3h4m5s
 *        2. zero fields are not filled (except for p. 3): 5d1s
 *        3. zero seconds value is presented as "0s"
 */
static void sec2str(time_t seconds, char *buf)
{
        const char spec[] = "smhdw";
        const unsigned long mult[] = {1, 60, 60*60, 24*60*60, 7*24*60*60};
        unsigned long c;
        char* tail = buf;
        int i;

        for (i = sizeof(mult) / sizeof(mult[0]) - 1 ; i >= 0; i--) {
                c = seconds / mult[i];

                if (c > 0 || (i == 0 && buf == tail))
                        tail += snprintf(tail, 40-(tail-buf), "%lu%c", c, spec[i]);

                seconds %= mult[i];
        }
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
        sec2str(seconds - now, buf);
}

static void print_quota_title(char *name, struct if_quotactl *qctl)
{
        printf("Disk quotas for %s %s (%cid %u):\n",
               type2name(qctl->qc_type), name,
               *type2name(qctl->qc_type), qctl->qc_id);
        printf("%15s%8s %7s%8s%8s%8s %7s%8s%8s\n",
               "Filesystem",
               "kbytes", "quota", "limit", "grace",
               "files", "quota", "limit", "grace");
}

#define GENERAL_QUOTA_INFO 1
#define MDS_QUOTA_INFO     2
#define OST_QUOTA_INFO     3

static void print_quota(char *mnt, struct if_quotactl *qctl, int type)
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
                        sprintf(numbuf[0], (dqb->dqb_valid & QIF_SPACE) ?
                                LPU64 : "["LPU64"]", toqb(dqb->dqb_curspace));
                        if (type == GENERAL_QUOTA_INFO)
                                sprintf(numbuf[1], (dqb->dqb_valid & QIF_BLIMITS)
                                        ? LPU64 : "["LPU64"]",
                                        dqb->dqb_bsoftlimit);
                        else
                                sprintf(numbuf[1], "%s", "");
                        sprintf(numbuf[2], (dqb->dqb_valid & QIF_BLIMITS)
                                ? LPU64 : "["LPU64"]", dqb->dqb_bhardlimit);
                        printf(" %7s%c %6s %7s %7s",
                               numbuf[0], bover ? '*' : ' ', numbuf[1],
                               numbuf[2], bover > 1 ? timebuf : "");

                        if (iover)
                                diff2str(dqb->dqb_itime, timebuf, now);

                        sprintf(numbuf[0], (dqb->dqb_valid & QIF_INODES) ?
                                LPU64 : "["LPU64"]", dqb->dqb_curinodes);
                       if (type == GENERAL_QUOTA_INFO)
                                sprintf(numbuf[1], (dqb->dqb_valid & QIF_ILIMITS)
                                        ? LPU64 : "["LPU64"]",
                                        dqb->dqb_isoftlimit);
                        else
                                sprintf(numbuf[1], "%s", "");
                        sprintf(numbuf[2], (dqb->dqb_valid & QIF_ILIMITS) ?
                                LPU64 : "["LPU64"]", dqb->dqb_ihardlimit);
                        if (type != OST_QUOTA_INFO)
                                printf(" %7s%c %6s %7s %7s",
                                       numbuf[0], iover ? '*' : ' ', numbuf[1],
                                       numbuf[2], iover > 1 ? timebuf : "");
                        printf("\n");
                }
        } else if (qctl->qc_cmd == LUSTRE_Q_GETINFO ||
                   qctl->qc_cmd == Q_GETOINFO) {
                char bgtimebuf[40];
                char igtimebuf[40];

                sec2str(qctl->qc_dqinfo.dqi_bgrace, bgtimebuf);
                sec2str(qctl->qc_dqinfo.dqi_igrace, igtimebuf);
                printf("Block grace time: %s; Inode grace time: %s\n",
                       bgtimebuf, igtimebuf);
        }
}

static int print_mds_quota(char *mnt, struct if_quotactl *qctl)
{
        int rc;

        /* XXX: this is a flag to mark that only mds quota is wanted */
        qctl->qc_dqblk.dqb_valid = 1;
        rc = llapi_quotactl(mnt, qctl);
        if (rc) {
                fprintf(stderr, "quotactl failed: %s\n", strerror(errno));
                return rc;
        }

        print_quota(obd_uuid2str(&qctl->obd_uuid), qctl, MDS_QUOTA_INFO);
        return 0;
}

static int print_lov_quota(char *mnt, struct if_quotactl *qctl)
{
        DIR *dir;
        struct obd_uuid *uuids = NULL, *uuidp;
        int obdcount = 1024;
        int i, rc = 0, rc1 = 0;

        dir = opendir(mnt);
        if (!dir) {
                fprintf(stderr, "open %s failed: %s\n", mnt, strerror(errno));
                return -ENOENT;
        }

        uuids = (struct obd_uuid *)malloc(INIT_ALLOC_NUM_OSTS *
                                          sizeof(struct obd_uuid));
        if (uuids == NULL)
                goto out;

retry_get_uuids:
        rc = llapi_lov_get_uuids(dirfd(dir), uuids, &obdcount);
        if (rc != 0) {
                struct obd_uuid *uuids_temp;

                if (rc == -EOVERFLOW) {
                        uuids_temp = realloc(uuids, obdcount *
                                             sizeof(struct obd_uuid));
                        if (uuids_temp != NULL)
                                goto retry_get_uuids;
                        else
                                rc = -ENOMEM;
                }

                fprintf(stderr, "get ost uuid failed: %s\n", strerror(rc));
                goto out;
        }

        for (i = 0, uuidp = uuids; i < obdcount; i++, uuidp++) {
                memcpy(&qctl->obd_uuid, uuidp, sizeof(*uuidp));

                /* XXX clear this flag to get quota from osts */
                qctl->qc_dqblk.dqb_valid = 0;
                rc = llapi_quotactl(mnt, qctl);
                if (rc) {
                        if (!rc1)
                                rc1 = rc;
                        fprintf(stderr, "%s quotactl failed: %s\n",
                                uuidp->uuid, strerror(errno));
                        continue;
                }

                print_quota((char *)uuidp->uuid, qctl, OST_QUOTA_INFO);
        }

out:
        closedir(dir);
        if (!rc)
                rc = rc1;
        return rc;
}

static int lfs_quota(int argc, char **argv)
{
        int c;
        char *name = NULL, *mnt;
        struct if_quotactl qctl = { .qc_cmd = LUSTRE_Q_GETQUOTA,
                                    .qc_type = 0x01 };
        char *obd_type = (char *)qctl.obd_type;
        char *obd_uuid = (char *)qctl.obd_uuid.uuid;
        int rc, rc1 = 0, rc2 = 0, rc3 = 0;

        optind = 0;
        while ((c = getopt(argc, argv, "ugto:")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type = 0x01;
                        break;
                case 'g':
                        qctl.qc_type = 0x02;
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


        if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA) {
                if (optind + 2 != argc) {
                        fprintf(stderr, "error: missing quota argument(s)\n");
                        return CMD_HELP;
                }

                name = argv[optind++];
                rc = name2id(&qctl.qc_id, name, qctl.qc_type);
                if (rc) {
                        fprintf(stderr,"error: can't find id for name %s: %s\n",
                                name, strerror(errno));
                        return CMD_HELP;
                }
                print_quota_title(name, &qctl);
        } else if (optind + 1 != argc) {
                fprintf(stderr, "error: missing quota info argument(s)\n");
                return CMD_HELP;
        }

        mnt = argv[optind];

        rc1 = llapi_quotactl(mnt, &qctl);
        if (rc1 && *obd_type)
                fprintf(stderr, "%s %s ", obd_type, obd_uuid);

        if (!name)
                rc = id2name(&name, getuid(), qctl.qc_type);

        if (*obd_uuid) {
                mnt = "";
                name = obd_uuid;
        }

        print_quota(mnt, &qctl, GENERAL_QUOTA_INFO);

        if (!*obd_uuid && qctl.qc_cmd != LUSTRE_Q_GETINFO) {
                rc2 = print_mds_quota(mnt, &qctl);
                rc3 = print_lov_quota(mnt, &qctl);
        }

        if (rc1 || rc2 || rc3)
                printf("Some errors happened when getting quota info. "
                       "Some devices may be not working or deactivated. "
                       "The data in \"[]\" is inaccurate.\n");
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
