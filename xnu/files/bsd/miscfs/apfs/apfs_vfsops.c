/*
 * apfs mount-level operations.  See apfs.h; the format is in apfs_lib.c.
 */

#include <kern/assert.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/apfs/apfs.h>
#include <miscfs/apfs/apfs_write.h>
#include <sys/attr.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <uuid/uuid.h>
#include <sys/sysctl.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <sys/hakios.input.h>
#include <sys/hakios.virtio_blk.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

/* the vnode-lock side of the same watchdog (vfs_subr.c) */
extern void vfs_vnode_wd_report(void);
/* and the name cache (vfs_cache.c) */
extern void vfs_cache_wd_report(void);

static LCK_GRP_DECLARE(apfs_lck_grp, "apfs");

/* Blocks are read from the device in pieces of this size. */
#define APFS_META_CHUNK 4096

extern void loader_log_line(const char *line);
#define apfs_klog(fmt, ...) do { \
	char _lbuf[256]; \
	snprintf(_lbuf, sizeof(_lbuf), fmt, ##__VA_ARGS__); \
	loader_log_line(_lbuf); \
	kprintf(fmt, ##__VA_ARGS__); \
} while (0)


/*
 * The device, as apfs_lib wants it.  Offsets and lengths are whole container blocks, and
 * a container block is a multiple of the device's block size (both are powers of two and
 * the container's is at least 4096), so every read lines up.
 */
struct apfs_devctx {
	vnode_t  dc_devvp;
	uint32_t dc_devblock;
	uint64_t dc_base;               /* where on the device the container starts (bytes) */
};

static int
apfs_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0) {
		apfs_klog("apfs_dev_read: unaligned off=%llu devblock=%u\n", (unsigned long long)off, dc->dc_devblock);
		return EINVAL;
	}
	while (len > 0) {
		buf_t bp = NULL;
		size_t chunk = len < APFS_META_CHUNK ? len : APFS_META_CHUNK;
		int error = (int)buf_meta_bread(dc->dc_devvp,
		    (daddr64_t)(off / dc->dc_devblock), APFS_META_CHUNK, NOCRED, &bp);

		if (error) {
			apfs_klog("apfs_dev_read: buf_meta_bread off=%llu blkno=%llu error=%d\n",
			    (unsigned long long)off, (unsigned long long)(off / dc->dc_devblock), error);
			if (bp != NULL) {
				buf_brelse(bp);
			}
			return error;
		}
		memcpy(out, (const void *)buf_dataptr(bp), chunk);
		/*
		 * apfs_lib keeps its own cache of what it needs, and a volume this size would
		 * otherwise fill the buffer cache with blocks nobody asks for twice.
		 */
		buf_markinvalid(bp);
		buf_brelse(bp);

		out += chunk;
		off += chunk;
		len -= chunk;
	}
	return 0;
}

/*
 * A file's contents, straight into the caller's memory: one buffer header of our own and
 * one VNOP_STRATEGY a request, past the buffer cache.  apfs_lib calls this without the
 * volume's lock, from several threads at once; nothing here is shared but the device.
 * (Tried once before, in QEMU, in place of the block cache for the middle of a request and
 * with the lock held, it was slower there; here it is the device reads coming out from
 * under the lock, on a device where one costs a memory copy -- see apfs_lib.c.)
 */
#define APFS_DATA_MAX (128 * 1024)

static int
apfs_dev_read_data(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0 || (len % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < APFS_DATA_MAX ? len : APFS_DATA_MAX;
		buf_t bp = buf_alloc(dc->dc_devvp);
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, B_READ);
		buf_setblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setlblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)out);
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		/* read() must fill the whole buffer, and so must this (md cuts the count short) */
		if (error == 0 && (buf_resid(bp) != 0 || buf_count(bp) != n)) {
			error = EIO;
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		out += n;
		off += n;
		len -= n;
	}
	return 0;
}

/*
 * apfs_lib's writes: whole blocks, both the metadata of a transaction and the contents of
 * one file (apfs_write_in_place).  Like apfs_dev_read_data this is called with the volume's
 * lock let go, so it touches nothing but the device context.  The write goes straight to
 * the device, past the cache, and is waited for: a commit's blocks have to arrive in the
 * order it writes them.
 */
/* (what writing costs, for kern.apfs_stats: one count per request the device is given) */
static uint64_t apfs_wr_calls, apfs_wr_bytes, apfs_wr_ticks;

/*
 * Who gives the device its writes.  The per-process table of the mount is filled by the vnops, and
 * p409 showed it empty while the device took 371 716 writes (2.3 GB, 294 s) -- either those writes
 * are not made by a vnop of this mount at all or the accounting never runs.  This one is here, at
 * the device, where the number is certainly counted: the process name of whoever is writing, and
 * how much time the device took for it.
 */
#define APFS_WR_WHO 24
static struct {
	char     name[MAXCOMLEN + 1];
	uint64_t calls, ticks, bytes;
} apfs_wr_who[APFS_WR_WHO];

static void
apfs_wr_note(uint64_t ticks, size_t bytes)
{
	char name[MAXCOMLEN + 1];
	unsigned i;

	proc_selfname(name, sizeof(name));
	for (i = 0; i < APFS_WR_WHO; i++) {
		if (apfs_wr_who[i].name[0] != 0 && strncmp(apfs_wr_who[i].name, name, sizeof(name)) == 0) {
			os_atomic_inc(&apfs_wr_who[i].calls, relaxed);
			os_atomic_add(&apfs_wr_who[i].ticks, ticks, relaxed);
			os_atomic_add(&apfs_wr_who[i].bytes, (uint64_t)bytes, relaxed);
			return;
		}
		if (apfs_wr_who[i].name[0] == 0) {
			strlcpy(apfs_wr_who[i].name, name, sizeof(apfs_wr_who[i].name));
			os_atomic_inc(&apfs_wr_who[i].calls, relaxed);
			os_atomic_add(&apfs_wr_who[i].ticks, ticks, relaxed);
			os_atomic_add(&apfs_wr_who[i].bytes, (uint64_t)bytes, relaxed);
			return;
		}
	}
}

static int
apfs_dev_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	const char *in = buf;
	size_t total = len;
	uint64_t t0 = apfs_ticks();
	int error = 0;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0 || (len % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < APFS_DATA_MAX ? len : APFS_DATA_MAX;
		buf_t bp = buf_alloc(dc->dc_devvp);
		uint64_t wrote_at = apfs_ticks();
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, B_WRITE);
		buf_setblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setlblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)in);
		/*
		 * A write that completes through the buffer layer takes one off the vnode's
		 * outstanding-write count; that count is only ever put up here, at the start,
		 * because apfs_lib writes blocks itself and not through cluster_write().
		 * Without this the first write of each block panics ("numoutput < 0").
		 */
		vnode_startwrite(dc->dc_devvp);
		error = VNOP_STRATEGY(bp);
		if (error) {
			vnode_writedone(dc->dc_devvp);          /* no completion will come */
		} else {
			error = (int)buf_biowait(bp);
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		in += n;
		off += n;
		len -= n;
		apfs_wr_calls++;
		apfs_wr_note(apfs_ticks() - wrote_at, n);
	}
	apfs_wr_bytes += (uint64_t)total;
	apfs_wr_ticks += apfs_ticks() - t0;
	return error;
}

/* ------------------------------------------------------------- where the time goes */

/*
 * One mounted volume, for the sysctl below to look at.  This port mounts one APFS volume
 * and the counters are a bring-up aid, so a single pointer is enough; it is set when the
 * volume is mounted and cleared when it goes away.
 */
static struct apfs_mount *apfs_the_mount;
static LCK_MTX_DECLARE(apfs_pool_lock, &apfs_lck_grp);   /* the pool's threads, and apfs_mounts */

/*
 * Why the last mount did or did not come up writable, in one line: the kernel's own printf
 * goes to the console (a UART on the phone), which the boot log brought back over USB does not
 * carry, so kern.apfs_stats has it too and init can print it where it is seen.
 */
char apfs_mount_why[1024];

/*
 * Every APFS mount, for a mount of another container of the same device to find that the
 * device is open already (apfs_mount): the root's RAM disk holds the boot volume and the OS
 * cryptex, each its own container.  Under apfs_pool_lock.
 */
static struct apfs_mount *apfs_mounts;

static bool
apfs_device_held(dev_t rdev)
{
	struct apfs_mount *m;
	bool held = false;

	lck_mtx_lock(&apfs_pool_lock);
	for (m = apfs_mounts; m != NULL && !held; m = m->am_next) {
		held = (m->am_rdev == rdev);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return held;
}

/*
 * Which volume kern.apfs_stats is about: the root's, when the root is APFS (the iOS volume on
 * the flash, once init has switched to it); else the one mounted last.
 */
extern vnode_t rootvnode;

static struct apfs_mount *
apfs_stats_mount(void)
{
	mount_t mp = rootvnode != NULLVP ? vnode_mount(rootvnode) : NULL;

	if (mp != NULL && strcmp(vfs_statfs(mp)->f_fstypename, "apfs") == 0 && vfs_fsprivate(mp) != NULL) {
		return vfs_fsprivate(mp);
	}
	return apfs_the_mount;
}

static uint64_t
apfs_ticks_ms(uint64_t ticks)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(ticks, &ns);
	return ns / 1000000;
}

/* ---------------------------------------------------------------- the watchdog */

/*
 * An operation that does not come back is invisible in the counters: they say how much was
 * waited for in all, not who is still in one.  Every ten seconds, name the operations that
 * have been going on for more than five, with the process doing them, and the writer's
 * mutex if it has been held that long -- so a thread that never returns names itself even
 * when the mount's own lock is free.
 */
#define APFS_WD_EVERY_NS    (10ULL * NSEC_PER_SEC)
#define APFS_WD_SLOW_NS     (5ULL * NSEC_PER_SEC)
#define APFS_WD_NAMED       8

/* How long an operation may last before the watchdog writes it down (boot-arg apfs_wd_ms,
 * 5000 by default; a run that has to see the slow ones asks for 200). */
uint64_t apfs_wd_slow_ns = APFS_WD_SLOW_NS;

static uint64_t
apfs_ns_to_ticks(uint64_t ns)
{
	uint64_t t = 0;

	nanoseconds_to_absolutetime(ns, &t);
	return t;
}

