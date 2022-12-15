/*
 * Copyright (C) 2011 David Goulet <dgoulet@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef _COMPAT_FCNTL_H
#define _COMPAT_FCNTL_H
#include <fcntl.h>
#include <sys/types.h>

#include <common/compat/errno.hpp>

#if (defined(__CYGWIN__))
typedef long long off64_t;
#endif

#if (defined(__FreeBSD__) || defined(__sun__))
typedef off64_t loff_t;
#endif

#ifdef __linux__
typedef off_t off64_t;
extern int compat_sync_file_range(int fd, off64_t offset, off64_t nbytes,
		unsigned int flags);
#define lttng_sync_file_range(fd, offset, nbytes, flags) \
	compat_sync_file_range(fd, offset, nbytes, flags)

#endif /* __linux__ */

#if (defined(__FreeBSD__) || defined(__CYGWIN__) || defined(__sun__))
/*
 * Possible flags under Linux. Simply nullify them and avoid wrapper.
 */
#define SYNC_FILE_RANGE_WAIT_AFTER    0
#define SYNC_FILE_RANGE_WAIT_BEFORE   0
#define SYNC_FILE_RANGE_WRITE         0

static inline int lttng_sync_file_range(
		int fd __attribute__((unused)),
		off64_t offset __attribute__((unused)),
		off64_t nbytes __attribute__((unused)),
		unsigned int flags __attribute__((unused)))
{
	return -ENOSYS;
}
#endif

#if (defined(__FreeBSD__) || defined(__CYGWIN__) || defined(__sun__))
/*
 * Possible flags under Linux. Simply nullify them and avoid wrappers.
 */
#define SPLICE_F_MOVE       0
#define SPLICE_F_NONBLOCK   0
#define SPLICE_F_MORE       0
#define SPLICE_F_GIFT       0

static inline ssize_t splice(
		int fd_in __attribute__((unused)),
		loff_t *off_in __attribute__((unused)),
		int fd_out __attribute__((unused)),
		loff_t *off_out __attribute__((unused)),
		size_t len __attribute__((unused)),
		unsigned int flags __attribute__((unused)))
{
	return -ENOSYS;
}
#endif

#if !(defined(__linux__) || defined(__FreeBSD__) || defined(__CYGWIN__) || defined(__sun__) || defined(__APPLE__))
#error "Please add support for your OS."
#endif /* __linux__ , __FreeBSD__, __CYGWIN__, __sun__, __APPLE__ */

#endif /* _COMPAT_FCNTL_H */
