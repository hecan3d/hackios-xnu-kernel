/*
 * apfs_rw_test: the write path of the APFS driver, from user space.
 *
 * Boot-arg apfs_rw=1 lets the driver write at all; this mounts a test image from the
 * virtio disk (/dev/rvblk0, boot-arg virtio_blk=1) read-write on a directory of the RAM
 * file system and does to it what a file system has to survive: make and remove files and
 * directories, write and read them back, rename, truncate, attributes.
 *
 * Every step prints its result; a line that says "not implemented" is a hook that is not
 * there yet (ENOTSUP), which is not an error of the volume.  The image on the host is
 * changed as it goes, so apfsck can be run on it afterwards.
 *
 * On the phone the volume is not a whole device: userdata begins with the persistent system
 * volume, and the one this port may write starts at UFS_WRITE_FIRST (8 GiB).  There the test
 * is given the device, a container image to put there first, and where (MB):
 *
 *   apfs_rw_test /dev/ufs_userdata /test.apfs 102400
 *
 * The image goes in through /dev/rufs_userdata (the character device, the only one that takes
 * writes from user space), then the container is mounted rw from that offset (mount spec
 * "b=MB") and the same operations run on it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>

#define DEV     "/dev/rvblk0"
#define MPOINT  "/private/var/apfs-rw"
#define BLOCK   4096                    /* the loader and the partition's blocks */

static int failures;

static void
result(const char *what, int rc)
{
	if (rc == 0) {
		printf("ok   %s\n", what);
	} else if (rc == ENOTSUP || rc == EOPNOTSUPP) {
		printf("---  %s: not implemented (%d)\n", what, rc);
	} else {
		printf("FAIL %s: %s (%d)\n", what, strerror(rc), rc);
		failures++;
	}
}

static int
make(const char *path, const char *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ssize_t n;

	if (fd < 0) {
		return errno;
	}
	n = len == 0 ? 0 : write(fd, data, len);
	if (n != (ssize_t)len) {
		int e = n < 0 ? errno : EIO;
		close(fd);
		return e;
	}
	return close(fd) == 0 ? 0 : errno;
}

static int
readback(const char *path, const char *want, size_t len, int *bad)
{
	char buf[8192];
	struct stat st;
	int fd;
	ssize_t n;

	*bad = 0;
	if (stat(path, &st) != 0) {
		return errno;
	}
	if ((size_t)st.st_size != len) {
		printf("     %s: size %lld, wanted %zu\n", path, (long long)st.st_size, len);
		*bad = 1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return errno;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0) {
		return errno;
	}
	if ((size_t)n != len || memcmp(buf, want, len) != 0) {
		printf("     %s: read %zd bytes, contents %s\n", path, n,
		    (size_t)n == len ? "differ" : "short");
		*bad = 1;
	}
	return 0;
}

/* The character device of a partition: the one that takes writes from user space. */
static const char *
chardev_of(const char *dev, char *buf, size_t cap)
{
	if (strncmp(dev, "/dev/ufs_", 9) == 0) {
		snprintf(buf, cap, "/dev/rufs_%s", dev + 9);
		return buf;
	}
	return dev;
}

/*
 * The device itself, before any file system is on it: a block written and read back.  It is
 * not done when a container is already in place, since the block it writes would land in that
 * container's first block.
 */
static void
raw_check(const char *dev, uint64_t at)
{
	static char w[4096], r[4096];
	int fd = open(dev, O_RDWR);
	ssize_t n = -1, m = -1;

	for (size_t i = 0; i < sizeof(w); i++) {
		w[i] = (char)(i * 7 + 3);
	}
	if (fd >= 0) {
		n = pwrite(fd, w, sizeof(w), (off_t)at);
		m = pread(fd, r, sizeof(r), (off_t)at);
	}
	printf("     raw block write/read: %s (%zd/%zd)%s\n",
	    fd < 0 ? strerror(errno) : (n == (ssize_t)sizeof(w) &&
	    m == (ssize_t)sizeof(r) ? "ok" : "short"),
	    n, m, (m == (ssize_t)sizeof(r) && memcmp(w, r, sizeof(w)) != 0) ?
	    " CONTENTS DIFFER" : "");
	if (fd >= 0) {
		close(fd);
	}
}