static void
apfs_watchdog_call(void *p0, void *p1)
{
	struct apfs_mount *am = p0;
	uint64_t now, slow, deadline, wlock_ms = 0;
	struct {
		const char *site;
		char        name[MAXCOMLEN + 1];
		uint64_t    ms;
		uint32_t    tid;
	} held[APFS_WD_NAMED];
	char wlock_name[MAXCOMLEN + 1];
	unsigned n = 0, i;
	bool wlock = false;
	static unsigned quiet;         /* ticks with nothing to say, for the once-a-minute line */

	(void)p1;
	now = apfs_ticks();
	slow = apfs_ns_to_ticks(apfs_wd_slow_ns);
	wlock_name[0] = 0;
	if (am->am_ops != NULL && lck_mtx_try_lock(&am->am_lock)) {
		for (i = 0; i < am->am_nops && n < APFS_WD_NAMED; i++) {
			struct apfs_op *op = &am->am_ops[i];
			uint64_t age = now - op->start, ns;

			if (age < slow) {
				continue;
			}
			absolutetime_to_nanoseconds(age, &ns);
			held[n].site = op->site;
			strlcpy(held[n].name, op->name, sizeof(held[n].name));
			held[n].ms = ns / 1000000;
			held[n].tid = thread_tid(op->th);
			n++;
		}
		if (am->am_wlock_since != 0 && now - am->am_wlock_since >= slow) {
			uint64_t ns;

			absolutetime_to_nanoseconds(now - am->am_wlock_since, &ns);
			wlock = true;
			wlock_ms = ns / 1000000;
			strlcpy(wlock_name, am->am_wlock_name, sizeof(wlock_name));
		}
		lck_mtx_unlock(&am->am_lock);
	}
	for (i = 0; i < n; i++) {
		apfs_klog("apfs: wd: %s[thread %u] in %s for %llu ms\n", held[i].name, held[i].tid,
		    held[i].site, (unsigned long long)held[i].ms);
	}
	if (wlock) {
		apfs_klog("apfs: wd: the writer's mutex held by %s for %llu ms\n", wlock_name,
		    (unsigned long long)wlock_ms);
	}
	if (n == 0 && !wlock) {
		/*
		 * A line a minute to say it is still watching: the absence of the lines above
		 * then means "nothing was stuck", not "the watchdog never ran".
		 */
		if (++quiet % 6 == 0) {
			apfs_klog("apfs: wd: nothing over 5 s; %u operations going on\n", am->am_nops);
		}
	} else {
		quiet = 0;
	}
	/* and the vnode locks: a lookup, an open or a stat can be stuck in the VFS itself */
	vfs_vnode_wd_report();
	vfs_cache_wd_report();
	if (am->am_wd_stop || am->am_wd_call == NULL) {
		return;
	}
	deadline = apfs_ticks() + apfs_ns_to_ticks(APFS_WD_EVERY_NS);
	thread_call_enter_delayed((thread_call_t)am->am_wd_call, deadline);
}

void
apfs_watchdog_begin(struct apfs_mount *am)
{
	thread_call_t call = thread_call_allocate(apfs_watchdog_call, am);

	if (call == NULL) {
		return;
	}
	am->am_wd_call = call;
	am->am_wd_stop = false;
	thread_call_enter_delayed(call, apfs_ticks() + apfs_ns_to_ticks(APFS_WD_EVERY_NS));
}

/* (the call is not freed: it may be running on its thread right now, and a mount is one) */
void
apfs_watchdog_end(struct apfs_mount *am)
{
	thread_call_t call = am->am_wd_call;

	if (call == NULL) {
		return;
	}
	am->am_wd_call = NULL;
	am->am_wd_stop = true;
	thread_call_cancel(call);
}

/* The volume's lock by who takes it, and how many others waited, appended to buf */
static void
apfs_lock_report(struct apfs_mount *am, char *buf, size_t len)
{
	size_t at = strlen(buf);
	uint32_t i;

	/* (first: the report is cut off at len, and the per-process lines alone are half of it) */
	{
		unsigned shown = 0;

		at += scnprintf(buf + at, len - at, "writes by who (requests the device was given, and the time it took):");
		for (i = 0; i < APFS_WR_WHO && apfs_wr_who[i].name[0] != 0 && at < len; i++) {
			if (shown >= 8) {
				break;
			}
			shown++;
			at += scnprintf(buf + at, len - at, " %s %llu req %llu ms %llu MB%s", apfs_wr_who[i].name,
			    (unsigned long long)apfs_wr_who[i].calls,
			    (unsigned long long)apfs_ticks_ms(apfs_wr_who[i].ticks),
			    (unsigned long long)(apfs_wr_who[i].bytes >> 20), i + 1 < APFS_WR_WHO ? "," : "\n");
		}
		at += scnprintf(buf + at, len - at, "\n");
	}

	at += scnprintf(buf + at, len - at, "lookups  %llu of names not there, now in the name cache as such; %llu "
	    "answered by it so\n", am->am_lookup_negative, am->am_lookup_negative_hits);
	at += scnprintf(buf + at, len - at, "inactive %llu with nothing to give back, without the lock\n",
	    am->am_inactive_free);
	at += scnprintf(buf + at, len - at, "vnodes   %llu found there already (%llu of them by a lookup, its name now in the "
	    "name cache), %llu made; %llu lookups the name cache answered with the vnode\n", am->am_node_found,
	    am->am_node_named, am->am_node_made, am->am_lookup_positive_hits);
	at += scnprintf(buf + at, len - at, "lock     waited by priority (all ms; over 1 ms: how many, ms): background (<=4) %llu; "
	    "%llu, %llu; utility (<=20) %llu; %llu, %llu; default (<=31) %llu; %llu, %llu; user-initiated (<=46) %llu; %llu, "
	    "%llu; interactive %llu; %llu, %llu\n",
	    apfs_ticks_ms(am->am_prio_wait[0]), am->am_prio_long[0], apfs_ticks_ms(am->am_prio_long_wait[0]),
	    apfs_ticks_ms(am->am_prio_wait[1]), am->am_prio_long[1], apfs_ticks_ms(am->am_prio_long_wait[1]),
	    apfs_ticks_ms(am->am_prio_wait[2]), am->am_prio_long[2], apfs_ticks_ms(am->am_prio_long_wait[2]),
	    apfs_ticks_ms(am->am_prio_wait[3]), am->am_prio_long[3], apfs_ticks_ms(am->am_prio_long_wait[3]),
	    apfs_ticks_ms(am->am_prio_wait[4]), am->am_prio_long[4], apfs_ticks_ms(am->am_prio_long_wait[4]));
	{
		static const char *const band[5] = { "background", "utility", "default", "user-initiated", "interactive" };

		at += scnprintf(buf + at, len - at, "lock     waits over 1 ms, where the time went (ms: blocked, the lock not "
		    "given / queued, runnable without a processor / on a processor):");
		for (i = 0; i < 5; i++) {
			at += scnprintf(buf + at, len - at, " %s %llu/%llu/%llu%s", band[i],
			    (unsigned long long)apfs_ticks_ms(am->am_long_blocked[i]),
			    (unsigned long long)apfs_ticks_ms(am->am_long_queued[i]),
			    (unsigned long long)apfs_ticks_ms(am->am_long_oncpu[i]), i < 4 ? ";" : "\n");
		}
		at += scnprintf(buf + at, len - at, "lock     waits over 1 ms, the processor each ended on:");
		for (i = 0; i < 8; i++) {
			at += scnprintf(buf + at, len - at, " cpu %u %llu%s", i, am->am_long_cpu[i], i < 7 ? "," : "\n");
		}
	}
	at += scnprintf(buf + at, len - at, "lock     takes with others waiting: none %llu, 1 %llu, 2-3 %llu, 4-7 %llu, "
	    "8 or more %llu\n", am->am_waiters_hist[0], am->am_waiters_hist[1], am->am_waiters_hist[2],
	    am->am_waiters_hist[3], am->am_waiters_hist[4]);
	{
		uint64_t max_ns;

		absolutetime_to_nanoseconds(am->am_lock_hold_max, &max_ns);
		at += scnprintf(buf + at, len - at, "lock     holds: %llu under 10 us, %llu to 100 us, %llu to 1 ms, %llu to "
		    "10 ms, %llu longer; longest %llu us\n", am->am_lock_hold_hist[0], am->am_lock_hold_hist[1],
		    am->am_lock_hold_hist[2], am->am_lock_hold_hist[3], am->am_lock_hold_hist[4], max_ns / 1000);
	}
	for (i = 0; i < APFS_LOCK_SITES && am->am_sites[i].name != NULL && at < len; i++) {
		const struct apfs_lock_site *ls = &am->am_sites[i];
		uint64_t wmax, hmax;

		absolutetime_to_nanoseconds(ls->wait_max, &wmax);
		absolutetime_to_nanoseconds(ls->hold_max, &hmax);
		at += scnprintf(buf + at, len - at, "lock by  %-22s %7llu takes %7llu again; waited %6llu ms (longest %6llu us, "
		    "%5llu over 1 ms); held %6llu ms (longest %6llu us, %5llu over 1 ms)\n", ls->name, ls->takes, ls->relocks,
		    (unsigned long long)apfs_ticks_ms(ls->wait), wmax / 1000, ls->wait_long,
		    (unsigned long long)apfs_ticks_ms(ls->hold), hmax / 1000, ls->hold_long);
	}
	for (i = 0; am->am_procs != NULL && i < APFS_LOCK_PROCS && i < am->am_nprocs && at < len; i++) {
		const struct apfs_lock_proc *lp = &am->am_procs[i];
		uint64_t wmax, wwmax;

		/* (only who waited 1 ms or more in all, or read 1 MB or more, or wrote for a second) */
		if (apfs_ticks_ms(lp->wait) == 0 && lp->read_bytes + lp->pagein_bytes < (1 << 20) &&
		    apfs_ticks_ms(lp->write_held + lp->write_wait) == 0) {
			continue;
		}
		absolutetime_to_nanoseconds(lp->wait_max, &wmax);
		absolutetime_to_nanoseconds(lp->write_wait_max, &wwmax);
		at += scnprintf(buf + at, len - at, "who      %-16.16s waited %6llu times over 10 us, %6llu ms (longest %7llu us, "
		    "%4llu over 1 ms); read(2) %6llu calls %10llu bytes %6llu ms; pagein %6llu calls %10llu bytes %6llu ms; "
		    "wrote %6llu epochs %6llu ms (longest wait %6llu us), waited %6llu ms\n",
		    lp->name, lp->waits, (unsigned long long)apfs_ticks_ms(lp->wait), wmax / 1000, lp->wait_long,
		    lp->read_calls, lp->read_bytes, (unsigned long long)apfs_ticks_ms(lp->read_ticks), lp->pagein_calls,
		    lp->pagein_bytes, (unsigned long long)apfs_ticks_ms(lp->pagein_ticks), lp->write_epochs,
		    (unsigned long long)apfs_ticks_ms(lp->write_held), wwmax / 1000,
		    (unsigned long long)apfs_ticks_ms(lp->write_wait));
	}
}

