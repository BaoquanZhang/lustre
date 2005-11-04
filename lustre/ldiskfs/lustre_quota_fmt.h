/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre administrative quota format
 *
 * from
 * include/linux/quotaio_v2.h
 */
#ifndef _LUSTRE_QUOTA_FMT_H
#define _LUSTRE_QUOTA_FMT_H

#include <linux/types.h>
#include <linux/quota.h>

/*
 * Definitions of magics and versions of current quota files
 * Same with quota v2's magic
 */
#define LUSTRE_INITQMAGICS {\
	0xd9c01f11,	/* USRQUOTA */\
	0xd9c01927	/* GRPQUOTA */\
}

#define LUSTRE_INITQVERSIONS {\
	0,		/* USRQUOTA */\
	0		/* GRPQUOTA */\
}

/*
 * The following structure defines the format of the disk quota file
 * (as it appears on disk) - the file is a radix tree whose leaves point
 * to blocks of these structures.
 */
struct lustre_disk_dqblk {
        __u32 dqb_id;           /* id this quota applies to */
        __u32 dqb_ihardlimit;   /* absolute limit on allocated inodes */
        __u32 dqb_isoftlimit;   /* preferred inode limit */
        __u32 dqb_curinodes;    /* current # allocated inodes */
        __u32 dqb_bhardlimit;   /* absolute limit on disk space (in QUOTABLOCK_SIZE) */
        __u32 dqb_bsoftlimit;   /* preferred limit on disk space (in QUOTABLOCK_SIZE) */
        __u64 dqb_curspace;     /* current space occupied (in bytes) */
        __u64 dqb_btime;        /* time limit for excessive disk use */
        __u64 dqb_itime;        /* time limit for excessive inode use */
};

/*
 * Here are header structures as written on disk and their in-memory copies
 */
/* First generic header */
struct lustre_disk_dqheader {
        __u32 dqh_magic;        /* Magic number identifying file */
        __u32 dqh_version;      /* File version */
};

/* Header with type and version specific information */
struct lustre_disk_dqinfo {
        __u32 dqi_bgrace;       /* Time before block soft limit becomes hard limit */
        __u32 dqi_igrace;       /* Time before inode soft limit becomes hard limit */
        __u32 dqi_flags;        /* Flags for quotafile (DQF_*) */
        __u32 dqi_blocks;       /* Number of blocks in file */
        __u32 dqi_free_blk;     /* Number of first free block in the list */
        __u32 dqi_free_entry;   /* Number of block with at least one free entry */
};

/*
 *  Structure of header of block with quota structures. It is padded to 16 bytes so
 *  there will be space for exactly 21 quota-entries in a block
 */
struct lustre_disk_dqdbheader {
        __u32 dqdh_next_free;   /* Number of next block with free entry */
        __u32 dqdh_prev_free;   /* Number of previous block with free entry */
        __u16 dqdh_entries;     /* Number of valid entries in block */
        __u16 dqdh_pad1;
        __u32 dqdh_pad2;
};

#define LUSTRE_DQINFOOFF	sizeof(struct lustre_disk_dqheader)     /* Offset of info header in file */
#define LUSTRE_DQBLKSIZE_BITS	10
#define LUSTRE_DQBLKSIZE	(1 << LUSTRE_DQBLKSIZE_BITS)    /* Size of block with quota structures */
#define LUSTRE_DQTREEOFF	1       /* Offset of tree in file in blocks */
#define LUSTRE_DQTREEDEPTH	4       /* Depth of quota tree */
#define LUSTRE_DQSTRINBLK	((LUSTRE_DQBLKSIZE - sizeof(struct lustre_disk_dqdbheader)) / sizeof(struct lustre_disk_dqblk)) /* Number of entries in one blocks */

#endif                          /* lustre_quota_fmt.h */
