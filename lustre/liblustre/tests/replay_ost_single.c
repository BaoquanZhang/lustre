/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Light user test program
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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

#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/queue.h>
#include <signal.h>

#include <sysio.h>
#include <mount.h>

#include "test_common.h"



static char mds_server[1024] = {0,};
static char barrier_script[1024] = {0,};
static char failover_script[1024] = {0,};
static char barrier_cmd[1024] = {0,};
static char failover_cmd[1024] = {0,};

static void replay_barrier()
{
        int rc;

        if ((rc = system(barrier_cmd))) {
                printf("excute barrier error: %d\n", rc);
                exit(rc);
        }
}

static void mds_failover()
{
        int rc;

        if ((rc = system(failover_cmd))) {
                printf("excute failover error: %d\n", rc);
                exit(rc);
        }
}


#define ENTRY(str)                                                      \
        do {                                                            \
                char buf[100];                                          \
                int len;                                                \
                sprintf(buf, "===== START: %s ", (str));                \
                len = strlen(buf);                                      \
                if (len < 79) {                                         \
                        memset(buf+len, '=', 100-len);                  \
                        buf[79] = '\n';                                 \
                        buf[80] = 0;                                    \
                }                                                       \
                printf("%s", buf);                                      \
        } while (0)

#define LEAVE()                                                         \
        do {                                                            \
                printf("----- END TEST successfully ---");              \
                printf("-----------------------------");                \
                printf("-------------------\n");                        \
        } while (0)

void t0()
{
        const int bufsize = 4096;
        char *path = "/mnt/lustre/rp_ost_t0_file";
        char buf[bufsize];
        int fd, i, j, rc;
        ENTRY("open-failover-write-verification (no ping involved)");

        printf("create/open file...\n");
        t_touch(path);
        fd = t_open(path);
        printf("OST failover...\n");
        replay_barrier();
        mds_failover();

        printf("write file...\n");
        for (i = 0; i < 20; i++) {
                memset(buf, i, bufsize);
                if ((rc = write(fd, buf, bufsize)) != bufsize) {
                        perror("write error after failover");
                        printf("i = %d, rc = %d\n", i, rc);
                        exit(-1);
                }
        }

        /* verify */
        printf("read & verify...\n");
        lseek(fd, 0, SEEK_SET);
        for (i = 0; i < 20; i++) {
                memset(buf, -1, bufsize);
                if ((rc = read(fd, buf, bufsize)) != bufsize) {
                        perror("read error rc");
                        printf("i = %d, rc = %d\n", i, rc);
                        exit(-1);
                }
                for (j = 0; j < bufsize; j++) {
                        if (buf[j] != i) {
                                printf("verify error!\n");
                                exit(-1);
                        }
                }
        }
        t_close(fd);
        t_unlink(path);
        LEAVE();
}

void t1()
{
        const int bufsize = 4096;
        char *path = "/mnt/lustre/rp_ost_t1_file";
        char buf[bufsize];
        int fd, i, j;
        ENTRY("open-write-close-open-failover-read (no ping involved)");

        printf("create/open file...\n");
        t_touch(path);
        fd = t_open(path);
        printf("write file...\n");
        for (i = 0; i < 20; i++) {
                memset(buf, i, bufsize);
                if (write(fd, buf, bufsize) != bufsize) {
                        perror("write error");
                        exit(-1);
                }
        }
        printf("close/reopen...\n");
        t_close(fd);
        fd = t_open(path);
        lseek(fd, 0, SEEK_SET);

        printf("OST failover...\n");
        replay_barrier();
        mds_failover();

        printf("read & verify...\n");
        for (i = 0; i < 20; i++) {
                memset(buf, -1, bufsize);
                if (read(fd, buf, bufsize) != bufsize) {
                        perror("read error after failover");
                        exit(-1);
                }
                for (j = 0; j < bufsize; j++) {
                        if (buf[j] != i) {
                                printf("verify error after failover\n");
                                exit(-1);
                        }
                }
        }

        t_close(fd);
        t_unlink(path);
        LEAVE();
}