/*
 * kern.apfs_stats: reading it says where the time went, writing anything zeroes the
 * counters.  The point is to tell apart, on one road through the driver, the time spent
 * waiting for the device, the time spent decompressing, and the rest -- which is the
 * driver's own work and the paging machinery above it.
 */
/* the kernel's own page counts: what decides how much the paging machinery asks a fault for */
extern unsigned int vm_page_free_count, vm_page_free_target, vm_page_throttle_limit;
extern unsigned int vm_page_speculative_count, vm_page_inactive_count, vm_page_active_count;
extern unsigned int vm_page_wire_count;

static int
sysctl_apfs_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	struct apfs_mount *am = apfs_stats_mount();
	size_t cap = 16384;
	char *text = kalloc_data(cap, Z_WAITOK | Z_ZERO);
	int error;

	if (text == NULL) {
		return ENOMEM;
	}
	if (am == NULL) {
		strlcpy(text, "apfs: nothing mounted\n", cap);
	} else {
		struct apfs_nx *nx = &am->am_nx;

		snprintf(text, cap,
		    "mount    %s\n"
		    "pagein   %llu calls  %llu ms  %llu bytes\n"
		    "read     %llu calls  %llu ms  %llu bytes; from the page cache %llu, read into it %llu (%s)\n"
		    "device   %llu calls  %llu ms  %llu bytes\n"
		    "writes   %llu calls  %llu ms  %llu bytes\n"
		    "data     %llu calls  %llu ms  %llu bytes\n"
		    "inflate  %llu calls  %llu ms  %llu bytes in  %llu bytes out  %llu by zlib\n"
		    "batch    %llu requests  %llu pieces  %u threads at most\n"
		    "ahead    %llu times  %llu pieces  %llu of them used\n"
		    "pra      %u KB a window, %llu windows filled, %llu bytes\n"
		    "pagein   sizes: %llu up to 32 KB, %llu to 64 KB, %llu to 128 KB, %llu to 256 KB, %llu bigger\n"
		    "vm       free %u (target %u, throttle %u) speculative %u, inactive %u, active %u, wired %u\n"
		    "blocks   %llu read  %llu cache hits\n"
		    "lookups  %llu by the name's hash  %llu by reading the directory\n"
		    "lock     %llu takes  %llu ms waiting  %llu ms held; metadata read without it %llu times\n",
		    apfs_mount_why,
		    (unsigned long long)am->am_pagein_calls,
		    (unsigned long long)apfs_ticks_ms(am->am_pagein_ticks),
		    (unsigned long long)am->am_pagein_bytes,
		    (unsigned long long)am->am_read_calls,
		    (unsigned long long)apfs_ticks_ms(am->am_read_ticks),
		    (unsigned long long)am->am_read_bytes,
		    (unsigned long long)am->am_read_cached,
		    (unsigned long long)am->am_read_filled,
		    am->am_ubc_read ? "read(2) through it" : "read(2) past it: apfs_ubc_read=0",
		    (unsigned long long)nx->dev_calls,
		    (unsigned long long)apfs_ticks_ms(nx->dev_ticks),
		    (unsigned long long)nx->dev_bytes,
		    (unsigned long long)apfs_wr_calls,
		    (unsigned long long)apfs_ticks_ms(apfs_wr_ticks),
		    (unsigned long long)apfs_wr_bytes,
		    (unsigned long long)nx->data_calls,
		    (unsigned long long)apfs_ticks_ms(nx->data_ticks),
		    (unsigned long long)nx->data_bytes,
		    (unsigned long long)nx->inflate_calls,
		    (unsigned long long)apfs_ticks_ms(nx->inflate_ticks),
		    (unsigned long long)nx->inflate_in,
		    (unsigned long long)nx->inflate_out,
		    (unsigned long long)nx->inflate_fallbacks,
		    (unsigned long long)nx->batch_calls,
		    (unsigned long long)nx->batch_pieces,
		    nx->parallel != NULL ? nx->parallel_max : 1,
		    (unsigned long long)nx->ra_batches,
		    (unsigned long long)nx->ra_pieces,
		    (unsigned long long)nx->ra_hits,
		    (unsigned)(am->am_pra / 1024),
		    (unsigned long long)am->am_pra_calls,
		    (unsigned long long)am->am_pra_bytes,
		    (unsigned long long)am->am_pagein_size[0],
		    (unsigned long long)am->am_pagein_size[1],
		    (unsigned long long)am->am_pagein_size[2],
		    (unsigned long long)am->am_pagein_size[3],
		    (unsigned long long)am->am_pagein_size[4],
		    vm_page_free_count, vm_page_free_target, vm_page_throttle_limit,
		    vm_page_speculative_count, vm_page_inactive_count, vm_page_active_count,
		    vm_page_wire_count,
		    (unsigned long long)nx->reads,
		    (unsigned long long)nx->hits,
		    (unsigned long long)nx->lookups_hashed,
		    (unsigned long long)nx->lookups_scanned,
		    (unsigned long long)am->am_lock_waits,
		    (unsigned long long)apfs_ticks_ms(am->am_lock_wait_ticks),
		    (unsigned long long)apfs_ticks_ms(am->am_lock_held_ticks),
		    (unsigned long long)nx->unlocked_fills);
		apfs_lock_report(am, text, cap);
	}
	error = SYSCTL_OUT(req, text, strlen(text) + 1);
	kfree_data(text, cap);
	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (am != NULL) {
		apfs_lock(am);
		am->am_pagein_calls = am->am_pagein_ticks = am->am_pagein_bytes = 0;
		am->am_read_calls = am->am_read_ticks = am->am_read_bytes = 0;
		am->am_nx.dev_calls = am->am_nx.dev_ticks = am->am_nx.dev_bytes = 0;
		apfs_wr_calls = apfs_wr_bytes = apfs_wr_ticks = 0;
		am->am_nx.data_calls = am->am_nx.data_ticks = am->am_nx.data_bytes = 0;
		am->am_nx.inflate_calls = am->am_nx.inflate_ticks = 0;
		am->am_nx.inflate_in = am->am_nx.inflate_out = am->am_nx.inflate_fallbacks = 0;
		am->am_nx.reads = am->am_nx.hits = 0;
		am->am_nx.batch_calls = am->am_nx.batch_pieces = 0;
		am->am_nx.ra_batches = am->am_nx.ra_pieces = am->am_nx.ra_hits = 0;
		am->am_pra_calls = 0;
		am->am_pra_bytes = 0;
		memset(am->am_pagein_size, 0, sizeof(am->am_pagein_size));
		am->am_lock_waits = am->am_lock_wait_ticks = am->am_lock_held_ticks = 0;
		memset(am->am_lock_wait_hist, 0, sizeof(am->am_lock_wait_hist));
		am->am_lock_wait_max = 0;
		memset(am->am_lock_hold_hist, 0, sizeof(am->am_lock_hold_hist));
		am->am_lock_hold_max = 0;
		memset(am->am_waiters_hist, 0, sizeof(am->am_waiters_hist));
		am->am_lookup_negative = 0;
		am->am_lookup_negative_hits = 0;
		am->am_inactive_free = 0;
		am->am_lookup_positive_hits = am->am_node_found = am->am_node_made = am->am_node_named = 0;
		memset(am->am_prio_wait, 0, sizeof(am->am_prio_wait));
		memset(am->am_prio_long, 0, sizeof(am->am_prio_long));
		memset(am->am_prio_long_wait, 0, sizeof(am->am_prio_long_wait));
		memset(am->am_long_blocked, 0, sizeof(am->am_long_blocked));
		memset(am->am_long_queued, 0, sizeof(am->am_long_queued));
		memset(am->am_long_oncpu, 0, sizeof(am->am_long_oncpu));
		memset(am->am_long_cpu, 0, sizeof(am->am_long_cpu));
		lck_spin_lock(&am->am_procs_spin);
		if (am->am_procs != NULL) {
			memset(am->am_procs, 0, APFS_LOCK_PROCS * sizeof(am->am_procs[0]));
		}
		am->am_nprocs = 0;
		lck_spin_unlock(&am->am_procs_spin);
		am->am_read_cached = am->am_read_filled = 0;
		for (uint32_t i = 0; i < APFS_LOCK_SITES; i++) {
			const char *name = am->am_sites[i].name;

			memset(&am->am_sites[i], 0, sizeof(am->am_sites[i]));
			am->am_sites[i].name = name;
		}
		apfs_unlock(am);
	}
	return 0;
}

/*
 * The same breakdown, straight to the log.  Needed where there is no user space to read
 * the sysctl: when the volume is the root, nothing else is running.
 */
