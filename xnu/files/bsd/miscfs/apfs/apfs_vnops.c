/*
 * apfs vnode operations.  Read only; see apfs.h.
 *
 * Every call into apfs_lib is made under the mount's lock, because apfs_lib hands out
 * pointers into its own block cache and its cursors carry no locking.
 */

#include <kern/kalloc.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <miscfs/apfs/apfs.h>
#include <sys/attr.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/lockf.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/mount_internal.h>
#include <sys/unistd.h>
#include <sys/vnode_internal.h>
#include <sys/xattr.h>
#include <kern/clock.h>
#include <miscfs/apfs/apfs_write.h>
#include <os/atomic_private.h>
#include <vfs/vfs_support.h>

/*
 * A name with its terminator, as apfs_lib wants it.  apfs_fs_write.c's own limit -- 1024,
 * the room its keys are made with -- not the 255 of a path component: namei has refused
 * anything longer before this is reached.
 */
#define APFS_NAME_BUF 1024

/* The wall clock, in nanoseconds since the Unix epoch: what an inode records. */
static uint64_t
apfs_now(void)
{
	uint64_t sec = 0, nsec = 0;

	clock_get_calendar_nanotime(&sec, &nsec);
	return sec * 1000000000ULL + nsec;
}

/*
 * Is this volume one this driver may change, and was it mounted for writing?  apfs_write.c
 * has the first answer; the second is the caller's (stage0 mounts the RAM disk's volume
 * read-only on purpose, and VFS does not stand in the way of every operation).
 */

static bool
apfs_writable(struct apfs_mount *am)
{
	const char *why = NULL;
	bool asked = (vfs_flags(am->am_mp) & MNT_RDONLY) == 0 ||
	    (am->am_mp->mnt_kern_flag & MNTK_WANTRDWR) != 0;
	bool ok = asked && apfs_write_volume_ok(&am->am_vol, &why) == 0;

	return ok;
}

/* Take the inode and the size of a vnode again: after an operation changed them. */
static void
apfs_node_refresh(struct apfs_mount *am, struct apfs_node *an)
{
	struct apfs_inode ino;
	uint64_t size;

	apfs_lock(am);
	if (apfs_inode_get(&am->am_vol, an->an_id, &ino) == 0) {
		size = an->an_size;
		if (apfs_inode_size(&am->am_vol, &ino, &size) != 0) {
			size = ino.size;
		}
		an->an_ino = ino;
		an->an_size = size;
		an->an_mode = ino.mode;
		an->an_parent = ino.parent_id;
	} else {
		/*
		 * No inode: the last name went and the file was not open, so it is gone.  The
		 * vnode that named it still answers fstat until it is reclaimed, and its link
		 * count is none.
		 */
		an->an_ino.nchildren = 0;
	}
	apfs_unlock(am);
}

/* A timespec, as an inode keeps a time: nanoseconds since the Unix epoch. */
static uint64_t
apfs_ts_ns(struct timespec ts)
{
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* How much is read out of a file at a time when copying into a uio. */

extern integer_t thread_kern_get_pri(thread_t thr);    /* the thread's base priority */
extern void thread_runnable_and_run_self(uint64_t *runnable, uint64_t *run);

/* The statistics of the calling process (with the lock held) */
static struct apfs_lock_proc *
lock_proc(struct apfs_mount *am)
{
	static struct apfs_lock_proc nowhere;   /* (no table: counted, and never shown) */
	struct apfs_lock_proc *lp;
	char name[MAXCOMLEN + 1];
	uint32_t i;

	if (am->am_procs == NULL) {
		return &nowhere;
	}
	proc_selfname(name, sizeof(name));
	lck_spin_lock(&am->am_procs_spin);
	for (i = 0; i < am->am_nprocs; i++) {
		if (strncmp(am->am_procs[i].name, name, sizeof(name)) == 0) {
			lp = &am->am_procs[i];
			goto out;
		}
	}
	if (am->am_nprocs < APFS_LOCK_PROCS - 1) {
		strlcpy(am->am_procs[am->am_nprocs].name, name, sizeof(name));
		lp = &am->am_procs[am->am_nprocs++];
		goto out;
	}
	if (am->am_nprocs == APFS_LOCK_PROCS - 1) {
		strlcpy(am->am_procs[am->am_nprocs].name, "(the rest)", sizeof(name));
		am->am_nprocs++;
	}
	lp = &am->am_procs[APFS_LOCK_PROCS - 1];
out:
	lck_spin_unlock(&am->am_procs_spin);
	return lp;
}

/* The statistics of a caller of apfs_lock (with the lock held) */
static struct apfs_lock_site *
lock_site(struct apfs_mount *am, const char *name)
{
	uint32_t i;

	for (i = 0; i < APFS_LOCK_SITES && am->am_sites[i].name != NULL; i++) {
		if (am->am_sites[i].name == name) {
			return &am->am_sites[i];
		}
	}
	if (i == APFS_LOCK_SITES) {
		i = APFS_LOCK_SITES - 1;        /* (more callers than kept: the last one takes the rest) */
	} else {
		am->am_sites[i].name = name;
	}
	return &am->am_sites[i];
}

/* The lock taken, by site: how long it was waited for, how many others were waiting */
static void
lock_take(struct apfs_mount *am, const char *site, bool relock)
{
	static uint64_t limits[4];
	uint64_t t0 = apfs_ticks(), waited, long_wait, runnable0, run0;
	uint32_t others = os_atomic_inc_orig(&am->am_lock_waiting, relaxed);
	struct apfs_lock_site *ls;
	unsigned k;

	if (limits[3] == 0) {
		nanoseconds_to_absolutetime(2000, &limits[0]);
		nanoseconds_to_absolutetime(10000, &limits[1]);
		nanoseconds_to_absolutetime(100000, &limits[2]);
		nanoseconds_to_absolutetime(1000000, &limits[3]);
	}
	thread_runnable_and_run_self(&runnable0, &run0);
	lck_mtx_lock(&am->am_lock);
	os_atomic_dec(&am->am_lock_waiting, relaxed);
	waited = apfs_ticks() - t0;
	long_wait = limits[3];
	/* (the holder's own: no atomics needed) */
	am->am_lock_wait_ticks += waited;
	am->am_lock_waits++;
	for (k = 0; k < 4 && waited >= limits[k]; k++) {
	}
	am->am_lock_wait_hist[k]++;
	if (waited > am->am_lock_wait_max) {
		am->am_lock_wait_max = waited;
	}
	am->am_waiters_hist[others == 0 ? 0 : others == 1 ? 1 : others <= 3 ? 2 : others <= 7 ? 3 : 4]++;
	{
		int pri = thread_kern_get_pri(current_thread());
		unsigned b = pri <= 4 ? 0 : pri <= 20 ? 1 : pri <= 31 ? 2 : pri <= 46 ? 3 : 4;

		am->am_prio_wait[b] += waited;
		if (waited >= limits[3]) {
			uint64_t runnable, run;

			am->am_prio_long[b]++;
			am->am_prio_long_wait[b] += waited;
			/* where the time went: runnable includes running, and the rest was blocked */
			thread_runnable_and_run_self(&runnable, &run);
			runnable -= runnable0;
			run -= run0;
			am->am_long_blocked[b] += waited > runnable ? waited - runnable : 0;
			am->am_long_queued[b] += runnable > run ? runnable - run : 0;
			am->am_long_oncpu[b] += run;
			am->am_long_cpu[cpu_number() & 7]++;
		}
	}
	if (waited >= limits[1]) {
		struct apfs_lock_proc *lp = lock_proc(am);

		lp->waits++;
		lp->wait += waited;
		if (waited > lp->wait_max) {
			lp->wait_max = waited;
		}
		lp->wait_long += waited >= limits[3];
	}
	if (relock) {
		/* apfs_lib taking it back in the middle of an operation: the operation's */
		thread_t self = current_thread();
		uint32_t i;

		site = "apfs_lib";
		for (i = 0; i < am->am_nops; i++) {
			if (am->am_ops[i].th == self) {
				site = am->am_ops[i].site;
				break;
			}
		}
	}
	ls = lock_site(am, site);
	if (relock) {
		ls->relocks++;
	} else {
		ls->takes++;
	}
	ls->wait += waited;
	if (waited > ls->wait_max) {
		ls->wait_max = waited;
	}
	ls->wait_long += waited >= long_wait;
	am->am_lock_site = site;
	am->am_lock_t0 = apfs_ticks();
}

void
apfs_unlock_inner(struct apfs_mount *am)
{
	static uint64_t limits[4];
	uint64_t held = apfs_ticks() - am->am_lock_t0;
	struct apfs_lock_site *ls;
	unsigned k;

	if (limits[3] == 0) {
		nanoseconds_to_absolutetime(10000, &limits[0]);
		nanoseconds_to_absolutetime(100000, &limits[1]);
		nanoseconds_to_absolutetime(1000000, &limits[2]);
		nanoseconds_to_absolutetime(10000000, &limits[3]);
	}
	am->am_lock_held_ticks += held;
	for (k = 0; k < 4 && held >= limits[k]; k++) {
	}
	am->am_lock_hold_hist[k]++;
	if (held > am->am_lock_hold_max) {
		am->am_lock_hold_max = held;
	}
	if (am->am_lock_site != NULL) {
		ls = lock_site(am, am->am_lock_site);
		ls->hold += held;
		if (held > ls->hold_max) {
			ls->hold_max = held;
		}
		ls->hold_long += held >= limits[2];
	}
	lck_mtx_unlock(&am->am_lock);
}

/* An operation begins: the lock, and the epoch it began at noted */
void
apfs_lock_at(struct apfs_mount *am, const char *site)
{
	lock_take(am, site, false);
	if (am->am_ops != NULL && am->am_nops < APFS_OPS_MAX) {
		struct apfs_op *op = &am->am_ops[am->am_nops];

		op->th = current_thread();
		op->epoch = am->am_nx.epoch;
		op->site = site;
		op->start = apfs_ticks();
		proc_selfname(op->name, sizeof(op->name));
		am->am_nops++;
	} else {
		am->am_untracked++;
	}
}

/* An operation ends: what nobody still going can be reading is freed, and the lock let go of */
void
apfs_unlock(struct apfs_mount *am)
{
	thread_t self = current_thread();
	uint64_t oldest = am->am_nx.epoch;
	uint32_t i;
	bool found = false;

	for (i = 0; i < am->am_nops; i++) {
		if (!found && am->am_ops[i].th == self) {
			struct apfs_op *op = &am->am_ops[i];
			uint64_t age = apfs_ticks() - op->start;

			/*
			 * An operation that took long is written down as it ends: the watchdog above
			 * looks every ten seconds and misses most of them, while a commit of a cache
			 * file holds the writer's mutex for hundreds of milliseconds and everything
			 * else writes waits for it.
			 */
			if (age > 0 && apfs_wd_slow_ns != 0) {
				uint64_t ns = 0;

				absolutetime_to_nanoseconds(age, &ns);
				if (ns >= apfs_wd_slow_ns) {
					apfs_log("op %s by %s took %llu ms\n", op->site, op->name,
					    (unsigned long long)(ns / 1000000));
				}
			}
			am->am_ops[i] = am->am_ops[--am->am_nops];
			found = true;
			if (i == am->am_nops) {
				break;
			}
		}
		if (am->am_ops[i].epoch < oldest) {
			oldest = am->am_ops[i].epoch;
		}
	}
	if (!found && am->am_untracked > 0) {
		am->am_untracked--;
	}
	if (am->am_untracked == 0 && am->am_nx.retired != NULL) {
		apfs_nx_reclaim(&am->am_nx, oldest);
	}
	apfs_unlock_inner(am);
}

/* The inode record of the object a vnode stands for. */
static int
apfs_vp_inode(vnode_t vp, struct apfs_inode *ino)
{
	struct apfs_node *an = APFS_VP(vp);

	return apfs_inode_get(&an->an_mount->am_vol, an->an_id, ino);
}

/* ------------------------------------------------------------------ lookup */

static int
apfs_lookup_impl(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	uint64_t id = 0;
	uint8_t dt = 0;
	int error;

	*ap->a_vpp = NULLVP;

	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (error == 0) {
			*ap->a_vpp = dvp;
		}
		return error;
	}
	if (cnp->cn_flags & ISDOTDOT) {
		return apfs_node_vnode(am, dn->an_parent, NULLVP, NULL, ap->a_vpp);
	}

	/*
	 * The name cache first, as a file system's lookup is to ask it (namei's own quick path
	 * leaves the entries for names that are not there to us): -1, the vnode, with an iocount;
	 * ENOENT, known not to be there (for a CREATE or RENAME it says 0 and forgets the entry).
	 */
	error = cache_lookup(dvp, ap->a_vpp, cnp);
	if (error == -1) {
		os_atomic_inc(&am->am_lookup_positive_hits, relaxed);
		return 0;
	}
	if (error == ENOENT) {
		os_atomic_inc(&am->am_lookup_negative_hits, relaxed);
		/*
		 * "It is not there" is the answer for a lookup; for a name that is to be made it
		 * is the answer to go on and make it -- the same answer the volume's own ENOENT
		 * gets below.  A negative entry in the name cache must not turn a create into a
		 * plain ENOENT.
		 */
		if ((cnp->cn_flags & ISLASTCN) &&
		    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
			return apfs_writable(am) ? EJUSTRETURN : EROFS;
		}
		return ENOENT;
	}

	apfs_lock(am);
	if (dn->an_id >= APFS_SYNTH_BASE) {
		error = ENOENT;                 /* nothing of a synthetic directory is on the volume */
	} else {
		error = apfs_lookup(&am->am_vol, dn->an_id, cnp->cn_nameptr,
		    (size_t)cnp->cn_namelen, &id, &dt);
	}
	if (error == ENOENT && am->am_nsynth > 0) {
		const struct apfs_synth *sy = apfs_synth_find(am, dn->an_id, cnp->cn_nameptr,
		    (size_t)cnp->cn_namelen);

		if (sy != NULL) {
			id = sy->id;
			error = 0;
		}
	}
	apfs_unlock(am);
	/*
	 * What is there is found whatever the caller means to do with it -- VFS refuses the
	 * change itself on a read-only mount, and mkdir of a directory that exists is to fail
	 * with EEXIST, not EROFS (xpcproxy gives up on the latter).  Only a name that is not
	 * there and is to be made is refused here.
	 */
	if (error == ENOENT && (cnp->cn_flags & ISLASTCN) &&
	    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
		/*
		 * namei wants EJUSTRETURN, not ENOENT, from the lookup of the last
		 * component of a name that is to be created: it is the "nothing there,
		 * go on and make it" answer.  On a volume this driver may not write,
		 * the answer is the refusal instead (and mkdir of something that does
		 * exist has failed with EEXIST already, before this).
		 */
		return apfs_writable(am) ? EJUSTRETURN : EROFS;
	}
	/*
	 * A name that is not there stays not there (the volume cannot change; a synthetic
	 * directory added later purges its parent's entries): the name cache says so from now on,
	 * instead of asking again -- iOS asks for missing files thousands of times after a boot.
	 */
	if (error == ENOENT && cnp->cn_nameiop == LOOKUP && (cnp->cn_flags & MAKEENTRY) &&
	    !apfs_writable(am)) {
		/*
		 * Only where the volume cannot change.  On the data volume a name another
		 * process made a moment ago would stay "not there" here, and the create that
		 * follows would come back EEXIST.
		 */
		cache_enter(dvp, NULLVP, cnp);
		os_atomic_inc(&am->am_lookup_negative, relaxed);
	}
	if (error) {
		return error;
	}
	return apfs_node_vnode(am, id, dvp, cnp, ap->a_vpp);
}

