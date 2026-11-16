/*
 * Reading the Apple File System: the part that knows the format and nothing about vnodes.
 *
 * The same file is built into the kernel and, by tools/apfs/build-tool.sh, into a program
 * on the build machine (tools/apfs/apfs-tool.c) that runs it against a real image and
 * compares the result with tools/apfs/apfs.py, which is itself checked against 7-Zip.
 * Nothing goes into the kernel that has not agreed with the image on the computer first.
 *
 * The layout being read is in apfs_format.h, written from Apple's reference.
 *
 * Where the bytes come from is the caller's business (struct apfs_dev): in the kernel a
 * block device, in the tool a raw image.  Every structure is checked before it is used --
 * a block's Fletcher-64, every offset and length inside a B-tree node, every record's
 * length against the fields read out of it -- so a damaged or hostile image comes back as
 * EIO and never as a wild pointer.
 *
 * Read only, and nothing here ever writes to the device.
 *
 * What is read: unencrypted volumes, sealed or not; the live tree only (no snapshots);
 * inodes, directories, symbolic links, hard links, extended attributes (embedded and in
 * their own data stream), file extents and holes, and files the operating system
 * compressed (decmpfs: zlib and stored, in the attribute or in the resource fork).
 * What is not: encryption, snapshots, Fusion, and writing of any kind.
 */

#ifndef _MISCFS_APFS_APFS_LIB_H_
#define _MISCFS_APFS_APFS_LIB_H_

#include "apfs_format.h"

/*
 * A clock for the breakdown counters below.  Raw ticks: the kernel's are mach absolute
 * time and are turned into nanoseconds where they are printed, the host's already are
 * nanoseconds.
 */