void
apfs_stats_print(void)
{
	struct apfs_mount *am = apfs_stats_mount();
	struct apfs_nx *nx;

	if (am == NULL) {
		printf("apfs: nothing mounted\n");
		return;
	}
	nx = &am->am_nx;
	printf("apfs: pagein  %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)am->am_pagein_calls,
	    (unsigned long long)apfs_ticks_ms(am->am_pagein_ticks),
	    (unsigned long long)am->am_pagein_bytes);
	printf("apfs: read    %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)am->am_read_calls,
	    (unsigned long long)apfs_ticks_ms(am->am_read_ticks),
	    (unsigned long long)am->am_read_bytes);
	printf("apfs: device  %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)nx->dev_calls,
	    (unsigned long long)apfs_ticks_ms(nx->dev_ticks),
	    (unsigned long long)nx->dev_bytes);
	/* file contents, read without the lock (the line above is metadata, with it) */
	printf("apfs: data    %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)nx->data_calls,
	    (unsigned long long)apfs_ticks_ms(nx->data_ticks),
	    (unsigned long long)nx->data_bytes);
	/* "by zlib": streams libdeflate refused (bad, or more out than the piece holds), zlib's then */
	printf("apfs: inflate %llu calls  %llu ms  %llu bytes in  %llu bytes out  %llu by zlib\n",
	    (unsigned long long)nx->inflate_calls,
	    (unsigned long long)apfs_ticks_ms(nx->inflate_ticks),
	    (unsigned long long)nx->inflate_in, (unsigned long long)nx->inflate_out,
	    (unsigned long long)nx->inflate_fallbacks);
	/* requests whose whole pieces went out together (nx->parallel), and how many pieces */
	printf("apfs: batch   %llu requests  %llu pieces  %u threads at most\n",
	    (unsigned long long)nx->batch_calls, (unsigned long long)nx->batch_pieces,
	    nx->parallel != NULL ? nx->parallel_max : 1);
	printf("apfs: ahead   %llu times  %llu pieces  %llu of them used\n",
	    (unsigned long long)nx->ra_batches, (unsigned long long)nx->ra_pieces,
	    (unsigned long long)nx->ra_hits);
	printf("apfs: blocks  %llu read  %llu cache hits  highest block %llu\n",
	    (unsigned long long)nx->reads, (unsigned long long)nx->hits,
	    (unsigned long long)nx->max_addr);
	printf("apfs: lock    %llu takes  %llu ms waiting  %llu ms held\n",
	    (unsigned long long)am->am_lock_waits,
	    (unsigned long long)apfs_ticks_ms(am->am_lock_wait_ticks),
	    (unsigned long long)apfs_ticks_ms(am->am_lock_held_ticks));
	{
		uint64_t max_ns;

		absolutetime_to_nanoseconds(am->am_lock_wait_max, &max_ns);
		printf("apfs: lock    waits: %llu under 2 us, %llu to 10 us, %llu to 100 us, %llu to 1 ms, %llu longer; "
		    "longest %llu us; metadata read from the device %llu ms\n",
		    am->am_lock_wait_hist[0], am->am_lock_wait_hist[1], am->am_lock_wait_hist[2], am->am_lock_wait_hist[3],
		    am->am_lock_wait_hist[4], max_ns / 1000, (unsigned long long)apfs_ticks_ms(nx->dev_ticks));
		absolutetime_to_nanoseconds(am->am_lock_hold_max, &max_ns);
		printf("apfs: lock    holds: %llu under 10 us, %llu to 100 us, %llu to 1 ms, %llu to 10 ms, %llu longer; "
		    "longest %llu us\n", am->am_lock_hold_hist[0], am->am_lock_hold_hist[1], am->am_lock_hold_hist[2],
		    am->am_lock_hold_hist[3], am->am_lock_hold_hist[4], max_ns / 1000);
		printf("apfs: lookups %llu by the name's hash, %llu by reading the directory\n", nx->lookups_hashed,
		    nx->lookups_scanned);
		{
			char *rep = kalloc_data(16384, Z_WAITOK | Z_ZERO);

			if (rep != NULL) {
				apfs_lock_report(am, rep, 16384);
				printf("%s", rep);
				kfree_data(rep, 16384);
			}
		}
		printf("apfs: metadata read without the lock: %llu times, %llu of them already in by another's read; "
		    "retired blocks %llu now, %llu at most; operations going on now %u\n", nx->unlocked_fills, nx->fill_races,
		    nx->retired_now, nx->retired_most, am->am_nops);
	}
}

/* kern.hakios_memory: the kernel's line about memory (osfmk/arm64/loader_return.c), for a job list to keep */
extern void hakios_memory_line(char *buf, size_t len);

static int
sysctl_hakios_memory SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[256];

	hakios_memory_line(line, sizeof(line));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, hakios_memory, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_hakios_memory, "A", "where the memory is, in MB");

SYSCTL_PROC(_kern, OID_AUTO, apfs_stats, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_apfs_stats, "A", "APFS: where the time goes; write to zero it");

/* ------------------------------------------------------------------ vnodes */

static uint32_t
apfs_hash_index(uint64_t obj_id)
{
	/* the low bits of an identifier are the ones that differ between neighbours */
	return (uint32_t)((obj_id ^ (obj_id >> 17)) % APFS_HASH_BUCKETS);
}

/* The vnode for an object, making one if there is not one already. */
int
apfs_node_vnode(struct apfs_mount *am, uint64_t obj_id, vnode_t dvp,
    struct componentname *cnp, vnode_t *vpp)
{
	struct vnode_fsparam vfsp;
	struct apfs_node *an;
	struct apfs_inode ino;
	uint32_t b = apfs_hash_index(obj_id);
	uint64_t size = 0;
	enum vtype vtype;
	int error;

	/* already have one? */
	apfs_lock(am);
	for (an = am->am_hash[b]; an != NULL; an = an->an_next) {
		if (an->an_id == obj_id && an->an_attached) {
			vnode_t vp = an->an_vp;
			uint32_t vid = vnode_vid(vp);

			apfs_unlock(am);
			if (vnode_getwithvid(vp, vid) == 0) {
				os_atomic_inc(&am->am_node_found, relaxed);
				/*
				 * Found by a lookup: the name goes into the name cache, as vnode_create
				 * puts it there for a new vnode -- else every later lookup of this
				 * name comes back here for the lock.  (The volume does not change;
				 * a synthetic name is looked for only where there is no real one, and
				 * adding one purges its directory's entries.)  The name cache keeps
				 * one name per vnode and leaves one that has a name already as it is.
				 */
				if (dvp != NULLVP && cnp != NULL && (cnp->cn_flags & MAKEENTRY)) {
					cache_enter(dvp, vp, cnp);
					os_atomic_inc(&am->am_node_named, relaxed);
				}
				*vpp = vp;
				return 0;
			}
			/* it was going away under us: fall through and make a new one */
			apfs_lock(am);
			break;
		}
	}

	if (obj_id >= APFS_SYNTH_BASE) {
		/* a synthetic directory: an empty one, root's, made up here */
		const struct apfs_synth *sy = NULL;
		uint32_t i;

		for (i = 0; i < am->am_nsynth; i++) {
			if (am->am_synth[i].id == obj_id) {
				sy = &am->am_synth[i];
			}
		}
		bzero(&ino, sizeof(ino));
		if (sy == NULL) {
			error = ENOENT;
		} else {
			ino.obj_id = obj_id;
			ino.parent_id = sy->parent;
			ino.mode = APFS_S_IFDIR | 0755;
			error = 0;
		}
	} else {
		error = apfs_inode_get(&am->am_vol, obj_id, &ino);
		if (error) {
			apfs_log("node_vnode %llu: inode_get failed: %d\n",
			    (unsigned long long)obj_id, error);
		}
		if (error == 0) {
			uint32_t i;

			error = apfs_inode_size(&am->am_vol, &ino, &size);
			if (error) {
				apfs_log("node_vnode %llu: inode_size failed: %d\n",
				    (unsigned long long)obj_id, error);
				size = ino.size;       /* the stream's size is still better than nothing */
				error = 0;
			}
			/* a directory with another name: ".." is where that name is */
			for (i = 0; i < am->am_nsynth; i++) {
				if (am->am_synth[i].alias && am->am_synth[i].id == obj_id) {
					ino.parent_id = am->am_synth[i].parent;
				}
			}
		}
	}
	apfs_unlock(am);
	if (error) {
		return error;
	}

	switch (ino.mode & APFS_S_IFMT) {
	case APFS_S_IFREG:  vtype = VREG; break;
	case APFS_S_IFDIR:  vtype = VDIR; break;
	case APFS_S_IFLNK:  vtype = VLNK; break;
	case APFS_S_IFCHR:  vtype = VCHR; break;
	case APFS_S_IFBLK:  vtype = VBLK; break;
	case APFS_S_IFIFO:  vtype = VFIFO; break;
	case APFS_S_IFSOCK: vtype = VSOCK; break;
	default:
		apfs_log("node_vnode %llu: unknown mode 0%o\n",
		    (unsigned long long)obj_id, (unsigned)ino.mode);
		return EIO;
	}

	an = kalloc_type(struct apfs_node, Z_WAITOK | Z_ZERO);
	if (an == NULL) {
		return ENOMEM;
	}
	an->an_mount = am;
	an->an_id = obj_id;
	an->an_parent = ino.parent_id;
	an->an_mode = ino.mode;
	an->an_size = size;
	an->an_ino = ino;       /* nothing here can change: the mount is read only */

	bzero(&vfsp, sizeof(vfsp));
	vfsp.vnfs_mp = am->am_mp;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "apfs";
	vfsp.vnfs_dvp = dvp;
	vfsp.vnfs_fsnode = an;
	vfsp.vnfs_vops = apfs_vnodeop_p;
	vfsp.vnfs_markroot = (obj_id == am->am_root_id);
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_rdev = (vtype == VCHR || vtype == VBLK) ? (dev_t)ino.rdev : 0;
	vfsp.vnfs_filesize = size;
	vfsp.vnfs_cnp = cnp;
	/* let VFS cache the name only when it came from a lookup that has one */
	vfsp.vnfs_flags = (dvp != NULL && cnp != NULL) ? 0 : VNFS_NOCACHE;

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &an->an_vp);
	if (error) {
		apfs_log("node_vnode %llu: vnode_create failed: %d\n",
		    (unsigned long long)obj_id, error);
		kfree_type(struct apfs_node, an);
		return error;
	}
	vnode_settag(an->an_vp, VT_OTHER);

	apfs_lock(am);
	an->an_next = am->am_hash[b];
	am->am_hash[b] = an;
	an->an_attached = true;
	apfs_unlock(am);
	os_atomic_inc(&am->am_node_made, relaxed);

	*vpp = an->an_vp;
	return 0;
}

/* A synthetic directory by where it is and its name; with am_lock held. */
const struct apfs_synth *
apfs_synth_find(struct apfs_mount *am, uint64_t parent, const char *name, size_t len)
{
	uint32_t i;

	for (i = 0; i < am->am_nsynth; i++) {
		if (am->am_synth[i].parent == parent && strlen(am->am_synth[i].name) == len &&
		    memcmp(am->am_synth[i].name, name, len) == 0) {
			return &am->am_synth[i];
		}
	}
	return NULL;
}