/*
 * The operations that change the volume take the writer's mutex around the whole
 * operation: the mount's own lock is let go of around device reads, and a second
 * writer entering there would begin a transaction from the same checkpoint.  The
 * same thread may take it again (a create undoing itself, a write reaching pageout).
 */
static void
apfs_lock_write(struct apfs_mount *am)
{
	thread_t self = current_thread();
	uint64_t t0;

	if (am->am_wlock_depth != 0 && am->am_wlock_owner == self) {
		am->am_wlock_depth++;
		apfs_lock(am);
		return;
	}
	t0 = apfs_ticks();
	lck_mtx_lock(&am->am_wlock);
	/*
	 * The writer's mutex is held across a whole operation -- a transaction, its commit, the
	 * blocks on their way to the flash -- so every other writer waits for the longest one.
	 * A wait of a quarter of a second is written down with the name of the process that had
	 * it: p392 had SpringBoard's unlink() wait 4.1 s, and this says who it waited for.
	 */
	{
		uint64_t waited = apfs_ticks() - t0, ns = 0;
		struct apfs_lock_proc *lp = lock_proc(am);

		absolutetime_to_nanoseconds(waited, &ns);
		if (ns >= 250000000ULL) {
			apfs_log("write waited %llu ms for the writer's mutex (the one before: %s)\n",
			    (unsigned long long)(ns / 1000000),
			    am->am_wlock_name[0] != 0 ? am->am_wlock_name : "?");
		}
		/* what each process pays for the writing: this wait, and the epoch it then holds */
		os_atomic_add(&lp->write_wait, waited, relaxed);
		if (waited > lp->write_wait_max) {
			os_atomic_store(&lp->write_wait_max, waited, relaxed);
		}
	}
	am->am_wlock_owner = self;
	am->am_wlock_depth = 1;
	am->am_wlock_since = apfs_ticks();
	proc_selfname(am->am_wlock_name, sizeof(am->am_wlock_name));
	apfs_lock(am);
}

static void
apfs_unlock_write(struct apfs_mount *am)
{
	apfs_unlock(am);
	if (--am->am_wlock_depth == 0) {
		/* how long this process held the writer's mutex: the transaction, its commit, the blocks */
		struct apfs_lock_proc *lp = lock_proc(am);

		os_atomic_inc(&lp->write_epochs, relaxed);
		os_atomic_add(&lp->write_held, apfs_ticks() - am->am_wlock_since, relaxed);
		am->am_wlock_owner = NULL;
		am->am_wlock_since = 0;
		/* (the name stays: a writer that waits for it says whose it was) */
		lck_mtx_unlock(&am->am_wlock);
	}
}

/* ------------------------------------------------------------------ creation and removal */

/*
 * The name in a componentname is not terminated; apfs_lib wants it so.  ERRNO is 0 or the
 * error the caller returns.
 */
/*
 * Debug: what the lookup answered, in the string init prints (kern.apfs_stats).  The kernel's
 * own output goes to the UART on the phone and never comes back over USB.
 */
static int
apfs_vnop_lookup(struct vnop_lookup_args *ap)
{
	return apfs_lookup_impl(ap);
}

static int
apfs_name(struct componentname *cnp, char *name)
{
	if (cnp->cn_namelen >= APFS_NAME_BUF) {
		return ENAMETOOLONG;
	}
	memcpy(name, cnp->cn_nameptr, (size_t)cnp->cn_namelen);
	name[cnp->cn_namelen] = 0;
	return 0;
}

