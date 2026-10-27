// SPDX-License-Identifier: GPL-2.0
/*
 * gptcas: compare-and-swap a disk's GPT sectors, as planned by
 * scripts/nvme-install/plan-layout.py (docs/nvme-install/DESIGN.md).
 *
 *   gptcas [--dry-run] [--reverse] DEVICE PLAN_DIR
 *
 * PLAN_DIR holds plan.cfg and {old,new}-{primary,backup}.bin. gptcas writes
 * the new primary and backup GPT regions only if each copy on the disk is
 * exactly the old one or already the new one (byte for byte; so a run
 * interrupted between the copies is finished by running again), the new
 * ones are valid GPT with partition 1's start unchanged, and, when
 * partition 1 shrinks, the APFS container on it already fits the new size
 * (its newest valid checkpoint says so). It then reads both regions back.
 * --reverse swaps old and new (rollback). --dry-run checks everything and
 * writes nothing.
 *
 * Runs on jailbroken iOS against /dev/rdisk0 (signed with the entitlements
 * in third_party/hoolock-docs/tools/gdisk.xml), and on macOS/Linux against
 * test images or loop devices. Exit: 0 written or already in place,
 * 2 usage, 3 refused (nothing written), 4 I/O error or verify mismatch.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ioccom.h>
#define DKIOCGETBLOCKSIZE	_IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT	_IOR('d', 25, uint64_t)
#define DKIOCSYNCHRONIZECACHE	_IO('d', 22)
#else
#include <linux/fs.h>
#endif

#define MAXREG	64	/* LBAs per region, enough for 128 entries at 512 B */

struct plan {
	uint64_t lba_size, disk_lbas, primary_lba, primary_count;
	uint64_t backup_lba, backup_count, p1_first, p1_last_old, p1_last_new;
};

static int fd = -1;
static struct plan P;

static void die(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
#include <stdarg.h>
static void die(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "gptcas: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(code);
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffff;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320 & -(c & 1));
	}
	return ~c;
}

static uint64_t le64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void *xalloc(size_t n)
{
	void *p;

	if (posix_memalign(&p, 4096, n))
		die(4, "out of memory");
	memset(p, 0, n);
	return p;
}

static void read_lbas(uint64_t lba, uint64_t count, uint8_t *buf)
{
	size_t n = count * P.lba_size;
	ssize_t r = pread(fd, buf, n, (off_t)(lba * P.lba_size));

	if (r != (ssize_t)n)
		die(4, "read of LBA %llu+%llu failed: %s", (unsigned long long)lba,
		    (unsigned long long)count, r < 0 ? strerror(errno) : "short read");
}

/*
 * Best effort: a raw character device may support none of these. Never a
 * reason to stop between the two GPT copies; the read-back verify follows.
 */
static void sync_dev(void)
{
#ifdef F_FULLFSYNC
	if (!fcntl(fd, F_FULLFSYNC))
		return;
#endif
	if (!fsync(fd))
		return;
#ifdef DKIOCSYNCHRONIZECACHE
	if (!ioctl(fd, DKIOCSYNCHRONIZECACHE))
		return;
#endif
	fprintf(stderr, "gptcas: warning: no cache flush available (%s); verifying by read-back\n",
		strerror(errno));
}

static void write_lbas(uint64_t lba, uint64_t count, const uint8_t *buf)
{
	size_t n = count * P.lba_size;
	ssize_t r = pwrite(fd, buf, n, (off_t)(lba * P.lba_size));

	if (r != (ssize_t)n)
		die(4, "WRITE of LBA %llu+%llu failed: %s (run gptcas again: it finishes or reverses a half-written GPT)",
		    (unsigned long long)lba, (unsigned long long)count,
		    r < 0 ? strerror(errno) : "short write");
	sync_dev();
}

static uint8_t *load(const char *dir, const char *name, size_t want)
{
	char path[1024];
	uint8_t *buf = xalloc(want + 1);
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
		die(2, "%s: %s", path, strerror(errno));
	n = fread(buf, 1, want + 1, f);
	fclose(f);
	if (n != want)
		die(2, "%s: %zu bytes, expected %zu", path, n, want);
	return buf;
}