/*
 * A container is mostly holes: a 48 GiB one written by mkapfs is 150 KB of real data in a
 * sparse file, and what the flash needs is those bytes plus zeros where the file has holes.
 * That is what "HCK1" carries (tools/apfs/seed-chunks.py), so the image does not have to
 * travel to the phone whole: "HCK1", a count, then for each record kind (0 data, 1 zeros),
 * offset, length, and the bytes of a data record -- all little-endian.
 */
#define SEED_CHUNK (1 << 20)

static int
seed_chunks(const char *chardev, int in, int out, uint64_t at, uint32_t records)
{
	static char buf[SEED_CHUNK];
	uint64_t total = 0;
	int rc = 0;

	memset(buf, 0, sizeof(buf));
	for (uint32_t i = 0; i < records && rc == 0; i++) {
		unsigned char hdr[16];
		uint32_t kind, len;
		uint64_t off;

		if (read(in, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
			printf("FAIL the seed list ends early at record %u\n", i);
			return EINVAL;
		}
		kind = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
		off = 0;
		for (int b = 11; b >= 4; b--) {
			off = (off << 8) | hdr[b];
		}
		len = (uint32_t)hdr[12] | (uint32_t)hdr[13] << 8 | (uint32_t)hdr[14] << 16 | (uint32_t)hdr[15] << 24;
		if (len == 0 || len > SEED_CHUNK || (len % BLOCK) != 0) {
			printf("FAIL the seed list's record %u has a length of %u\n", i, len);
			return EINVAL;
		}
		if (kind == 1) {
			memset(buf, 0, len);
		} else if (kind == 0) {
			if (read(in, buf, len) != (ssize_t)len) {
				printf("FAIL the seed list ends inside record %u\n", i);
				return EINVAL;
			}
		} else {
			printf("FAIL the seed list's record %u has kind %u\n", i, kind);
			return EINVAL;
		}
		if (pwrite(out, buf, len, (off_t)(at + off)) != (ssize_t)len) {
			rc = errno != 0 ? errno : EIO;
			printf("FAIL write %u bytes at %llu into %s: %s\n", len,
			    (unsigned long long)(at + off), chardev, strerror(rc));
			break;
		}
		total += len;
	}
	if (rc == 0) {
		printf("     seed list: %u records, %llu bytes at %llu in %s\n", records,
		    (unsigned long long)total, (unsigned long long)at, chardev);
	}
	return rc;
}

/*
 * Write an APFS container image to the device at a byte offset, whole blocks.  The loader
 * verifies every block it writes, so a "ok" here means the flash has it.
 */
static int
seed(const char *chardev, const char *image, uint64_t at, int force)
{
	static char buf[1 << 20];
	int in, out;
	ssize_t n;
	uint64_t off = 0;
	int rc = 0;

	/*
	 * A container that is already there is left alone: seeding is a first boot's act, and a
	 * second boot that found the volume as the last one left it is what persistence means.
	 * "force" is for a test that wants the volume the image has, not the one the last run
	 * left: the metadata is written over whole.
	 */
	out = open(chardev, O_RDONLY);
	if (out >= 0 && !force) {
		char sb[64];

		if (pread(out, sb, sizeof(sb), (off_t)at) == (ssize_t)sizeof(sb) &&
		    memcmp(sb + 32, "NXSB", 4) == 0) {
			close(out);
			printf("     %s already holds a container at %llu: not written\n", chardev,
			    (unsigned long long)at);
			return 0;
		}
		close(out);
	}

	raw_check(chardev, at);

	in = open(image, O_RDONLY);
	if (in < 0) {
		int e = errno;

		printf("FAIL open(%s): %s\n", image, strerror(e));
		return e;
	}
	out = open(chardev, O_WRONLY);
	if (out < 0) {
		int e = errno;

		printf("FAIL open(%s, O_WRONLY): %s\n", chardev, strerror(e));
		close(in);
		return e;
	}
	{
		unsigned char magic[8];

		if (read(in, magic, sizeof(magic)) == (ssize_t)sizeof(magic) &&
		    memcmp(magic, "HCK1", 4) == 0) {
			uint32_t records = (uint32_t)magic[4] | (uint32_t)magic[5] << 8 |
			    (uint32_t)magic[6] << 16 | (uint32_t)magic[7] << 24;

			rc = seed_chunks(chardev, in, out, at, records);
			close(in);
			close(out);
			return rc;
		}
		if (lseek(in, 0, SEEK_SET) != 0) {
			printf("FAIL rewind %s\n", image);
			close(in);
			close(out);
			return EIO;
		}
	}
	while (rc == 0 && (n = read(in, buf, sizeof(buf))) > 0) {
		size_t len = (size_t)n;

		if (len % BLOCK != 0) {
			size_t pad = BLOCK - len % BLOCK;

			memset(buf + len, 0, pad);
			len += pad;
		}
		if (pwrite(out, buf, len, (off_t)(at + off)) != (ssize_t)len) {
			rc = errno != 0 ? errno : EIO;
			printf("FAIL write %zu bytes at %llu into %s: %s\n", len,
			    (unsigned long long)(at + off), chardev, strerror(rc));
		}
		off += len;
	}
	if (n < 0 && rc == 0) {
		rc = errno;
	}
	close(in);
	close(out);
	if (rc == 0) {
		printf("     container image %s: %llu bytes at %llu in %s\n", image,
		    (unsigned long long)off, (unsigned long long)at, chardev);
	}
	return rc;
}

/*
 * The phone's store is written by one program at a time, and the driver asks for the claim
 * first (kern.hakios.var_busy): a write to /dev/rufs_NAME without it is EPERM.  The writes the
 * mounted volume makes do not go through that path -- they are the filesystem's own -- but the
 * raw blocks this test puts in first do.
 */
static int
store_claim(void)
{
	int one = 1;

	return sysctlbyname("kern.hakios.var_busy", NULL, NULL, &one, sizeof(one));
}

static void
store_release(void)
{
	int zero = 0;

	(void)sysctlbyname("kern.hakios.var_busy", NULL, NULL, &zero, sizeof(zero));
}

/* The same for a mount spec: "device,b=MB" when the container is not at the device's start. */
static const char *
mount_spec(const char *dev, unsigned mb, char *buf, size_t cap)
{
	if (mb != 0) {
		snprintf(buf, cap, "%s,b=%u", dev, mb);
		return buf;
	}
	return dev;
}

/* What the data volume looks like from here, when this run is one that boots with it. */
static void
var_stats(void)
{
	struct statvfs sv;

	if (statvfs("/private/var", &sv) == 0) {
		printf("     /private/var: %llu of %llu MB free, %llu blocks of %lu bytes\n",
		    (unsigned long long)(sv.f_bfree * sv.f_frsize >> 20),
		    (unsigned long long)(sv.f_blocks * sv.f_frsize >> 20),
		    (unsigned long long)sv.f_blocks, (unsigned long)sv.f_frsize);
	}
}


/* The operations a file system is asked for after the first write; run on a file whose
 * bytes are already in the page cache, which is where a direct write path goes wrong. */
static void
ops_test(void)
{
	enum { TOTAL = 100 * 1024 };
	static char data[TOTAL], buf[4096];
	char *map;
	struct stat st, a, b;
	ssize_t n;
	int fd, fd2, rc, bad;

	printf("     -- operations on a file already read --\n");
	for (int i = 0; i < TOTAL; i++) {
		data[i] = (char)(i * 7 + 3);
	}
	rc = make(MPOINT "/ops", data, TOTAL);
	result("ops: create 100 KB", rc);
	if (rc) {
		return;
	}
	fd = open(MPOINT "/ops", O_RDWR);
	if (fd < 0) {
		result("ops: open", errno);
		return;
	}
	/* read it all: the page cache now holds the old bytes, and a write must not leave them */
	bad = 0;
	for (int at = 0; at < TOTAL;) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			bad = 1;
			break;
		}
		at += (int)n;
	}
	result("ops: read it all (fills the page cache)", bad);

	rc = pwrite(fd, "0123456789", 10, 50000) == 10 ? 0 : errno;
	result("ops: overwrite in the middle", rc);
	if (rc == 0) {
		char t[12];

		rc = pread(fd, t, 12, 49999) == 12 && t[0] == data[49999] &&
		    memcmp(t + 1, "0123456789", 10) == 0 && t[11] == data[50010] ? 0 : EIO;
		result("ops: read the middle back", rc);
	}

	fd2 = open(MPOINT "/ops", O_WRONLY | O_APPEND);
	n = fd2 >= 0 ? write(fd2, "tail!", 5) : -1;
	rc = fd2 >= 0 && n == 5 && fstat(fd, &st) == 0 &&
	    st.st_size == TOTAL + 5 ? 0 : (fd2 < 0 ? errno : (n < 0 ? errno : EIO));
	if (rc) {
		printf("     append: fd2 %d, write %zd, errno %d, size %lld\n", fd2, n,
		    errno, fd >= 0 ? (long long)st.st_size : -1);
	}
	if (fd2 >= 0) {
		close(fd2);
	}
	result("ops: append", rc);
	if (rc == 0) {
		rc = pread(fd, buf, 5, TOTAL) == 5 && memcmp(buf, "tail!", 5) == 0 ? 0 : EIO;
		result("ops: read the append back", rc);
	}

	rc = ftruncate(fd, 1000) == 0 && fstat(fd, &st) == 0 && st.st_size == 1000 ? 0 : EIO;
	result("ops: truncate to 1000", rc);
	rc = ftruncate(fd, 40000) == 0 && fstat(fd, &st) == 0 && st.st_size == 40000 ? 0 : EIO;
	result("ops: extend to 40000", rc);
	if (rc == 0) {
		int first = -1;

		memset(buf, 0xaa, sizeof(buf));
		n = pread(fd, buf, 2000, 0);
		rc = n == 2000 ? 0 : EIO;
		for (int i = 0; i < 2000; i++) {
			if (buf[i] != (i < 1000 ? data[i] : 0)) {
				rc = EIO;
				if (first < 0) {
					first = i;
				}
			}
		}
		if (rc) {
			printf("     extend read: %zd bytes, first bad %d (got %d, want %d)\n",
			    n, first, first >= 0 ? buf[first] : -1,
			    first >= 0 ? (first < 1000 ? data[first] : 0) : -1);
		}
		result("ops: old bytes kept, extended part zeros", rc);
	}
	close(fd);

	rc = rename(MPOINT "/ops", MPOINT "/ops-renamed") == 0 ? 0 : errno;
	result("ops: rename", rc);
	if (rc == 0) {
		rc = stat(MPOINT "/ops", &st) != 0 && errno == ENOENT &&
		    stat(MPOINT "/ops-renamed", &st) == 0 ? 0 : EIO;
		result("ops: old name gone, new name there", rc);
	}

	rc = link(MPOINT "/ops-renamed", MPOINT "/ops-hard") == 0 ? 0 : errno;
	result("ops: hard link", rc);
	if (rc == 0) {
		rc = stat(MPOINT "/ops-renamed", &a) == 0 && stat(MPOINT "/ops-hard", &b) == 0 &&
		    a.st_ino == b.st_ino && a.st_nlink == 2 ? 0 : EIO;
		result("ops: both names one inode, link count 2", rc);
	}

	rc = symlink("some/where else", MPOINT "/ops-sym") == 0 ? 0 : errno;
	result("ops: symlink", rc);
	if (rc == 0) {
		n = readlink(MPOINT "/ops-sym", buf, sizeof(buf) - 1);
		rc = n > 0 ? 0 : EIO;
		if (rc == 0) {
			buf[n] = 0;
			rc = n == 15 && strcmp(buf, "some/where else") == 0 ? 0 : EIO;
		}
		result("ops: readlink", rc);
	}

	fd = open(MPOINT "/ops-renamed", O_RDWR);
	rc = fd >= 0 ? 0 : errno;
	if (rc == 0 && ftruncate(fd, 65536) != 0) {
		rc = errno;
	}
	if (rc == 0) {
		map = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		rc = map != MAP_FAILED ? 0 : errno;
		result("ops: shared mapping", rc);
		if (rc == 0) {
			memset(map + 1000, 'M', 100);
			n = pread(fd, buf, 102, 999);
			rc = n == 102 && buf[0] == data[999] && buf[1] == 'M' &&
			    buf[100] == 'M' && buf[101] == 0 ? 0 : EIO;
			if (rc) {
				printf("     mapping read: %zd bytes, [%d %d %d %d] want [0 M M 0]\n",
				    n, buf[0], buf[1], buf[100], buf[101]);
			}
			result("ops: what the mapping stored, read() sees", rc);
			rc = pwrite(fd, "WWWW", 4, 40000) == 4 && map[40000] == 'W' &&
			    map[40003] == 'W' && map[1000] == 'M' ? 0 : EIO;
			result("ops: what write() stored, the mapping sees", rc);
			munmap(map, 65536);
		}
	}
	if (fd >= 0) {
		close(fd);
	}

	unlink(MPOINT "/ops-hard");
	unlink(MPOINT "/ops-sym");
	unlink(MPOINT "/ops-renamed");
}