/*
 * sysctl kern.apfs_synthetic=PATH: a synthetic directory at PATH, whose parent must be a
 * directory of an APFS volume (real or synthetic) and which must not exist yet.
 * PATH=TARGET: at PATH, another name for the directory TARGET of the same volume.  Root
 * only; nothing ever takes one away again but unmounting.
 */
static int
apfs_sysctl_synthetic SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char path[MAXPATHLEN], name[APFS_SYNTH_NAMELEN], *target;
	vnode_t dvp = NULLVP, vp = NULLVP, tvp = NULLVP;
	struct apfs_mount *am;
	struct apfs_node *dn;
	uint64_t target_id = 0;
	const char *slash;
	size_t len;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, "", 1);          /* reading it says nothing */
	}
	if ((error = suser(kauth_cred_get(), NULL)) != 0) {
		return error;
	}
	if (req->newlen == 0 || req->newlen >= sizeof(path)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, path, req->newlen);
	if (error) {
		return error;
	}
	path[req->newlen] = '\0';
	target = strchr(path, '=');
	if (target != NULL) {
		*target++ = '\0';
		error = vnode_lookup(target, 0, &tvp, vfs_context_current());
		if (error) {
			return error;
		}
		if (vnode_vtype(tvp) != VDIR || vnode_mount(tvp) == NULL ||
		    strcmp(vfs_statfs(vnode_mount(tvp))->f_fstypename, "apfs") != 0) {
			vnode_put(tvp);
			return ENOTDIR;
		}
		target_id = APFS_VP(tvp)->an_id;
	}
	while ((len = strlen(path)) > 1 && path[len - 1] == '/') {
		path[len - 1] = '\0';
	}
	slash = strrchr(path, '/');
	if (slash == NULL || slash[1] == '\0') {
		error = EINVAL;
		goto out;
	}
	len = strlen(slash + 1);
	if (len >= sizeof(name)) {
		error = EINVAL;
		goto out;
	}
	strlcpy(name, slash + 1, sizeof(name));        /* before the path is cut to its parent */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		error = EINVAL;
		goto out;
	}
	if (vnode_lookup(path, 0, &vp, vfs_context_current()) == 0) {
		vnode_put(vp);
		error = EEXIST;
		goto out;
	}
	path[slash - path == 0 ? 1 : slash - path] = '\0';     /* the parent ("/" for "/name") */
	error = vnode_lookup(path, 0, &dvp, vfs_context_current());
	if (error) {
		goto out;
	}
	if (vnode_vtype(dvp) != VDIR || vnode_mount(dvp) == NULL ||
	    strcmp(vfs_statfs(vnode_mount(dvp))->f_fstypename, "apfs") != 0 ||
	    (tvp != NULLVP && vnode_mount(tvp) != vnode_mount(dvp))) {
		vnode_put(dvp);
		error = ENOTSUP;
		goto out;
	}
	dn = APFS_VP(dvp);
	am = dn->an_mount;
	apfs_lock(am);
	if (am->am_synth == NULL) {
		am->am_synth = kalloc_data(APFS_SYNTH_MAX * sizeof(*am->am_synth), Z_WAITOK | Z_ZERO);
	}
	if (am->am_synth == NULL) {
		error = ENOMEM;
	} else if (apfs_synth_find(am, dn->an_id, name, len) != NULL) {
		error = EEXIST;
	} else if (am->am_nsynth >= APFS_SYNTH_MAX) {
		error = ENOSPC;
	} else {
		uint32_t n0 = am->am_nsynth;
		uint64_t id = (tvp != NULLVP) ? target_id : APFS_SYNTH_BASE + n0;
		struct apfs_inode ino;

		/* an id the volume does not use itself, or nothing (reading the tree may let go of the lock) */
		if (tvp == NULLVP && apfs_inode_get(&am->am_vol, id, &ino) != ENOENT) {
			error = EEXIST;
		} else if (am->am_nsynth != n0 || apfs_synth_find(am, dn->an_id, name, len) != NULL) {
			error = EBUSY;          /* another was made meanwhile: try again */
		} else {
			struct apfs_synth *sy = &am->am_synth[am->am_nsynth];

			sy->parent = dn->an_id;
			sy->alias = (tvp != NULLVP);
			sy->id = id;
			strlcpy(sy->name, name, sizeof(sy->name));
			am->am_nsynth++;
			error = 0;
		}
	}
	apfs_unlock(am);
	/* the parent's entries changed: what the name cache has of it is stale */
	cache_purge(dvp);
	cache_purge_negatives(dvp);     /* (the new name was known not to be there) */
	vnode_put(dvp);
out:
	if (tvp != NULLVP) {
		vnode_put(tvp);
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, apfs_synthetic, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, apfs_sysctl_synthetic, "A",
    "write PATH: an empty synthetic directory on an APFS volume (a mount point)");

/* Take a node out of the hash; called from vnop_reclaim. */
void
apfs_node_detach(struct apfs_node *an)
{
	struct apfs_mount *am = an->an_mount;
	uint32_t b = apfs_hash_index(an->an_id);
	struct apfs_node **pp;

	apfs_lock(am);
	for (pp = &am->am_hash[b]; *pp != NULL; pp = &(*pp)->an_next) {
		if (*pp == an) {
			*pp = an->an_next;
			break;
		}
	}
	an->an_attached = false;
	apfs_unlock(am);
}

/*
 * apfs_lib asks these around a device read and round decompression.  They do
 * nothing: the lock stays for the device work.  A transfer of the UFS block
 * service is set up one at a time (ufs_write/ufs_write_restore), so a read left
 * in flight while a commit wrote a block came back with another block's content
 * -- bad checksums, nodes lost -- and the write was lost.  Slower, but the bus
 * is taken by one transfer at a time.
 */
static void
apfs_lib_unlock(void *ctx)
{
	(void)ctx;
}

static void
apfs_lib_relock(void *ctx)
{
	(void)ctx;
}

/* ------------------------------------------------------- threads for one request */

/*
 * apfs_lib's nx->parallel: a few kernel threads that help a request of many compressed
 * pieces along, each reading and decompressing whole pieces into the caller's buffer while
 * the caller does the same.  A request posts a job and wakes as many helpers as it can
 * use; a helper that comes takes a worker number and then items until none are left.  The
 * caller works through items too, then takes its job off the list -- no helper can join
 * after that -- and waits for the helpers still inside it.  The job lives on the caller's
 * stack, which is why nothing may touch it once the caller is back.
 *
 * Boot-arg apfs_par=N: at most N threads on one request, the caller's included (default:
 * four, or the processors if fewer -- see apfs_pool_start(); 1 turns the helpers off).
 */
struct apfs_pjob {
	struct apfs_pjob *pj_next;
	int             (*pj_fn)(void *arg, uint32_t item, uint32_t worker);
	void             *pj_arg;
	uint32_t          pj_items;
	uint32_t          pj_workers;
	volatile SInt32   pj_taken;     /* items handed out so far (may run past pj_items) */
	volatile SInt32   pj_error;
	uint32_t          pj_joined;    /* worker numbers given out; under apfs_pool_lock */
	uint32_t          pj_active;    /* helpers inside the job now; under apfs_pool_lock */
};

static struct apfs_pjob *apfs_pool_jobs;        /* oldest first */
static uint32_t apfs_pool_threads;
static uint32_t apfs_par_max;                   /* threads on one request, the caller's included */

static void
apfs_pjob_work(struct apfs_pjob *pj, uint32_t worker)
{
	for (;;) {
		SInt32 item = OSIncrementAtomic(&pj->pj_taken);
		int error;

		if (item < 0 || (uint32_t)item >= pj->pj_items) {
			break;
		}
		error = pj->pj_fn(pj->pj_arg, (uint32_t)item, worker);
		if (error != 0) {
			OSCompareAndSwap(0, (UInt32)error, (volatile UInt32 *)&pj->pj_error);
		}
	}
}

static void
apfs_pool_thread(__unused void *arg, __unused wait_result_t wr)
{
	lck_mtx_lock(&apfs_pool_lock);
	for (;;) {
		struct apfs_pjob *pj;
		uint32_t worker;

		for (pj = apfs_pool_jobs; pj != NULL; pj = pj->pj_next) {
			if (pj->pj_joined < pj->pj_workers &&
			    (uint32_t)pj->pj_taken < pj->pj_items) {
				break;
			}
		}
		if (pj == NULL) {
			(void)msleep(&apfs_pool_jobs, &apfs_pool_lock, PRIBIO, "apfs_pool", NULL);
			continue;
		}
		worker = pj->pj_joined++;
		pj->pj_active++;
		lck_mtx_unlock(&apfs_pool_lock);
		apfs_pjob_work(pj, worker);
		lck_mtx_lock(&apfs_pool_lock);
		if (--pj->pj_active == 0) {
			wakeup(&pj->pj_active);
		}
	}
}

