/*
 *    This Cplant(TM) source code is the property of Sandia National
 *    Laboratories.
 *
 *    This Cplant(TM) source code is copyrighted by Sandia National
 *    Laboratories.
 *
 *    The redistribution of this Cplant(TM) source code is subject to the
 *    terms of the GNU Lesser General Public License
 *    (see cit/LGPL or http://www.gnu.org/licenses/lgpl.html)
 *
 *    Cplant(TM) Copyright 1998-2004 Sandia Corporation. 
 *    Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 *    license for use of this work by or on behalf of the US Government.
 *    Export of this program may require a license from the United States
 *    Government.
 */

/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Questions or comments about this library should be sent to:
 *
 * Lee Ward
 * Sandia National Laboratories, New Mexico
 * P.O. Box 5800
 * Albuquerque, NM 87185-1110
 *
 * lee@sandia.gov
 */

/*
 * System IO common information.
 */

#include <limits.h>
#include <stdarg.h>

#include "sysio-cmn.h"

#ifndef PATH_SEPARATOR
/*
 * Path separator.
 */
#define PATH_SEPARATOR			'/'
#endif

#ifndef MAX_SYMLINK
/*
 * Max recursion depth allowed when resoving symbolic links.
 */
#define MAX_SYMLINK			250
#endif

/*
 * Internally, all directory entries are carried in the 64-bit capable
 * structure.
 */
#if _LARGEFILE64_SOURCE
#define intnl_dirent dirent64
#else
#define intnl_dirent dirent
#endif
struct dirent;

/*
 * Internally, all file status is carried in the 64-bit capable
 * structure.
 */
#if _LARGEFILE64_SOURCE
#define intnl_stat stat64
#else
#define intnl_stat stat
#endif
struct stat;

#ifdef _HAVE_STATVFS
#if _LARGEFILE64_SOURCE
#define intnl_statvfs statvfs64
#else
#define intnl_statvfs statvfs
#define INTNL_STATVFS_IS_NATURAL	1
#endif
struct statvfs;
struct intnl_statvfs;
#endif

struct utimbuf;

struct intnl_stat;

struct pnode;

#if DEFER_INIT_CWD
extern const char *_sysio_init_cwd;
#endif

extern struct pnode *_sysio_cwd;

extern mode_t _sysio_umask;

extern int _sysio_init(void);
extern void _sysio_shutdown(void);
#if DEFER_INIT_CWD
extern int _sysio_boot(const char *buf, const char *path);
#else
extern int _sysio_boot(const char *buf);
#endif

/*
 * Option-value pair information.
 */
struct option_value_info {
	const char *ovi_name;					/* name */
	char *ovi_value;					/* value */
};

extern const char * _sysio_get_token(const char *buf,
				     int accepts,
				     const char *delim,
				     const char *ignore,
				     char *tbuf);
extern char * _sysio_get_args(char *buf, struct option_value_info *vec);

#define _SYSIO_LOCAL_TIME()	_sysio_local_time()

extern time_t _sysio_local_time(void);

/*
 * The following should be defined by the system includes, and probably are,
 * but it's not illegal to have multiple externs, so long as they are the
 * same. It helps when building the library in a standalone fashion.
 */
extern int SYSIO_INTERFACE_NAME(access)(const char *path, int amode);
extern int SYSIO_INTERFACE_NAME(chdir)(const char *path);
extern int SYSIO_INTERFACE_NAME(chmod)(const char *path, mode_t mode);
extern int SYSIO_INTERFACE_NAME(fchmod)(int fd, mode_t mode);
extern int SYSIO_INTERFACE_NAME(chown)(const char *path, uid_t owner,
				       gid_t group);