void t2()
{
        char *path = "/mnt/lustre/rp_ost_t2_file";
        char *str = "xxxxjoiwlsdf98lsjdfsjfoajflsjfajfoaidfojaj08eorje;";
        ENTRY("empty replay");

        replay_barrier();
        mds_failover();

        t_echo_create(path, str);
        t_grep(path, str);
        t_unlink(path);
}

void t3()
{
        char *path = "/mnt/lustre/rp_ost_t3_file";
        char *str = "xxxxjoiwlsdf98lsjdfsjfoajflsjfajfoaidfojaj08eorje;";
        ENTRY("touch");

        replay_barrier();
        t_echo_create(path, str);
        mds_failover();

        t_grep(path, str);
        t_unlink(path);
}

void t4()
{
        char *path = "/mnt/lustre/rp_ost_t4_file";
        char namebuf[1024];
        char str[1024];
        int count = 10, i;
        ENTRY("|X| 10 open(CREAT)s (ping involved)");

        replay_barrier();
        for (i = 0; i < count; i++) {
                sprintf(namebuf, "%s%02d", path, i);
                sprintf(str, "%s-%08d-%08x-AAAAA", "content", i, i);
                t_echo_create(namebuf, str);
        }
        mds_failover();

        for (i = 0; i < count; i++) {
                sprintf(namebuf, "%s%02d", path, i);
                sprintf(str, "%s-%08d-%08x-AAAAA", "content", i, i);
                t_grep(namebuf, str);
                t_unlink(namebuf);
        }
}

extern int portal_debug;
extern int portal_subsystem_debug;

extern void __liblustre_setup_(void);
extern void __liblustre_cleanup_(void);

void usage(const char *cmd)
{
        printf("Usage: \t%s --target mdsnid:/mdsname/profile -s ost_hostname "
                "-b \"barrier cmd\" -f \"failover cmd\"\n", cmd);
        printf("       \t%s --dumpfile dumpfile -s ost_hostname -b \"barrier cmd\" "
                "-f \"failover cmd\"\n", cmd);
        exit(-1);
}

void test_ssh()
{
        char cmd[1024];

        sprintf(cmd, "ssh %s cat /dev/null", mds_server);
        if (system(cmd)) {
                printf("ssh can't access server node: %s\n", mds_server);
                exit(-1);
        }
}

int main(int argc, char * const argv[])
{
        int opt_index, c;
        static struct option long_opts[] = {
                {"target", 1, 0, 0},
                {"dumpfile", 1, 0, 0},
                {0, 0, 0, 0}
        };

        if (argc < 4)
                usage(argv[0]);

        while ((c = getopt_long(argc, argv, "s:b:f:", long_opts, &opt_index)) != -1) {
                switch (c) {
                case 0: {
                        if (!optarg[0])
                                usage(argv[0]);

                        if (!strcmp(long_opts[opt_index].name, "target")) {
                                setenv(ENV_LUSTRE_MNTTGT, optarg, 1);
                        } else if (!strcmp(long_opts[opt_index].name, "dumpfile")) {
                                setenv(ENV_LUSTRE_DUMPFILE, optarg, 1);
                        } else
                                usage(argv[0]);
                        break;
                }
                case 's':
                        strcpy(mds_server, optarg);
                        break;
                case 'b':
                        strcpy(barrier_script, optarg);
                        break;
                case 'f':
                        strcpy(failover_script, optarg);
                        break;
                default:
                        usage(argv[0]);
                }
        }

        if (optind != argc)
                usage(argv[0]);
        if (!strlen(mds_server) || !strlen(barrier_script) ||
            !strlen(failover_script))
                usage(argv[0]);

        test_ssh();

        /* prepare remote command */
        sprintf(barrier_cmd, "ssh %s \"%s\"", mds_server, barrier_script);
        sprintf(failover_cmd, "ssh %s \"%s\"", mds_server, failover_script);

        setenv(ENV_LUSTRE_TIMEOUT, "5", 1);

        __liblustre_setup_();

        t0();
        t1();
        t2();
/* XXX still have problems
        t3();
        t4();
 */

	printf("liblustre is about shutdown\n");
        __liblustre_cleanup_();

	printf("complete successfully\n");
	return 0;
}
