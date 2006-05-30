/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  create_iam.c
 *  User-level tool for creation of iam files.
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Wang Di <wangdi@clusterfs.com>
 *   Author: Nikita Danilov <nikita@clusterfs.com>
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

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>

#ifdef HAVE_ENDIAN_H
#include <endian.h>
#endif

#include <libcfs/libcfs.h>

void usage(void)
{
        printf("usage: create_iam "
               "[-h] [-k <keysize>] [-r recsize] [-b <blocksize] [-p <ptrsize>] [-v]\n");
}

enum {
        IAM_LFIX_ROOT_MAGIC = 0xbedabb1edULL
};

struct iam_lfix_root {
        u_int64_t  ilr_magic;
        u_int16_t  ilr_keysize;
        u_int16_t  ilr_recsize;
        u_int16_t  ilr_ptrsize;
        u_int16_t  ilr_indirect_levels;
        u_int16_t  ilr_padding;
};

enum {
        IAM_LEAF_HEADER_MAGIC = 0x1976
};

struct iam_leaf_head {
        u_int16_t ill_magic;
        u_int16_t ill_count;
};

struct dx_countlimit {
        u_int16_t limit;
        u_int16_t count;
};

#define LEAF_HEAD_MAGIC 0x1976
int main(int argc, char **argv)
{
        int rc;
        int opt;
        int blocksize = 4096;
        int keysize   = 8;
        int recsize   = 8;
        int ptrsize   = 4;
        int verbose   = 0;
        void *buf;

        struct iam_lfix_root *root;
        struct iam_leaf_head *head;
        struct dx_countlimit *limit;
        void *entry;

        do {
                opt = getopt(argc, argv, "hb:k:r:p:v");
                switch (opt) {
                case 'v':
                        verbose++;
                case -1:
                        break;
                case 'b':
                        blocksize = atoi(optarg);
                        break;
                case 'k':
                        keysize = atoi(optarg);
                        break;
                case 'r':
                        recsize = atoi(optarg);
                        break;
                case 'p':
                        ptrsize = atoi(optarg);
                        break;
                case '?':
                default:
                        fprintf(stderr, "Unable to parse options.");
                case 'h':
                        usage();
                        return 0;
                }
        } while (opt != -1);

        if (ptrsize != 4 && ptrsize != 8) {
                fprintf(stderr, "Invalid ptrsize (%i). "
                        "Only 4 and 8 are supported\n", ptrsize);
                return 1;
        }

        if (blocksize <= 100 || keysize < 1 || recsize < 0) {
                fprintf(stderr, "Too small record, key or block block\n");
                return 1;
        }

        if (keysize + recsize + sizeof(struct iam_leaf_head) > blocksize / 3) {
                fprintf(stderr, "Too large (record, key) or too small block\n");
                return 1;
        }
        if (verbose > 0) {
                fprintf(stderr,
                        "key: %i, rec: %i, ptr: %i, block: %i\n",
                        keysize, recsize, ptrsize, blocksize);
        }
        buf = malloc(blocksize);
        if (buf == NULL) {
                fprintf(stderr, "Unable to allocate %i bytes\n", blocksize);
                return 1;
        }

        root = memset(buf, 0, blocksize);

        *root = (typeof(*root)) {
                .ilr_magic           = cpu_to_le64(IAM_LFIX_ROOT_MAGIC),
                .ilr_keysize         = cpu_to_le16(keysize),
                .ilr_recsize         = cpu_to_le16(recsize),
                .ilr_ptrsize         = cpu_to_le16(ptrsize),
                .ilr_indirect_levels = cpu_to_le16(1)
        };

        limit = (void *)(root + 1);
        *limit = (typeof(*limit)){
                /*
                 * limit itself + one pointer to the leaf.
                 */
                .count = cpu_to_le16(2),
                .limit = (blocksize -
                          sizeof(struct iam_lfix_root)) / (keysize + ptrsize)
        };

        entry = root + 1;
        /*
         * Skip over @limit.
         */
        entry += keysize + ptrsize;

        /*
         * Entry format is <key> followed by <ptr>. In the minimal tree
         * consisting of a root and single node, <key> is a minimal possible
         * key.
         *
         * XXX: this key is hard-coded to be a sequence of 0's.
         */
        entry += keysize;
        /* now @entry points to <ptr> */
        if (ptrsize == 4)
                *(u_int32_t *)entry = cpu_to_le32(1);
        else
                *(u_int64_t *)entry = cpu_to_le64(1);
        rc = write(1, buf, blocksize);
        if (rc != blocksize) {
                fprintf(stderr, "Unable to write root node: %m (%i)\n", rc);
                return 1;
        }

        /* form leaf */
        head = memset(buf, 0, blocksize);
        *head = (struct iam_leaf_head) {
                .ill_magic = cpu_to_le16(LEAF_HEAD_MAGIC),
                /*
                 * Leaf contains an entry with the smallest possible key
                 * (created by zeroing).
                 */
                .ill_count = cpu_to_le16(1),
        };

        rc = write(1, buf, blocksize);
        if (rc != blocksize) {
                fprintf(stderr, "Unable to write leaf node: %m (%i)\n", rc);
                return 1;
        }
        return 0;
}