extern int SYSIO_INTERFACE_NAME(fchown)(int fd, uid_t owner, gid_t group);
extern int SYSIO_INTERFACE_NAME(close)(int d);
extern int SYSIO_INTERFACE_NAME(dup)(int oldfd);
extern int SYSIO_INTERFACE_NAME(dup2)(int oldfd, int newfd);
extern int SYSIO_INTERFACE_NAME(fcntl)(int fd, int cmd, ...);
extern int SYSIO_INTERFACE_NAME(fstat)(int fd, struct stat *buf);
extern int SYSIO_INTERFACE_NAME(fsync)(int fd);
extern char *SYSIO_INTERFACE_NAME(getcwd)(char *buf, size_t size);
extern off_t SYSIO_INTERFACE_NAME(lseek)(int fd, off_t offset, int whence);
#if _LARGEFILE64_SOURCE
extern off64_t SYSIO_INTERFACE_NAME(lseek64)(int fd, off64_t offset, 
					     int whence);
#endif
extern int SYSIO_INTERFACE_NAME(lstat)(const char *path, struct stat *buf);
#ifdef BSD
extern int SYSIO_INTERFACE_NAME(getdirentries)(int fd, char *buf, int nbytes , 
					       long *basep);
#else
extern ssize_t SYSIO_INTERFACE_NAME(getdirentries)(int fd, char *buf, 
						   size_t nbytes, off_t *basep);
#if _LARGEFILE64_SOURCE
extern ssize_t SYSIO_INTERFACE_NAME(getdirentries64)(int fd,
						     char *buf,
						     size_t nbytes,
						     off64_t *basep);
#endif
#endif
extern int SYSIO_INTERFACE_NAME(mkdir)(const char *path, mode_t mode);
extern int SYSIO_INTERFACE_NAME(open)(const char *path, int flag, ...);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(open64)(const char *path, int flag, ...);
#endif
extern int SYSIO_INTERFACE_NAME(creat)(const char *path, mode_t mode);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(creat64)(const char *path, mode_t mode);
#endif
extern int SYSIO_INTERFACE_NAME(stat)(const char *path, struct stat *buf);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(stat64)(const char *path, struct stat64 *buf);
#endif
#ifdef _HAVE_STATVFS
extern int SYSIO_INTERFACE_NAME(statvfs)(const char *path, struct statvfs *buf);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(statvfs64)(const char *path, 
				struct statvfs64 *buf);
#endif
extern int SYSIO_INTERFACE_NAME(fstatvfs)(int fd, struct statvfs *buf);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(fstatvfs64)(int fd, struct statvfs64 *buf);
#endif
#endif
extern int SYSIO_INTERFACE_NAME(truncate)(const char *path, off_t length);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(truncate64)(const char *path, off64_t length);
#endif
extern int SYSIO_INTERFACE_NAME(ftruncate)(int fd, off_t length);
#if _LARGEFILE64_SOURCE
extern int SYSIO_INTERFACE_NAME(ftruncate64)(int fd, off64_t length);
#endif
extern int SYSIO_INTERFACE_NAME(rmdir)(const char *path);
extern int SYSIO_INTERFACE_NAME(symlink)(const char *path1, const char *path2);
extern int SYSIO_INTERFACE_NAME(readlink)(const char *path,
				char *buf,
				size_t bufsiz);
extern int SYSIO_INTERFACE_NAME(link)(const char *oldpath, const char *newpath);
extern int SYSIO_INTERFACE_NAME(unlink)(const char *path);
extern int SYSIO_INTERFACE_NAME(rename)(const char *oldpath, 
					const char *newpath);
extern int SYSIO_INTERFACE_NAME(fdatasync)(int fd);
extern int SYSIO_INTERFACE_NAME(ioctl)(int fd, unsigned long request, ...);
extern mode_t SYSIO_INTERFACE_NAME(umask)(mode_t mask);
extern int SYSIO_INTERFACE_NAME(mknod)(const char *path, 
				       mode_t mode, dev_t dev);
extern int SYSIO_INTERFACE_NAME(utime)(const char *path, 
				       const struct utimbuf *buf);
extern int SYSIO_INTERFACE_NAME(mount)(const char *source, const char *target,
				       const char *filesystemtype,
				       unsigned long mountflags,
				       const void *data);
extern int SYSIO_INTERFACE_NAME(umount)(const char *target);
