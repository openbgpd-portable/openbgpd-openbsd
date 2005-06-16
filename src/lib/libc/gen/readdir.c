/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char rcsid[] = "$OpenBSD: readdir.c,v 1.9 2005/06/16 03:01:25 millert Exp $";
#endif /* LIBC_SCCS and not lint */

#include <sys/param.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include "thread_private.h"

/*
 * get next entry in a directory.
 */
struct dirent *
readdir(DIR *dirp)
{
	struct dirent *dp;

	for (;;) {
		if (dirp->dd_loc >= dirp->dd_size) {
			if (dirp->dd_flags & __DTF_READALL)
				return (NULL);
			dirp->dd_loc = 0;
		}
		if (dirp->dd_loc == 0 && !(dirp->dd_flags & __DTF_READALL)) {
			dirp->dd_size = getdirentries(dirp->dd_fd,
			    dirp->dd_buf, dirp->dd_len, &dirp->dd_seek);
			if (dirp->dd_size <= 0)
				return (NULL);
		}
		dp = (struct dirent *)(dirp->dd_buf + dirp->dd_loc);
		if ((long)dp & 03)	/* bogus pointer check */
			return (NULL);
		if (dp->d_reclen <= 0 ||
		    dp->d_reclen > dirp->dd_len + 1 - dirp->dd_loc)
			return (NULL);
		dirp->dd_loc += dp->d_reclen;
		if (dp->d_ino == 0)
			continue;
		return (dp);
	}
}

int
readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result)
{
	struct dirent *dp;
	int ret;

	if (dirp->dd_fd < 0) {
		return EBADF;
	}
	if ((ret = _FD_LOCK(dirp->dd_fd, FD_READ, NULL)) != 0)
		return ret;
	errno = 0;
	dp = readdir(dirp);
	if (dp == NULL && errno != 0) {
		_FD_UNLOCK(dirp->dd_fd, FD_READ);
		return errno;
	}
	if (dp != NULL) 
		memcpy(entry, dp, sizeof (struct dirent) - MAXNAMLEN + dp->d_namlen);
	_FD_UNLOCK(dirp->dd_fd, FD_READ);
	if (dp != NULL)
	*result = entry;
	else
		*result = NULL;
	return 0;
}