static int
apfs_vnop_create(struct vnop_create_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
	char name[APFS_NAME_BUF];
	uint64_t id = 0;
	uint32_t uid = kauth_cred_getuid(cred);
	uint32_t gid = kauth_cred_getgid(cred);
	uint16_t mode = APFS_S_IFREG | 0644;
	int error;

	*ap->a_vpp = NULLVP;
	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if ((error = apfs_name(cnp, name)) != 0) {
		return error;
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		uid = vap->va_uid;
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		gid = vap->va_gid;
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		mode = APFS_S_IFREG | (vap->va_mode & 07777);
	}

	apfs_lock_write(am);
	error = apfs_create_file(&am->am_vol, dn->an_id, name, mode, uid, gid, apfs_now(), &id);
	apfs_unlock_write(am);
	cache_purge_negatives(dvp);
	if (error) {
		apfs_log("create %s in %llu: %d\n", name, (unsigned long long)dn->an_id, error);
		return error;
	}
	error = apfs_node_vnode(am, id, dvp, cnp, ap->a_vpp);
	if (error) {
		apfs_log("create %s: inode %llu, no vnode: %d\n", name,
		    (unsigned long long)id, error);
	}
	if (error) {
		/* no vnode to hold it: take the file back rather than leave it in the tree */
		apfs_lock_write(am);
		(void)apfs_unlink(&am->am_vol, dn->an_id, name, apfs_now());
		apfs_unlock_write(am);
	}
	return error;
}

/*
 * A socket or a FIFO is a name, an inode and no data: this is the whole of what mknod needs.
 * A device node would stand for hardware this volume has no business making, so it is refused.
 */
static int
apfs_vnop_mknod(struct vnop_mknod_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
	char name[APFS_NAME_BUF];
	uint64_t id = 0;
	uint32_t uid = kauth_cred_getuid(cred);
	uint32_t gid = kauth_cred_getgid(cred);
	uint16_t type, mode;
	int error;

	*ap->a_vpp = NULLVP;
	/* the VFS puts the kind of node in va_type, and only the permission bits in va_mode */
	switch (vap->va_type) {
	case VSOCK:
		type = APFS_S_IFSOCK;
		break;
	case VFIFO:
		type = APFS_S_IFIFO;
		break;
	default:
		return EPERM;
	}
	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if ((error = apfs_name(ap->a_cnp, name)) != 0) {
		return error;
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		uid = vap->va_uid;
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		gid = vap->va_gid;
	}
	mode = (uint16_t)(type | (vap->va_mode & 07777));
	apfs_lock_write(am);
	error = apfs_create_file(&am->am_vol, dn->an_id, name, mode, uid, gid, apfs_now(), &id);
	apfs_unlock_write(am);
	cache_purge_negatives(dvp);
	if (error) {
		apfs_log("mknod %s in %llu: %d\n", name, (unsigned long long)dn->an_id, error);
		return error;
	}
	error = apfs_node_vnode(am, id, dvp, ap->a_cnp, ap->a_vpp);
	if (error) {
		apfs_lock_write(am);
		(void)apfs_unlink(&am->am_vol, dn->an_id, name, apfs_now());
		apfs_unlock_write(am);
	}
	return error;
}

static int
apfs_vnop_mkdir(struct vnop_mkdir_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
	char name[APFS_NAME_BUF];
	uint64_t id = 0;
	uint32_t uid = kauth_cred_getuid(cred);
	uint32_t gid = kauth_cred_getgid(cred);
	uint16_t mode = APFS_S_IFDIR | 0755;
	int error;

	*ap->a_vpp = NULLVP;
	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if ((error = apfs_name(cnp, name)) != 0) {
		return error;
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		uid = vap->va_uid;
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		gid = vap->va_gid;
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		mode = APFS_S_IFDIR | (vap->va_mode & 07777);
	}

	apfs_lock_write(am);
	error = apfs_mkdir(&am->am_vol, dn->an_id, name, mode, uid, gid, apfs_now(), &id);
	apfs_unlock_write(am);
	cache_purge_negatives(dvp);
	if (error) {
		return error;
	}
	error = apfs_node_vnode(am, id, dvp, cnp, ap->a_vpp);
	if (error) {
		apfs_lock_write(am);
		(void)apfs_rmdir(&am->am_vol, dn->an_id, name, apfs_now());
		apfs_unlock_write(am);
	}
	return error;
}


static int
apfs_vnop_link(struct vnop_link_args *ap)
{
	vnode_t vp = ap->a_vp;
	vnode_t tdvp = ap->a_tdvp;
	struct apfs_mount *am = APFS_VP_MOUNT(tdvp);
	struct apfs_node *tn = APFS_VP(tdvp);
	char name[APFS_NAME_BUF];
	int error;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (vnode_isdir(vp)) {
		return EPERM;
	}
	if (!vnode_isdir(tdvp)) {
		return ENOTDIR;
	}
	if ((error = apfs_name(ap->a_cnp, name)) != 0) {
		return error;
	}
	apfs_lock_write(am);
	error = apfs_link(&am->am_vol, tn->an_id, name, APFS_VP(vp)->an_id, apfs_now());
	apfs_unlock_write(am);
	if (error) {
		return error;
	}
	apfs_node_refresh(am, APFS_VP(vp));
	cache_purge_negatives(tdvp);
	return 0;
}

static int
apfs_vnop_symlink(struct vnop_symlink_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
	char name[APFS_NAME_BUF];
	uint64_t id = 0;
	uint32_t uid = kauth_cred_getuid(cred);
	uint32_t gid = kauth_cred_getgid(cred);
	uint16_t mode = APFS_S_IFLNK | 0777;
	size_t target_len;
	int error;

	*ap->a_vpp = NULLVP;
	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if (ap->a_target == NULL) {
		return EINVAL;
	}
	if ((error = apfs_name(ap->a_cnp, name)) != 0) {
		return error;
	}
	if (ap->a_vap != NULL && VATTR_IS_ACTIVE(ap->a_vap, va_mode)) {
		mode = (uint16_t)(APFS_S_IFLNK | (ap->a_vap->va_mode & 07777));
	}
	target_len = strlen(ap->a_target);
	apfs_lock_write(am);
	error = apfs_create_file(&am->am_vol, dn->an_id, name, mode, uid, gid,
	    apfs_now(), &id);
	if (error == 0) {
		error = apfs_setxattr(&am->am_vol, id, APFS_SYMLINK_EA_NAME, ap->a_target,
		    target_len, APFS_XATTR_FILE_SYSTEM_OWNED, apfs_now());
	}
	apfs_unlock_write(am);
	cache_purge_negatives(dvp);
	if (error) {
		return error;
	}
	error = apfs_node_vnode(am, id, dvp, ap->a_cnp, ap->a_vpp);
	if (error) {
		apfs_lock_write(am);
		(void)apfs_unlink(&am->am_vol, dn->an_id, name, apfs_now());
		apfs_unlock_write(am);
	}
	return error;
}

static int
apfs_vnop_remove(struct vnop_remove_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	char name[APFS_NAME_BUF];
	int error, keep;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if (vnode_isdir(vp)) {
		return EPERM;           /* rmdir() has its own hook */
	}
	if ((error = apfs_name(ap->a_cnp, name)) != 0) {
		return error;
	}

	/*
	 * A file that is still open when its last name goes is not deleted here: POSIX keeps
	 * the file for the reader, and so does this (the inode's link count falls to zero, and
	 * VNOP_INACTIVE takes the file away at the last close).
	 */
	/*
	 * A file that is deleted is kept, open or not, until its vnode goes inactive: the
	 * last close is the only place that knows no reader is left.  Deciding here on
	 * vnode_isinuse() took the inode from under a handle that was still writing it
	 * ("write inode ...: 2" from SQLite's lock files), and the next write failed.
	 */
	keep = 1;
	apfs_lock_write(am);
	error = keep ? apfs_unlink_keep(&am->am_vol, dn->an_id, name, apfs_now()) :
	    apfs_unlink(&am->am_vol, dn->an_id, name, apfs_now());
	apfs_unlock_write(am);
	if (error) {
		apfs_log("remove %s in %llu (open %d): %d\n", name,
		    (unsigned long long)dn->an_id, keep, error);
		return error;
	}
	if (keep && APFS_VP(vp)->an_ino.nchildren <= 1) {
		/*
		 * Only the last name leaves a file with nothing naming it: a file that still has
		 * another name is not an orphan, and its link count is the one the tree now has.
		 */
		APFS_VP(vp)->an_unlinked = true;
	}
	/* the link count the tree now has (zero for the last name), not the one it had */
	apfs_node_refresh(am, APFS_VP(vp));
	/* the name is gone: no lookup finds it again, and its pages are stale */
	cache_purge(vp);
	return 0;
}

static int
apfs_vnop_rmdir(struct vnop_rmdir_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	char name[APFS_NAME_BUF];
	int error;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if (!vnode_isdir(vp)) {
		return ENOTDIR;
	}
	if ((error = apfs_name(ap->a_cnp, name)) != 0) {
		return error;
	}

	apfs_lock_write(am);
	error = apfs_rmdir(&am->am_vol, dn->an_id, name, apfs_now());
	apfs_unlock_write(am);
	if (error) {
		return error;
	}
	cache_purge(vp);
	return 0;
}

/* ------------------------------------------------------------------ data and metadata */

/*
 * Set the length of a file.  Growing is refused by apfs_truncate (see apfs_lib.h): the new
 * bytes would have to read as zeros and that is a different operation.
 */