static int
apfs_parallel(__unused void *ctx, uint32_t items, uint32_t workers,
    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg)
{
	struct apfs_pjob pj, **pp;
	uint32_t i;

	bzero(&pj, sizeof(pj));
	pj.pj_fn = fn;
	pj.pj_arg = arg;
	pj.pj_items = items;
	pj.pj_workers = workers;
	pj.pj_joined = 1;               /* the caller is worker 0 */

	lck_mtx_lock(&apfs_pool_lock);
	for (pp = &apfs_pool_jobs; *pp != NULL; pp = &(*pp)->pj_next) {
	}
	*pp = &pj;
	for (i = 1; i < workers; i++) {
		wakeup_one((caddr_t)&apfs_pool_jobs);
	}
	lck_mtx_unlock(&apfs_pool_lock);

	apfs_pjob_work(&pj, 0);

	lck_mtx_lock(&apfs_pool_lock);
	for (pp = &apfs_pool_jobs; *pp != &pj; pp = &(*pp)->pj_next) {
	}
	*pp = pj.pj_next;
	while (pj.pj_active > 0) {
		(void)msleep(&pj.pj_active, &apfs_pool_lock, PRIBIO, "apfs_pjob", NULL);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return pj.pj_error;
}

/*
 * The helpers, once, at the first mount.  Four threads on one request at most unless
 * apfs_par says otherwise: on this phone one reader with 1 MB reads took 765 ms over the
 * restore ramdisk with two, 612 with four or five, 714 with all eight -- the four small
 * cores take two and a half times as long over a piece, and a request waits for its
 * slowest one.
 */
static void
apfs_pool_start(void)
{
	uint32_t want = ml_get_topology_info()->num_cpus, i;

	if (want > 4) {
		want = 4;
	}
	if (want < 1) {
		want = 1;
	}
	PE_parse_boot_argn("apfs_par", &want, sizeof(want));
	if (want < 1) {
		want = 1;
	}
	lck_mtx_lock(&apfs_pool_lock);
	if (apfs_par_max != 0) {
		lck_mtx_unlock(&apfs_pool_lock);
		return;
	}
	apfs_par_max = want;
	lck_mtx_unlock(&apfs_pool_lock);
	for (i = 0; i + 1 < want; i++) {
		thread_t th;

		if (kernel_thread_start(apfs_pool_thread, NULL, &th) != KERN_SUCCESS) {
			break;
		}
		thread_deallocate(th);
		OSIncrementAtomic((volatile SInt32 *)&apfs_pool_threads);
	}
	printf("apfs: %u threads on a request of many pieces (%u helpers)\n", want,
	    apfs_pool_threads);
}

/* ------------------------------------------------------------------ mount */

static void
apfs_free_mount(struct apfs_mount *am)
{
	apfs_watchdog_end(am);
	if (am->am_synth != NULL) {
		kfree_data(am->am_synth, APFS_SYNTH_MAX * sizeof(*am->am_synth));
	}
	while (am->am_iobuf_kept > 0) {
		kfree_data(am->am_iobuf[--am->am_iobuf_kept], am->am_iobuf_size);
	}
	apfs_nx_close(&am->am_nx);
	if (am->am_ops != NULL) {
		kfree_data(am->am_ops, APFS_OPS_MAX * sizeof(am->am_ops[0]));
	}
	if (am->am_procs != NULL) {
		kfree_data(am->am_procs, APFS_LOCK_PROCS * sizeof(am->am_procs[0]));
	}
	lck_spin_destroy(&am->am_iobuf_spin, &apfs_lck_grp);
	lck_spin_destroy(&am->am_procs_spin, &apfs_lck_grp);
	lck_mtx_destroy(&am->am_wlock, &apfs_lck_grp);
	lck_mtx_destroy(&am->am_lock, &apfs_lck_grp);
	kfree_type(struct apfs_mount, am);
}

/*
 * Which container of the device and which volume of it: containers lie back to back, each
 * saying in its superblock how big it is (nx_block_count blocks of nx_block_size), so the
 * N-th starts where the one before it ends -- the RAM disks of this port hold several (the
 * boot volume, a cryptex) and so may the phone's darwin partition.  volume < 0: the first
 * that has a root directory.  base: where the first container starts (the data volume of the
 * phone is 8 GiB into userdata, after the system's own container), 0 when it is the device's
 * beginning.
 */
static int
apfs_mount_common(mount_t mp, vnode_t devvp, vfs_context_t ctx, uint64_t base,
    uint32_t container, int volume)
{
	struct apfs_mount *am;
	struct apfs_devctx *dc;
	struct apfs_dev dev;
	struct vfsstatfs *sp;
	uint32_t devblock = 0;
	uint64_t devblocks = 0, nx_size = 0;
	int error;
	int rw = 0;
	bool writable = false;

	if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, (caddr_t)&devblock, 0, ctx) != 0 ||
	    devblock == 0 || (APFS_META_CHUNK % devblock) != 0) {
		apfs_klog("apfs_mount_common: DKIOCGETBLOCKSIZE error/bad: devblock=%u\n", devblock);
		return EINVAL;
	}
	(void)VNOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT, (caddr_t)&devblocks, 0, ctx);
	apfs_klog("apfs_mount_common: devblock=%u devblocks=%llu base=%llu container=%u volume=%d\n",
	    devblock, (unsigned long long)devblocks, (unsigned long long)base, container, volume);
	/* where memory stands before the volume is in use: how much of it was wired away by the boot */
	{
		extern unsigned int vm_page_free_count, vm_page_wire_count, vm_page_active_count;
		extern unsigned int vm_page_inactive_count, vm_page_speculative_count;
		extern unsigned int vm_page_pageable_external_count, vm_page_pageable_internal_count;

		apfs_klog("apfs: memory at mount: free %u MB, files %u MB, programs %u MB, wired %u MB; "
		    "active %u, inactive %u, speculative %u pages\n",
		    (unsigned)((uint64_t)vm_page_free_count * PAGE_SIZE >> 20),
		    (unsigned)((uint64_t)vm_page_pageable_external_count * PAGE_SIZE >> 20),
		    (unsigned)((uint64_t)vm_page_pageable_internal_count * PAGE_SIZE >> 20),
		    (unsigned)((uint64_t)vm_page_wire_count * PAGE_SIZE >> 20),
		    vm_page_active_count, vm_page_inactive_count, vm_page_speculative_count);
	}

	am = kalloc_type(struct apfs_mount, Z_WAITOK | Z_ZERO);
	if (am == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&am->am_lock, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&am->am_wlock, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_spin_init(&am->am_iobuf_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_spin_init(&am->am_procs_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	{
		uint32_t ubc = 1;

		(void)PE_parse_boot_argn("apfs_ubc_read", &ubc, sizeof(ubc));
		am->am_ubc_read = (ubc != 0);
	}
	{
		/*
		 * Read-ahead for pagein, in KB (0: off).  The paging machinery asks 32 KB at a
		 * time, so a file read through comes a thousand faults, and a reader going
		 * forward is given the pieces after the one it asked for (see apfs_pagein_ahead).
		 */
		uint32_t pra = 0;

		(void)PE_parse_boot_argn("apfs_pra", &pra, sizeof(pra));
		am->am_pra = MIN(pra, 1024u) * 1024u;
		if (am->am_pra != 0) {
			apfs_klog("apfs: pagein reads ahead %u KB for a reader going forward (apfs_pra)\n",
			    (unsigned)(am->am_pra / 1024));
		}
	}
	{
		/* how slow an operation has to be for the watchdog to write it down (apfs_wd_ms) */
		uint32_t wd_ms = 5000;

		(void)PE_parse_boot_argn("apfs_wd_ms", &wd_ms, sizeof(wd_ms));
		if (wd_ms < 50) {
			wd_ms = 50;
		}
		apfs_wd_slow_ns = (uint64_t)wd_ms * 1000000ULL;
		if (wd_ms != 5000) {
			apfs_klog("apfs: operations over %u ms are written down (apfs_wd_ms)\n", wd_ms);
		}
	}
	am->am_procs = kalloc_data(APFS_LOCK_PROCS * sizeof(am->am_procs[0]), Z_WAITOK | Z_ZERO);
	am->am_ops = kalloc_data(APFS_OPS_MAX * sizeof(am->am_ops[0]), Z_WAITOK | Z_ZERO);
	apfs_watchdog_begin(am);
	am->am_mp = mp;
	am->am_devvp = devvp;
	am->am_devblock = devblock;

	dc = kalloc_type(struct apfs_devctx, Z_WAITOK | Z_ZERO);
	if (dc == NULL) {
		apfs_free_mount(am);
		return ENOMEM;
	}
	dc->dc_devvp = devvp;
	dc->dc_devblock = devblock;
	dc->dc_base = base;
	{
		uint8_t *sb = kalloc_data(APFS_META_CHUNK, Z_WAITOK);
		uint64_t devsize = devblocks * (uint64_t)devblock;
		uint32_t i;

		error = (sb == NULL) ? ENOMEM : 0;
		for (i = 0; error == 0; i++) {
			uint32_t bs;
			uint64_t bc;

			error = apfs_dev_read(dc, 0, sb, APFS_META_CHUNK);
			if (error == 0 && memcmp(sb + 32, "NXSB", 4) != 0) {
				apfs_klog("apfs_mount_common: i=%u base=%llu magic mismatch: %.4s (0x%08x), bytes 0..16: %08x %08x %08x %08x\n",
				    i, (unsigned long long)dc->dc_base, (char *)sb + 32, *(uint32_t *)(sb + 32),
				    *(uint32_t *)(sb + 0), *(uint32_t *)(sb + 4), *(uint32_t *)(sb + 8), *(uint32_t *)(sb + 12));
				error = EINVAL;         /* (no container here: as the root, the next filesystem's turn) */
			}
			if (error) {
				apfs_klog("apfs_mount_common: dev_read error=%d at base=%llu\n", error, (unsigned long long)dc->dc_base);
			}
			if (error) {
				break;
			}
			bs = (uint32_t)sb[36] | (uint32_t)sb[37] << 8 | (uint32_t)sb[38] << 16 | (uint32_t)sb[39] << 24;
			bc = 0;
			for (int b = 7; b >= 0; b--) {
				bc = (bc << 8) | sb[40 + b];
			}
			nx_size = bc * bs;
			if (bs < 4096 || bc == 0 || (nx_size % devblock) != 0 ||
			    (devsize != 0 && dc->dc_base + nx_size > devsize)) {
				apfs_klog("apfs: container %u: %llu blocks of %u do not fit the device (devsize=%llu)\n", i,
				    (unsigned long long)bc, bs, (unsigned long long)devsize);
				error = EINVAL;
				break;
			}
			if (i == container) {
				break;
			}
			dc->dc_base += nx_size;
		}
		if (sb != NULL) {
			kfree_data(sb, APFS_META_CHUNK);
		}
		if (error) {
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return error;
		}
	}

	/*
	 * Writes are opt-in: without apfs_rw=1 the device takes none, and apfs_lib answers
	 * EROFS exactly as it always has.  The mount stays read-only then, too.
	 */
	(void)PE_parse_boot_argn("apfs_rw", &rw, sizeof(rw));
	bzero(&dev, sizeof(dev));
	dev.ctx = dc;
	dev.size = nx_size;
	dev.read = apfs_dev_read;
	dev.read_data = apfs_dev_read_data;
	if (rw != 0) {
		dev.write = apfs_dev_write;
	}

	error = apfs_nx_open(&am->am_nx, &dev);
	if (error == 0) {
		/* (after the container is open: that is where its own fields are made) */
		uint32_t verify = 1;

		(void)PE_parse_boot_argn("apfs_verify", &verify, sizeof(verify));
		am->am_nx.write_verify = (verify != 0);
		if (!am->am_nx.write_verify) {
			apfs_klog("apfs: the blocks of a commit are not read back (apfs_verify=0)\n");
		}
	}
	if (error) {
		apfs_klog("apfs_mount_common: apfs_nx_open returned %d\n", error);
		if (error != EINVAL) {
			apfs_klog("apfs: cannot read the container: %d\n", error);
		}
		kfree_type(struct apfs_devctx, dc);
		apfs_free_mount(am);
		return error;
	}

	/* apfs_nx_open() starts from a clean slate, so the lock is handed over only now */
	am->am_nx.unlock = apfs_lib_unlock;
	am->am_nx.relock = apfs_lib_relock;
	am->am_nx.lock_ctx = am;
	am->am_iobuf_size = APFS_IOBUF_DEFAULT;
	{
		uint32_t round = 0;

		if (PE_parse_boot_argn("apfs_round", &round, sizeof(round)) &&
		    round >= 64 * 1024 && round <= APFS_IOBUF_MAX && (round % (64 * 1024)) == 0) {
			am->am_iobuf_size = round;
		}
	}
	(void)PE_parse_boot_argn("apfs_meta_cluster", &am->am_nx.meta_cluster,
	    sizeof(am->am_nx.meta_cluster));
	/*
	 * The phone's flash and QEMU's virtio disk cost tens of microseconds a request; this
	 * port's other disks (md, the resident image; rsp, part of an image) answer with a
	 * memory copy.  Boot-arg apfs_cheap=0/1 says otherwise.
	 */
	{
		dev_t rdev = vnode_specrdev(devvp);
		uint32_t cheap;

		am->am_nx.cheap_requests = !hakios_ufs_dev(rdev) && !hakios_virtio_blk_dev(rdev);
		if (PE_parse_boot_argn("apfs_cheap", &cheap, sizeof(cheap))) {
			am->am_nx.cheap_requests = (cheap != 0);
		}
		printf("apfs: device %d,%d: requests to it %s\n", major(rdev), minor(rdev),
		    am->am_nx.cheap_requests ? "cost next to nothing (a disk in memory)" :
		    "are worth putting together");
	}
	apfs_pool_start();
	if (apfs_pool_threads > 0) {
		am->am_nx.parallel = apfs_parallel;
		am->am_nx.parallel_max = apfs_pool_threads + 1;
	}

	/* the first volume that has a root directory */
	{
		uint32_t i;
		bool got = false;

		/* with the lock, as every call into apfs_lib from now on (it may let go of it) */
		apfs_lock(am);
		for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS && !got; i++) {
			struct apfs_inode ino;

			if (am->am_nx.fs_oid[i] == 0 || (volume >= 0 && i != (uint32_t)volume)) {
				continue;
			}
			if (apfs_vol_open(&am->am_nx, i, &am->am_vol) != 0) {
				continue;
			}
			if (apfs_inode_get(&am->am_vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
				got = true;
			}
		}
		apfs_unlock(am);
		if (!got) {
			apfs_klog("apfs: no readable volume with a root directory\n");
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return EINVAL;
		}
	}
	am->am_root_id = APFS_ROOT_DIR_INO_NUM;

	{
		const char *why = NULL;

		/*
		 * The caller may have asked for read-only (stage0 mounts the RAM disk's volume
		 * so); that is honoured.  Otherwise the one answer: the device, the seal, the
		 * snapshots, the role.
		 */
		/*
		 * The vfs entry of this filesystem says MNT_RDONLY, so a mount(2) starts with
		 * it set and VFS marks MNTK_WANTRDWR instead when the caller did not ask for
		 * read-only.  The root (apfs_mountroot) has no caller: it cleared the flag.
		 */
		writable = ((vfs_flags(mp) & MNT_RDONLY) == 0 ||
		    (mp->mnt_kern_flag & MNTK_WANTRDWR) != 0) &&
		    apfs_write_volume_ok(&am->am_vol, &why) == 0;
		snprintf(apfs_mount_why, sizeof(apfs_mount_why),
		    "volume \"%s\" base %llu rw %d write %d flags 0x%x kern 0x%x role 0x%x "
		    "writable %d: %s", am->am_vol.name, (unsigned long long)(base >> 20), rw,
		    dev.write != NULL, (unsigned)vfs_flags(mp),
		    (unsigned)(mp->mnt_kern_flag & MNTK_WANTRDWR), am->am_vol.role, (int)writable,
		    why != NULL ? why : "-");
		printf("apfs: mount %s\n", apfs_mount_why);
		if (dev.write != NULL && !writable) {
			printf("apfs: volume %u stays read-only: %s\n", am->am_vol.index,
			    why != NULL ? why : "mounted read-only by the caller");
		}
	}

	vfs_setfsprivate(mp, am);
	vfs_setflags(mp, MNT_LOCAL);
	if (writable) {
		vfs_clearflags(mp, MNT_RDONLY);
	} else {
		vfs_setflags(mp, MNT_RDONLY);
	}
	{
		size_t at = strlen(apfs_mount_why);

		snprintf(apfs_mount_why + (at < sizeof(apfs_mount_why) ? at : 0),
		    sizeof(apfs_mount_why) - (at < sizeof(apfs_mount_why) ? at : 0),
		    "; after: flags 0x%x", (unsigned)vfs_flags(mp));
	}
	vfs_setlocklocal(mp);

	sp = vfs_statfs(mp);
	sp->f_bsize = am->am_nx.block_size;
	sp->f_iosize = am->am_nx.block_size;
	sp->f_blocks = am->am_nx.block_count;
	sp->f_bfree = 0;
	sp->f_bavail = 0;
	if (writable) {
		struct apfs_spaceman sm;

		/*
		 * apfs_lib reads the checkpoint map and the manager itself here, and a block
		 * missing from its cache is read with the mount's lock let go of and taken back
		 * (nx_block_unlocked).  This is after the volume was opened, so take the lock as
		 * every other call into apfs_lib does: without it nx_unlock() finds the mutex
		 * owned by nobody.
		 */
		apfs_lock(am);
		if (apfs_spaceman_open(&am->am_nx, &sm) == 0) {
			/*
			 * In blocks, not bytes: f_bsize says what a block is, and f_blocks is already a
			 * count of them.  Written in bytes the free space came out 4096 times the size of
			 * the volume, and iOS -- which multiplies by f_bsize itself -- showed a 52 GB
			 * volume with 0 KB available.
			 */
			sp->f_bfree = sm.free_count;
			sp->f_bavail = sp->f_bfree;
			apfs_spaceman_close(&sm);
		}
		apfs_unlock(am);
	}
	sp->f_bused = am->am_vol.alloc_count;
	sp->f_files = am->am_vol.num_files + am->am_vol.num_dirs;
	sp->f_ffree = 1000000;   /* the real thing reports free inodes; 0 made file-saving clients think the volume was full */
	/* (several volumes of one device are told apart by which container and volume) */
	sp->f_fsid.val[0] = (int32_t)vnode_specrdev(devvp);
	sp->f_fsid.val[1] = (int32_t)((vfs_typenum(mp) & 0xff) | (container << 8) | (am->am_vol.index << 20));
	strlcpy(sp->f_fstypename, "apfs", sizeof(sp->f_fstypename));

	apfs_the_mount = am;
	am->am_rdev = vnode_specrdev(devvp);
	lck_mtx_lock(&apfs_pool_lock);
	am->am_next = apfs_mounts;
	apfs_mounts = am;
	/*
	 * The volume group: the biggest container of the volumes mounted here, the writable one
	 * preferred -- the data volume.  A read-only volume of the group reports that container's
	 * size and free space (apfs_vfs_getattr), as "/" does on an iPhone, where the system and
	 * the data volume are two volumes of one container.  Here they are two containers, and in
	 * a RAM boot they are not even on one device (the system image is md0), hence no test on
	 * the device: whatever the volumes share is the space iOS can put applications in.
	 */
	{
		struct apfs_mount *m, *big = NULL;

		for (m = apfs_mounts; m != NULL; m = m->am_next) {
			if (!vfs_isrdonly(m->am_mp) &&
			    (big == NULL || m->am_nx.block_count > big->am_nx.block_count)) {
				big = m;
			}
		}
		if (big == NULL) {
			for (m = apfs_mounts; m != NULL; m = m->am_next) {
				if (big == NULL || m->am_nx.block_count > big->am_nx.block_count) {
					big = m;
				}
			}
		}
		for (m = apfs_mounts; m != NULL; m = m->am_next) {
			m->am_group = big;
		}
	}
	lck_mtx_unlock(&apfs_pool_lock);
	if (writable) {
		printf("apfs: volume %u mounted read-write, %llu MB free\n", am->am_vol.index,
		    (unsigned long long)(sp->f_bfree >> 20));
	}
	printf("apfs: volume %u \"%s\" of container %u (at %llu MB): %llu files, %llu directories, %llu blocks of %u used\n",
	    am->am_vol.index, am->am_vol.name, container, (unsigned long long)(dc->dc_base >> 20), (unsigned long long)am->am_vol.num_files,
	    (unsigned long long)am->am_vol.num_dirs,
	    (unsigned long long)am->am_vol.alloc_count, am->am_nx.block_size);
	return 0;
}

/*
 * As the root filesystem: EINVAL lets vfs_mountroot() try the next quietly.  Boot-args
 * apfs_root_c=N and apfs_root_v=M say which container of the device and which volume of it
 * (default: the first container, its first volume with a root directory).
 */
int
apfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
	uint32_t c = 0;
	int v = -1;

	(void)PE_parse_boot_argn("apfs_root_c", &c, sizeof(c));
	(void)PE_parse_boot_argn("apfs_root_v", &v, sizeof(v));
	/*
	 * The root stays read-only whatever apfs_rw says: the volume it is mounted from is
	 * iOS's own (or a copy of it), and the one this port writes is mounted over
	 * /private/var later.  apfs_mount_common sees MNT_RDONLY and keeps it.
	 */
	return apfs_mount_common(mp, devvp, ctx, 0, c, v);
}

/*
 * mount(2), and kernel_mount().  The mount data is the path of the block device holding
 * the container, as a string.  VFS does not resolve it for us the way it does for a
 * filesystem marked VFC_VFSLOCALARGS -- see the note in vfs_conf.c for why this one is
 * not -- so it is looked up and opened here, and closed again in apfs_unmount().
 */
static int
apfs_mount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	char *path;
	vnode_t vp = NULLVP;
	size_t len = 0;
	uint64_t base = 0;
	uint32_t container = 0;
	int volume = -1;
	int error;

	if (vfs_isupdate(mp)) {
		/* nothing about a mounted volume can change, least of all to read-write */
		return vfs_iswriteupgrade(mp) ? EROFS : 0;
	}
	if (devvp != NULLVP) {
		/* mounted as the root: VFS found the device and owns the vnode */
		return apfs_mountroot(mp, devvp, ctx);
	}
	if (data == USER_ADDR_NULL) {
		apfs_klog("apfs_mount: data is NULL!\n");
		return EINVAL;
	}
	path = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (path == NULL) {
		return ENOMEM;
	}
	if (vfs_iskernelmount(mp)) {
		/* kernel_mount() hands the pointer through untouched */
		error = (strlcpy(path, CAST_DOWN(char *, data), MAXPATHLEN) < MAXPATHLEN)
		    ? 0 : ENAMETOOLONG;
	} else {
		error = copyinstr(data, path, MAXPATHLEN, &len);
	}
	apfs_klog("apfs_mount: copied path='%s' error=%d len=%zu kernelmount=%d\n",
	    path, error, len, (int)vfs_iskernelmount(mp));
	/* "DEVICE[,c=N][,v=M][,b=MB]": the container (from 0), the volume (by index), and where
	 * the first container starts on the device (the phone's data volume, 8 GiB into userdata) */
	if (error == 0) {
		char *opt = strchr(path, ',');

		while (opt != NULL) {
			char *next = strchr(opt + 1, ',');

			*opt = '\0';
			if (strncmp(opt + 1, "c=", 2) == 0) {
				container = (uint32_t)strtoul(opt + 3, NULL, 10);
			} else if (strncmp(opt + 1, "v=", 2) == 0) {
				volume = (int)strtoul(opt + 3, NULL, 10);
			} else if (strncmp(opt + 1, "b=", 2) == 0) {
				base = (uint64_t)strtoul(opt + 3, NULL, 10) << 20;
			} else {
				error = EINVAL;
			}
			opt = next;
		}
	}
	if (error == 0) {
		error = vnode_lookup(path, 0, &vp, ctx);
		apfs_klog("apfs_mount: vnode_lookup %s -> error=%d vp=%p isblk=%d\n",
		    path, error, vp, vp ? vnode_isblk(vp) : 0);
	}
	if (error == 0 && !vnode_isblk(vp)) {
		error = ENOTBLK;
	}
	if (error == 0) {
		bool shared = false;

		apfs_klog("apfs_mount: path=%s base=%llu container=%u volume=%d\n",
		    path, (unsigned long long)base, container, volume);
		error = VNOP_OPEN(vp, FREAD, ctx);
		if (error == EBUSY && apfs_device_held(vnode_specrdev(vp))) {
			/*
			 * Mounted already -- by us, another container of it (the root's RAM disk and the
			 * cryptex after the boot volume): read through it as it is open, not opened
			 * again.  Anything else mounted on a device stays EBUSY.
			 */
			shared = true;
			error = 0;
		}
		apfs_klog("apfs_mount: VNOP_OPEN error=%d shared=%d rdev=0x%x held=%d\n",
		    error, (int)shared, (unsigned)vnode_specrdev(vp), (int)apfs_device_held(vnode_specrdev(vp)));
		if (error == 0) {
			/*
			 * The iocount from the lookup is kept for as long as the volume is
			 * mounted; apfs_unmount() closes and drops it.
			 */
			error = apfs_mount_common(mp, vp, ctx, base, container, volume);
			apfs_klog("apfs_mount: apfs_mount_common returned %d\n", error);
			if (error == 0) {
				APFS_MP(mp)->am_owns_devvp = true;
				APFS_MP(mp)->am_shares_devvp = shared;
				/* what mount(8) and df show as where it came from */
				if (vfs_statfs(mp)->f_mntfromname[0] == '\0' || container != 0 || volume >= 0) {
					char opts[40] = "";

					if (container != 0) {
						snprintf(opts, sizeof(opts), ",c=%u", container);
					}
					if (volume >= 0) {
						snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), ",v=%d", volume);
					}
					snprintf(vfs_statfs(mp)->f_mntfromname, MAXPATHLEN, "%s%s", path, opts);
				}
			} else if (!shared) {
				(void)VNOP_CLOSE(vp, FREAD, ctx);
			}
		}
		if (error != 0) {
			vnode_put(vp);
		}
	} else if (vp != NULLVP) {
		vnode_put(vp);
	}
	kfree_data(path, MAXPATHLEN);
	return error;
}