static void load_plan(const char *dir)
{
	char path[1024], key[64];
	unsigned long long v;
	FILE *f;
	int seen = 0;

	snprintf(path, sizeof(path), "%s/plan.cfg", dir);
	f = fopen(path, "r");
	if (!f)
		die(2, "%s: %s", path, strerror(errno));
	while (fscanf(f, "%63s %llu", key, &v) == 2) {
#define K(name) if (!strcmp(key, #name)) { P.name = v; seen++; continue; }
		K(lba_size) K(disk_lbas) K(primary_lba) K(primary_count) K(backup_lba)
		K(backup_count) K(p1_first) K(p1_last_old) K(p1_last_new)
#undef K
		die(2, "%s: unknown key %s", path, key);
	}
	fclose(f);
	if (seen != 9 || P.lba_size != 4096 || P.primary_lba != 1 ||
	    !P.primary_count || P.primary_count > MAXREG || P.backup_count != P.primary_count ||
	    P.backup_lba + P.backup_count != P.disk_lbas)
		die(2, "%s: incomplete or unexpected plan", path);
}

/*
 * One GPT copy (header + entry array) as it will be written. hdr_at/ent_at
 * are LBA offsets inside the region; my/alt are the header's own LBAs.
 */
static void check_gpt(const uint8_t *reg, uint64_t hdr_at, uint64_t ent_at,
		      uint64_t my, uint64_t alt, uint64_t p1_last, const char *what)
{
	const uint8_t *h = reg + hdr_at * P.lba_size;
	const uint8_t *e = reg + ent_at * P.lba_size;
	uint8_t hc[92];
	uint64_t first_usable, last_usable, f[128], l[128];
	uint32_t num, esz;
	int used = 0;

	if (memcmp(h, "EFI PART", 8) || le32(h + 8) != 0x00010000 || le32(h + 12) != 92)
		die(3, "%s: not a GPT header", what);
	memcpy(hc, h, 92);
	memset(hc + 16, 0, 4);
	if (crc32(hc, 92) != le32(h + 16))
		die(3, "%s: header CRC mismatch", what);
	if (le64(h + 24) != my || le64(h + 32) != alt)
		die(3, "%s: header says LBA %llu/%llu, expected %llu/%llu", what,
		    (unsigned long long)le64(h + 24), (unsigned long long)le64(h + 32),
		    (unsigned long long)my, (unsigned long long)alt);
	first_usable = le64(h + 40);
	last_usable = le64(h + 48);
	num = le32(h + 80);
	esz = le32(h + 84);
	if (esz != 128 || num > 128 || (uint64_t)num * esz > (P.primary_count - 1) * P.lba_size)
		die(3, "%s: unexpected entry array (%u x %u)", what, num, esz);
	if (le64(h + 72) != (hdr_at == 0 ? P.primary_lba + 1 : P.backup_lba))
		die(3, "%s: entry array LBA %llu doesn't match the plan", what,
		    (unsigned long long)le64(h + 72));
	if (crc32(e, (size_t)num * esz) != le32(h + 88))
		die(3, "%s: entry array CRC mismatch", what);
	if (last_usable >= P.backup_lba || first_usable < P.primary_lba + P.primary_count)
		die(3, "%s: usable range %llu-%llu overlaps a GPT copy", what,
		    (unsigned long long)first_usable, (unsigned long long)last_usable);
	for (uint32_t i = 0; i < num; i++) {
		static const uint8_t zero[16];
		const uint8_t *x = e + (size_t)i * esz;

		if (!memcmp(x, zero, 16))
			continue;
		f[used] = le64(x + 32);
		l[used] = le64(x + 40);
		if (f[used] > l[used] || f[used] < first_usable || l[used] > last_usable)
			die(3, "%s: partition %u (%llu-%llu) outside the usable range", what, i + 1,
			    (unsigned long long)f[used], (unsigned long long)l[used]);
		for (int j = 0; j < used; j++)
			if (f[used] <= l[j] && f[j] <= l[used])
				die(3, "%s: partition %u overlaps another", what, i + 1);
		if (i == 0 && (f[0] != P.p1_first || l[0] != p1_last))
			die(3, "%s: partition 1 is %llu-%llu, the plan says %llu-%llu", what,
			    (unsigned long long)f[0], (unsigned long long)l[0],
			    (unsigned long long)P.p1_first, (unsigned long long)p1_last);
		used++;
	}
	if (!used)
		die(3, "%s: no partitions", what);
}