static int
apfs_setlength(vnode_t vp, uint64_t len)
{
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	uint64_t old = an->an_size;
	int error;

	if (len == old) {
		return 0;
	}
	apfs_lock_write(am);
	error = apfs_truncate(&am->am_vol, an->an_id, len, apfs_now());
	apfs_unlock_write(am);
	if (error) {
		apfs_log("truncate inode %llu to %llu (was %llu): %d\n",
		    (unsigned long long)an->an_id, (unsigned long long)len,
		    (unsigned long long)old, error);
		return error;
	}
	if (len < old) {
		(void)ubc_msync(vp, (off_t)len, (off_t)old, NULL, UBC_PUSHALL | UBC_INVALIDATE);
	}
	(void)ubc_setsize(vp, (off_t)len);
	apfs_node_refresh(am, an);
	return 0;
}

/*
 * Contents.  In place when the write stays inside what the file already has (apfs_write.c
 * changes no structure for that), appended when it starts at the end; a write that starts
 * inside and runs past the end is both, in that order.  A hole (a write past the end) is
 * refused: apfs_truncate does not grow a file either.
 */
static int
apfs_vnop_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	struct uio *uio = ap->a_uio;
	uint64_t old = an->an_size;
	uint64_t off = (uint64_t)uio_offset(uio);
	size_t len = (size_t)uio_resid(uio);
	char *buf;
	int error;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (vnode_isdir(vp)) {
		return EISDIR;
	}
	if (len == 0) {
		return 0;
	}
	/* O_APPEND: VFS passes the flag, not an offset at the end (write(2) with the flag set) */
	if ((ap->a_ioflag & IO_APPEND) != 0) {
		off = old;
	}
	/*
	 * A write that starts past the end is a hole, and apfs_write_data writes
	 * those: SQLite extends a WAL shared-memory file by writing one byte at the
	 * end of each page, and refusing that turned every WAL database into
	 * "disk I/O error" (SQLITE_IOERR_SHMSIZE).
	 */
	buf = apfs_alloc(len);
	if (buf == NULL) {
		return ENOMEM;
	}
	error = uiomove(buf, len, uio);
	if (error) {
		goto out;
	}
	/*
	 * The contents are written past the page cache, so the cache has to be brought into
	 * line: what a mapping has left dirty in the range goes out first (through pageout),
	 * and after the write the pages of the range are dropped, so the next read sees what
	 * was just written instead of what was cached before it.
	 */
	if (UBCINFOEXISTS(vp)) {
		(void)ubc_msync(vp, (off_t)off, (off_t)(off + len), NULL, UBC_PUSHALL);
	}
	apfs_lock_write(am);
	error = apfs_write_data(&am->am_vol, an->an_id, off, buf, len, apfs_now());
	apfs_unlock_write(am);
	if (error) {
		apfs_log("write inode %llu at %llu, %zu bytes: %d\n",
		    (unsigned long long)an->an_id, (unsigned long long)off, len, error);
	}
	if (error == 0) {
		apfs_node_refresh(am, an);
		if (UBCINFOEXISTS(vp)) {
			(void)ubc_msync(vp, (off_t)off, (off_t)(off + len), NULL,
			    UBC_INVALIDATE);
		}
	}
out:
	apfs_free(buf, len);
	return error;
}

/*
 * Rename.  VFS has checked the two parents; the bookkeeping is apfs_rename's.  What is left
 * is the name cache and the vnode: the old name must stop finding it, the new one must.
 */
static int
apfs_vnop_rename(struct vnop_rename_args *ap)
{
	vnode_t fdvp = ap->a_fdvp;
	vnode_t fvp = ap->a_fvp;
	vnode_t tdvp = ap->a_tdvp;
	struct apfs_mount *am = APFS_VP_MOUNT(fdvp);
	struct apfs_node *dn = APFS_VP(fdvp);
	struct apfs_node *tn = APFS_VP(tdvp);
	char from[APFS_NAME_BUF], to[APFS_NAME_BUF];
	int error;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (!vnode_isdir(fdvp) || !vnode_isdir(tdvp)) {
		return ENOTDIR;
	}
	if ((error = apfs_name(ap->a_fcnp, from)) != 0 ||
	    (error = apfs_name(ap->a_tcnp, to)) != 0) {
		return error;
	}

	apfs_lock_write(am);
	/*
	 * POSIX rename replaces what is there.  The driver's own rename does not (it answers
	 * EEXIST for a name that is taken), so the entry that is in the way goes first -- a file,
	 * or a directory that is empty.  Every atomic save in the system does this (a temporary
	 * file renamed over the one it replaces), and the container manager traps on the error.
	 */
	if (ap->a_tvp != NULLVP) {
		error = vnode_isdir(ap->a_tvp) ?
		    apfs_rmdir(&am->am_vol, tn->an_id, to, apfs_now()) :
		    apfs_unlink(&am->am_vol, tn->an_id, to, apfs_now());
		if (error != 0) {
			apfs_unlock_write(am);
			return error;
		}
	}
	error = apfs_rename(&am->am_vol, dn->an_id, from, tn->an_id, to, apfs_now());
	if (error == 0) {
		APFS_VP(fvp)->an_parent = tn->an_id;
	}
	apfs_unlock_write(am);
	if (error) {
		return error;
	}
	cache_purge_negatives(fdvp);
	cache_purge_negatives(tdvp);
	cache_purge(fvp);
	cache_enter(tdvp, fvp, ap->a_tcnp);
	return 0;
}

static int
apfs_vnop_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_setattr sa;
	int error = 0;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (vnode_isdir(vp)) {
			return EISDIR;
		}
		error = apfs_setlength(vp, (uint64_t)vap->va_data_size);
		if (error) {
			return error;
		}
	}

	memset(&sa, 0, sizeof(sa));
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		sa.mode_set = true;
		sa.mode = (uint16_t)((an->an_mode & APFS_S_IFMT) | (vap->va_mode & 07777));
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		sa.owner_set = true;
		sa.owner = vap->va_uid;
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		sa.group_set = true;
		sa.group = vap->va_gid;
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		sa.flags_set = true;
		sa.bsd_flags = vap->va_flags;
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		sa.atime_set = true;
		sa.atime = apfs_ts_ns(vap->va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		sa.mtime_set = true;
		sa.mtime = apfs_ts_ns(vap->va_modify_time);
	}
	if (sa.mode_set || sa.owner_set || sa.group_set || sa.flags_set || sa.atime_set ||
	    sa.mtime_set) {
		apfs_lock_write(am);
		error = apfs_inode_setattr(&am->am_vol, an->an_id, &sa, apfs_now());
		apfs_unlock_write(am);
		if (error) {
			return error;
		}
		apfs_node_refresh(am, an);
	}
	return error;
}

static int
apfs_vnop_setxattr(struct vnop_setxattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	size_t len = (size_t)uio_resid(ap->a_uio);
	char *buf;
	int error;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	if (len > APFS_XATTR_MAX_EMBEDDED_SIZE) {
		return E2BIG;
	}
	buf = apfs_alloc(len == 0 ? 1 : len);
	if (buf == NULL) {
		return ENOMEM;
	}
	error = uiomove(buf, len, ap->a_uio);
	if (error == 0) {
		apfs_lock_write(am);
		error = apfs_setxattr(&am->am_vol, an->an_id, ap->a_name, buf, len, 0,
		    apfs_now());
		apfs_unlock_write(am);
	}
	apfs_free(buf, len == 0 ? 1 : len);
	return error;
}

static int
apfs_vnop_removexattr(struct vnop_removexattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	int error;

	if (!apfs_writable(am)) {
		return EROFS;
	}
	apfs_lock_write(am);
	error = apfs_removexattr(&am->am_vol, an->an_id, ap->a_name, apfs_now());
	apfs_unlock_write(am);
	return error;
}

/* ------------------------------------------------------------------ attributes */

