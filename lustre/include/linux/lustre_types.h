#ifndef _LUSTRE_LINUX_TYPES_H
#define _LUSTRE_LINUX_TYPES_H

#if (!defined(_LINUX_TYPES_H) && !defined(_BLKID_TYPES_H) && \
     !defined(_EXT2_TYPES_H) && !defined(_I386_TYPES_H) && \
     !defined(_X86_64_TYPES_H))

typedef unsigned short umode_t;
/*
 * __xx is ok: it doesn't pollute the POSIX namespace. Use these in the
 * header files exported to user space
 */

typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;

typedef __signed__ long long __s64;
typedef unsigned long long __u64;
#endif

#endif