static uint64_t fletcher64(const uint8_t *b, size_t n)
{
	uint64_t s1 = 0, s2 = 0, c1, c2;

	for (size_t i = 8; i < n; i += 4) {
		s1 = (s1 + le32(b + i)) % 0xffffffff;
		s2 = (s2 + s1) % 0xffffffff;
	}
	c1 = 0xffffffff - ((s1 + s2) % 0xffffffff);
	c2 = 0xffffffff - ((s1 + c1) % 0xffffffff);
	return (c2 << 32) | c1;
}

/*
 * Bytes the APFS container on partition 1 spans, from its newest valid
 * container superblock in the checkpoint descriptor area (as apfs-nx.py).
 */
static uint64_t apfs_bytes(void)
{
	uint8_t *b0 = xalloc(4096), *blk;
	uint64_t base = P.p1_first * P.lba_size, best_xid = 0, best = 0;
	uint32_t bs, nblocks;
	uint64_t desc;

	if (pread(fd, b0, 4096, (off_t)base) != 4096 || le32(b0 + 32) != 0x4253584e)
		die(3, "no APFS container superblock at LBA %llu", (unsigned long long)P.p1_first);
	bs = le32(b0 + 36);
	nblocks = le32(b0 + 0x68);
	desc = le64(b0 + 0x70);
	if (bs != 4096 || (nblocks & 0x80000000) || !nblocks || nblocks > 65536)
		die(3, "unexpected APFS checkpoint layout (block size %u, %#x descriptor blocks)", bs, nblocks);
	blk = xalloc(bs);
	for (uint32_t i = 0; i < nblocks; i++) {
		if (pread(fd, blk, bs, (off_t)(base + (desc + i) * bs)) != (ssize_t)bs)
			die(4, "read of the APFS checkpoint area failed");
		if ((le32(blk + 24) & 0xffff) != 1 || le32(blk + 32) != 0x4253584e ||
		    le64(blk) != fletcher64(blk, bs))
			continue;
		if (le64(blk + 16) > best_xid) {
			best_xid = le64(blk + 16);
			best = le64(blk + 0x28) * bs;
		}
	}
	if (!best_xid)
		die(3, "no valid APFS container superblock in the checkpoint area");
	printf("APFS container: %llu bytes (checkpoint xid %llu)\n",
	       (unsigned long long)best, (unsigned long long)best_xid);
	return best;
}

static void geometry(void)
{
	uint64_t lbs = 0, count = 0;
	struct stat st;

	if (fstat(fd, &st))
		die(4, "fstat: %s", strerror(errno));
	if (S_ISREG(st.st_mode)) {
		lbs = P.lba_size;
		count = (uint64_t)st.st_size / lbs;
	} else {
#ifdef __APPLE__
		uint32_t bs;

		if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) || ioctl(fd, DKIOCGETBLOCKCOUNT, &count))
			die(3, "not a disk: %s", strerror(errno));
		lbs = bs;
#else
		int bs;
		uint64_t bytes;

		if (ioctl(fd, BLKSSZGET, &bs) || ioctl(fd, BLKGETSIZE64, &bytes))
			die(3, "not a disk: %s", strerror(errno));
		lbs = (uint64_t)bs;
		count = bytes / lbs;
#endif
	}
	if (lbs != P.lba_size || count != P.disk_lbas)
		die(3, "disk is %llu x %llu bytes, the plan is for %llu x %llu",
		    (unsigned long long)count, (unsigned long long)lbs,
		    (unsigned long long)P.disk_lbas, (unsigned long long)P.lba_size);
}

int main(int argc, char **argv)
{
	int dry = 0, rev = 0, i = 1, flags;
	const char *dev, *dir;
	uint8_t *from_p, *from_b, *to_p, *to_b, *cur_p, *cur_b;
	size_t n;
	uint64_t to_p1_last, from_p1_last;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else if (!strcmp(argv[i], "--reverse"))
			rev = 1;
		else
			die(2, "unknown option %s", argv[i]);
	}
	if (argc - i != 2)
		die(2, "usage: gptcas [--dry-run] [--reverse] DEVICE PLAN_DIR");
	dev = argv[i];
	dir = argv[i + 1];
	load_plan(dir);
	n = P.primary_count * P.lba_size;
	from_p = load(dir, rev ? "new-primary.bin" : "old-primary.bin", n);
	from_b = load(dir, rev ? "new-backup.bin" : "old-backup.bin", n);
	to_p = load(dir, rev ? "old-primary.bin" : "new-primary.bin", n);
	to_b = load(dir, rev ? "old-backup.bin" : "new-backup.bin", n);
	from_p1_last = rev ? P.p1_last_new : P.p1_last_old;
	to_p1_last = rev ? P.p1_last_old : P.p1_last_new;

	flags = dry ? O_RDONLY : O_RDWR;