static int
apfs_vnop_getattr(struct vnop_getattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_inode ino;
	uint64_t size = 0;
	int error;

	ino = an->an_ino;
	size = an->an_size;

	VATTR_RETURN(vap, va_rdev, (dev_t)ino.rdev);
	VATTR_RETURN(vap, va_nlink, vnode_isdir(vp) ? 2 : (an->an_unlinked ? 0 :
	    (uint32_t)(ino.nchildren > 0 ? ino.nchildren : 1)));
	VATTR_RETURN(vap, va_data_size, size);
	VATTR_RETURN(vap, va_total_size, size);
	VATTR_RETURN(vap, va_total_alloc, ino.alloced_size);
	VATTR_RETURN(vap, va_data_alloc, ino.alloced_size);
	VATTR_RETURN(vap, va_iosize, am->am_nx.block_size);
	VATTR_RETURN(vap, va_uid, ino.owner);
	VATTR_RETURN(vap, va_gid, ino.group);
	VATTR_RETURN(vap, va_mode, ino.mode & 07777);
	VATTR_RETURN(vap, va_flags, ino.bsd_flags);
	VATTR_RETURN(vap, va_fileid, an->an_id);
	VATTR_RETURN(vap, va_linkid, an->an_id);
	VATTR_RETURN(vap, va_parentid, ino.parent_id);
	VATTR_RETURN(vap, va_fsid, (uint32_t)vfs_statfs(am->am_mp)->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_type, vnode_vtype(vp));

	/* the times are nanoseconds since the Unix epoch */
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		vap->va_access_time.tv_sec = (long)(ino.access_time / 1000000000ULL);
		vap->va_access_time.tv_nsec = (long)(ino.access_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		vap->va_modify_time.tv_sec = (long)(ino.mod_time / 1000000000ULL);
		vap->va_modify_time.tv_nsec = (long)(ino.mod_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		vap->va_change_time.tv_sec = (long)(ino.change_time / 1000000000ULL);
		vap->va_change_time.tv_nsec = (long)(ino.change_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_change_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		vap->va_create_time.tv_sec = (long)(ino.create_time / 1000000000ULL);
		vap->va_create_time.tv_nsec = (long)(ino.create_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	return 0;
}

/* ------------------------------------------------------------------ reading */

/*
 * A buffer for one round of vnop_read: one of those kept, or a new one.  The kept ones have
 * a lock of their own, so that a read takes am_lock only for the reading itself.
 */
static uint8_t *
apfs_iobuf_get(struct apfs_mount *am)
{
	uint8_t *buf = NULL;

	lck_spin_lock(&am->am_iobuf_spin);
	if (am->am_iobuf_kept > 0) {
		buf = am->am_iobuf[--am->am_iobuf_kept];
	}
	lck_spin_unlock(&am->am_iobuf_spin);
	if (buf == NULL) {
		buf = kalloc_data(am->am_iobuf_size, Z_WAITOK);
	}
	return buf;
}

static void
apfs_iobuf_put(struct apfs_mount *am, uint8_t *buf)
{
	lck_spin_lock(&am->am_iobuf_spin);
	if (am->am_iobuf_kept < APFS_IOBUF_KEEP) {
		am->am_iobuf[am->am_iobuf_kept++] = buf;
		buf = NULL;
	}
	lck_spin_unlock(&am->am_iobuf_spin);
	if (buf != NULL) {
		kfree_data(buf, am->am_iobuf_size);
	}
}

/*
 * read(2) through the page cache, as decmpfs reads a compressed HFS file
 * (decmpfs_read_compressed): what the cache has is copied from it; for the rest the file's
 * pages are taken in a UPL, the ones not valid yet read into it by apfs_read_file -- what
 * pagein does -- and the UPL committed, so that the pages stay: the next read(2) of them
 * finds them, and so does a mapping of the file, which is the same pages.  At least
 * APFS_FILL_ALIGN at a time, aligned to it: a compressed file is kept in 64 KB pieces, and a
 * piece decompressed for a small read would be decompressed again for the next one.
 */
#define APFS_FILL_ALIGN         (64 * 1024)
#define APFS_FILL_MAX           (1024 * 1024)   /* at most this much in one UPL */
/*
 * A reader going through a file in order (it starts where the last read(2) of the file ended)
 * is given more than it asked for, twice as much each time it comes to the end of what it was
 * given, up to APFS_FILL_MAX: fewer UPLs, larger requests to the device.  The cluster layer
 * does the same for a file system that uses it.
 */

/* [at, at + len) of the file into dst, as pagein reads it; past the end, zeros */
static int
apfs_fill(struct apfs_mount *am, vnode_t vp, struct apfs_inode *ino, uint64_t size, off_t at,
    uint8_t *dst, size_t len, uint64_t *filled)
{
	int error = 0;

	while (len > 0) {
		size_t want, got = 0;

		if ((uint64_t)at >= size) {
			bzero(dst, len);
			break;
		}
		want = (size_t)MIN((uint64_t)len, size - (uint64_t)at);
		os_atomic_inc(&am->am_nx.requests, relaxed);
		apfs_lock(am);
		error = apfs_read_file(&am->am_vol, ino, &APFS_VP(vp)->an_cf, (uint64_t)at, dst, want, &got);
		apfs_unlock(am);
		os_atomic_dec(&am->am_nx.requests, relaxed);
		if (error) {
			break;
		}
		if (got == 0) {
			bzero(dst, len);        /* (shorter than its size says: zeros, as pagein gives) */
			break;
		}
		at += (off_t)got;
		dst += got;
		len -= got;
		*filled += got;
	}
	return error;
}

static int
apfs_read_cached(struct apfs_mount *am, vnode_t vp, struct uio *uio, struct apfs_inode *ino,
    uint64_t size, uint64_t *cached, uint64_t *filled)
{
	int error = 0;

	while (uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t left = MIN((uint64_t)uio_resid(uio), size - (uint64_t)uio_offset(uio));
		int count = (int)MIN(left, (uint64_t)INT_MAX), resid = count, copy, copy_resid, npages, i;
		off_t pos, want_end, upl_pos, upl_end;
		upl_page_info_t *pl = NULL;
		vm_offset_t address = 0;
		upl_t upl = NULL;
		int upl_size;

		/* what the cache has already, up to the first page it has not */
		error = cluster_copy_ubc_data(vp, uio, &resid, 0);
		*cached += (uint64_t)(count - resid);
		if (error) {
			break;
		}
		if (resid == 0) {
			continue;
		}

		/* the rest: the pages from there, the missing ones read in */
		pos = uio_offset(uio);
		left = MIN((uint64_t)uio_resid(uio), size - (uint64_t)pos);
		want_end = pos + (off_t)left;
		upl_pos = pos & ~((off_t)APFS_FILL_ALIGN - 1);
		upl_end = (want_end + APFS_FILL_ALIGN - 1) & ~((off_t)APFS_FILL_ALIGN - 1);
		{
			struct apfs_node *an = APFS_VP(vp);
			uint32_t window = APFS_FILL_ALIGN;

			/* (where this read(2) began: before what the cache gave it just now) */
			if ((uint64_t)pos - (uint64_t)(count - resid) == os_atomic_load(&an->an_read_next, relaxed)) {
				window = MIN(MAX(os_atomic_load(&an->an_read_window, relaxed) * 2,
				    (uint32_t)APFS_FILL_ALIGN), (uint32_t)APFS_FILL_MAX);
			}
			os_atomic_store(&an->an_read_window, window, relaxed);
			if (upl_end < upl_pos + (off_t)window) {
				upl_end = upl_pos + (off_t)window;
			}
		}
		if (upl_end > (off_t)round_page_64(size)) {
			upl_end = (off_t)round_page_64(size);
		}
		if (upl_end - upl_pos > APFS_FILL_MAX) {
			upl_end = upl_pos + APFS_FILL_MAX;
		}
		upl_size = (int)(upl_end - upl_pos);
		if (ubc_create_upl_kernel(vp, upl_pos, upl_size, &upl, &pl, UPL_SET_LITE,
		    VM_KERN_MEMORY_FILE) != KERN_SUCCESS || upl == NULL) {
			error = EIO;
			break;
		}
		if (ubc_upl_map(upl, &address) != KERN_SUCCESS || address == 0) {
			ubc_upl_abort_range(upl, 0, (upl_size_t)upl_size, UPL_ABORT_FREE_ON_EMPTY);
			error = EIO;
			break;
		}
		npages = upl_size / PAGE_SIZE;
		for (i = 0; i < npages && error == 0;) {
			int j;

			if (upl_valid_page(pl, i)) {
				i++;
				continue;
			}
			for (j = i + 1; j < npages && !upl_valid_page(pl, j); j++) {
			}
			error = apfs_fill(am, vp, ino, size, upl_pos + (off_t)i * PAGE_SIZE,
			    (uint8_t *)address + (size_t)i * PAGE_SIZE, (size_t)(j - i) * PAGE_SIZE, filled);
			i = j;
		}
		(void)ubc_upl_unmap(upl);
		if (error) {
			ubc_upl_abort_range(upl, 0, (upl_size_t)upl_size, UPL_ABORT_FREE_ON_EMPTY);
			break;
		}
		/* out of the UPL what this read asked for; then the pages go into the cache */
		copy = (int)(MIN(upl_end, want_end) - pos);
		copy_resid = copy;
		error = cluster_copy_upl_data(uio, upl, (int)(pos - upl_pos), &copy_resid);
		ubc_upl_commit_range(upl, 0, (upl_size_t)upl_size, UPL_COMMIT_FREE_ON_EMPTY |
		    UPL_COMMIT_INACTIVATE | UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_WRITTEN_BY_KERNEL);
		if (error) {
			break;
		}
		if (copy_resid == copy) {
			error = EIO;            /* (nothing copied: cannot happen, and must not loop) */
			break;
		}
	}
	return error;
}

/* read(2) the old way: into a buffer of the driver's and out of it, every time (apfs_ubc_read=0) */
static int
apfs_read_direct(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_inode ino;
	uint8_t *buf;
	uint64_t size = 0;
	uint64_t t0, read_bytes = 0;
	struct apfs_lock_proc *lp = NULL;
	int error = 0;

	ino = APFS_VP(vp)->an_ino;
	size = APFS_VP(vp)->an_size;
	t0 = apfs_ticks();
	if (uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		buf = apfs_iobuf_get(am);
		if (buf == NULL) {
			return ENOMEM;
		}
		/* (how many reads are going on: a hint to apfs_lib, kept without the lock) */
		os_atomic_inc(&am->am_nx.requests, relaxed);
		apfs_lock(am);
		{
			lp = lock_proc(am);
			os_atomic_inc(&lp->read_calls, relaxed);
			os_atomic_add(&lp->read_bytes,
			    (uint64_t)MIN((uint64_t)uio_resid(uio), size - (uint64_t)uio_offset(uio)), relaxed);
		}
		/*
		 * The lock is held for the reading of each round and let go of for its uiomove;
		 * taken again only if another round follows.  What comes after the last one (the
		 * buffer given back, the counts) needs no am_lock.
		 */
		for (;;) {
			size_t round = (os_atomic_load(&am->am_nx.requests, relaxed) <= 1)
			    ? am->am_iobuf_size : MIN(am->am_iobuf_size, APFS_ROUND_SHARED);
			size_t want = (size_t)MIN((user_ssize_t)round, uio_resid(uio));
			size_t got = 0;

			error = apfs_read_file(&am->am_vol, &ino, &APFS_VP(vp)->an_cf,
			    (uint64_t)uio_offset(uio), buf, want, &got);
			apfs_unlock(am);
			if (error || got == 0) {
				break;
			}
			error = uiomove((caddr_t)buf, (int)got, uio);
			if (error) {
				break;
			}
			read_bytes += got;
			if (uio_resid(uio) <= 0 || (uint64_t)uio_offset(uio) >= size) {
				break;
			}
			apfs_lock(am);
		}
		os_atomic_dec(&am->am_nx.requests, relaxed);
		apfs_iobuf_put(am, buf);
	}
	if (lp != NULL) {
		/* (the entry stays where it is; a zeroing meanwhile costs at most this one read's time) */
		os_atomic_add(&lp->read_ticks, apfs_ticks() - t0, relaxed);
	}
	os_atomic_add(&am->am_read_ticks, apfs_ticks() - t0, relaxed);
	os_atomic_inc(&am->am_read_calls, relaxed);
	os_atomic_add(&am->am_read_bytes, read_bytes, relaxed);
	return error;
}

static int
apfs_vnop_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_inode ino;
	struct apfs_lock_proc *lp;
	uint64_t size, t0, before, cached = 0, filled = 0;
	int error = 0;

	if (vnode_isdir(vp)) {
		return EISDIR;
	}
	if (!vnode_isreg(vp)) {
		return EINVAL;
	}
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	if (!am->am_ubc_read || !UBCINFOEXISTS(vp)) {
		return apfs_read_direct(ap);
	}
	ino = APFS_VP(vp)->an_ino;
	size = APFS_VP(vp)->an_size;
	t0 = apfs_ticks();
	before = (uint64_t)uio_resid(uio);
	lp = lock_proc(am);
	os_atomic_inc(&lp->read_calls, relaxed);
	if (uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		error = apfs_read_cached(am, vp, uio, &ino, size, &cached, &filled);
	}
	os_atomic_store(&APFS_VP(vp)->an_read_next, (uint64_t)uio_offset(uio), relaxed);
	os_atomic_add(&lp->read_bytes, before - (uint64_t)uio_resid(uio), relaxed);
	os_atomic_add(&lp->read_ticks, apfs_ticks() - t0, relaxed);
	os_atomic_add(&am->am_read_ticks, apfs_ticks() - t0, relaxed);
	os_atomic_inc(&am->am_read_calls, relaxed);
	os_atomic_add(&am->am_read_bytes, before - (uint64_t)uio_resid(uio), relaxed);
	os_atomic_add(&am->am_read_cached, cached, relaxed);
	os_atomic_add(&am->am_read_filled, filled, relaxed);
	return error;
}


/*
 * Page out.  A writable shared mapping leaves its pages dirty in the page cache; before
 * they can go (a msync, the end of a mapping, memory pressure), they come here and are
 * written into the file's extents.  The slow in-place path is the right one: it never
 * changes a structure, and a mapping cannot write past the end of the file.
 */
static int
apfs_vnop_pageout(struct vnop_pageout_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	upl_t upl = ap->a_pl;
	upl_offset_t upl_offset = ap->a_pl_offset;
	off_t offset = ap->a_f_offset;
	size_t size = ap->a_size;
	vm_offset_t address = 0;
	uint64_t old = an->an_size;
	int error = 0;

	if (upl == NULL || size == 0) {
		error = EINVAL;
	} else if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
		error = ENOMEM;
	} else {
		size_t len = size;

		if ((uint64_t)offset < old) {
			if ((uint64_t)len > old - (uint64_t)offset) {
				len = (size_t)(old - (uint64_t)offset);
			}
			apfs_lock_write(am);
			error = apfs_write_data(&am->am_vol, an->an_id, (uint64_t)offset,
			    (const uint8_t *)address + upl_offset, len, apfs_now());
			apfs_unlock_write(am);
			if (error == 0) {
				apfs_node_refresh(am, an);
			}
		}
		ubc_upl_unmap(upl);
	}
	if (!(ap->a_flags & UPL_NOCOMMIT)) {
		if (error) {
			ubc_upl_abort_range(upl, upl_offset, (upl_size_t)size,
			    UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
		} else {
			ubc_upl_commit_range(upl, upl_offset, (upl_size_t)size,
			    UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

/*
 * Page in.  Everything goes through apfs_read_file rather than the cluster layer: a
 * compressed file has no block mapping at all, and a plain one is read out of the extents
 * just as cheaply here.
 */
#define APFS_PRA_MAX    (1024 * 1024)

/*
 * Reading ahead for pagein.  The paging machinery asks 32 KB at a time, so a file read
 * through comes a thousand faults -- each its own UPL, its own turn at the lock, and its
 * own 64 KB piece decompressed on one processor (a request of one piece leaves the helper
 * threads nothing to take).  A reader going forward is given the pages after the one it
 * asked for, filled into the page cache the way cluster_read does it, so that the faults
 * that follow find theirs there and never come down here.  What it is given doubles each
 * time it comes to the end of it, up to a megabyte; a fault anywhere else starts it over at
 * the boot-arg's size (apfs_pra, KB; 0: none).
 *
 * Off by default, and it stays off: p380, with 256 KB, read ahead 1 664 MB where the run
 * faulted in 640 MB -- pages put in as UPL_COMMIT_INACTIVATE are the first to go, so the
 * same window was filled again and again, and the decompression under the lock grew 2.3
 * times (14 050 pieces to 30 052; pagein 3 962 ms to 5 100, lock waits 3 489 to 7 574).
 * The volume has ~800 MB for file pages (2.5 GB of the 4.2 GB are the system image held in
 * RAM), which is less than what the daemons touch: prefetching into it only pushes out what
 * is about to be asked for.  What the same runs say is worth having: the paging machinery's
 * own cluster (32 to 256 KB, from vm_object_cluster_size) and the decompression are the
 * cost, not the device (646 MB of data reads in 135 ms, 21 MB from the device in 35 ms).
 */
static void
apfs_pagein_ahead(struct apfs_mount *am, vnode_t vp, struct apfs_inode *ino,
    uint64_t size, off_t at)
{
	struct apfs_node *an = APFS_VP(vp);
	upl_t upl = NULL;
	upl_page_info_t *pl = NULL;
	vm_offset_t address = 0;
	uint64_t filled = 0;
	off_t end;
	uint32_t window, prev, npages, i;
	int upl_size, error = 0;

	if (am->am_pra == 0 || size == 0 || at < 0 || (uint64_t)at >= size ||
	    !UBCINFOEXISTS(vp) || !an->an_cf.compressed) {
		return;                 /* (a plain file comes out of the block cache: nothing to save) */
	}
	at = (off_t)((uint64_t)at & ~(uint64_t)PAGE_MASK_64);
	prev = os_atomic_load(&an->an_pra_window, relaxed);
	if (prev > (uint32_t)APFS_PRA_MAX / 2) {
		prev = (uint32_t)APFS_PRA_MAX / 2;
	}
	if ((uint64_t)at == os_atomic_load(&an->an_pra_next, relaxed)) {
		window = MAX(prev * 2, am->am_pra);
	} else {
		window = am->am_pra;
	}
	os_atomic_store(&an->an_pra_window, window, relaxed);
	end = (off_t)((uint64_t)at + window);
	if ((uint64_t)end > size) {
		end = (off_t)(size & ~(uint64_t)PAGE_MASK_64);
	}
	if (end <= at) {
		return;
	}
	upl_size = (int)(end - at);
	if (ubc_create_upl_kernel(vp, at, upl_size, &upl, &pl, UPL_SET_LITE,
	    VM_KERN_MEMORY_FILE) != KERN_SUCCESS || upl == NULL) {
		return;
	}
	if (ubc_upl_map(upl, &address) != KERN_SUCCESS || address == 0) {
		ubc_upl_abort_range(upl, 0, (upl_size_t)upl_size, UPL_ABORT_FREE_ON_EMPTY);
		return;
	}
	npages = (uint32_t)(upl_size / PAGE_SIZE);
	for (i = 0; i < npages && error == 0;) {
		uint32_t j;

		if (upl_valid_page(pl, i)) {
			i++;            /* (somebody read it meanwhile: not read again) */
			continue;
		}
		for (j = i + 1; j < npages && !upl_valid_page(pl, j); j++) {
		}
		error = apfs_fill(am, vp, ino, size, at + (off_t)i * PAGE_SIZE,
		    (uint8_t *)address + (size_t)i * PAGE_SIZE, (size_t)(j - i) * PAGE_SIZE,
		    &filled);
		i = j;
	}
	(void)ubc_upl_unmap(upl);
	if (error) {
		ubc_upl_abort_range(upl, 0, (upl_size_t)upl_size, UPL_ABORT_FREE_ON_EMPTY);
		os_atomic_store(&an->an_pra_next, 0, relaxed);  /* (nothing to go on from) */
		return;
	}
	ubc_upl_commit_range(upl, 0, (upl_size_t)upl_size, UPL_COMMIT_FREE_ON_EMPTY |
	    UPL_COMMIT_INACTIVATE | UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_WRITTEN_BY_KERNEL);
	os_atomic_store(&an->an_pra_next, (uint64_t)end, relaxed);
	os_atomic_inc(&am->am_pra_calls, relaxed);
	os_atomic_add(&am->am_pra_bytes, (uint64_t)upl_size, relaxed);
}

static int
apfs_vnop_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	upl_t upl = ap->a_pl;
	upl_offset_t upl_offset = ap->a_pl_offset;
	off_t offset = ap->a_f_offset;
	struct apfs_inode ino;
	vm_offset_t address = 0;
	uint64_t size = 0;
	uint64_t t0 = apfs_ticks();
	struct apfs_lock_proc *lp = NULL;
	off_t at, end;
	int error = 0;

	if (upl == NULL) {
		return EINVAL;
	}
	if (offset < 0 || (offset & PAGE_MASK_64) || (ap->a_size & PAGE_MASK) ||
	    (upl_offset & PAGE_MASK)) {
		error = EINVAL;
	} else {
		ino = APFS_VP(vp)->an_ino;
		size = APFS_VP(vp)->an_size;
		if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
			error = ENOMEM;
		}
	}
	if (error) {
		if (!(ap->a_flags & UPL_NOCOMMIT)) {
			ubc_upl_abort_range(upl, upl_offset, (upl_size_t)ap->a_size,
			    UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
		}
		return error;
	}

	/*
	 * Say once how big a range the paging machinery asks for.  It decides how much the
	 * holding of a decompressed piece can save: if a request already covers a whole
	 * 64 KB piece, the piece was being decompressed once per request anyway.
	 */
	{
		static bool said;

		if (!said) {
			said = true;
			printf("apfs: pagein asks for %u bytes at a time (page size %u)\n",
			    (unsigned)ap->a_size, (unsigned)PAGE_SIZE);
		}
	}
	end = offset + (off_t)ap->a_size;
	for (at = offset; at < end;) {
		uint8_t *dst = (uint8_t *)address + upl_offset + (at - offset);
		size_t want, got = 0;

		if ((uint64_t)at >= size) {
			bzero(dst, (size_t)(end - at));         /* past the end of the file */
			break;
		}
		want = (size_t)MIN(end - at, (off_t)(size - (uint64_t)at));
		os_atomic_inc(&am->am_nx.requests, relaxed);
		apfs_lock(am);
		if (at == offset) {
			lp = lock_proc(am);
			lp->pagein_calls++;
			lp->pagein_bytes += ap->a_size;
		}
		error = apfs_read_file(&am->am_vol, &ino, &APFS_VP(vp)->an_cf, (uint64_t)at,
		    dst, want, &got);
		apfs_unlock(am);
		os_atomic_dec(&am->am_nx.requests, relaxed);
		if (error) {
			break;
		}
		if (got == 0) {
			bzero(dst, (size_t)(end - at));
			break;
		}
		at += got;
		/* the last page of the file: zero what lies past its end */
		if ((uint64_t)at >= size && at < end) {
			bzero((uint8_t *)address + upl_offset + (at - offset),
			    (size_t)(end - at));
			break;
		}
	}

	ubc_upl_unmap(upl);
	/* (the counts: without the lock, which would be taken for nothing else) */
	if (lp != NULL) {
		os_atomic_add(&lp->pagein_ticks, apfs_ticks() - t0, relaxed);
	}
	os_atomic_add(&am->am_pagein_ticks, apfs_ticks() - t0, relaxed);
	os_atomic_inc(&am->am_pagein_calls, relaxed);
	os_atomic_add(&am->am_pagein_bytes, (uint64_t)ap->a_size, relaxed);
	/* (what the paging machinery asked for: the VM gives less under memory pressure -- 512 KB / 16) */
	os_atomic_inc(&am->am_pagein_size[ap->a_size <= 2 * PAGE_SIZE ? 0 :
	    (ap->a_size <= 4 * PAGE_SIZE ? 1 : (ap->a_size <= 8 * PAGE_SIZE ? 2 :
	    (ap->a_size <= 16 * PAGE_SIZE ? 3 : 4)))], relaxed);
	if (!(ap->a_flags & UPL_NOCOMMIT)) {
		if (error) {
			ubc_upl_abort_range(upl, upl_offset, (upl_size_t)ap->a_size,
			    UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
		} else {
			ubc_upl_commit_range(upl, upl_offset, (upl_size_t)ap->a_size,
			    UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	/* (after this fault's own pages: what a reader going on would ask for next) */
	if (error == 0) {
		apfs_pagein_ahead(am, vp, &ino, size, offset + (off_t)ap->a_size);
	}
	return error;
}

/* ------------------------------------------------------------------ directories */

#define APFS_DIRSIZ(namlen) \
	((sizeof(struct dirent) - (MAXNAMLEN + 1)) + (((namlen) + 1 + 3) & ~3))

struct apfs_rd_ctx {
	struct uio *rd_uio;
	off_t       rd_start;           /* where this call is to resume */
	off_t       rd_pos;             /* where we are in the stream of entries */
	int         rd_count;
	bool        rd_full;
	int         rd_error;
};

static int
apfs_vnop_readdir_one(struct apfs_rd_ctx *rc, const char *name, size_t name_len,
    uint64_t file_id, uint8_t d_type)
{
	struct dirent de;

	if (name_len > MAXNAMLEN) {
		return 0;               /* cannot be named through this interface; skip it */
	}
	bzero(&de, sizeof(de));
	de.d_fileno = (ino_t)file_id;
	de.d_type = d_type;
	de.d_namlen = (uint8_t)name_len;
	de.d_reclen = (uint16_t)APFS_DIRSIZ(name_len);

	if (rc->rd_pos >= rc->rd_start) {
		if (uio_resid(rc->rd_uio) < (user_ssize_t)de.d_reclen) {
			rc->rd_full = true;
			return EEXIST;      /* stop the walk: no room for this one */
		}
		memcpy(de.d_name, name, name_len);
		rc->rd_error = uiomove((caddr_t)&de, de.d_reclen, rc->rd_uio);
		if (rc->rd_error) {
			return EEXIST;
		}
		rc->rd_count++;
	}
	rc->rd_pos += de.d_reclen;
	return 0;
}

static int
apfs_readdir_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id,
    uint8_t d_type)
{
	return apfs_vnop_readdir_one(ctx, name, name_len, file_id, d_type);
}

static int
apfs_vnop_readdir(struct vnop_readdir_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_rd_ctx rc;
	int error;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (!vnode_isdir(vp)) {
		return ENOTDIR;
	}
	bzero(&rc, sizeof(rc));
	rc.rd_uio = ap->a_uio;
	rc.rd_start = uio_offset(ap->a_uio);

	/*
	 * A directory's entries are a run of records in the tree with no index, so there is
	 * nothing to seek to: the walk starts at the beginning every time and skips what
	 * has already been handed over.  "." and ".." come first and are not in the tree.
	 */
	error = apfs_vnop_readdir_one(&rc, ".", 1, an->an_id, DT_DIR);
	if (error == 0) {
		error = apfs_vnop_readdir_one(&rc, "..", 2, an->an_parent, DT_DIR);
	}
	if (error == 0) {
		uint32_t i;

		apfs_lock(am);
		if (an->an_id < APFS_SYNTH_BASE) {
			error = apfs_readdir(&am->am_vol, an->an_id, apfs_readdir_cb, &rc);
		}
		/* the synthetic directories in this one, after what is on the volume */
		for (i = 0; error == 0 && i < am->am_nsynth; i++) {
			if (am->am_synth[i].parent == an->an_id) {
				error = apfs_vnop_readdir_one(&rc, am->am_synth[i].name,
				    strlen(am->am_synth[i].name), am->am_synth[i].id, DT_DIR);
			}
		}
		apfs_unlock(am);
	}
	if (error == EEXIST) {
		error = 0;              /* we stopped it ourselves */
	}
	if (rc.rd_error != 0) {
		return rc.rd_error;
	}
	if (error != 0) {
		return error;
	}
	uio_setoffset(ap->a_uio, rc.rd_pos);
	if (ap->a_eofflag != NULL) {
		*ap->a_eofflag = rc.rd_full ? 0 : 1;
	}
	if (ap->a_numdirent != NULL) {
		*ap->a_numdirent = rc.rd_count;
	}
	return 0;
}

static int
apfs_vnop_readlink(struct vnop_readlink_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_link *link;
	char *buf;
	size_t n = 0;
	int error;

	if (!vnode_islnk(vp)) {
		return EINVAL;
	}
	link = os_atomic_load(&an->an_link, acquire);
	if (link == NULL) {
		/* the first time: from the tree, with the lock; kept for every time after */
		buf = kalloc_data(MAXPATHLEN, Z_WAITOK);
		if (buf == NULL) {
			return ENOMEM;
		}
		apfs_lock(am);
		error = apfs_readlink(&am->am_vol, an->an_id, buf, MAXPATHLEN, &n);
		apfs_unlock(am);
		if (error) {
			kfree_data(buf, MAXPATHLEN);
			return error;
		}
		link = kalloc_data(sizeof(*link) + n, Z_WAITOK);
		if (link == NULL) {
			error = uiomove(buf, (int)n, ap->a_uio);
			kfree_data(buf, MAXPATHLEN);
			return error;
		}
		link->len = n;
		memcpy(link->text, buf, n);
		kfree_data(buf, MAXPATHLEN);
		if (!os_atomic_cmpxchg(&an->an_link, NULL, link, release)) {
			kfree_data(link, sizeof(*link) + n);        /* somebody else's is in */
			link = os_atomic_load(&an->an_link, acquire);
		}
	}
	return uiomove(link->text, (int)link->len, ap->a_uio);
}

/* ------------------------------------------------------------------ attributes */

/*
 * The attributes the operating system keeps for itself are not shown to user space: they
 * are how a symbolic link and a compressed file are stored, not things a reader should
 * see.
 */
static bool
apfs_xattr_hidden(const char *name)
{
	return strcmp(name, APFS_SYMLINK_EA_NAME) == 0 ||
	       strcmp(name, APFS_DECMPFS_XATTR_NAME) == 0 ||
	       strcmp(name, APFS_RSRC_FORK_XATTR_NAME) == 0;
}

static int
apfs_vnop_getxattr(struct vnop_getxattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_xattr xa;
	uint8_t *buf;
	size_t n = 0, whole;
	int error;

	if (ap->a_name == NULL) {
		return EINVAL;
	}
	if (apfs_xattr_hidden(ap->a_name)) {
		return ENOATTR;
	}
	apfs_lock(am);
	error = apfs_xattr_lookup(&am->am_vol, an->an_id, ap->a_name, &xa);
	apfs_unlock(am);
	if (error) {
		return error == ENOENT ? ENOATTR : error;
	}
	whole = (xa.flags & APFS_XATTR_DATA_STREAM) ? (size_t)xa.stream_size : xa.data_len;
	if (ap->a_uio == NULL) {
		*ap->a_size = whole;    /* only the size was asked for */
		return 0;
	}
	if (uio_resid(ap->a_uio) < (user_ssize_t)whole) {
		return ERANGE;
	}
	buf = kalloc_data(whole ? whole : 1, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	apfs_lock(am);
	error = apfs_xattr_read(&am->am_vol, an->an_id, ap->a_name, buf, whole, &n);
	apfs_unlock(am);
	if (error == 0) {
		error = uiomove((caddr_t)buf, (int)n, ap->a_uio);
	}
	kfree_data(buf, whole ? whole : 1);
	return error;
}

struct apfs_lx_ctx {
	struct uio *lx_uio;
	size_t      lx_size;
	int         lx_error;
};

static int
apfs_listxattr_cb(void *ctx, const char *name, size_t name_len,
    __unused const struct apfs_xattr *xa)
{
	struct apfs_lx_ctx *lc = ctx;

	if (apfs_xattr_hidden(name)) {
		return 0;
	}
	if (lc->lx_uio == NULL) {
		lc->lx_size += name_len + 1;
		return 0;
	}
	if (uio_resid(lc->lx_uio) < (user_ssize_t)(name_len + 1)) {
		lc->lx_error = ERANGE;
		return EEXIST;
	}
	lc->lx_error = uiomove((caddr_t)(uintptr_t)name, (int)name_len + 1, lc->lx_uio);
	return lc->lx_error ? EEXIST : 0;
}

static int
apfs_vnop_listxattr(struct vnop_listxattr_args *ap)
{
	struct apfs_mount *am = APFS_VP_MOUNT(ap->a_vp);
	struct apfs_node *an = APFS_VP(ap->a_vp);
	struct apfs_lx_ctx lc;
	int error;

	bzero(&lc, sizeof(lc));
	lc.lx_uio = ap->a_uio;
	apfs_lock(am);
	error = apfs_xattr_list(&am->am_vol, an->an_id, apfs_listxattr_cb, &lc);
	apfs_unlock(am);
	if (error == EEXIST) {
		error = 0;
	}
	if (error == 0 && lc.lx_error != 0) {
		return lc.lx_error;
	}
	if (error) {
		return error;
	}
	if (ap->a_uio == NULL && ap->a_size != NULL) {
		*ap->a_size = lc.lx_size;
	}
	return 0;
}

/* ------------------------------------------------------------------ the rest */

static int
apfs_vnop_open(struct vnop_open_args *ap)
{
	if ((ap->a_mode & (FWRITE | O_TRUNC | O_CREAT)) &&
	    !apfs_writable(APFS_VP_MOUNT(ap->a_vp))) {
		return EROFS;
	}
	return 0;
}

static int
apfs_vnop_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
apfs_vnop_mmap(__unused struct vnop_mmap_args *ap)
{
	/* the mount is read-only, so VFS has already refused a writable mapping */
	return 0;
}

static int
apfs_vnop_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

/*
 * The last reference to this file has gone.  Give back what was worked out about how its
 * contents are stored -- the table of compressed pieces and, above all, the 64 KB of the
 * piece last decompressed.
 *
 * It used to be kept until the vnode was reclaimed, which is later: VFS holds on to
 * vnodes it may want again, up to desiredvnodes of them (1024 here), so up to 64 MB of
 * pieces nobody was reading.  Here each file gives it back as soon as it is closed, and
 * the cache still covers what it is for: the reads of one file, one after another.
 * (This was first suspected of resetting the phone on long walks; it was not -- memory
 * stayed flat throughout -- see hakios.apfs_check.c for what was.)
 */
static int
apfs_vnop_inactive(struct vnop_inactive_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp);

	if (an == NULL) {
		return 0;
	}
	/* the last close of a file whose last name is long gone: now it goes for good */
	if (an->an_unlinked) {
		struct apfs_mount *am = an->an_mount;

		apfs_lock_write(am);
		(void)apfs_delete_inode(&am->am_vol, an->an_id, apfs_now());
		apfs_unlock_write(am);
		an->an_unlinked = false;
	}
	/*
	 * Most files hold nothing to give back (a directory, a link, a file not compressed): then
	 * not even the lock. Looked at without it: a state being set up meanwhile is given back with
	 * the vnode instead (apfs_vnop_reclaim), which is all a wrong "nothing" can cost.
	 */
	if (os_atomic_load(&an->an_cf.piece_off, relaxed) == NULL &&
	    os_atomic_load(&an->an_cf.piece_len, relaxed) == NULL &&
	    os_atomic_load(&an->an_cf.held_data, relaxed) == NULL &&
	    os_atomic_load(&an->an_cf.ra_data, relaxed) == NULL) {
		os_atomic_inc(&an->an_mount->am_inactive_free, relaxed);
		return 0;
	}
	apfs_lock(an->an_mount);
	apfs_cfile_fini(&an->an_cf);
	apfs_unlock(an->an_mount);
	return 0;
}

static int
apfs_vnop_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);

	if (an != NULL) {
		apfs_lock(an->an_mount);
		apfs_cfile_fini(&an->an_cf);
		apfs_unlock(an->an_mount);
		apfs_node_detach(an);
		vnode_clearfsnode(vp);
		if (an->an_link != NULL) {
			kfree_data(an->an_link, sizeof(*an->an_link) + an->an_link->len);
		}
		kfree_type(struct apfs_node, an);
	}
	return 0;
}

/*
 * No ioctl or fsctl means anything here.  ENOTTY is what callers take for "this filesystem
 * does not have that"; the default, ENOTSUP, is not -- the iOS dyld, probing the root
 * volume for APFS features, gives up on it.
 */
/*
 * Advisory record locks (fcntl/F_SETLK, flock).  The kernel keeps the records on the
 * vnode itself, so this is the kernel's own manager: securityd locks the keychain
 * databases, iconservicesagent its caches, and without this every one of them either
 * leaves with an error or aborts before the splash screen is taken down.
 */
static int
apfs_vnop_advlock(struct vnop_advlock_args *ap)
{
	return lf_advlock(ap);
}

static int
apfs_vnop_ioctl(__unused struct vnop_ioctl_args *ap)
{
	return ENOTTY;
}

static int
apfs_vnop_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 32767;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = 255;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return 0;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return 0;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		return 0;
	case _PC_CASE_SENSITIVE:
		*ap->a_retval = (APFS_VP_MOUNT(ap->a_vp)->am_vol.incompat &
		    APFS_INCOMPAT_CASE_INSENSITIVE) ? 0 : 1;
		return 0;
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_XATTR_SIZE_BITS:
		*ap->a_retval = 32;
		return 0;
	default:
		return EINVAL;
	}
}

#define VOPFUNC int (*)(void *)

int(**apfs_vnodeop_p)(void *);
const struct vnodeopv_entry_desc apfs_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_ioctl_desc, .opve_impl = (VOPFUNC)apfs_vnop_ioctl },
	{ .opve_op = &vnop_advlock_desc, .opve_impl = (VOPFUNC)apfs_vnop_advlock },
	{ .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)apfs_vnop_lookup },
	{ .opve_op = &vnop_create_desc, .opve_impl = (VOPFUNC)apfs_vnop_create },
	{ .opve_op = &vnop_mkdir_desc, .opve_impl = (VOPFUNC)apfs_vnop_mkdir },
	{ .opve_op = &vnop_mknod_desc, .opve_impl = (VOPFUNC)apfs_vnop_mknod },
	{ .opve_op = &vnop_remove_desc, .opve_impl = (VOPFUNC)apfs_vnop_remove },
	{ .opve_op = &vnop_rmdir_desc, .opve_impl = (VOPFUNC)apfs_vnop_rmdir },
	{ .opve_op = &vnop_rename_desc, .opve_impl = (VOPFUNC)apfs_vnop_rename },
	{ .opve_op = &vnop_link_desc, .opve_impl = (VOPFUNC)apfs_vnop_link },
	{ .opve_op = &vnop_symlink_desc, .opve_impl = (VOPFUNC)apfs_vnop_symlink },
	{ .opve_op = &vnop_setattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_setattr },
	{ .opve_op = &vnop_write_desc, .opve_impl = (VOPFUNC)apfs_vnop_write },
	{ .opve_op = &vnop_setxattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_setxattr },
	{ .opve_op = &vnop_removexattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_removexattr },
	{ .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)apfs_vnop_open },
	{ .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)apfs_vnop_close },
	{ .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_getattr },
	{ .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)apfs_vnop_read },
	{ .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_getxattr },
	{ .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_listxattr },
	{ .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)nop_fsync },
	{ .opve_op = &vnop_readdir_desc, .opve_impl = (VOPFUNC)apfs_vnop_readdir },
	{ .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)apfs_vnop_readlink },
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)apfs_vnop_inactive },
	{ .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)apfs_vnop_reclaim },
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)apfs_vnop_pathconf },
	{ .opve_op = &vnop_mmap_desc, .opve_impl = (VOPFUNC)apfs_vnop_mmap },
	{ .opve_op = &vnop_mnomap_desc, .opve_impl = (VOPFUNC)apfs_vnop_mnomap },
	{ .opve_op = &vnop_pagein_desc, .opve_impl = (VOPFUNC)apfs_vnop_pagein },
	{ .opve_op = &vnop_pageout_desc, .opve_impl = (VOPFUNC)apfs_vnop_pageout },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};
const struct vnodeopv_desc apfs_vnodeop_opv_desc = {
	.opv_desc_vector_p = &apfs_vnodeop_p,
	.opv_desc_ops = apfs_vnodeop_entries
};