static int
apfs_unmount(mount_t mp, int mntflags, vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	int error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);

	if (error) {
		return error;
	}
	if (apfs_the_mount == am) {
		apfs_the_mount = NULL;
	}
	lck_mtx_lock(&apfs_pool_lock);
	for (struct apfs_mount **pp = &apfs_mounts; *pp != NULL; pp = &(*pp)->am_next) {
		if (*pp == am) {
			*pp = am->am_next;
			break;
		}
	}
	/* (the group it was in: whatever is left of it reports its own size again) */
	for (struct apfs_mount *m = apfs_mounts; m != NULL; m = m->am_next) {
		if (m->am_group == am) {
			m->am_group = m;
		}
	}
	lck_mtx_unlock(&apfs_pool_lock);
	vfs_setfsprivate(mp, NULL);
	if (am->am_owns_devvp && am->am_devvp != NULLVP) {
		if (!am->am_shares_devvp) {
			(void)VNOP_CLOSE(am->am_devvp, FREAD, ctx);
		}
		vnode_put(am->am_devvp);
	}
	apfs_free_mount(am);
	return 0;
}

static int
apfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);

	return apfs_node_vnode(am, am->am_root_id, NULLVP, NULL, vpp);
}

static int
apfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	return apfs_node_vnode(APFS_MP(mp), (uint64_t)ino, NULLVP, NULL, vpp);
}