#ifdef KERNEL
#include <kern/clock.h>
#define apfs_ticks()    mach_absolute_time()
#else
#include <time.h>
static inline uint64_t
apfs_ticks(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

#ifdef KERNEL
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#define apfs_alloc(n)        kalloc_data((vm_size_t)(n), Z_WAITOK | Z_ZERO)
#define apfs_free(p, n)      kfree_data((p), (vm_size_t)(n))
#define apfs_log(...)        kprintf("apfs: " __VA_ARGS__)
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define apfs_alloc(n)        calloc(1, (n))
#define apfs_free(p, n)      ((void)(n), free(p))
#define apfs_log(...)        fprintf(stderr, "apfs: " __VA_ARGS__)
#endif

/*
 * How many blocks of metadata to keep (8 MB at 4096 bytes a block).  64 was plenty from a
 * disk in memory; from the phone's flash a block not kept is a random 4 KB read of 113 us.
 * A walk of the whole root volume read 123940 blocks with 64 kept, 83841 with 512, 63789
 * with 2048 and 61831 with 8192.  Found through a hash of the address, the one to give up
 * picked by a clock hand (a block used since the hand last passed gets another round).
 */
#define APFS_CACHE_BLOCKS 2048
#define APFS_CACHE_HASH   4096          /* a power of two */

/* Room for a symbolic link's target (Darwin's own limit is 1024 bytes; it is kept in an xattr) */
#define APFS_LINK_MAX 4096

/* How many pieces a sequential reader's read-ahead decompresses at a time */
#define APFS_RA_PIECES 7

/*
 * The device.  read() must fill the whole buffer or return an error; offsets and lengths
 * are always whole blocks of the container.
 *
 * read_data() is the same for the contents of files, which are read once and not kept:
 * straight into the caller's memory, past any cache, and called without the caller's lock
 * -- so it must be safe on several threads at once and touch nothing apfs_lib keeps.
 * NULL: read() is used for those too (still without the lock, so read() must then be safe
 * that way as well).
 *
 * write() is for apfs_write.c and nothing else: whole blocks, and only ever blocks that
 * hold the contents of one file (apfs_write_in_place).  A device that leaves it NULL -- as
 * every mount of this driver does -- cannot be written to at all, which is what it was
 * before this field existed.
 */
struct apfs_dev {
	void     *ctx;
	uint64_t  size;                 /* bytes, 0 if not known */
	int     (*read)(void *ctx, uint64_t off, void *buf, size_t len);
	int     (*read_data)(void *ctx, uint64_t off, void *buf, size_t len);
	int     (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
};

/* A cached block's memory, given up while somebody may still be reading it (nx->retired) */
struct apfs_retired {
	struct apfs_retired *next;
	uint64_t             epoch;
	uint8_t             *data;
};

/* One cached block of metadata. */
struct apfs_buf {
	uint64_t  addr;                 /* block address, APFS_BUF_EMPTY if unused */
	int32_t   next;                 /* the next in its hash chain, or -1 */
	bool      ref;                  /* used since the clock hand last passed it */
	uint32_t  used;                 /* nx->fetches when it was last used */
	bool      checked;              /* its checksum has been verified */
	uint8_t  *data;
};

/*
 * Blocks of metadata are read a few at a time, aligned: the one asked for and its
 * neighbours, which a walk mostly wants next (meta_cluster, boot-arg apfs_meta_cluster,
 * up to APFS_META_CLUSTER).  From the phone's flash, walking the whole root volume: one
 * block a read, 64459 reads and 7294 ms; four, 36086 and 5680 ms; eight, 27918 and
 * 5673 ms; sixteen, 26214 and 6937 ms -- a random read costs about 100 us whatever its
 * size, and then the bytes.  The walk: 46328 / 44701 / 44823 / 45439 ms.
 */
#define APFS_META_CLUSTER         16
#define APFS_META_CLUSTER_DEFAULT 4

/* A block used fewer than this many fetches ago is never given up (see nx_block) */
#define APFS_CACHE_RECENT 64

#define APFS_BUF_EMPTY ((uint64_t)~0ULL)

/* A B-tree, ready to be walked. */
struct apfs_btree {
	struct apfs_nx *nx;
	struct apfs_omap *omap;         /* NULL for a tree whose node ids are block addresses */
	uint64_t  root;                 /* block address of the root node */
	uint64_t  root_oid;             /* its object id: a sealed tree names its nodes from it */
	uint32_t  flags;                /* bt_flags out of the root's btree_info */
	uint32_t  key_size;             /* nonzero only for fixed-size keys */
	uint32_t  val_size;
	uint64_t  key_count;
	uint64_t  node_count;
};

/* An object map: it turns an object identifier into a block address. */
struct apfs_omap {
	struct apfs_btree tree;
	bool              ready;
};

/*
 * What the object maps have answered: a B-tree node of the file-system tree is named by
 * a virtual id, and every step down that tree used to be a walk down an object map too.
 * The mount is read-only at one checkpoint, so an answer never goes stale.  One cache
 * for the container's map and its volumes', direct-mapped, made at apfs_nx_open.
 */
#define APFS_OMAP_CACHE 2048            /* a power of two */

struct apfs_omap_hit {
	uint64_t map;                   /* which object map: the root of its tree */
	uint64_t oid;                   /* 0: empty */
	uint64_t xid;
	uint64_t addr;
	uint64_t key_xid;               /* the transaction the record's key names */
};

struct apfs_scratch;            /* apfs_lib.c's own */

/* A mounted container. */
struct apfs_nx {
	struct apfs_dev   dev;
	uint32_t          block_size;
	uint64_t          block_count;
	apfs_xid_t        xid;          /* the checkpoint we are reading */
	uint64_t          omap_addr;
	apfs_oid_t        fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
	uint32_t          fs_count;
	uint8_t           uuid[16];
	struct apfs_omap  omap;         /* resolves volume superblocks */
	/*
	 * The checkpoint rings, as the live superblock names them.  The read path does not
	 * need the data ring at all and reads the descriptor ring only at open; they are kept
	 * so that apfs_write.c can refuse outright a write that would land in either of them
	 * -- the one bound that does not depend on having read any tree correctly.
	 */
	uint64_t          xp_desc_base, xp_data_base;
	uint32_t          xp_desc_blocks, xp_data_blocks;
	/*
	 * The live container superblock itself, kept only when the device takes writes: a
	 * transaction copies it, changes what it has to, and commits a new one.  NULL on a
	 * read-only mount, and a writer refuses to begin without it.
	 */
	uint8_t          *sb;
	/*
	 * The extent of the rings the live checkpoint occupies: its descriptor blocks hold
	 * the checkpoint map, and the map is how the ephemeral objects of that checkpoint --
	 * the space manager among them -- are found in the data ring.
	 */
	uint32_t          xp_desc_next, xp_data_next;   /* where the next checkpoint goes */
	uint32_t          xp_desc_index, xp_desc_len;
	/*
	 * The caller's lock round this container, which apfs_lib may let go of while it
	 * does work that touches nothing shared -- reading a file's contents off the device
	 * (dev.read_data), decompressing a piece into a buffer no one else sees -- and take
	 * back afterwards.  Everything else here (the block cache, the cursors, a file's held
	 * piece, the scratch list) is only touched with it held.  NULL for a caller
	 * with no lock, such as the single-threaded program on the build machine.
	 */
	void            (*unlock)(void *ctx);
	void            (*relock)(void *ctx);
	void             *lock_ctx;
	struct apfs_buf  *cache;        /* APFS_CACHE_BLOCKS of them, made at apfs_nx_open */
	int32_t          *cache_hash;   /* APFS_CACHE_HASH chain heads, -1 for none */
	uint32_t          hand;
	uint32_t          fetches;      /* blocks read into the cache, ever (wraps) */
	uint8_t          *cluster;      /* APFS_META_CLUSTER blocks, made at the first miss */
	/*
	 * With a caller's lock (unlock/relock set), a block of metadata missing from the cache is
	 * read with the lock let go of -- other threads go on meanwhile -- into memory of its own,
	 * and put in afterwards (unless somebody else put it in first). So that nobody ever reads
	 * a cached block's memory while it is given to another block, a block that leaves the
	 * cache leaves its memory behind on `retired`, tagged with `epoch`, and the cache slot
	 * gets new memory; the caller frees what no operation still running can be looking at
	 * (apfs_nx_reclaim, with the oldest epoch any of its operations began at).
	 */
	uint64_t          lookups_hashed, lookups_scanned;     /* names found by their hash, by reading the directory */
	uint64_t          epoch;
	struct apfs_retired *retired;   /* newest first */
	uint64_t          retired_now, retired_most, unlocked_fills, fill_races;
	uint32_t          meta_cluster; /* blocks a metadata read brings in, 1..APFS_META_CLUSTER (0: the default) */
	/*
	 * A request to the device costs next to nothing (a disk in memory): no reading pieces
	 * together in batch_group, one block a metadata read.  Set by the caller; from the
	 * phone's memory disk batch_group cost one reader 612 -> 663 ms over the restore
	 * ramdisk, from its flash it saved 52.9 -> 46.3 s over the root volume.
	 */
	bool              cheap_requests;
	/*
	 * Whether a commit's blocks are read back and compared after they are written (boot-arg
	 * apfs_verify, default 1).  A write of the loader's block service costs some 400 us
	 * whatever its length, so the read back cost as much as the write did: with it off, a
	 * commit is half the requests (p393: the writer's mutex was held 0.3 to 1.1 s at a time).
	 */
	bool              write_verify;
	uint64_t          wr_calls, wr_bytes, wr_ticks;   /* the device's writes: requests, bytes, time */
	uint64_t          prefetched, prefetch_used;   /* neighbours brought in, and later asked for */
	uint64_t          reads;
	uint64_t          hits;
	/*
	 * Where the time goes, to tell apart waiting for the device from decompressing.
	 * Ticks, not nanoseconds -- see apfs_ticks().
	 */
	uint64_t          dev_calls, dev_ticks, dev_bytes;
	/* the same for file contents, read without the lock (read_data) */
	uint64_t          data_calls, data_ticks, data_bytes;
	/*
	 * The highest block either kind of read has reached.  What it is for: the phone has
	 * no driver for its own storage, so a volume there has to live in a RAM disk, and
	 * that says how much of the front of the image would be enough.
	 */
	uint64_t          max_addr;             /* any block */
	uint64_t          max_data_addr;        /* blocks of file contents only */
	uint64_t          inflate_calls, inflate_ticks, inflate_in, inflate_out;
	uint64_t          inflate_fallbacks;   /* streams libdeflate refused and zlib took */
	/*
	 * Decompressors and buffers for reading compressed pieces, kept from one read to the
	 * next instead of allocated for each; touched only with the lock held.
	 */
	struct apfs_scratch *scratch;
	uint32_t          scratch_kept;
	/*
	 * Several threads for one request: run fn(arg, item, worker) for every item below
	 * `items`, on up to `workers` threads at once, the caller's among them, each with a
	 * worker number below `workers`; return the first error any item gave.  Called
	 * without the lock, and fn takes none.  NULL (the program on the build machine): the
	 * items are done one after another on the caller's thread.
	 */
	int             (*parallel)(void *ctx, uint32_t items, uint32_t workers,
	    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg);
	void             *parallel_ctx;
	uint32_t          parallel_max;         /* threads it can bring, the caller's included */
	/*
	 * Reads in flight on the container, kept by the caller with the lock held.  The
	 * threads nx->parallel brings are shared out among them, parallel_max / requests
	 * each: with four, all of them to a request that is alone, two each to two, none
	 * from three up -- the processors are busy already, and on the phone four readers
	 * came out 7% slower with helpers than without.  0 (the program on the build
	 * machine) counts as alone.
	 */
	uint32_t          requests;
	uint64_t          batch_calls, batch_pieces;   /* how many requests used it, for how much */
	uint64_t          ra_batches, ra_pieces, ra_hits;  /* read-ahead: filled, and used */
	struct apfs_omap_hit *omap_cache;       /* APFS_OMAP_CACHE of them, or NULL */
	uint64_t          omap_hits, omap_misses;
};

/* A volume inside the container. */
struct apfs_vol {
	struct apfs_nx   *nx;
	uint32_t          index;
	apfs_oid_t        oid;
	apfs_paddr_t      sb_addr;      /* where the volume superblock is now */
	apfs_paddr_t      omap_addr;    /* and its object map, a physical object */
	uint64_t          incompat;
	uint64_t          fs_flags;
	uint16_t          role;
	/*
	 * Whether directory-entry keys carry a hash of the name.  Apple's reference
	 * describes both shapes of the key but never says which a volume uses; the hash it
	 * defines (CRC-32C of the name decomposed and widened) is only of use for looking
	 * a name up without regard to case or normalization, and the iOS 16.5 volume, which
	 * is normalization-insensitive, does carry it.  So: hashed when either of those two
	 * features is on.  A wrong guess does not go unnoticed -- the name length would not
	 * fit the key and the record is refused.
	 */
	bool              hashed_names;
	/*
	 * A sealed volume (the iOS 16 cryptexes) keeps its files' extents in a tree of their own,
	 * keyed by stream and offset, rather than among the file-system tree's records.
	 */
	struct apfs_btree fext;
	bool              has_fext;
	/*
	 * The extent-reference tree: which stream owns a run of physical blocks and how many
	 * references there are to it.  The read path has no use for it -- a file's own extent
	 * records say where its bytes are -- but it is the only place that says whether those
	 * blocks are shared with a clone, so apfs_write.c will not write a block without it.
	 * Only the identifier is kept here, and the tree is opened into the caller's own memory
	 * (apfs_extentref_open): a whole struct apfs_btree more in every mount would put
	 * struct apfs_mount over the 16 KB a mount is allowed (apfs_vfsops.c).
	 */
	apfs_oid_t        extentref_oid;        /* a block address: the tree is physical; 0 if none */
	uint64_t          num_snapshots;        /* a snapshot shares blocks with the live tree */
	apfs_oid_t        integrity_meta_oid;   /* a sealed volume: hashes of the files' contents */
	uint64_t          num_files;
	uint64_t          num_dirs;
	uint64_t          num_symlinks;
	uint64_t          alloc_count;
	/*
	 * The first object identifier this mount may hand out.  apfs_next_obj_id in the
	 * volume superblock says the same on a clean image, but a commit that failed
	 * after its tree blocks were written leaves the superblock behind the tree.
	 * A create would then reuse an oid that a live B-tree node already owns, the
	 * node's entry in the object map would be shadowed, and part of the tree would
	 * stop being reachable.  Never below anything either tree already names.
	 */
	uint64_t          id_floor;
	char              name[APFS_VOLNAME_LEN];
	struct apfs_omap  omap;         /* resolves the nodes of the tree below */
	struct apfs_btree tree;         /* the file-system tree */
};

/* One record out of a file-system tree: pointers into a cached block. */
struct apfs_rec {
	const uint8_t *key;
	uint16_t       key_len;
	const uint8_t *val;
	uint16_t       val_len;
	uint64_t       obj_id;
	uint8_t        type;
};

/*
 * A walk over the records of a tree.  It holds one cached block per level, so a cursor
 * must not outlive the container and only one may be open at a time per container unless
 * the cache is larger than the tree is deep.
 */
#define APFS_MAX_LEVELS 8

struct apfs_cursor {
	struct apfs_btree *tree;
	int                depth;
	uint64_t           node[APFS_MAX_LEVELS];  /* the block address at each level,
	                                            * never an object identifier */
	uint32_t           index[APFS_MAX_LEVELS];
	uint64_t           hash8[APFS_MAX_LEVELS];  /* a sealed tree's nodes: the first 8 bytes of the
	                                            * SHA-256 the parent keeps of each (0 for the root) */
	bool               done;
};

/* What an inode record says, after the extended fields have been picked apart. */
struct apfs_inode {
	uint64_t obj_id;
	uint64_t parent_id;
	uint64_t private_id;            /* the data stream's id, for a regular file */
	uint64_t create_time;
	uint64_t mod_time;
	uint64_t change_time;
	uint64_t access_time;
	uint64_t internal_flags;
	int32_t  nchildren;             /* a directory: children; otherwise: links */
	uint32_t bsd_flags;
	uint32_t owner;
	uint32_t group;
	uint16_t mode;
	uint64_t size;                  /* the logical size, what stat reports */
	uint64_t alloced_size;
	uint32_t rdev;
	bool     has_dstream;
	bool     is_sparse;
};

/* One extent of a data stream. */
struct apfs_extent {
	uint64_t logical;
	uint64_t length;
	uint64_t phys;                  /* 0 is a hole */
};

/* An extended attribute, as found in its record. */
struct apfs_xattr {
	uint16_t       flags;
	const uint8_t *data;            /* embedded value, or an apfs_j_xattr_dstream */
	uint16_t       data_len;
	uint64_t       stream_id;       /* when the value is in its own stream */
	uint64_t       stream_size;
};

/* No piece is being held. */
#define APFS_NO_PIECE ((uint64_t)~0ULL)

/*
 * How a file's contents are stored, and what has been worked out about it so far.
 *
 * A compressed file's bytes are cut into 64 KB pieces, each its own zlib stream, with a
 * table in the resource fork saying where each piece is.  Reaching that table costs
 * several walks of the file-system tree -- the attribute record, the fork's header, the
 * table entry -- and a piece has to be decompressed whole however few of its bytes are
 * wanted.  A reader that comes back for the next page would pay all of that again, which
 * is what made paging a mapped file slow.
 *
 * So this holds what has been found: the fork's stream, the whole table, and the last
 * piece decompressed.  One of these belongs to whoever is reading the file -- the driver
 * keeps one per vnode -- and it is handed to apfs_read_file() on every call.
 * apfs_cfile_fini() gives back what it holds.  Passing NULL instead works and is correct,
 * and is the slow way.
 */
struct apfs_cfile {
	bool      ready;                /* set up, for the inode in obj_id */
	uint64_t  obj_id;
	bool      compressed;
	uint32_t  type;                 /* decmpfs compression type */
	uint64_t  size;                 /* uncompressed size */
	bool      in_rsrc;              /* the bytes are in com.apple.ResourceFork */

	/* the resource fork and its table of pieces, read once */
	uint64_t  rsrc_id;
	uint64_t  rsrc_size;
	uint64_t  table_base;
	uint32_t  piece_count;
	uint32_t *piece_off;            /* piece_count entries, from table_base */
	uint32_t *piece_len;

	/* the piece last decompressed */
	uint64_t  held;                 /* its index, or APFS_NO_PIECE */
	uint8_t  *held_data;
	size_t    held_len;
	size_t    held_cap;             /* 0 when this file is not worth holding */

	/*
	 * Read-ahead.  A reader with threads to spare (alone on the volume, or one of two)
	 * that asks for the piece right after the one its last read ended on is taken to be
	 * reading the file through: the pieces
	 * after it are decompressed in the same batch, on the other threads, into ra_data,
	 * and its next reads are copies out of that.  All of it happens inside the request
	 * that asked; ra_filling keeps everyone else off ra_data while the lock is let go of.
	 */
	uint64_t  ra_expect;            /* the piece the last whole-piece read ended before */
	uint64_t  ra_first;             /* the first piece in ra_data */
	uint32_t  ra_count;             /* how many are there; 0: none */
	bool      ra_filling;
	uint8_t  *ra_data;              /* APFS_RA_PIECES pieces, made when first wanted */
	uint32_t  ra_len[APFS_RA_PIECES];
};

/* ---- container and volumes ---- */

int apfs_nx_open(struct apfs_nx *nx, const struct apfs_dev *dev);
void apfs_nx_close(struct apfs_nx *nx);
/* The hash APFS keeps with a name in its directory record (22 bits); false: the name cannot be hashed */
bool apfs_name_hash(const char *name, size_t name_len, bool fold, uint32_t *hash);
/* Free the retired blocks' memory no operation that began at `oldest` or later can see (lock held) */
void apfs_nx_reclaim(struct apfs_nx *nx, uint64_t oldest);

/* The cached copy of a block, gone: the transaction that wrote it calls this for each. */
void apfs_nx_cache_drop(struct apfs_nx *nx, uint64_t addr);

extern const char *apfs_where;    /* debug only */
#ifndef KERNEL
/* For tests on the build machine: every cached block out (retired, with a caller's lock), so all is read again */
void apfs_nx_cache_flush(struct apfs_nx *nx);
/* Every directory record of the volume, with the hash stored in its key (hashed keys only) */
int apfs_dirrec_walk(struct apfs_vol *vol, int (*cb)(void *ctx, uint64_t dir_id, const char *name,
    size_t name_len, uint32_t stored_hash, uint64_t file_id), void *ctx);
#endif
int apfs_vol_open(struct apfs_nx *nx, uint32_t index, struct apfs_vol *vol);

/* ---- records ---- */

/*
 * Put a cursor on the first record of (obj_id, type), or on the first record after it if
 * there is none.  A file-system tree sorts by object id first and record type second, so
 * the records of one object and type are a run; apfs_cursor_next() walks it and the
 * caller stops when obj_id or type changes.
 */
int apfs_seek(struct apfs_btree *tree, uint64_t obj_id, uint8_t type,
    struct apfs_cursor *cur);
int apfs_cursor_next(struct apfs_cursor *cur, struct apfs_rec *rec);

/* The greatest object identifier any key of a tree names: its rightmost leaf. */
int apfs_btree_max_key_oid(struct apfs_btree *tree, uint64_t *oid);

/* ---- what the records mean ---- */

/*
 * Read an inode record.  ino->size is the size of the data stream, which for a compressed
 * file or a symbolic link is not the size a reader of the file sees -- those keep their
 * contents elsewhere.  This stays cheap, touching only the one record; use
 * apfs_inode_size() where the size reported to a caller has to be right.
 */
int apfs_inode_get(struct apfs_vol *vol, uint64_t obj_id, struct apfs_inode *ino);

/*
 * The raw value of an inode record: what a writer copies and changes, so that the
 * extended fields it does not understand (the name, the data stream) stay as they
 * are.  ENOENT when there is no such record, ENOSPC when the caller's buffer is
 * too small.
 */
int apfs_inode_value_get(struct apfs_vol *vol, uint64_t obj_id, uint8_t *buf,
    size_t cap, uint16_t *len);

/* An object map's answer for one identifier at one transaction: the block. */
int apfs_omap_lookup(struct apfs_omap *omap, apfs_oid_t oid, apfs_xid_t xid,
    uint64_t *addr);

/* The same, and the transaction of the record's key: a writer needs it to retire the record
 * the new one replaces. */
int apfs_omap_lookup_key(struct apfs_omap *omap, apfs_oid_t oid, apfs_xid_t xid,
    uint64_t *addr, apfs_xid_t *key_xid);

/*
 * The logical size of a file: the uncompressed size for a compressed one, the length of
 * the target for a symbolic link, the data stream's size otherwise.  Costs an attribute
 * lookup for anything that is not a plain file.
 */
int apfs_inode_size(struct apfs_vol *vol, const struct apfs_inode *ino, uint64_t *size);
int apfs_lookup(struct apfs_vol *vol, uint64_t dir_id, const char *name, size_t name_len,
    uint64_t *file_id, uint8_t *d_type);
int apfs_path_lookup(struct apfs_vol *vol, const char *path, uint64_t *file_id,
    uint8_t *d_type);

typedef int (*apfs_dir_cb)(void *ctx, const char *name, size_t name_len,
    uint64_t file_id, uint8_t d_type);
int apfs_readdir(struct apfs_vol *vol, uint64_t dir_id, apfs_dir_cb cb, void *ctx);

/*
 * Find an attribute and report where its value is kept.  Only the fields that are values
 * are filled in -- xa->data is always NULL, because a record's bytes live in a cached
 * block that the next read may reuse.  To get at the value, use apfs_xattr_read().
 */
int apfs_xattr_lookup(struct apfs_vol *vol, uint64_t obj_id, const char *name,
    struct apfs_xattr *xa);

/*
 * The value of one attribute by name, wherever it is kept: copied straight out of the
 * record while it is still in hand if it is embedded there, read from its data stream if
 * it has one.  *out is how many bytes were put in buf, which is less than the whole value
 * if buf is too small.
 */
int apfs_xattr_read(struct apfs_vol *vol, uint64_t obj_id, const char *name, void *buf,
    size_t len, size_t *out);

/*
 * Every attribute of an object.  Inside the callback xa->data points at the value if it
 * is embedded, and is good only until the callback returns; a callback that wants to keep
 * it copies it.  Returning nonzero from the callback ends the walk with that value.
 */
typedef int (*apfs_xattr_cb)(void *ctx, const char *name, size_t name_len,
    const struct apfs_xattr *xa);
int apfs_xattr_list(struct apfs_vol *vol, uint64_t obj_id, apfs_xattr_cb cb, void *ctx);

/* The target of a symbolic link, without the trailing zero byte. */
int apfs_readlink(struct apfs_vol *vol, uint64_t obj_id, char *buf, size_t len,
    size_t *out);

/* ---- file contents ---- */

/*
 * Raw bytes of a data stream, holes reading as zeros.  `stream_id` is an inode's
 * private_id for a file, or an attribute's stream id.  Short reads only at end of stream.
 */
int apfs_read_stream(struct apfs_vol *vol, uint64_t stream_id, uint64_t stream_size,
    uint64_t off, void *buf, size_t len, size_t *out);

/*
 * The extent of a data stream that covers logical offset `off`: where it begins, how long
 * it is, and the block it starts at (0 for a hole).  ENOENT past the last extent.  A
 * reader of contents has no use for this -- apfs_read_stream walks the extents itself --
 * but a writer has to know exactly which blocks are the file's own.
 */
int apfs_extent_at(struct apfs_vol *vol, uint64_t stream_id, uint64_t off,
    struct apfs_extent *ext);
int apfs_extent_next(struct apfs_vol *vol, uint64_t stream_id, uint64_t off,
    struct apfs_extent *ext);

/*
 * Open the volume's extent-reference tree into `tree`, which is the caller's to keep for as
 * long as the volume is open.  ENOENT when the volume has none.
 */
int apfs_extentref_open(struct apfs_vol *vol, struct apfs_btree *tree);

/*
 * Work out how a file's contents are stored and read in whatever is worth reading once.
 * Calling it again for the same inode does nothing; for a different one it starts over.
 */
int apfs_cfile_init(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf);

/* Give back everything a context holds.  Safe on a zeroed or already finished one. */
void apfs_cfile_fini(struct apfs_cfile *cf);

/*
 * The contents of a file as a reader of the file would see them: the data stream for a
 * plain file, the decompressed bytes for a compressed one.
 */
int apfs_read_file(struct apfs_vol *vol, const struct apfs_inode *ino,
    struct apfs_cfile *cf, uint64_t off, void *buf, size_t len, size_t *out);

/*
 * Where a file's bytes lie on the device, when they lie in one piece as they are: not
 * compressed, and one extent from its start covering its size.  *addr is that extent's first
 * block.  ENOENT otherwise (compressed, in pieces, empty).
 */
int apfs_file_in_place(struct apfs_vol *vol, const struct apfs_inode *ino, uint64_t *addr);

/* ---- odds and ends a writer needs ---- */

/*
 * A block of metadata through the cache, its checksum verified.  The pointer is into the
 * cache and stays good for fewer further reads than APFS_CACHE_RECENT, so a writer copies
 * what it means to change.  EIO for an address outside the container or a block that does
 * not check out.
 */
int apfs_block_read(struct apfs_nx *nx, uint64_t addr, const uint8_t **block);

/*
 * The same without the checksum, for the blocks that are not objects: a chunk's bitmap and
 * the internal pool's are plain bits from their first byte, with no header to check.
 */
int apfs_block_read_raw(struct apfs_nx *nx, uint64_t addr, const uint8_t **block);

/* ---- the space manager ---- */

/*
 * A container's space manager, read out of its checkpoint.  Nothing holds a pointer into
 * the cached blocks it came from: the object is copied into memory of its own (the
 * checkpoint data ring may be reused by the next transaction, and a writer will rewrite
 * this object rather than read it again), and the chunk-info blocks are read one at a time
 * as chunks are asked for.
 */
#define APFS_SM_MAX_MAPS 16     /* the ephemeral objects of one checkpoint */

struct apfs_sm_map {
	uint32_t   type;                /* storage class in the high half, type in the low */
	uint32_t   subtype;
	uint32_t   size;
	apfs_oid_t oid;
	uint64_t   paddr;               /* where it lies in the checkpoint data ring */
};

struct apfs_spaceman {
	struct apfs_nx *nx;
	uint8_t        *sm;             /* the whole object, sm_size bytes */
	uint32_t        sm_size;
	uint32_t        block_size;
	uint32_t        blocks_per_chunk;
	uint32_t        chunks_per_cib;
	uint32_t        cibs_per_cab;
	uint64_t        block_count;    /* of device 0, the only one this port has */
	uint32_t        chunk_count;
	uint32_t        cib_count;
	uint32_t        cab_count;
	uint64_t        free_count;     /* what the space manager says is free */
	uint32_t        addr_offset;    /* of the chunk-info (or address-block) array */
	uint64_t        ip_block_count;
	uint64_t        ip_bm_base;
	uint64_t        ip_base;
	uint32_t        ip_bm_size_in_blocks;
	uint32_t        ip_bm_block_count;
	uint32_t        ip_bm_xid_offset;
	uint32_t        ip_bitmap_offset;
	uint32_t        ip_bm_free_next_offset;
	uint16_t        ip_bm_free_head;
	uint16_t        ip_bm_free_tail;
	apfs_xid_t      xid;            /* the transaction the object belongs to */
	/* the checkpoint map the object came from: the ephemeral objects a commit copies */
	uint32_t        map_count;
	struct apfs_sm_map map[APFS_SM_MAX_MAPS];
};

/* ---- transactions (apfs_txn.c) ---- */

struct apfs_txn;

/*
 * A transaction being built: the blocks it will write are held until it commits, in the order
 * a crash has to survive.  NULL when the device does not take writes.  apfs_txn_abort() gives
 * back everything a transaction holds and is safe on NULL.
 */
struct apfs_txn *apfs_txn_begin(struct apfs_nx *nx, struct apfs_spaceman *sm);
void apfs_txn_abort(struct apfs_txn *txn);

/*
 * Write everything, in order, the container superblock last; then block zero.  On an error
 * some of it may have gone out: what a reader sees is still one whole checkpoint, either the
 * one before or the one this meant to make.
 */
int apfs_txn_commit(struct apfs_txn *txn);

/*
 * The modification time of one inode, as a transaction.  The file-system tree, the volume's
 * object map and the container's have to be one node each -- ENOENT when there is no such
 * inode, ENOTSUP when a tree is deeper than that, EROFS when the volume may not be written.
 */
int apfs_inode_set_mod_time(struct apfs_vol *vol, uint64_t obj_id, uint64_t when);

/*
 * An empty file, and a file taken away: the records of the file-system tree, the
 * volume's object map and the superblocks, committed as one transaction.  The
 * file's data, and a directory, are not here yet.
 */
int apfs_create_file(struct apfs_vol *vol, uint64_t parent, const char *name,
    uint16_t mode, uint32_t owner, uint32_t group, uint64_t when, uint64_t *new_id);
int apfs_unlink(struct apfs_vol *vol, uint64_t parent, const char *name,
    uint64_t when);
/* the last name of a file that is still open: zero links, the file kept for the reader */
int apfs_unlink_keep(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t when);
/* its end, at the last close: the inode, its extents, its attributes */
int apfs_delete_inode(struct apfs_vol *vol, uint64_t obj_id, uint64_t when);

/*
 * Append to a file's data stream.  The blocks are new, the file-extent record and its entry in
 * the extent-reference tree are new, and the inode's data stream grows.  Appending only, and
 * only at a block boundary: writing inside a file's existing blocks is apfs_write.c's job and
 * touches no structure.
 */
int apfs_append_file(struct apfs_vol *vol, uint64_t obj_id, const void *buf, size_t len,
    uint64_t when);
int apfs_write_data(struct apfs_vol *vol, uint64_t obj_id, uint64_t off, const void *buf,
    size_t len, uint64_t when);

/* A directory, and how it goes away: empty first, as on any Unix. */
int apfs_mkdir(struct apfs_vol *vol, uint64_t parent, const char *name, uint16_t mode,
    uint32_t owner, uint32_t group, uint64_t when, uint64_t *new_id);
int apfs_rmdir(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t when);

/*
 * Rename: the entry `name` in `parent` becomes `new_name` in `new_parent`; the file keeps
 * its identifier, its blocks and its data stream.  EEXIST when the new name is taken by
 * another file, ENOTDIR when the new parent is not a directory, EINVAL for a directory
 * moved inside itself.
 */
int apfs_rename(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t new_parent,
    const char *new_name, uint64_t when);
int apfs_link(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t obj_id,
    uint64_t when);

/*
 * Cut a file short: the extents past the new size go back to the space manager, the one the
 * file ends in keeps only its blocks that are still used, and the data stream says the new
 * size.  Growing is not this; EINVAL for a size larger than the file's, EISDIR for a
 * directory.
 */
int apfs_truncate(struct apfs_vol *vol, uint64_t obj_id, uint64_t size, uint64_t when);

/*
 * An extended attribute, set or taken away.  The value is embedded in the record
 * (APFS_XATTR_MAX_EMBEDDED_SIZE at most); a value with a stream of its own is not made here.
 * `flags` are the caller's own bits; APFS_XATTR_DATA_EMBEDDED is set and
 * APFS_XATTR_DATA_STREAM is refused.  ENOENT when removing one that is not there.
 */
int apfs_setxattr(struct apfs_vol *vol, uint64_t obj_id, const char *name, const void *data,
    size_t len, uint16_t flags, uint64_t when);
int apfs_removexattr(struct apfs_vol *vol, uint64_t obj_id, const char *name, uint64_t when);

/*
 * Attributes of an inode that setattr may change: which of them is in effect is said by the
 * flags, so that a caller changing the mode does not have to know (or keep) the owner.
 * The identifier, the parent and the data stream are not here: those are the operations
 * that move them.
 */
struct apfs_setattr {
	bool     mode_set;
	uint16_t mode;
	bool     owner_set;
	uint32_t owner;
	bool     group_set;
	uint32_t group;
	bool     flags_set;
	uint32_t bsd_flags;
	bool     atime_set;
	uint64_t atime;
	bool     mtime_set;
	uint64_t mtime;
};

int apfs_inode_setattr(struct apfs_vol *vol, uint64_t obj_id,
    const struct apfs_setattr *sa, uint64_t when);

#ifndef KERNEL
/*
 * A check of the extended-field re-packing, for the host tests: rebuild an inode's record
 * from the values it already has, report whether the bytes came out the same, and put the
 * rebuilt record back through a transaction when they did.
 */
int apfs_inode_repack_check(struct apfs_vol *vol, uint64_t obj_id, bool *same,
    uint64_t when);
#endif

/*
 * Read the container's space manager.  *sm is the caller's; apfs_spaceman_close() gives
 * back what it holds.  An error here (EIO for a structure that does not check out, ENOENT
 * when the checkpoint map names no space manager) means the container cannot be written.
 */
int apfs_spaceman_open(struct apfs_nx *nx, struct apfs_spaceman *sm);
void apfs_spaceman_close(struct apfs_spaceman *sm);

/*
 * One chunk: where it begins, how many blocks it holds, how many are free, and the block
 * its bitmap is in (0 when the chunk is entirely free).  ENOENT past the last chunk.
 */
int apfs_spaceman_chunk(struct apfs_spaceman *sm, uint32_t index, struct apfs_chunk_info *ci);

/*
 * A chunk's bitmap: one bit per block of the chunk, in the block the chunk-info record
 * names, read from offset 0 (it is not an object: no header, no checksum).  The pointer is
 * into the block cache and stays good for fewer further reads than APFS_CACHE_RECENT; NULL
 * when every block of the chunk is free.  ENOENT for a chunk with no bitmap at all.
 */
int apfs_spaceman_bitmap(struct apfs_spaceman *sm, const struct apfs_chunk_info *ci,
    const uint8_t **bitmap);

/* Is that block in use, as the bitmaps say?  ENOENT when it is outside every chunk. */
int apfs_spaceman_block_used(struct apfs_spaceman *sm, uint64_t block, bool *used);

/* The chunk a block belongs to, and its index in that chunk's bitmap. */
int apfs_spaceman_locate(struct apfs_spaceman *sm, uint64_t block, uint32_t *chunk,
    uint32_t *bit);

/* ---- odds and ends worth testing on their own ---- */

uint64_t apfs_fletcher64(const uint8_t *data, size_t len);
bool apfs_checksum_ok(const uint8_t *block, uint32_t block_size);
uint16_t apfs_mode_to_dtype(uint16_t mode);

#endif /* _MISCFS_APFS_APFS_LIB_H_ */