int
main(int argc, char **argv)
{
	static char data[8192];
	struct statvfs sv;
	const char *dev = DEV;
	const char *seedfile = NULL;
	const char *rawdev;
	char chardev[64], spec[96];
	unsigned seed_mb = 0;
	uint64_t raw_at = 1000 * (uint64_t)BLOCK;
	int bad, rc, seed_only = 0, seed_force = 0;

	if (argc > 1) {
		dev = argv[1];
	}
	if (argc > 2) {
		seedfile = argv[2];
	}
	if (argc > 3) {
		seed_mb = (unsigned)strtoul(argv[3], NULL, 10);
	}
	if (argc > 4 && strcmp(argv[4], "seed") == 0) {
		seed_only = 1;  /* put the container there and stop: the boot mounts it */
	}
	if (argc > 5 && strcmp(argv[5], "force") == 0) {
		seed_force = 1; /* write the image over what is there, not leave it alone */
	}
	if (seed_mb != 0) {
		raw_at = (uint64_t)seed_mb << 20;
	}
	rawdev = chardev_of(dev, chardev, sizeof(chardev));

	memset(data, 0, sizeof(data));
	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = (char)('a' + i % 26);
	}

	mkdir("/private/var", 0755);
	/*
	 * Seeding writes raw blocks and does not mount: the data volume it replaces may be the
	 * very thing that is broken, and a mkdir on it would fail before the write (a corrupted
	 * space manager answers EIO). The mount point is only for the tests that mount.
	 */
	if (!seed_only && mkdir(MPOINT, 0755) != 0 && errno != EEXIST) {
		printf("FAIL mkdir %s: %s\n", MPOINT, strerror(errno));
		return 1;
	}
	if (seedfile != NULL) {
		/* (a device that is not this port's store has no such sysctl: the writes need no claim) */
		rc = store_claim();
		printf("     the store claim: %s\n", rc == 0 ? "held" : strerror(errno));
		rc = seed(rawdev, seedfile, raw_at, seed_force);
		result("write the container image into the device", rc);
		store_release();
		if (rc != 0) {
			return 1;
		}
		if (seed_only) {
			var_stats();
			printf("RESULT: %d failure(s)\n", failures);
			return failures != 0;
		}
	} else {
		/* the device on its own: a block written and read back, before any file system is on it */
		raw_check(rawdev, raw_at);
	}

	if (mount("apfs", MPOINT, 0, mount_spec(dev, seed_mb, spec, sizeof(spec))) != 0) {
		printf("FAIL mount %s: %s (%d)\n", dev, strerror(errno), errno);
		return 1;
	}
	printf("ok   mount %s on %s read-write\n", dev, MPOINT);

	if (statvfs(MPOINT, &sv) == 0) {
		printf("     %llu of %llu blocks free, %lu byte blocks\n",
		    (unsigned long long)sv.f_bfree, (unsigned long long)sv.f_blocks,
		    (unsigned long)sv.f_frsize);
	}

	/* what the volume already has: reading proves the mount, not the hooks */
	{
		char buf[64];
		int fd = open(MPOINT "/hello.txt", O_RDONLY);

		printf("     open(hello.txt): %s\n", fd < 0 ? strerror(errno) : "ok");
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);

			if (n > 0) {
				buf[n] = 0;
				printf("     hello.txt: %s", buf);
			}
			close(fd);
		}
	}
	{
		int fd = open(MPOINT "/probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);

		printf("     open(probe, O_CREAT): %s\n",
		    fd < 0 ? strerror(errno) : "ok");
		if (fd >= 0) {
			ssize_t n = write(fd, "x", 1);

			printf("     write(probe, 1): %s\n",
			    n < 0 ? strerror(errno) : "ok");
			close(fd);
		}
	}
	{
		DIR *d = opendir(MPOINT);
		struct dirent *de;
		int n = 0;

		while (d != NULL && (de = readdir(d)) != NULL) {
			n++;
		}
		if (d != NULL) {
			closedir(d);
		}
		printf("     entries in %s: %d\n", MPOINT, n);
	}

	rc = make(MPOINT "/one", data, 100);
	result("create one (100 bytes)", rc);
	if (rc == 0) {
		rc = readback(MPOINT "/one", data, 100, &bad);
		result("read one back", rc ? rc : (bad ? EIO : 0));
	}

	rc = make(MPOINT "/two", data, 4096);
	result("create two (4096 bytes)", rc);

	result("mkdir dir", (mkdir(MPOINT "/dir", 0755) == 0) ? 0 : errno);
	rc = make(MPOINT "/dir/three", data, 10);
	result("create dir/three", rc);
	rc = (rename(MPOINT "/two", MPOINT "/renamed") == 0) ? 0 : errno;
	result("rename two -> renamed", rc);
	if (rc == 0) {
		result("truncate renamed to 1000",
		    (truncate(MPOINT "/renamed", 1000) == 0) ? 0 : errno);
	}
	result("setxattr",
	    (setxattr(MPOINT "/one", "user.test", "hello", 5, 0, 0) == 0) ? 0 : errno);

	result("unlink dir/three", (unlink(MPOINT "/dir/three") == 0) ? 0 : errno);
	result("rmdir dir", (rmdir(MPOINT "/dir") == 0) ? 0 : errno);
	result("unlink one", (unlink(MPOINT "/one") == 0) ? 0 : errno);
	if (rc == 0) {
		result("unlink renamed", (unlink(MPOINT "/renamed") == 0) ? 0 : errno);
	}

	ops_test();

	var_stats();
	printf("%s: %d failure(s)\n", failures ? "RESULT" : "RESULT", failures);
	/* unmount: left to the reboot; the volume is committed by each operation */
	return failures != 0;
}