static int
apfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	uint64_t files = am->am_vol.num_files + am->am_vol.num_dirs;

	VFSATTR_RETURN(fsap, f_objcount, files);
	/*
	 * The free space, for the volume attributes as well as for statfs(2): the volume says
	 * VOL_CAP_FMT_FAST_STATFS, so iOS asks here (getattrlist) rather than through statfs, and
	 * zeros here are what put "iPhone Storage Full" on a volume with fifty gigabytes free.
	 * The space manager is read at every ask: it is Settings and the store's build that ask,
	 * not every file call.
	 */
	{
		uint64_t free_blocks = 0;
		uint64_t blocks = am->am_nx.block_count;
		uint64_t used = am->am_vol.alloc_count;

		if (!vfs_isrdonly(mp)) {
			struct apfs_spaceman sm;

			apfs_lock(am);
			if (apfs_spaceman_open(&am->am_nx, &sm) == 0) {
				free_blocks = sm.free_count;
				apfs_spaceman_close(&sm);
			}
			apfs_unlock(am);
		}
		/*
		 * The volume group (am_group, set at mount): on an iPhone the system and the data
		 * volume are two volumes of one container, and "/" reports that container's size and
		 * free space.  Here they are two containers of one device; without this, iOS took the
		 * 2.5 GB of the system image for the whole of "iPhone Storage" -- 3 GB with 2.6 used --
		 * and zero free, which is "Storage Full" on a device with fifty gigabytes free.  The
		 * group's own space manager is read here, under its lock; a read-only volume holds
		 * nothing that it then asks for.
		 */
		if (am->am_group != NULL && am->am_group != am) {
			struct apfs_mount *g = am->am_group;
			struct apfs_spaceman sm;

			blocks = g->am_nx.block_count;
			used = g->am_vol.alloc_count;
			apfs_lock(g);
			if (apfs_spaceman_open(&g->am_nx, &sm) == 0) {
				free_blocks = sm.free_count;
				apfs_spaceman_close(&sm);
			}
			apfs_unlock(g);
		}
		VFSATTR_RETURN(fsap, f_bsize, am->am_nx.block_size);
		VFSATTR_RETURN(fsap, f_iosize, am->am_nx.block_size);
		VFSATTR_RETURN(fsap, f_blocks, blocks);
		VFSATTR_RETURN(fsap, f_bfree, free_blocks);
		VFSATTR_RETURN(fsap, f_bavail, free_blocks);
		VFSATTR_RETURN(fsap, f_bused, used);
		VFSATTR_RETURN(fsap, f_files, files);
		VFSATTR_RETURN(fsap, f_ffree, free_blocks);
		/* (kept in the mount's own statfs too: df and statfs(2) read that, not this) */
		{
			struct vfsstatfs *sp = vfs_statfs(mp);

			if (sp != NULL) {
				sp->f_blocks = blocks;
				sp->f_bfree = free_blocks;
				sp->f_bavail = free_blocks;
			}
		}
	}

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;
		bool nocase = (am->am_vol.incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;

		bzero(cap, sizeof(*cap));
		/*
		 * An object's identifier never changes and apfs_vget() finds an object by it,
		 * which is what lets fsgetpath(2) and /.vol work for this volume: without it,
		 * what asks for a file's path by its identifier (LaunchServices among them) gets
		 * nothing.
		 */
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_PATH_FROM_ID |
		    (nocase ? 0 : VOL_CAP_FMT_CASE_SENSITIVE);
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_HIDDEN_FILES |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, am->am_vol.name, MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		memcpy(fsap->f_uuid, am->am_nx.uuid, sizeof(fsap->f_uuid));
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	return 0;
}

/* Nothing is ever dirty. */
static int
apfs_sync(__unused mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	return 0;
}

static int
apfs_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops apfs_vfsops = {
	.vfs_mount = apfs_mount,
	.vfs_unmount = apfs_unmount,
	.vfs_root = apfs_root,
	.vfs_getattr = apfs_vfs_getattr,
	.vfs_sync = apfs_sync,
	.vfs_vget = apfs_vget,
	.vfs_init = apfs_init,
};
