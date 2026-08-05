/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file rpmsgfs_wire.c
 *
 * Compile-time guard on the rpmsgfs wire contract between this R5 and the
 * A53's px4-rpmsgfs-server daemon.
 *
 * The daemon lives in the Petalinux repo
 * (recipes-apps/px4-rpmsgfs-server/files/px4_rpmsgfs_server.py) and speaks
 * NuttX's rpmsgfs protocol in Python.  It therefore hardcodes struct layouts
 * and constants that NuttX derives from Kconfig: 32- versus 64-bit off_t and
 * time_t change sizeof(struct stat), CONFIG_NAME_MAX changes what a directory
 * entry may carry, and so on.  A mismatch would not fail to build or link --
 * it would silently corrupt stat data or truncate filenames on a storage path
 * that holds sensor calibration.
 *
 * So assert every value the Python side assumes, here, where a NuttX config
 * change breaks the firmware build with a message naming the file to update.
 * There is deliberately no code in this translation unit.
 *
 * The two files must be changed together.  Nothing else in PX4 depends on any
 * of this.
 */

#include <nuttx/config.h>

#if defined(CONFIG_FS_RPMSGFS)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

#define WIRE_ASSERT(cond, what) \
	_Static_assert(cond, "rpmsgfs wire contract changed (" what "): update " \
		       "px4_rpmsgfs_server.py in the Petalinux repo to match")

/* --- scalar widths: what makes struct stat 60 bytes rather than 88 ------- */

WIRE_ASSERT(sizeof(off_t) == 4, "off_t width, e.g. CONFIG_FS_LARGEFILE");
WIRE_ASSERT(sizeof(time_t) == 4, "time_t width, e.g. CONFIG_SYSTEM_TIME64");

/* --- struct stat: STAT_FMT / STAT_LEN ------------------------------------ */

WIRE_ASSERT(sizeof(struct stat) == 60, "sizeof(struct stat)");
WIRE_ASSERT(offsetof(struct stat, st_dev) == 0, "st_dev offset");
WIRE_ASSERT(offsetof(struct stat, st_ino) == 4, "st_ino offset");
WIRE_ASSERT(sizeof(((struct stat *)0)->st_ino) == 2, "st_ino width");
WIRE_ASSERT(offsetof(struct stat, st_mode) == 8, "st_mode offset");
WIRE_ASSERT(offsetof(struct stat, st_nlink) == 12, "st_nlink offset");
WIRE_ASSERT(offsetof(struct stat, st_uid) == 14, "st_uid offset");
WIRE_ASSERT(offsetof(struct stat, st_gid) == 16, "st_gid offset");
WIRE_ASSERT(offsetof(struct stat, st_rdev) == 20, "st_rdev offset");
WIRE_ASSERT(offsetof(struct stat, st_size) == 24, "st_size offset");
WIRE_ASSERT(offsetof(struct stat, st_atim) == 28, "st_atim offset");
WIRE_ASSERT(offsetof(struct stat, st_mtim) == 36, "st_mtim offset");
WIRE_ASSERT(offsetof(struct stat, st_ctim) == 44, "st_ctim offset");
WIRE_ASSERT(offsetof(struct stat, st_blksize) == 52, "st_blksize offset");
WIRE_ASSERT(offsetof(struct stat, st_blocks) == 56, "st_blocks offset");

/* struct stat and struct statfs both cross the wire inside a
 * union { ...; uint32_t reserved[16]; }, so they must also still fit. */
WIRE_ASSERT(sizeof(struct stat) <= 64, "struct stat no longer fits its union");

/* --- struct statfs: STATFS_FMT / STATFS_LEN ----------------------------- */

