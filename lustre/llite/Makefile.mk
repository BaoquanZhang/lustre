# Copyright (C) 2003  Cluster File Systems, Inc.
#
# This code is issued under the GNU General Public License.
# See the file COPYING in this distribution

include $(src)/../portals/Kernelenv

obj-y += llite.o
llite-objs := llite_lib.o dcache.o super.o rw.o \
	super25.o file.o dir.o symlink.o namei.o lproc_llite.o \
	rw26.o llite_nfs.o llite_close.o llite_gns.o special.o
