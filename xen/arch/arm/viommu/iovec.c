/*
 *	iovec manipulation routines.
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Fixes:
 *		Andrew Lunn	:	Errors in iovec copying.
 *		Pedro Roque	:	Added memcpy_fromiovecend and
 *					csum_..._fromiovecend.
 *		Andi Kleen	:	fixed error handling for 2.1
 *		Alexey Kuznetsov:	2.1 optimisations
 *		Andi Kleen	:	Fix csum*fromiovecend for IPv6.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <sys/uio.h>
#include <kvm/iovec.h>
#include <string.h>

/*
 *	Copy kernel to iovec. Returns -EFAULT on error.
 *
 *	Note: this modifies the original iovec.
 */

int memcpy_toiovec(struct iovec *iov, unsigned char *kdata, int len)
{
	while (len > 0) {
		if (iov->iov_len) {
			int copy = min_t(unsigned int, iov->iov_len, len);
			memcpy(iov->iov_base, kdata, copy);
			kdata += copy;
			len -= copy;
			iov->iov_len -= copy;
			iov->iov_base += copy;
		}
		iov++;
	}

	return 0;
}

/*
 *	Copy kernel to iovec. Returns -EFAULT on error.
 */

int memcpy_toiovecend(const struct iovec *iov, unsigned char *kdata,
		      size_t offset, int len)
{
	int copy;
	for (; len > 0; ++iov) {
		/* Skip over the finished iovecs */
		if (unlikely(offset >= iov->iov_len)) {
			offset -= iov->iov_len;
			continue;
		}
		copy = min_t(unsigned int, iov->iov_len - offset, len);
		memcpy(iov->iov_base + offset, kdata, copy);
		offset = 0;
		kdata += copy;
		len -= copy;
	}

	return 0;
}

/*
 *	Copy iovec to kernel. Returns -EFAULT on error.
 *
 *	Note: this modifies the original iovec.
 */

int memcpy_fromiovec(unsigned char *kdata, struct iovec *iov, int len)
{
	while (len > 0) {
		if (iov->iov_len) {
			int copy = min_t(unsigned int, len, iov->iov_len);
			memcpy(kdata, iov->iov_base, copy);
			len -= copy;
			kdata += copy;
			iov->iov_base += copy;
			iov->iov_len -= copy;
		}
		iov++;
	}

	return 0;
}

/*
 *	Copy iovec to buffer, avoiding overflow. Returns -EFAULT on error, or
 *	the remaining len.
 *
 *	Note: this modifies the original iovec, the iov pointer, and the
 *	iovcount to describe the remaining buffer.
 */
ssize_t memcpy_fromiovec_safe(void *buf, struct iovec **iov, size_t len,
			      size_t *iovcount)
{
	size_t copy;

	while (len && *iovcount) {
		copy = min(len, (*iov)->iov_len);
		memcpy(buf, (*iov)->iov_base, copy);
		buf += copy;
		len -= copy;

		/* Move iov cursor */
		(*iov)->iov_base += copy;
		(*iov)->iov_len -= copy;

		if (!(*iov)->iov_len) {
			(*iov)++;
			(*iovcount)--;
		}
	}

	return len;
}

/*
 *	Copy iovec from kernel. Returns -EFAULT on error.
 */

int memcpy_fromiovecend(unsigned char *kdata, const struct iovec *iov,
			size_t offset, int len)
{
	/* Skip over the finished iovecs */
	while (offset >= iov->iov_len) {
		offset -= iov->iov_len;
		iov++;
	}

	while (len > 0) {
		char *base = iov->iov_base + offset;
		int copy = min_t(unsigned int, len, iov->iov_len - offset);

		offset = 0;
		memcpy(kdata, base, copy);
		len -= copy;
		kdata += copy;
		iov++;
	}

	return 0;
}

#include "kvm/util.h"
#include "kvm/util-init.h"
#define SELFTEST_ASSERT(cond) do {						\
	if (!(cond)) {								\
		pr_warning("%s:%d: " #cond " failed", __FILE__, __LINE__);	\
		return -EFAULT;							\
	}									\
	} while (0);


static int iovec_selftest(struct kvm *kvm)
{
	size_t i;
	size_t iovcount;
	ssize_t len;
	size_t buf_len = 256;
	u8 in_buf[buf_len];
	u8 out_buf[buf_len];
	struct iovec *iovptr;
	struct iovec iov[] = {
		{
			.iov_base	= in_buf + 128,
			.iov_len	= 128,
		},
		{
			.iov_base	= in_buf,
			.iov_len	= 16
		},
		{
			.iov_base	= in_buf + 16,
			.iov_len	= 32,
		},
		{
			.iov_base	= in_buf + 48,
			.iov_len	= 80,
		}
	};

	for (i = 0; i < buf_len; i++) {
		in_buf[i] = i;
		out_buf[i] = 0;
	}

	iovcount = 4;
	iovptr = iov;
	len = memcpy_fromiovec_safe(out_buf + 128, &iovptr, 128, &iovcount);
	SELFTEST_ASSERT(len == 0);
	SELFTEST_ASSERT(iovcount == 3);
	SELFTEST_ASSERT(iovptr == &iov[1]);
	for (i = 128; i < 256; i++)
		SELFTEST_ASSERT(out_buf[i] == i);

	len = memcpy_fromiovec_safe(out_buf, &iovptr, 0, &iovcount);
	SELFTEST_ASSERT(len == 0);
	SELFTEST_ASSERT(iovcount == 3);
	SELFTEST_ASSERT(iovptr == &iov[1]);

	len = memcpy_fromiovec_safe(out_buf, &iovptr, 1, &iovcount);
	SELFTEST_ASSERT(len == 0);
	SELFTEST_ASSERT(iovcount == 3);
	SELFTEST_ASSERT(iovptr == &iov[1]);
	for (i = 0; i < 1; i++)
		SELFTEST_ASSERT(out_buf[i] == i);

	len = memcpy_fromiovec_safe(out_buf + i, &iovptr, 2, &iovcount);
	SELFTEST_ASSERT(len == 0);
	SELFTEST_ASSERT(iovcount == 3);
	SELFTEST_ASSERT(iovptr == &iov[1]);
	for (; i < 3; i++)
		SELFTEST_ASSERT(out_buf[i] == i);

	len = memcpy_fromiovec_safe(out_buf + i, &iovptr, 37, &iovcount);
	SELFTEST_ASSERT(len == 0);
	SELFTEST_ASSERT(iovcount == 2); /* Cross iov boundary */
	SELFTEST_ASSERT(iovptr == &iov[2]);
	for (; i < 40; i++)
		SELFTEST_ASSERT(out_buf[i] == i);

	len = memcpy_fromiovec_safe(out_buf + i, &iovptr, 100, &iovcount);
	SELFTEST_ASSERT(len == 12); /* Didn't walk past the end of the iovec */
	SELFTEST_ASSERT(iovcount == 0);
	for (; i < 128; i++)
		SELFTEST_ASSERT(out_buf[i] == i);

	return 0;
}
core_init(iovec_selftest);