WIRE_ASSERT(sizeof(struct statfs) == 32, "sizeof(struct statfs)");
WIRE_ASSERT(offsetof(struct statfs, f_type) == 0, "f_type offset");
WIRE_ASSERT(offsetof(struct statfs, f_namelen) == 4, "f_namelen offset");
WIRE_ASSERT(offsetof(struct statfs, f_bsize) == 8, "f_bsize offset");
WIRE_ASSERT(offsetof(struct statfs, f_blocks) == 12, "f_blocks offset");
WIRE_ASSERT(offsetof(struct statfs, f_bfree) == 16, "f_bfree offset");
WIRE_ASSERT(offsetof(struct statfs, f_bavail) == 20, "f_bavail offset");
WIRE_ASSERT(offsetof(struct statfs, f_files) == 24, "f_files offset");
WIRE_ASSERT(offsetof(struct statfs, f_ffree) == 28, "f_ffree offset");

/* --- open flags: NX_O_* / NX_O_FLAG_MAP --------------------------------- */

/* These are NOT the Linux values -- O_RDONLY is 1 here and 0 there -- which is
 * exactly why the server translates them. */
WIRE_ASSERT(O_RDONLY == (1 << 0), "O_RDONLY");
WIRE_ASSERT(O_WRONLY == (1 << 1), "O_WRONLY");
WIRE_ASSERT(O_CREAT == (1 << 2), "O_CREAT");
WIRE_ASSERT(O_EXCL == (1 << 3), "O_EXCL");
WIRE_ASSERT(O_APPEND == (1 << 4), "O_APPEND");
WIRE_ASSERT(O_TRUNC == (1 << 5), "O_TRUNC");
WIRE_ASSERT(O_NONBLOCK == (1 << 6), "O_NONBLOCK");
WIRE_ASSERT(O_SYNC == (1 << 7), "O_SYNC");
WIRE_ASSERT(O_CLOEXEC == (1 << 10), "O_CLOEXEC");
WIRE_ASSERT(O_DIRECTORY == (1 << 11), "O_DIRECTORY");

/* --- st_mode bits: the server passes these through untranslated --------- */

WIRE_ASSERT(S_IFMT == 0170000, "S_IFMT");
WIRE_ASSERT(S_IFREG == 0100000, "S_IFREG");
WIRE_ASSERT(S_IFDIR == 0040000, "S_IFDIR");
WIRE_ASSERT(S_IFCHR == 0020000, "S_IFCHR");
WIRE_ASSERT(S_IFBLK == 0060000, "S_IFBLK");
WIRE_ASSERT(S_IFIFO == 0010000, "S_IFIFO");
WIRE_ASSERT(S_IFLNK == 0120000, "S_IFLNK");
WIRE_ASSERT(S_IRUSR == 0000400, "S_IRUSR");
WIRE_ASSERT(S_IWUSR == 0000200, "S_IWUSR");

/* --- directory entries: DTYPE_* and R5_NAME_MAX ------------------------- */

WIRE_ASSERT(DTYPE_DIRECTORY == 4, "DTYPE_DIRECTORY");
WIRE_ASSERT(DTYPE_FILE == 8, "DTYPE_FILE");
WIRE_ASSERT(DTYPE_LINK == 10, "DTYPE_LINK");
WIRE_ASSERT(NAME_MAX == 32, "NAME_MAX -- R5_NAME_MAX in the server");
WIRE_ASSERT(sizeof(((struct dirent *)0)->d_name) == NAME_MAX + 1, "d_name size");

/* --- errno values the server maps by identity --------------------------- */

WIRE_ASSERT(ENOENT == 2, "ENOENT");
WIRE_ASSERT(EBADF == 9, "EBADF");
WIRE_ASSERT(EACCES == 13, "EACCES");
WIRE_ASSERT(EINVAL == 22, "EINVAL");
WIRE_ASSERT(ENOSPC == 28, "ENOSPC");

#endif /* CONFIG_FS_RPMSGFS */

/* Keep this a valid translation unit when rpmsgfs is configured out. */
typedef int rpmsgfs_wire_no_code;