#ifdef O_SHLOCK
	/*
	 * Darwin refuses a plain write open of a disk whose volumes are
	 * mounted (EBUSY, seen on the iPad for both disk0 and rdisk0); a
	 * shared-lock open is allowed, which is how gdisk writes there.
	 */
	if (!dry)
		flags |= O_SHLOCK;
#endif
	fd = open(dev, flags);
	if (fd < 0)
		die(4, "%s: %s", dev, strerror(errno));
#ifdef F_NOCACHE
	fcntl(fd, F_NOCACHE, 1);
#endif
	geometry();

	/* Both copies of the target must be valid before anything else */
	check_gpt(to_p, 0, 1, P.primary_lba, P.disk_lbas - 1, to_p1_last, "new primary");
	check_gpt(to_b, P.backup_count - 1, 0, P.disk_lbas - 1, P.primary_lba, to_p1_last, "new backup");
	check_gpt(from_p, 0, 1, P.primary_lba, P.disk_lbas - 1, from_p1_last, "expected primary");

	cur_p = xalloc(n);
	cur_b = xalloc(n);
	read_lbas(P.primary_lba, P.primary_count, cur_p);
	read_lbas(P.backup_lba, P.backup_count, cur_b);
	/*
	 * Compare-and-swap per copy: each one must already be the target
	 * (left from an interrupted run) or exactly the plan's source. So a
	 * half-written GPT is finished by running again, or rolled back with
	 * --reverse, by the same code.
	 */
	const char *what[2] = { "primary", "backup" };
	const uint8_t *cur[2] = { cur_p, cur_b }, *from[2] = { from_p, from_b }, *to[2] = { to_p, to_b };
	int todo[2];

	for (int c = 0; c < 2; c++) {
		if (!memcmp(cur[c], to[c], n)) {
			todo[c] = 0;
		} else if (!memcmp(cur[c], from[c], n)) {
			todo[c] = 1;
		} else {
			size_t off = 0;

			while (cur[c][off] == from[c][off])
				off++;
			die(3, "the %s GPT on %s is neither the plan's %s nor its %s copy (first difference from the %s at LBA %llu byte %zu); nothing written",
			    what[c], dev, rev ? "new" : "old", rev ? "old" : "new", rev ? "new" : "old",
			    (unsigned long long)((c ? P.backup_lba : P.primary_lba) + off / P.lba_size),
			    (size_t)(off % P.lba_size));
		}
		printf("gptcas: %s GPT (LBA %llu+%llu): %s\n", what[c],
		       (unsigned long long)(c ? P.backup_lba : P.primary_lba),
		       (unsigned long long)P.primary_count,
		       todo[c] ? (rev ? "the new one, to be restored" : "the original, to be replaced")
			       : (rev ? "already the original" : "already the new one"));
	}
	if (!todo[0] && !todo[1]) {
		printf("gptcas: %s already holds the %s GPT; nothing to do\n", dev, rev ? "old" : "new");
		return 0;
	}

	if (to_p1_last < from_p1_last) {
		uint64_t room = (to_p1_last - P.p1_first + 1) * P.lba_size, have = apfs_bytes();

		if (have > room)
			die(3, "the APFS container (%llu bytes) doesn't fit the new partition 1 (%llu bytes): run resize_apfs first; nothing written",
			    (unsigned long long)have, (unsigned long long)room);
	}
	printf("gptcas: partition 1 LBAs %llu-%llu -> %llu-%llu\n", (unsigned long long)P.p1_first,
	       (unsigned long long)from_p1_last, (unsigned long long)P.p1_first, (unsigned long long)to_p1_last);
	if (dry) {
		printf("gptcas: dry run: every check passed, nothing written\n");
		return 0;
	}

	for (int c = 0; c < 2; c++)
		if (todo[c])
			write_lbas(c ? P.backup_lba : P.primary_lba, P.primary_count, to[c]);
	read_lbas(P.primary_lba, P.primary_count, cur_p);
	read_lbas(P.backup_lba, P.backup_count, cur_b);
	if (memcmp(cur_p, to_p, n) || memcmp(cur_b, to_b, n))
		die(4, "VERIFY FAILED: the GPT read back differs from what was written");
	printf("gptcas: wrote and verified the %s GPT (primary and backup)\n", rev ? "old" : "new");
	return 0;
}
