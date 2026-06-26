// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include_next <sys/mount.h>

#ifndef _LIBSPL_SYS_MOUNT_H
#define	_LIBSPL_SYS_MOUNT_H

#include <assert.h>
#include <string.h>
#include <stdlib.h>

/*
 * Some old glibc headers don't define BLKGETSIZE64
 * and we don't want to require the kernel headers
 */
#if !defined(BLKGETSIZE64)
#define	BLKGETSIZE64		_IOR(0x12, 114, size_t)
#endif

/*
 * Some old glibc headers don't correctly define MS_DIRSYNC and
 * instead use the enum name S_WRITE.  When using these older
 * headers define MS_DIRSYNC to be S_WRITE.
 */
#if !defined(MS_DIRSYNC)
#define	MS_DIRSYNC		S_WRITE
#endif

/*
 * Some old glibc headers don't correctly define MS_POSIXACL and
 * instead leave it undefined.  When using these older headers define
 * MS_POSIXACL to the reserved value of (1<<16).
 */
#if !defined(MS_POSIXACL)
#define	MS_POSIXACL		(1<<16)
#endif

#define	MS_USERS	(MS_NOEXEC|MS_NOSUID|MS_NODEV)
#define	MS_OWNER	(MS_NOSUID|MS_NODEV)
#define	MS_GROUP	(MS_NOSUID|MS_NODEV)
#define	MS_COMMENT	0

/*
 * Older glibc <sys/mount.h> headers did not define all the available
 * umount2(2) flags.  Both MNT_FORCE and MNT_DETACH are supported in the
 * kernel back to 2.4.11 so we define them correctly if they are missing.
 */
#ifdef MNT_FORCE
#define	MS_FORCE	MNT_FORCE
#else
#define	MS_FORCE	0x00000001
#endif /* MNT_FORCE */

#ifdef MNT_DETACH
#define	MS_DETACH	MNT_DETACH
#else
#define	MS_DETACH	0x00000002
#endif /* MNT_DETACH */

/*
 * MS_OVERLAY and MS_CRYPT are libzfs-internal flags that are never meant to
 * reach the kernel.  They originally used 0x4 and 0x8, which alias the
 * umount2(2) flags MNT_EXPIRE (0x4) and UMOUNT_NOFOLLOW (0x8).  Because
 * do_unmount() passes its flags straight to umount2(2), a caller that set
 * UMOUNT_NOFOLLOW had that bit misread as MS_CRYPT (an unintended
 * encryption-key unload), and MS_CRYPT/MS_OVERLAY could likewise leak to the
 * kernel.  They now use high bits that alias nothing in the umount2(2) flag
 * set and are masked off in do_unmount() before the syscall.
 *
 * MS_OVERLAY: solaris/zfs compatibility flag (-O) to allow mounting over a
 * non-empty directory (overlay mounting is the Linux default).
 */
#define	MS_OVERLAY	0x20000000

/*
 * MS_CRYPT indicates that encryption keys should be loaded if they are not
 * already available.
 */
#define	MS_CRYPT	0x40000000

#endif /* _LIBSPL_SYS_MOUNT_H */
