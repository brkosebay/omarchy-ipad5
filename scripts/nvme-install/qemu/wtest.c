// SPDX-License-Identifier: GPL-2.0
/*
 * wtest: block-device probes for the QEMU write-window test
 * (scripts/nvme-install/qemu-test.sh fence). QEMU only; never on the iPad.
 *
 *   wtest write   DEV LBA COUNT BYTE   O_DIRECT write of COUNT 4 KiB blocks of BYTE, then fdatasync
 *   wtest openrw  DEV                  open(O_RDWR)
 *   wtest discard DEV LBA COUNT        BLKDISCARD
 *   wtest zeroout DEV LBA COUNT        BLKZEROOUT
 *
 * Prints one line: "RESULT <tag> ok" or "RESULT <tag> err <ERRNO>", where tag
 * is $WTEST_TAG. Always exits 0; the host checks the results.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BLK 4096

static const char *tag(void)
{
	const char *t = getenv("WTEST_TAG");

	return t ? t : "-";
}

static const char *errname(int err)
{
	switch (err) {
	case EIO: return "EIO";
	case EACCES: return "EACCES";
	case EROFS: return "EROFS";
	case EPERM: return "EPERM";
	case EOPNOTSUPP: return "EOPNOTSUPP";
	case EINVAL: return "EINVAL";
	case ENOSPC: return "ENOSPC";
	case ENOMEM: return "ENOMEM";
	case ENOENT: return "ENOENT";
	default: return "OTHER";
	}
}

static int report(int err)
{
	if (err)
		printf("RESULT %s err %s\n", tag(), errname(err));
	else
		printf("RESULT %s ok\n", tag());
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t range[2];
	void *buf;
	int fd;

	if (argc < 3)
		return 2;
	if (!strcmp(argv[1], "openrw")) {
		fd = open(argv[2], O_RDWR);
		return report(fd < 0 ? errno : 0);
	}
	if (argc < 5)
		return 2;
	range[0] = strtoull(argv[3], NULL, 0) * BLK;
	range[1] = strtoull(argv[4], NULL, 0) * BLK;

	if (!strcmp(argv[1], "write")) {
		if (argc < 6)
			return 2;
		fd = open(argv[2], O_RDWR | O_DIRECT);
		if (fd < 0)
			return report(errno);
		if (posix_memalign(&buf, BLK, range[1]))
			return report(ENOMEM);
		memset(buf, (int)strtoul(argv[5], NULL, 0), range[1]);
		if (pwrite(fd, buf, range[1], range[0]) != (ssize_t)range[1])
			return report(errno ? errno : EIO);
		return report(fdatasync(fd) ? errno : 0);
	}
	fd = open(argv[2], O_RDWR);
	if (fd < 0)
		return report(errno);
	if (!strcmp(argv[1], "discard"))
		return report(ioctl(fd, BLKDISCARD, range) ? errno : 0);
	if (!strcmp(argv[1], "zeroout"))
		return report(ioctl(fd, BLKZEROOUT, range) ? errno : 0);
	return 2;
}
